/*****************************************************************************

Copyright (c) 2020, 2023, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#define LOCK_MODULE_IMPLEMENTATION

#include "lock0guards.h"
#include "lock0priv.h"
#include "sync0rw.h"

/*
 * RAII 是 "Resource Acquisition Is Initialization" 的缩写，翻译过来就是“资源获取即初始化”。
 * 这是一种在 C++ 中广泛使用的资源管理技术，
 * 它基于对象的生命周期来自动管理资源，如内存、文件句柄、网络连接或互斥锁等。
 * 在 RAII 模式下，资源的获取发生在对象的构造过程中，而资源的释放则发生在对象的析构过程中。
 * 这意味着当一个对象不再需要时（例如，当它超出作用域或程序正常退出时），
 * 它的析构函数会被自动调用，从而释放之前获取的资源。
 * 这种机制可以确保即使在异常情况下，资源也能被正确地释放，从而避免资源泄露。
 * RAII 的几个关键点包括：
 * 封装：资源管理逻辑被封装在一个类的构造函数和析构函数中，外部代码不需要关心资源的释放问题。
 * 自动性：资源的释放是自动的，与对象的生命周期绑定，无需显式调用函数来释放资源。
 * 安全性：即使在抛出异常的情况下，析构函数也会被调用，因此可以保证资源总是被清理。
 * 可堆栈性：由于资源是在对象生命周期结束时释放的，
 * 所以可以安全地在一个函数调用中创建多个对象，而不用担心资源冲突或释放顺序问题。
 * */

namespace locksys {

/* Global_exclusive_latch_guard */

Global_exclusive_latch_guard::Global_exclusive_latch_guard(
    ut::Location location) {
  lock_sys->latches.global_latch.x_lock(location);
}

Global_exclusive_latch_guard::~Global_exclusive_latch_guard() {
  lock_sys->latches.global_latch.x_unlock();
}

/* Global_exclusive_try_latch */

Global_exclusive_try_latch::Global_exclusive_try_latch(ut::Location location) {
  m_owns_exclusive_global_latch =
      lock_sys->latches.global_latch.try_x_lock(location);
}

Global_exclusive_try_latch::~Global_exclusive_try_latch() {
  if (m_owns_exclusive_global_latch) {
    lock_sys->latches.global_latch.x_unlock();
    m_owns_exclusive_global_latch = false;
  }
}

/* Shard_naked_latch_guard */

Shard_naked_latch_guard::Shard_naked_latch_guard(ut::Location location,
                                                 Lock_mutex &shard_mutex)
    : m_shard_mutex{shard_mutex} {
  ut_ad(owns_shared_global_latch());
  mutex_enter_inline(&m_shard_mutex, location);
}

Shard_naked_latch_guard::Shard_naked_latch_guard(ut::Location location,
                                                 const table_id_t &table_id)
    : Shard_naked_latch_guard{
          location, lock_sys->latches.table_shards.get_mutex(table_id)} {}

/**
 * @brief 构造函数：Shard_naked_latch_guard
 *
 * 本构造函数用于在不持有任何锁的情况下初始化Shard_naked_latch_guard对象。
 * 它通过调用另一个重载的构造函数来实现，后者负责获取对应页面的互斥锁。
 * 这种设计允许在不需要锁的情况下创建一个latch guard对象，这在某些场景下
 * 对于管理latch的生命周期很有用。
 *
 * @param location 锁定的位置信息，用于调试和日志记录。
 * @param page_id 需要锁定的页面的ID，用于定位具体的页面资源。
 */
Shard_naked_latch_guard::Shard_naked_latch_guard(ut::Location location,
                                                 const page_id_t &page_id)
    : Shard_naked_latch_guard{
          location, lock_sys->latches.page_shards.get_mutex(page_id)} {}


Shard_naked_latch_guard::~Shard_naked_latch_guard() {
  mutex_exit(&m_shard_mutex);
}

/* Global_shared_latch_guard */

Global_shared_latch_guard::Global_shared_latch_guard(ut::Location location) {
  lock_sys->latches.global_latch.s_lock(location);
}

Global_shared_latch_guard::~Global_shared_latch_guard() {
  lock_sys->latches.global_latch.s_unlock();
}
bool Global_shared_latch_guard::is_x_blocked_by_us() {
  return lock_sys->latches.global_latch.is_x_blocked_by_our_s();
}

/* Shard_naked_latches_guard */

Shard_naked_latches_guard::Shard_naked_latches_guard(Lock_mutex &shard_mutex_a,
                                                     Lock_mutex &shard_mutex_b)
    : m_shard_mutex_1{*std::min(&shard_mutex_a, &shard_mutex_b, MUTEX_ORDER)},
      m_shard_mutex_2{*std::max(&shard_mutex_a, &shard_mutex_b, MUTEX_ORDER)} {
  ut_ad(owns_shared_global_latch());
  if (&m_shard_mutex_1 != &m_shard_mutex_2) {
    mutex_enter(&m_shard_mutex_1);
  }
  mutex_enter(&m_shard_mutex_2);
}

Shard_naked_latches_guard::Shard_naked_latches_guard(const buf_block_t &block_a,
                                                     const buf_block_t &block_b)
    : Shard_naked_latches_guard{
          lock_sys->latches.page_shards.get_mutex(block_a.get_page_id()),
          lock_sys->latches.page_shards.get_mutex(block_b.get_page_id())} {}

Shard_naked_latches_guard::~Shard_naked_latches_guard() {
  mutex_exit(&m_shard_mutex_2);
  if (&m_shard_mutex_1 != &m_shard_mutex_2) {
    mutex_exit(&m_shard_mutex_1);
  }
}

}  // namespace locksys
