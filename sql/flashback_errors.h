/* Copyright (c) 2025, Percona and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef FLASHBACK_ERRORS_INCLUDED
#define FLASHBACK_ERRORS_INCLUDED

/**
  @file sql/flashback_errors.h
  @brief Flashback 错误码声明与 my_error() 辅助函数.

  本文件提供:
  1. 闪回功能专用的错误码宏定义 (对应 mysqld_error.h 中的 #define)
  2. my_error() 包装函数, 用于在 SQL 层触发错误消息

  设计参考: DESIGN_UNDO_ARCHITECTURE.md §5.2 新增错误码, §5.3 错误处理流程

  错误码范围 (本项目实际分配, 对应设计文档 3820-3827):
  - ER_FLASHBACK_TIMESTAMP_UNAVAILABLE    8031
  - ER_FLASHBACK_PURGE_HOLD_FAILED        8032
  - ER_FLASHBACK_VERSION_CHAIN_BROKEN     8033
  - ER_FLASHBACK_ESTIMATE_TRX_ID_FAILED   8034
  - ER_FLASHBACK_WINDOW_EXCEEDED          8035
  - ER_FLASHBACK_DDL_BARRIER_TRIGGERED    8036
  - ER_FLASHBACK_SPACE_CRITICAL           8037

  已有错误码 (本项目早期分配):
  - ER_FLASHBACK_UNDO_PURGED              8020
  - ER_FLASHBACK_DDL_CONFLICT             8021
  - ER_FLASHBACK_NO_PRIMARY_KEY           8022
  - ER_FLASHBACK_ROW_IMAGE_INCOMPLETE     8023
  - ER_FLASHBACK_LOCK_TIMEOUT             8024
  - ER_FLASHBACK_MEMORY_LIMIT             8025
  - ER_FLASHBACK_NOT_ENABLED              8026
  - ER_FLASHBACK_MISSING_HISTORY          8027
  - ER_FLASHBACK_ENGINE_NONE              8028
  - ER_FLASHBACK_PRIVILEGE                8029
  - ER_FLASHBACK_VIEW_LIMIT               8030
*/

#include "my_inttypes.h"
#include "mysqld_error.h" /* ER_FLASHBACK_* 宏定义 */

/** my_error 调用时使用的 MYF 标志 (不产生系统级错误) */
#define FLASHBACK_ERR MYF(0)

namespace flashback {

/* ================================================================
 * my_error() 包装函数
 *
 * 每个函数对应一个错误码, 封装格式化参数并调用 my_error().
 * 函数名采用 report_<error_name> 格式, 语义清晰.
 *
 * 错误处理流程 (§5.3):
 *   E1 业务错误 → my_error(ER_FLASHBACK_*, FLASHBACK_ERR)
 *   E2 可恢复错误 → 清理资源 → my_error(...)
 *   E3 系统错误 → push_warning + my_error + log_error()
 * ================================================================ */

/**
  报告: 目标时间戳超出 undo 日志保留窗口.

  @param target_ts    目标时间戳字符串 (如 "2025-07-29 10:30:00")
  @param oldest_ts    最老可用时间戳字符串
*/
void report_timestamp_unavailable(const char *target_ts,
                                   const char *oldest_ts);

/**
  报告: 注册 Purge Hold 请求失败.

  @note 无额外参数, 消息固定.
*/
void report_purge_hold_failed();

/**
  报告: 版本链断裂, undo record 已被 purge.

  @param space_id  表空间 ID
  @param page_no   页号
  @param offset    记录偏移
*/
void report_version_chain_broken(uint32_t space_id, uint32_t page_no,
                                  uint32_t offset);

/**
  报告: 无法估算时间戳对应的事务 ID.

  @param timestamp_str  目标时间戳字符串
*/
void report_estimate_trx_id_failed(const char *timestamp_str);

/**
  报告: 闪回窗口限制超出.

  @param current_retention  当前保留秒数
  @param requested_ago      请求的回溯秒数
*/
void report_window_exceeded(ulonglong current_retention,
                             ulonglong requested_ago);

/**
  报告: 目标时间点后检测到 DDL 操作, 闪回不安全.

  @param table_name     表名
  @param target_ts      目标时间戳字符串
*/
void report_ddl_barrier_triggered(const char *table_name,
                                   const char *target_ts);

/**
  报告: Undo 表空间空间严重不足.

  @param usage_info  空间使用情况描述 (如 "95% used")
*/
void report_space_critical(const char *usage_info);

} // namespace flashback

#endif /* FLASHBACK_ERRORS_INCLUDED */
