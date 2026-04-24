/*****************************************************************************

Copyright (c) 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/flashback_purge_guard.h
 Flashback Purge Guard — RAII 保护器

 用于在闪回操作期间暂停 InnoDB Purge 线程，
 防止 Undo Log 被清理导致闪回版本链断裂。

 构造时调用 trx_purge_stop() 暂停 Purge,
 析构时调用 trx_purge_run() 恢复 Purge。

 使用示例:
   {
     FlashbackPurgeGuard guard;
     // ... 执行闪回操作 ...
   }  // guard 离开作用域时自动恢复 Purge
 */

#ifndef flashback_purge_guard_h
#define flashback_purge_guard_h

#include "univ.i"

/** Flashback 期间的 Purge 保护器 (RAII)
 *
 * WHY: 闪回操作依赖 Undo Log 版本链来重建历史数据版本。
 *      如果 Purge 线程在闪回过程中清理了 Undo 记录, 会导致
 *      DB_MISSING_HISTORY 错误, 闪回无法完成。
 *      此保护器确保在闪回操作期间 Purge 被暂停。
 *
 * 线程安全: 是。底层 trx_purge_stop/run 内部持有 purge_sys->latch。
 * 可重入性: 否。同一时刻只允许一个 FlashbackPurgeGuard 实例存在。
 */
class FlashbackPurgeGuard {
 public:
  /** 构造: 暂停 Purge 线程
   *
   * 如果 Purge 已被禁用 (PURGE_STATE_DISABLED), 则不做任何操作,
   * 此时 m_purge_was_disabled 标记为 true, 析构时也不会恢复。
   */
  FlashbackPurgeGuard();

  /** 析构: 恢复 Purge 线程
   *
   * 仅在构造时 Purge 未被禁用的情况下调用 trx_purge_run()。
   */
  ~FlashbackPurgeGuard();

  /** 禁止拷贝 */
  FlashbackPurgeGuard(const FlashbackPurgeGuard &) = delete;
  FlashbackPurgeGuard &operator=(const FlashbackPurgeGuard &) = delete;

  /** 禁止移动 — RAII 保护器不应该被转移所有权 */
  FlashbackPurgeGuard(FlashbackPurgeGuard &&) = delete;
  FlashbackPurgeGuard &operator=(FlashbackPurgeGuard &&) = delete;

  /** 查询构造时 Purge 是否已处于禁用状态
   * @return true 如果 Purge 在构造时为 DISABLED 状态
   */
  bool was_disabled() const { return m_purge_was_disabled; }

 private:
  /** 构造时 Purge 是否已被禁用 */
  bool m_purge_was_disabled;
};

#endif /* flashback_purge_guard_h */
