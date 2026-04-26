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
  @file sql/flashback_sysvars.h
  Flashback 模块系统变量声明

  本文件定义 Flashback 功能的 7 个系统变量:
  - innodb_flashback_retention_seconds:      Undo 保留窗口 (秒)
  - innodb_flashback_enable_binlog_recovery: Binlog 长窗口闪回开关
  - innodb_flashback_max_rows_per_txn:       单次事务最大行数
  - flashback_dry_run_default:               默认 DRY RUN 开关
  - flashback_require_full_row_image:        Binlog Row Image 检查
  - flashback_lock_wait_timeout:             锁等待超时 (秒)
  - flashback_max_rows:                      单次闪回最大行数上限

  设计参考: mysql_flashback_implementation_v2.md §4.3
            mysql_flashback_synthesis_report.md §2.5

  使用方式:
    #include "sql/flashback_sysvars.h"
    ulong retention = flashback::get_retention_seconds();
*/

#ifndef FLASHBACK_SYSVARS_INCLUDED
#define FLASHBACK_SYSVARS_INCLUDED

#include "my_inttypes.h"

namespace flashback {

/* ================================================================
 * 全局变量声明 (由 flashback_sysvars.cc 定义)
 * ================================================================ */

/** Undo 日志保留时间（秒）。默认 900 (15 分钟) */
extern ulong innodb_flashback_retention_seconds;

/** 是否启用基于 Binlog 的长窗口闪回。默认 true */
extern bool innodb_flashback_enable_binlog_recovery;

/** 单次闪回事务最大处理行数。默认 100000 */
extern ulong innodb_flashback_max_rows_per_txn;

/** 默认 DRY RUN 模式开关。默认 false */
extern bool flashback_dry_run_default;

/** 要求 binlog_row_image=FULL。默认 true */
extern bool flashback_require_full_row_image;

/** 闪回操作锁等待超时（秒）。默认 300 */
extern ulong flashback_lock_wait_timeout;

/** 单次闪回最大行数上限（安全阀值）。默认 10000000 */
extern ulonglong flashback_max_rows;

/* ================================================================
 * 便捷访问函数
 * ================================================================ */

/**
  获取 Undo 日志保留时间（秒）。
  控制闪回查询可回溯的最大时间窗口。
  @return 保留时间，单位秒
*/
ulong get_retention_seconds();

/**
  检查是否启用了基于 Binlog 的长窗口闪回。
  @return true 表示启用，false 表示禁用
*/
bool is_binlog_recovery_enabled();

/**
  获取单次闪回事务最大处理行数。
  用于防止大事务闪回导致 Undo 膨胀。
  @return 最大行数
*/
ulong get_max_rows_per_txn();

/**
  检查 FLASHBACK TABLE 是否默认使用 DRY RUN 模式。
  DRY RUN 模式下仅评估恢复行数，不实际修改数据。
  @return true 表示默认 DRY RUN
*/
bool is_dry_run_default();

/**
  检查基于 Binlog 闪回时是否要求 binlog_row_image=FULL。
  MINIMAL 模式无法完整逆向生成反向 SQL。
  @return true 表示要求 FULL 模式
*/
bool require_full_row_image();

/**
  获取闪回操作的锁等待超时时间（秒）。
  闪回表时需要对目标表加 MDL 锁，此参数控制等待超时。
  @return 超时时间，单位秒
*/
ulong get_lock_wait_timeout();

/**
  获取单次闪回允许处理的最大行数上限（安全阀值）。
  超过此行数将拒绝执行闪回。
  @return 最大行数上限
*/
ulonglong get_max_rows();

}  // namespace flashback

#endif /* FLASHBACK_SYSVARS_INCLUDED */
