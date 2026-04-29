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

  本文件定义 Flashback 功能的系统变量。

  第一批 (初始 7 个):
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

  第二批 (新增 10 个, 设计参考 DESIGN_UNDO_ARCHITECTURE.md §7):

  §7.1 H1 相关变量 (Flashback ReadView 管理):
  ┌──────────────────────────────────────────┬────────┬───────────────┐
  │ 变量名                                   │ 类型   │ 默认值        │
  ├──────────────────────────────────────────┼────────┼───────────────┤
  │ innodb_flashback_enabled                 │ bool   │ ON            │
  │ innodb_flashback_window_seconds          │ ulong  │ 900           │
  │ innodb_flashback_max_views               │ ulong  │ 256           │
  │ innodb_flashback_view_ttl_seconds        │ ulong  │ 300           │
  └──────────────────────────────────────────┴────────┴───────────────┘

  §7.2 H5 相关变量 (Purge Hold 调度):
  ┌──────────────────────────────────────────┬────────┬───────────────┐
  │ 变量名                                   │ 类型   │ 默认值        │
  ├──────────────────────────────────────────┼────────┼───────────────┤
  │ innodb_purge_hold_enabled                │ bool   │ ON            │
  │ innodb_purge_hold_max_requests           │ ulong  │ 1024          │
  │ innodb_purge_hold_time_multiplier        │ double │ 1.5           │
  └──────────────────────────────────────────┴────────┴───────────────┘

  §7.3 空间管理变量:
  ┌──────────────────────────────────────────┬────────┬───────────────┐
  │ 变量名                                   │ 类型   │ 默认值        │
  ├──────────────────────────────────────────┼────────┼───────────────┤
  │ innodb_undo_space_warning_threshold      │ double │ 0.75          │
  │ innodb_undo_space_critical_threshold     │ double │ 0.90          │
  │ innodb_undo_auto_truncate                │ bool   │ ON            │
  └──────────────────────────────────────────┴────────┴───────────────┘

  设计参考: mysql_flashback_implementation_v2.md §4.3
            mysql_flashback_synthesis_report.md §2.5
            DESIGN_UNDO_ARCHITECTURE.md §7
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
 * §7.1 H1 相关变量: Flashback ReadView 管理
 * 设计参考: DESIGN_UNDO_ARCHITECTURE.md §7.1
 * ================================================================ */

/** 闪回功能总开关。默认 ON */
bool flashback::innodb_flashback_enabled = true;

/** 闪回时间窗口（秒）。默认 900 (15 分钟) */
ulong flashback::innodb_flashback_window_seconds = 900;

/** 最大并发 ReadView 数。默认 256 */
ulong flashback::innodb_flashback_max_views = 256;

/** 单个 ReadView 的 TTL（秒）。默认 300 (5 分钟) */
ulong flashback::innodb_flashback_view_ttl_seconds = 300;

/* ================================================================
 * §7.2 H5 相关变量: Purge Hold 调度
 * 设计参考: DESIGN_UNDO_ARCHITECTURE.md §7.2
 * ================================================================ */

/** Purge Hold 调度开关。默认 ON */
bool flashback::innodb_purge_hold_enabled = true;

/** 最大 Hold 请求数。默认 1024 */
ulong flashback::innodb_purge_hold_max_requests = 1024;

/** 时间延迟乘数。默认 1.5 */
double flashback::innodb_purge_hold_time_multiplier = 1.5;

/* ================================================================
 * §7.3 空间管理变量
 * 设计参考: DESIGN_UNDO_ARCHITECTURE.md §7.3
 * ================================================================ */

/** Undo 空间警告阈值。默认 0.75 (75%) */
double flashback::innodb_undo_space_warning_threshold = 0.75;

/** Undo 空间严重阈值。默认 0.90 (90%) */
double flashback::innodb_undo_space_critical_threshold = 0.90;

/** Undo 自动截断开关。默认 ON */
bool flashback::innodb_undo_auto_truncate = true;

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

/* ================================================================
 * §7.1 H1 便捷访问函数: Flashback ReadView 管理
 * ================================================================ */

bool is_flashback_enabled() {
  return innodb_flashback_enabled;
}

ulong get_flashback_window_seconds() {
  return innodb_flashback_window_seconds;
}

ulong get_flashback_max_views() {
  return innodb_flashback_max_views;
}

ulong get_flashback_view_ttl_seconds() {
  return innodb_flashback_view_ttl_seconds;
}

/* ================================================================
 * §7.2 H5 便捷访问函数: Purge Hold 调度
 * ================================================================ */

bool is_purge_hold_enabled() {
  return innodb_purge_hold_enabled;
}

ulong get_purge_hold_max_requests() {
  return innodb_purge_hold_max_requests;
}

double get_purge_hold_time_multiplier() {
  return innodb_purge_hold_time_multiplier;
}

/* ================================================================
 * §7.3 便捷访问函数: 空间管理
 * ================================================================ */

double get_undo_space_warning_threshold() {
  return innodb_undo_space_warning_threshold;
}

double get_undo_space_critical_threshold() {
  return innodb_undo_space_critical_threshold;
}

bool is_undo_auto_truncate() {
  return innodb_undo_auto_truncate;
}

}  // namespace flashback
