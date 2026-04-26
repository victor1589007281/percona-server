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
  @file sql/flashback_scheduler.h
  Flashback 调度器 — 双引擎闪回的核心协调模块

  本模块是闪回功能的"大脑"，负责:
  1. select_engine():  根据目标时间和系统状态自动选择 UNDO 或 BINLOG 引擎
  2. run_safety_checks(): 执行前置安全检查（DDL 屏障、权限、binlog 模式等）
  3. execute():        编排完整的闪回流程（检查 → 加锁 → 执行 → 清理）

  设计参考: mysql_flashback_implementation.md §4.2 (引擎选择策略)
            mysql_flashback_implementation_v2.md §2.2.2, §3.3
            mysql_flashback_implementation_deep.md §4.3

  线程安全: 实例不可跨线程共享，每个 THD 使用独立实例。
*/

#ifndef FLASHBACK_SCHEDULER_INCLUDED
#define FLASHBACK_SCHEDULER_INCLUDED

#include "sql/flashback_ddl_barrier.h"  // DDLBarrier
#include "sql/flashback_types.h"        // FlashbackRequest, FlashbackResult, etc.

class THD;

namespace flashback {

/**
  Flashback 调度器

  作为闪回操作的统一入口点，调度器遵循以下执行流程:

  ┌──────────┐    ┌──────────────────┐    ┌─────────────────┐
  │ execute() │───▶│ run_safety_checks() │───▶│ select_engine() │
  └──────────┘    └──────────────────┘    └─────────────────┘
       │                                          │
       │          ┌──────────────────┐    ┌───────▼────────┐
       │◀─────────│ 执行引擎 (Undo/  │◀───│ 加 MDL 排他锁  │
       │          │ Binlog)          │    └────────────────┘
       │          └──────────────────┘
       │
  ┌────▼──────┐
  │ 清理/返回  │
  └───────────┘

  引擎选择决策流 (select_engine):

  FlashbackRequest.engine == AUTO?
    ├── YES → 根据目标时间间隔自动选择:
    │         间隔 ≤ undo_retention → UNDO 引擎
    │         间隔 ≤ binlog_retention → BINLOG 引擎
    │         否则 → NONE (超出窗口)
    └── NO → 使用用户指定的引擎 (但需验证可用性)
*/
class FlashbackScheduler {
 public:
  /**
    构造函数

    @param thd 当前线程上下文
  */
  explicit FlashbackScheduler(THD *thd);

  /** 析构函数: 自动清理内部持有的 DDLBarrier */
  ~FlashbackScheduler();

  /* ================================================================
   * 核心接口: select_engine / run_safety_checks / execute
   * ================================================================ */

  /**
    选择闪回引擎

    决策逻辑 (双引擎自动选择):
    1. 如果 request.engine != AUTO, 直接使用指定引擎
    2. 如果 request.engine == AUTO:
       a. 计算目标时间距今的秒数
       b. 如果间隔 ≤ innodb_flashback_retention_seconds → UNDO
       c. 如果 innodb_flashback_enable_binlog_recovery == true
          且间隔 ≤ binlog 保留天数 → BINLOG
       d. 否则 → NONE (超出窗口)

    对于 QUERY 和 VERSIONS 类型, 始终优先选择 UNDO 引擎
    (因为 binlog 引擎不支持闪回查询)。

    @param request 闪回请求参数
    @return        选定的引擎类型 (UNDO / BINLOG / NONE)
  */
  FlashbackEngineType select_engine(const FlashbackRequest &request);

  /**
    执行闪回前置安全检查

    按顺序检查以下项目:
    1. 参数校验: 表列表非空、目标时间有效
    2. 权限检查: 当前用户是否有闪回权限
    3. 引擎可用性: 选择的引擎是否可用
    4. Binlog 前置: 如使用 BINLOG 引擎, 检查 binlog 是否开启且为 ROW 格式
    5. Row Image 检查: 如使用 BINLOG 引擎, 检查 binlog_row_image=FULL
    6. DDL 屏障: 委托给 DDLBarrier::check() 检查不兼容 DDL
    7. 行数预估: 如果超过 flashback_max_rows 安全阀值, 拒绝执行

    @param request 闪回请求参数
    @param engine  已选定的闪回引擎
    @param result  输出参数, 用于填充错误信息 (如果检查失败)

    @retval FlashbackError::NONE       检查通过, 可以安全执行
    @retval FlashbackError::GENERIC    参数错误或权限不足
    @retval FlashbackError::OUT_OF_WINDOW 引擎不可用
    @retval FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL  Row Image 不满足要求
    @retval FlashbackError::DDL_INCOMPATIBLE  发现不兼容 DDL
  */
  FlashbackError run_safety_checks(const FlashbackRequest &request,
                                   FlashbackEngineType engine,
                                   FlashbackResult &result);

  /**
    执行完整的闪回操作

    流程:
    1. 选择引擎 (select_engine)
    2. 安全检查 (run_safety_checks)
    3. 如果 DRY_RUN 模式, 填充 result 后直接返回
    4. 获取 MDL_EXCLUSIVE 锁 (DDLBarrier::acquire_exclusive_lock)
    5. 开始事务
    6. 分发表到对应引擎执行
    7. 提交或回滚事务
    8. 释放锁
    9. 填充 result

    @param request 闪回请求参数
    @param result  输出参数, 用于填充执行结果

    @retval false  执行成功 (result.state == COMPLETED)
    @retval true   执行失败 (result.state == FAILED, result.error 有效)
  */
  bool execute(const FlashbackRequest &request, FlashbackResult &result);

  /* ================================================================
   * 辅助查询接口
   * ================================================================ */

  /**
    获取 DDL 屏障实例 (供内部使用)
    @return DDLBarrier 指针
  */
  DDLBarrier *ddl_barrier() { return &m_ddl_barrier; }

  /**
    获取最后一次选择引擎时的详细原因描述
    (用于调试和错误报告)
    @return 引擎选择原因的文本描述
  */
  const char *last_engine_selection_reason() const {
    return m_last_selection_reason;
  }

 private:
  /* ================================================================
   * 内部辅助方法
   * ================================================================ */

  /** 计算目标时间距今的秒数 */
  uint64_t compute_time_diff_seconds(my_time_t target_time);

  /** 检查 Undo 引擎是否可用 */
  bool is_undo_engine_available(const FlashbackRequest &request);

  /** 检查 Binlog 引擎是否可用 */
  bool is_binlog_engine_available();

  /** 验证 binlog_row_image 设置是否满足要求 */
  bool validate_binlog_row_image();

  /** 获取 binlog 保留秒数 (从 expire_logs_days 或 binlog_expire_logs_seconds) */
  uint64_t get_binlog_retention_seconds();

  /** 执行 UNDO 引擎闪回 (表类型) */
  bool execute_undo_flashback(const FlashbackRequest &request,
                              FlashbackResult &result);

  /** 执行 BINLOG 引擎闪回 (表类型) */
  bool execute_binlog_flashback(const FlashbackRequest &request,
                                FlashbackResult &result);

  /** 执行闪回查询 (AS OF TIMESTAMP) — 仅 Undo 引擎支持 */
  bool execute_flashback_query(const FlashbackRequest &request,
                               FlashbackResult &result);

  /** 执行 DRY RUN: 仅评估, 不实际修改数据 */
  bool execute_dry_run(const FlashbackRequest &request,
                       FlashbackResult &result, FlashbackEngineType engine);

  /* ================================================================
   * 成员变量
   * ================================================================ */

  /** 当前线程上下文 (不可为空) */
  THD *m_thd;

  /** DDL 屏障实例, 用于保护目标表不被不兼容 DDL 修改 */
  DDLBarrier m_ddl_barrier;

  /** 最近一次 select_engine() 的决策原因 (用于诊断) */
  const char *m_last_selection_reason;

  /** 闪回操作开始时间 (微秒级), 用于计算执行耗时 */
  uint64_t m_start_time_us;
};

}  // namespace flashback

#endif /* FLASHBACK_SCHEDULER_INCLUDED */
