/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/flashback_sysvars.cc
  Flashback 模块系统变量实现

  本文件定义 Flashback 功能的 7 个系统变量。

  变量列表:
  ┌──────────────────────────────────────────┬────────┬───────────────┐
  │ 变量名                                   │ 类型   │ 默认值        │
  ├──────────────────────────────────────────┼────────┼───────────────┤
  │ innodb_flashback_retention_seconds       │ ulong  │ 900           │
  │ innodb_flashback_enable_binlog_recovery  │ bool   │ ON            │
  │ innodb_flashback_max_rows_per_txn        │ ulong  │ 100000        │
  │ flashback_dry_run_default                │ bool   │ OFF           │
  │ flashback_require_full_row_image         │ bool   │ ON            │
  │ flashback_lock_wait_timeout              │ ulong  │ 300           │
  │ flashback_max_rows                       │ ulong  │ 10000000      │
  └──────────────────────────────────────────┴────────┴───────────────┘

  设计参考: mysql_flashback_implementation_v2.md §4.3
            mysql_flashback_synthesis_report.md §2.5
*/

#include "sql/flashback_sysvars.h"

#include "mysqld_error.h"
#include "sql/set_var.h"
#include "sql/sys_vars.h"

/* ================================================================
 * 全局变量存储
 *
 * WHY: 这些变量声明在 flashback_sysvars.h 中作为 extern,
 * 在此文件中定义。它们注册为 MySQL 系统变量, 可通过
 * @@GLOBAL.innodb_flashback_retention_seconds 等方式访问。
 * ================================================================ */

/** Undo 日志保留时间（秒） */
ulong flashback::innodb_flashback_retention_seconds = 900;

/** 是否启用基于 Binlog 的长窗口闪回 */
bool flashback::innodb_flashback_enable_binlog_recovery = true;

/** 单次闪回事务最大处理行数 */
ulong flashback::innodb_flashback_max_rows_per_txn = 100000;

/** 默认 DRY RUN 模式开关 */
bool flashback::flashback_dry_run_default = false;

/** 要求 binlog_row_image=FULL */
bool flashback::flashback_require_full_row_image = true;

/** 闪回操作锁等待超时（秒） */
ulong flashback::flashback_lock_wait_timeout = 300;

/** 单次闪回最大行数上限（安全阀值） */
ulonglong flashback::flashback_max_rows = 10000000;

/* ================================================================
 * 便捷访问函数实现
 * ================================================================ */

namespace flashback {

ulong get_retention_seconds() {
  return innodb_flashback_retention_seconds;
}

bool is_binlog_recovery_enabled() {
  return innodb_flashback_enable_binlog_recovery;
}

ulong get_max_rows_per_txn() {
  return innodb_flashback_max_rows_per_txn;
}

bool is_dry_run_default() {
  return flashback_dry_run_default;
}

bool require_full_row_image() {
  return flashback_require_full_row_image;
}

ulong get_lock_wait_timeout() {
  return flashback_lock_wait_timeout;
}

ulonglong get_max_rows() {
  return flashback_max_rows;
}

}  // namespace flashback
