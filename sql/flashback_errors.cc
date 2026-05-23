/* Copyright (c) 2025, Percona and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/flashback_errors.cc
  @brief Flashback 错误处理函数实现.

  提供 my_error() 包装函数, 将底层错误码转换为客户端可读的错误消息.
  所有函数位于 flashback 命名空间, 调用时自动使用 FLASHBACK_ERR 标志.

  设计参考: DESIGN_UNDO_ARCHITECTURE.md §5.2 新增错误码, §5.3 错误处理流程

  错误处理分类 (§5.3):
  - E1 业务错误: 直接 my_error, 返回错误给客户端
  - E2 可恢复错误: 先清理资源, 再 my_error
  - E3 系统错误: push_warning + my_error + log_error

  本文件仅实现 E1 (业务错误) 的包装函数.
  E2/E3 需要在调用方代码中手动处理资源清理.
*/

#include "sql/flashback_errors.h"

#include "my_dbug.h"
#include "my_sys.h"

namespace flashback {

void report_timestamp_unavailable(const char *target_ts,
                                   const char *oldest_ts) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_TIMESTAMP_UNAVAILABLE, FLASHBACK_ERR,
           target_ts, oldest_ts);
}

void report_purge_hold_failed() {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_PURGE_HOLD_FAILED, FLASHBACK_ERR);
}

void report_version_chain_broken(uint32_t space_id, uint32_t page_no,
                                  uint32_t offset) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_VERSION_CHAIN_BROKEN, FLASHBACK_ERR,
           space_id, page_no, offset);
}

void report_estimate_trx_id_failed(const char *timestamp_str) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_ESTIMATE_TRX_ID_FAILED, FLASHBACK_ERR,
           timestamp_str);
}

void report_window_exceeded(ulonglong current_retention,
                             ulonglong requested_ago) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_WINDOW_EXCEEDED, FLASHBACK_ERR,
           current_retention, requested_ago);
}

void report_ddl_barrier_triggered(const char *table_name,
                                   const char *target_ts) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_DDL_BARRIER_TRIGGERED, FLASHBACK_ERR,
           table_name, target_ts);
}

void report_space_critical(const char *usage_info) {
  DBUG_TRACE;
  my_error(ER_FLASHBACK_SPACE_CRITICAL, FLASHBACK_ERR,
           usage_info);
}

} // namespace flashback
