/* Copyright (c) 2025, Percona and/or its affiliates.

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

#ifndef FLASHBACK_TYPES_INCLUDED
#define FLASHBACK_TYPES_INCLUDED

/**
  @file sql/flashback_types.h
  @brief Flashback feature core type definitions.

  This file defines the shared types used across all flashback modules:
  - FlashbackType: the kind of flashback operation
  - FlashbackEngineType: which engine to use (UNDO / BINLOG)
  - FlashbackRequest: input parameters for a flashback operation
  - FlashbackResult: output statistics after execution
*/

#include <string>

#include "lex_string.h"       /* LEX_CSTRING */
#include "my_inttypes.h"
#include "my_time_t.h"        /* my_time_t */

/* Forward declarations */
class THD;

namespace flashback {

/**
  闪回操作类型枚举

  QUERY:       SELECT ... AS OF TIMESTAMP (闪回查询, 只读)
  TABLE:       FLASHBACK TABLE ... TO TIMESTAMP (闪回表, 写操作)
  VERSIONS:    SELECT ... VERSIONS BETWEEN (闪回版本查询, 只读)
  TRANSACTION: FLASHBACK TRANSACTION ... (闪回事务, 写操作)
*/
enum class FlashbackType {
  QUERY,        /**< 闪回查询: 按时间点查询历史版本 */
  TABLE,        /**< 闪回表: 将表数据恢复到指定时间点 */
  VERSIONS,     /**< 闪回版本: 查询行在时间区间内的所有版本 */
  TRANSACTION   /**< 闪回事务: 回滚指定事务的所有更改 */
};

/**
  闪回引擎类型枚举

  AUTO:    自动选择 (调度器根据时间窗口决策, 核心入口)
  NONE:    无可用引擎 (超出所有保留窗口)
  UNDO:    基于 Undo Log 的闪回引擎 (分钟级窗口, 高性能)
  BINLOG:  基于 Binlog 的闪回引擎 (天级窗口, 长恢复)
*/
enum class FlashbackEngineType {
  AUTO,       /**< 自动选择引擎 (调度器决策) */
  NONE,       /**< 无可用引擎 */
  UNDO,       /**< Undo Log 引擎 */
  BINLOG      /**< Binlog 逆向引擎 */
};

/** 无效事务 ID 常量 */
static constexpr uint64_t INVALID_TRX_ID = 0;

/**
  闪回操作状态机

  INIT:      初始状态, 闪回尚未开始
  CHECKING:  安全检查阶段
  RUNNING:   正在执行闪回
  COMPLETED: 闪回成功完成
  FAILED:    闪回失败 (见 error_message)
  DRY_RUN:   DRY RUN 模式, 仅评估不修改数据
*/
enum class FlashbackState {
  INIT,          /**< 初始状态 */
  CHECKING,      /**< 安全检查中 */
  RUNNING,       /**< 正在执行 */
  COMPLETED,     /**< 完成 */
  FAILED,        /**< 失败 */
  DRY_RUN,       /**< DRY RUN 模式 */
  INTERRUPTED    /**< 被用户中断 (KILL) */
};

/**
  闪回错误码

  NONE:                    无错误
  GENERIC:                 通用错误 (参数错误、权限不足等)
  OUT_OF_WINDOW:           目标时间超出所有保留窗口
  BINLOG_EXPIRED:          Binlog 文件已被清理
  BINLOG_ROW_IMAGE_NOT_FULL: binlog_row_image 不是 FULL
  DDL_INCOMPATIBLE:        发现不兼容的 DDL 操作
*/
enum class FlashbackError {
  NONE,                       /**< 无错误 */
  GENERIC,                    /**< 通用错误 */
  OUT_OF_WINDOW,              /**< 超出保留窗口 */
  BINLOG_EXPIRED,             /**< Binlog 已过期 */
  BINLOG_ROW_IMAGE_NOT_FULL,  /**< Row Image 不满足要求 */
  DDL_INCOMPATIBLE,           /**< DDL 不兼容 */
  INTERRUPTED                 /**< 被用户中断 */
};

/**
  闪回请求结构体

  封装一次闪回操作的所有输入参数, 包括:
  - 操作类型 (查询/表/版本/事务)
  - 目标时间点或时间区间
  - 涉及的表列表 (LEX_CSTRING 数组)
  - 事务号 (仅 TRANSACTION 类型)
  - DRY RUN 标志
  - 最大行数限制
*/
struct FlashbackRequest {
  /** 闪回操作类型 */
  FlashbackType type{FlashbackType::QUERY};

  /** 闪回引擎类型 (AUTO 表示由调度器自动选择) */
  FlashbackEngineType engine{FlashbackEngineType::AUTO};

  /** 目标时间戳 (用于 TABLE/QUERY, 秒级 Unix 时间) */
  my_time_t target_time{0};

  /** 目标事务 ID (仅 TRANSACTION 类型) */
  uint64_t target_trx_id{INVALID_TRX_ID};

  /** 版本查询开始时间 (仅 VERSIONS 类型) */
  my_time_t start_timestamp{0};

  /** 版本查询结束时间 (仅 VERSIONS 类型) */
  my_time_t end_timestamp{0};

  /** 需要闪回的表列表 (数组指针, 由调用者管理生命周期) */
  LEX_CSTRING *tables{nullptr};

  /** 表数量 */
  uint32_t table_count{0};

  /** 数据库名 (用于权限检查) */
  std::string db_name;

  /** DRY RUN 模式: 仅统计, 不修改数据 */
  bool dry_run{false};

  /** 最大处理行数 (约束 C8) */
  ulonglong max_rows{10000000};

  /** 锁等待超时秒数 */
  ulong lock_wait_timeout{300};

  /** 是否要求审计日志 */
  bool audit_log{true};
};

/**
  闪回结果结构体

  封装一次闪回操作的输出统计信息.
*/
struct FlashbackResult {
  /** 当前闪回状态 (INIT / CHECKING / RUNNING / COMPLETED / FAILED / DRY_RUN) */
  FlashbackState state{FlashbackState::INIT};

  /** 错误码 (失败时有效) */
  FlashbackError error{FlashbackError::NONE};

  /** 使用的引擎类型 */
  FlashbackEngineType engine_used{FlashbackEngineType::NONE};

  /** 扫描/处理的总行数 */
  ulonglong rows_processed{0};

  /** 实际恢复/返回的行数 */
  ulonglong rows_restored{0};

  /** 已处理的表数量 */
  uint32_t tables_processed{0};

  /** 耗时 (毫秒) */
  ulonglong elapsed_ms{0};

  /** 错误消息 (失败时填充) */
  std::string error_message;

  /** DRY RUN 时的预览信息 */
  std::string dry_run_summary;
};

/**
  闪回表信息 (用于内部传递表级元数据)
*/
struct FlashbackTableInfo {
  /** 数据字典表 ID */
  ulonglong table_id{0};

  /** 表名 */
  std::string table_name;

  /** 数据库名 */
  std::string schema_name;

  /** 是否有主键 (约束 C5) */
  bool has_primary_key{false};
};

} // namespace flashback

#endif /* FLASHBACK_TYPES_INCLUDED */
