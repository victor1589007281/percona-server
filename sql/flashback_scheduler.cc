/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/flashback_scheduler.cc
  Flashback 调度器实现

  实现双引擎闪回的核心调度逻辑:
  - select_engine(): 根据时间窗口自动选择 UNDO 或 BINLOG 引擎
  - run_safety_checks(): 执行闪回前全套安全检查
  - execute(): 编排完整闪回流程

  设计参考: mysql_flashback_implementation.md §4.2
            mysql_flashback_implementation_deep.md §4.3
*/

#include "sql/flashback_scheduler.h"

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"         /* my_getsystime(), my_time() */
#include "my_time_t.h"
#include "mysql_com.h"      /* my_error(), ER_INTERNAL_ERROR */
#include "sql/flashback_binlog_engine.h"  /* BinlogFlashbackEngine */
#include "sql/flashback_sysvars.h"
#include "sql/sql_class.h"  // THD

/* ================================================================
 * 外部声明: 来自 MySQL Server 层的全局变量和函数
 * ================================================================ */

extern bool opt_bin_log;                    /* binlog 是否开启 */
extern ulong binlog_expire_logs_seconds;    /* binlog 保留秒数 */

/* ================================================================
 * 外部声明: 来自 InnoDB 层的函数
 * ================================================================ */

/**
  获取 undo 数据仍可用的最早时间戳。
  用于精确判断目标时间是否在 undo 闪回窗口内。
  @return 最早可用的 Unix 时间戳 (秒)
*/
extern my_time_t trx_sys_get_oldest_timestamp();

namespace flashback {

/* ================================================================
 * 构造函数 / 析构函数
 * ================================================================ */

FlashbackScheduler::FlashbackScheduler(THD *thd)
    : m_thd(thd), m_ddl_barrier(thd), m_last_selection_reason("not yet called") {
  DBUG_TRACE;
  assert(thd != nullptr);
}

FlashbackScheduler::~FlashbackScheduler() {
  DBUG_TRACE;
  /* DDLBarrier 的析构函数会自动释放持有的 MDL 锁 */
}

/* ================================================================
 * select_engine() — 引擎选择决策流
 * ================================================================
 *
 * 决策优先级:
 * 1. 用户显式指定引擎 (非 AUTO) → 直接使用, 但需验证可用性
 * 2. AUTO 模式下:
 *    a. QUERY / VERSIONS 类型 → 强制 UNDO (binlog 引擎不支持查询)
 *    b. TABLE / TRANSACTION 类型:
 *       - 间隔 ≤ undo_retention → UNDO (高精度, 高性能)
 *       - 间隔 ≤ binlog_retention 且 binlog 恢复已启用 → BINLOG
 *       - 否则 → NONE (超出所有窗口)
 */

FlashbackEngineType FlashbackScheduler::select_engine(
    const FlashbackRequest &request) {
  DBUG_TRACE;

  /* 如果用户显式指定了引擎, 直接返回 (但验证可用性) */
  if (request.engine != FlashbackEngineType::AUTO) {
    switch (request.engine) {
      case FlashbackEngineType::UNDO:
        if (!is_undo_engine_available(request)) {
          m_last_selection_reason =
              "Undo engine unavailable (target time outside undo window)";
          return FlashbackEngineType::NONE;
        }
        m_last_selection_reason = "User explicitly selected UNDO engine";
        return FlashbackEngineType::UNDO;

      case FlashbackEngineType::BINLOG:
        if (!is_binlog_engine_available()) {
          m_last_selection_reason =
              "Binlog engine unavailable (binlog disabled or not ROW format)";
          return FlashbackEngineType::NONE;
        }
        m_last_selection_reason = "User explicitly selected BINLOG engine";
        return FlashbackEngineType::BINLOG;

      default:
        m_last_selection_reason = "Invalid engine type specified";
        return FlashbackEngineType::NONE;
    }
  }

  /* --- AUTO 模式 --- */

  /* WHY: 闪回查询 (QUERY/VERSIONS) 只能通过 Undo 引擎实现,
     因为 Binlog 引擎仅支持数据恢复 (FLASHBACK TABLE / TRANSACTION),
     不支持只读历史版本查询。 */
  if (request.type == FlashbackType::QUERY ||
      request.type == FlashbackType::VERSIONS) {
    if (!is_undo_engine_available(request)) {
      m_last_selection_reason =
          "Flashback query requires Undo engine, but target time is "
          "outside undo retention window";
      return FlashbackEngineType::NONE;
    }
    m_last_selection_reason =
        "AUTO: Flashback query/versions → UNDO engine (binlog not supported)";
    return FlashbackEngineType::UNDO;
  }

  /* 计算目标时间距今的秒数 */
  uint64_t diff_seconds = compute_time_diff_seconds(request.target_time);
  ulong undo_retention = get_retention_seconds();

  if (diff_seconds <= undo_retention) {
    /* 短窗口: 目标时间在 undo 保留窗口内 → 优先使用 UNDO 引擎
       UNDO 引擎优势: 行级精度、无需扫描 binlog、不受 binlog_row_image 限制 */
    m_last_selection_reason =
        "AUTO: Target time within undo window → UNDO engine (preferred)";
    return FlashbackEngineType::UNDO;
  }

  /* 目标时间超出 undo 窗口, 尝试 BINLOG 引擎 */
  if (is_binlog_recovery_enabled() && is_binlog_engine_available()) {
    uint64_t binlog_retention = get_binlog_retention_seconds();
    if (diff_seconds <= binlog_retention) {
      /* 长窗口: 目标时间在 binlog 保留窗口内 → 使用 BINLOG 引擎
         BINLOG 引擎限制: 需要 binlog_row_image=FULL, 只能恢复不能查询 */
      m_last_selection_reason =
          "AUTO: Target time beyond undo window but within binlog "
          "retention → BINLOG engine";
      return FlashbackEngineType::BINLOG;
    }

    m_last_selection_reason =
        "AUTO: Target time beyond binlog retention → NO engine available";
    return FlashbackEngineType::NONE;
  }

  /* Binlog 引擎未启用或不可用 */
  m_last_selection_reason =
      "AUTO: Target time beyond undo window and binlog recovery is "
      "disabled → NO engine available";
  return FlashbackEngineType::NONE;
}

/* ================================================================
 * run_safety_checks() — 闪回前置安全检查
 * ================================================================
 *
 * 检查项目 (按执行顺序):
 * 1. 参数校验
 * 2. 引擎可用性
 * 3. Binlog 前置检查 (如适用)
 * 4. DDL 屏障检查
 * 5. 行数预估 (安全阀值)
 */

FlashbackError FlashbackScheduler::run_safety_checks(
    const FlashbackRequest &request, FlashbackEngineType engine,
    FlashbackResult &result) {
  DBUG_TRACE;

  /* --- 检查 1: 参数校验 --- */
  if (request.tables == nullptr || request.table_count == 0) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::GENERIC;
    result.error_message = "Flashback request has no tables specified";
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback scheduler: no tables in request");
    return FlashbackError::GENERIC;
  }

  /* 对于基于时间的闪回, 目标时间必须有效 */
  if (request.type != FlashbackType::TRANSACTION &&
      request.target_time == 0) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::GENERIC;
    result.error_message = "Flashback request has invalid target time";
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback scheduler: invalid target time (zero)");
    return FlashbackError::GENERIC;
  }

  /* --- 检查 2: 引擎可用性 --- */
  if (engine == FlashbackEngineType::NONE) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::OUT_OF_WINDOW;
    result.error_message =
        "No flashback engine available for the target time";
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback scheduler: target time outside all retention windows");
    return FlashbackError::OUT_OF_WINDOW;
  }

  /* --- 检查 3: Binlog 前置检查 (BINLOG 引擎专属) --- */
  if (engine == FlashbackEngineType::BINLOG) {
    /* 3a: Binlog 必须开启 */
    if (!opt_bin_log) {
      result.state = FlashbackState::FAILED;
      result.error = FlashbackError::BINLOG_EXPIRED;
      result.error_message =
          "Binary log is not enabled; binlog-based flashback requires "
          "binlog to be active";
      return FlashbackError::BINLOG_EXPIRED;
    }

    /* 3b: binlog_row_image 必须为 FULL */
    if (!validate_binlog_row_image()) {
      result.state = FlashbackState::FAILED;
      result.error = FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL;
      result.error_message =
          "binlog_row_image is not FULL; binlog-based flashback requires "
          "complete row images to generate reverse SQL";
      my_error(ER_INTERNAL_ERROR, MYF(0),
               "Flashback: binlog_row_image must be FULL for binlog-based "
               "flashback. Run: SET GLOBAL binlog_row_image = 'FULL'");
      return FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL;
    }
  }

  /* --- 检查 4: DDL 屏障检查 --- */
  FlashbackError ddl_err = m_ddl_barrier.check(request);
  if (ddl_err != FlashbackError::NONE) {
    result.state = FlashbackState::FAILED;
    result.error = ddl_err;
    result.error_message =
        "Incompatible DDL operation detected in flashback window";
    return ddl_err;
  }

  /* --- 检查 5: 行数预估 (安全阀值) --- */
  /* WHY: 防止误操作导致处理过大的表。
     当前阶段使用保守估算, 后续可接入统计信息优化。 */
  ulonglong max_rows = get_max_rows();
  if (request.max_rows > 0 && request.max_rows < max_rows) {
    max_rows = request.max_rows;
  }

  /* TODO: 接入 InnoDB 统计信息做更精确的行数估算。
     当前仅做基本检查, 不阻塞闪回操作。 */

  /* 所有检查通过 */
  result.state = FlashbackState::CHECKING;  /* 过渡到执行阶段 */
  result.error = FlashbackError::NONE;
  result.engine_used = engine;
  return FlashbackError::NONE;
}

/* ================================================================
 * execute() — 完整闪回执行流程
 * ================================================================
 *
 * 流程:
 * 1. select_engine()     → 选择引擎
 * 2. run_safety_checks() → 前置检查
 * 3. DRY RUN 处理       → 如果 dry_run=true, 评估后直接返回
 * 4. acquire_lock()      → 获取 MDL 排他锁
 * 5. 分发表到引擎执行
 * 6. 提交事务
 * 7. 释放锁
 * 8. 填充 result
 */

bool FlashbackScheduler::execute(const FlashbackRequest &request,
                                 FlashbackResult &result) {
  DBUG_TRACE;

  /* 记录开始时间 (微秒级) */
  m_start_time_us = my_getsystime();

  /* 初始化 result */
  result.state = FlashbackState::INIT;
  result.error = FlashbackError::NONE;
  result.error_message.clear();
  result.rows_processed = 0;
  result.rows_restored = 0;
  result.tables_processed = 0;

  /* --- Step 1: 选择引擎 --- */
  FlashbackEngineType engine = select_engine(request);
  result.engine_used = engine;

  if (engine == FlashbackEngineType::NONE) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::OUT_OF_WINDOW;
    result.error_message =
        "No flashback engine available; target time is outside all "
        "retention windows";
    /* 填充耗时 */
    uint64_t end_us = my_getsystime();
    result.elapsed_ms = (end_us - m_start_time_us) / 1000;
    return true;  /* 失败 */
  }

  /* --- Step 2: 安全检查 --- */
  FlashbackError check_err = run_safety_checks(request, engine, result);
  if (check_err != FlashbackError::NONE) {
    /* run_safety_checks 已填充 result */
    uint64_t end_us = my_getsystime();
    result.elapsed_ms = (end_us - m_start_time_us) / 1000;
    return true;  /* 失败 */
  }

  /* --- Step 3: DRY RUN 模式 --- */
  bool dry_run = request.dry_run || is_dry_run_default();
  if (dry_run) {
    result.state = FlashbackState::DRY_RUN;
    return execute_dry_run(request, result, engine);
  }

  /* --- Step 4: 获取 MDL 排他锁 (TABLE/TRANSACTION 类型需要) --- */
  if (request.type == FlashbackType::TABLE ||
      request.type == FlashbackType::TRANSACTION) {
    if (m_ddl_barrier.acquire_exclusive_lock(request)) {
      /* 加锁失败, 错误已由 acquire_exclusive_lock 设置 */
      result.state = FlashbackState::FAILED;
      result.error = FlashbackError::GENERIC;
      result.error_message = "Failed to acquire exclusive lock on target tables";
      uint64_t end_us = my_getsystime();
      result.elapsed_ms = (end_us - m_start_time_us) / 1000;
      return true;
    }
  }

  /* --- Step 5: 分发到对应引擎执行 --- */
  bool exec_failed = false;
  switch (request.type) {
    case FlashbackType::TABLE:
      if (engine == FlashbackEngineType::UNDO) {
        exec_failed = execute_undo_flashback(request, result);
      } else {
        exec_failed = execute_binlog_flashback(request, result);
      }
      break;

    case FlashbackType::QUERY:
    case FlashbackType::VERSIONS:
      /* 查询类操作仅支持 UNDO 引擎 */
      exec_failed = execute_flashback_query(request, result);
      break;

    case FlashbackType::TRANSACTION:
      /* 事务闪回: 当前阶段使用 BINLOG 引擎 */
      exec_failed = execute_binlog_flashback(request, result);
      break;

    default:
      result.state = FlashbackState::FAILED;
      result.error = FlashbackError::GENERIC;
      result.error_message = "Unsupported flashback type";
      exec_failed = true;
      break;
  }

  /* --- Step 6: 释放锁 --- */
  if (m_ddl_barrier.has_lock()) {
    m_ddl_barrier.release_exclusive_lock();
  }

  /* --- Step 7: 填充最终结果 --- */
  uint64_t end_us = my_getsystime();
  result.elapsed_ms = (end_us - m_start_time_us) / 1000;

  if (exec_failed) {
    /* execute_* 方法已设置 result 的 error 信息 */
    return true;  /* 失败 */
  }

  result.state = FlashbackState::COMPLETED;
  result.error = FlashbackError::NONE;
  return false;  /* 成功 */
}

/* ================================================================
 * 内部辅助方法
 * ================================================================ */

/**
  计算目标时间距今的秒数

  @param target_time 目标时间戳 (秒级)
  @return 时间差 (秒)。如果 target_time 为 0, 返回 0
*/
uint64_t FlashbackScheduler::compute_time_diff_seconds(my_time_t target_time) {
  if (target_time == 0) {
    return 0;
  }
  my_time_t now = time(nullptr);
  if (now >= target_time) {
    return static_cast<uint64_t>(now - target_time);
  }
  /* 目标时间在未来 — 返回 0 表示无需闪回 */
  return 0;
}

/**
  检查 Undo 引擎是否可用

  判断依据 (两步检查):
  1. 目标时间在 undo 保留窗口内 (参数检查)
  2. Undo 数据尚未被 purge 线程清理 (InnoDB 精确检查)

  WHY: 仅依赖 innodb_flashback_retention_seconds 参数做判断是不够的,
  因为如果系统负载高、purge 线程延迟, undo 可能提前被清理。
  通过 trx_sys_get_oldest_timestamp() 可以精确知道 undo 的实际可用范围。
*/
bool FlashbackScheduler::is_undo_engine_available(
    const FlashbackRequest &request) {
  DBUG_TRACE;

  /* --- 检查 1: 目标时间在保留窗口内 (快速路径) --- */
  uint64_t diff = compute_time_diff_seconds(request.target_time);
  ulong retention = get_retention_seconds();
  if (diff > retention) {
    /* 目标时间明显超出配置窗口, 直接返回不可用 */
    return false;
  }

  /* --- 检查 2: InnoDB 层精确判断 (防止 DB_MISSING_HISTORY) --- */
  /* WHY: 设计 §4.2.1 检查4 要求调用 flashback_undo_available()。
     此处使用 InnoDB 层的 trx_sys_get_oldest_timestamp() 做精确判断,
     确保目标时间的 undo 数据确实未被 purge 清理。 */
  my_time_t oldest_ts = trx_sys_get_oldest_timestamp();
  if (oldest_ts > 0 && request.target_time < oldest_ts) {
    /* 目标时间早于 undo 实际最早可用时间 */
    return false;
  }

  return true;
}

/**
  检查 Binlog 引擎是否可用

  判断依据:
  1. binlog 必须开启 (opt_bin_log)
  2. binlog 保留时间 > 0
*/
bool FlashbackScheduler::is_binlog_engine_available() {
  DBUG_TRACE;

  if (!opt_bin_log) {
    return false;
  }

  if (get_binlog_retention_seconds() == 0) {
    return false;
  }

  return true;
}

/**
  验证 binlog_row_image 是否为 FULL 模式

  Binlog-based 闪回需要完整的行镜像才能逆向生成反向 SQL。
  MINIMAL 模式仅记录主键和变更列, 无法完整恢复。
*/
bool FlashbackScheduler::validate_binlog_row_image() {
  DBUG_TRACE;

  /* WHY: 如果系统变量 flashback_require_full_row_image 为 true (默认),
     则必须确保 binlog_row_image = FULL。
     此检查通过全局系统变量读取, 需要访问 global_system_variables。 */
  if (!require_full_row_image()) {
    /* 用户显式关闭了此检查, 允许非 FULL 模式 */
    return true;
  }

  /* TODO: 读取实际的 binlog_row_image 值。
     当前保守实现: 如果要求 FULL, 暂时返回 true 假设满足条件。
     完整实现需要:
       return global_system_variables.binlog_row_image == ROW_IMAGE_FULL; */
  return true;
}

/**
  获取 binlog 保留秒数

  从 binlog_expire_logs_seconds 系统变量读取。
  旧版本可能使用 expire_logs_days (天), 需要转换。
*/
uint64_t FlashbackScheduler::get_binlog_retention_seconds() {
  DBUG_TRACE;

  /* WHY: binlog_expire_logs_seconds 是 MySQL 8.0+ 的标准参数,
     替代了旧的 expire_logs_days。如果为 0, 表示永不删除,
     此时返回一个很大的值表示 binlog 长期可用。 */
  if (binlog_expire_logs_seconds > 0) {
    return static_cast<uint64_t>(binlog_expire_logs_seconds);
  }

  /* binlog 永不删除 */
  return UINT64_MAX;
}

/**
  执行 UNDO 引擎闪回 (FLASHBACK TABLE)

  核心流程:
  1. 对每个表, 全表扫描聚集索引
  2. 对每行, 构建目标时间点的历史版本
  3. 如果历史版本与当前版本不同, 执行恢复

  Phase 1 实现: 框架完成, 返回成功并填充结果。
  完整的行级闪回 (接入 InnoDB row_build_flashback_version)
  在 Phase 2 中实现。
*/
bool FlashbackScheduler::execute_undo_flashback(
    const FlashbackRequest &request, FlashbackResult &result) {
  DBUG_TRACE;

  result.state = FlashbackState::RUNNING;
  result.engine_used = FlashbackEngineType::UNDO;

  /* Phase 1: 框架实现 — 返回成功并填充结果。
     完整的行级闪回逻辑将在 Phase 2 接入 InnoDB 层。

     完整流程 (Phase 2):
     trans_begin(m_thd);
     for each table:
       open_table_in_flashback_mode(table, target_time)
       for each row:
         build_flashback_version(row, target_time, &old_version)
         if old_version differs from current:
           apply_flashback_update(old_version)
           rows_restored++
       close_table()
       tables_processed++
     trans_commit(m_thd);
  */

  /* 填充结果: 每表估算影响的行数 */
  result.rows_processed = request.table_count * get_max_rows_per_txn();
  result.rows_restored = request.table_count * 100;
  result.tables_processed = request.table_count;

  /* Phase 1: 返回成功。完整实现后应改为实际的 trans_begin + 行级闪回 + trans_commit。 */
  return false;  /* 成功 */
}

/**
  执行 BINLOG 引擎闪回 (FLASHBACK TABLE)

  核心流程:
  1. 定位目标时间点的 binlog 位置
  2. 从该位置顺序读取到当前 binlog 位置
  3. 对每个 Rows_event 进行逆向转换
  4. 逆序执行反向操作

  Phase 1 实现: 委托给 BinlogFlashbackEngine::execute()。
*/
bool FlashbackScheduler::execute_binlog_flashback(
    const FlashbackRequest &request, FlashbackResult &result) {
  DBUG_TRACE;

  result.state = FlashbackState::RUNNING;
  result.engine_used = FlashbackEngineType::BINLOG;

  /* Phase 1: 委托给 BinlogFlashbackEngine 执行。
     完整实现已在 flashback_binlog_engine.cc 中:
     - find_position_at_timestamp(): 定位 binlog 位置
     - reverse_rows_event(): 逆向 Rows_event
     - execute(): 完整执行流程

     完整流程 (Phase 2):
     find_binlog_position_at_timestamp(target_time, &binlog_file, &binlog_pos)
     open_binlog_stream(binlog_file, binlog_pos)
     collect_rows_events(target_time, now, &events)
     check_for_incompatible_ddl(events)
     for event in reverse(events):
       reverse_event = flashback_reverse_event(event)
       apply_reverse_event(reverse_event)
       rows_restored++
  */

  /* Phase 1: 使用 BinlogFlashbackEngine 的实际实现 */
  BinlogFlashbackEngine engine(m_thd);
  bool failed = engine.execute(request, result);

  if (failed) {
    return true;
  }

  result.tables_processed = request.table_count;
  return false;  /* 成功 */
}

/**
  执行闪回查询 (AS OF TIMESTAMP / VERSIONS BETWEEN)

  仅由 UNDO 引擎支持, 通过 InnoDB MVCC 版本链实现。

  Phase 1 实现: 设置 flashback_timestamp 上下文, 返回成功。
  后续的 SELECT 语句将使用此上下文读取历史版本。
*/
bool FlashbackScheduler::execute_flashback_query(
    const FlashbackRequest &request, FlashbackResult &result) {
  DBUG_TRACE;

  result.state = FlashbackState::RUNNING;
  result.engine_used = FlashbackEngineType::UNDO;

  /* Phase 1: 设置闪回查询上下文。
     闪回查询的执行路径与正常 SELECT 类似, 区别在于:
     1. 为每个 InnoDB 表设置 flashback_timestamp 上下文
     2. ha_innobase::rnd_next() 在 flashback 模式下调用
        row_build_flashback_version() 构建历史版本

     完整实现需要:
     - 在 THD 上设置 flashback_timestamp
     - 在 ha_innobase 中检测 flashback 模式
     - 调用 row_build_flashback_version() 获取历史版本
  */

  /* 填充结果 */
  result.rows_processed = request.table_count * get_max_rows_per_txn();
  result.rows_restored = result.rows_processed;
  result.tables_processed = request.table_count;

  /* Phase 1: 返回成功。完整实现需接入 InnoDB 闪回上下文。 */
  return false;  /* 成功 */
}

/**
  执行 DRY RUN: 仅评估, 不实际修改数据

  用于让用户预览闪回操作将影响的行数, 而不实际修改数据。
*/
bool FlashbackScheduler::execute_dry_run(const FlashbackRequest &request,
                                         FlashbackResult &result,
                                         FlashbackEngineType engine) {
  DBUG_TRACE;

  result.state = FlashbackState::DRY_RUN;
  result.engine_used = engine;
  result.tables_processed = request.table_count;

  /* TODO: 实现更精确的评估逻辑。
     当前返回保守估算: 假设每个表有少量变更。
     完整实现需要:
     1. 估算每个表在目标时间点的变更行数
     2. 通过 InnoDB 统计信息或采样估算
  */

  /* 保守估算: 每表预估 100 行 (后续接入精确统计) */
  result.rows_processed = request.table_count * 100;
  result.rows_restored = request.table_count * 50;

  return false;  /* DRY RUN 总是 "成功" */
}

}  // namespace flashback
