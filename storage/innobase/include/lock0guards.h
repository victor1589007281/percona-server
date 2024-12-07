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

#ifndef lock0guards_h
#define lock0guards_h

#include "lock0lock.h"
#include "ut0class_life_cycle.h"

namespace locksys {
/**
 * A RAII helper which latches global_latch in exclusive mode during constructor,
 * and unlatches it during destruction, preventing any other threads from activity
 * within lock_sys for it's entire scope.
 *
 * 一个RAII辅助类，在构造时以独占模式锁定global_latch，
 * 并在析构时解锁，防止其他线程在整个作用域内对lock_sys进行活动。
 */

class Global_exclusive_latch_guard : private ut::Non_copyable {
 public:
  /**
   * 构造函数，接收位置信息参数。
   *
   * @param location 代码的位置信息，用于日志或调试。
   *                表示正在获取锁的位置。
   */
  Global_exclusive_latch_guard(ut::Location location);

  /**
   * 析构函数，自动释放独占锁。
   */
  ~Global_exclusive_latch_guard();
};


/**
A RAII helper which tries to exclusively latch the global_lach in constructor
and unlatches it, if needed, during destruction, preventing any other threads
from activity within lock_sys for it's entire scope, if owns_lock().
*/
class Global_exclusive_try_latch : private ut::Non_copyable {
 public:
  Global_exclusive_try_latch(ut::Location location);
  ~Global_exclusive_try_latch();
  /** Checks if succeeded to latch the global_latch during construction.
  @return true iff the current thread owns (through this instance) the exclusive
          global lock_sys latch */
  bool owns_lock() const noexcept { return m_owns_exclusive_global_latch; }

 private:
  /** Did the constructor succeed to acquire exclusive global lock_sys latch? */
  bool m_owns_exclusive_global_latch;
};
/**
 * A RAII helper which latches global_latch in shared mode during constructor,
 * and unlatches it during destruction, preventing any other thread from acquiring
 * exclusive latch. This should be used in combination Shard_naked_latch_guard,
 * preferably by simply using Shard_latch_guard which combines the two for you.
 *
 * @brief 全局共享锁卫士，用于在构造时以共享模式获取全局锁，并在析构时释放。
 *        防止其他线程在该卫士作用期间获取独占锁。建议与Shard_naked_latch_guard配合使用，
 *        或直接使用结合两者的Shard_latch_guard。
 */

class Global_shared_latch_guard : private ut::Non_copyable {
 public:
  /**
   * @brief 构造函数。
   *
   * 在构造函数中尝试以共享模式获取全局锁。
   *
   * @param location 调用位置信息，用于调试。
   */
  Global_shared_latch_guard(ut::Location location);

  /**
   * @brief 析构函数。
   *
   * 在析构函数中释放已持有的全局锁。
   */
  ~Global_shared_latch_guard();

  /**
   * @brief 检查是否有线程因等待独占锁而被当前线程阻塞。
   *
   * 返回是否有一个或多个线程正在等待独占锁，而被当前线程持有的共享锁所阻塞。
   *
   * @return true 若有线程被当前线程的共享锁阻塞。
   * @return false 若没有线程被当前线程的共享锁阻塞。
   */
  bool is_x_blocked_by_us();
};

/**
 * A RAII helper which latches the mutex protecting given shard during constructor,
 * and unlatches it during destruction.
 *
 * 一个RAII风格的辅助类，在构造时锁定保护特定分片的互斥锁，并在析构时解锁。
 *
 * You quite probably don't want to use this class, which only takes a shard's
 * latch, without acquiring global_latch - which gives no protection from threads
 * which latch only the global_latch exclusively to prevent any activity.
 *
 * 您可能并不想在没有获取全局锁(global_latch)的情况下使用这个类，仅获取分片锁，
 * 因为这无法防止那些只独占获取全局锁来阻止任何活动的线程。
 *
 * You should use it in combination with Global_shared_latch_guard, so that you
 * first obtain an s-latch on the global_latch, or simply use the Shard_latch_guard
 * class which already combines the two for you.
 *
 * 您应该结合Global_shared_latch_guard一起使用，首先在全局锁上获得共享锁(s-latch)，
 * 或者直接使用Shard_latch_guard类，它已经为您合并了这两种锁。
 */

class Shard_naked_latch_guard : private ut::Non_copyable {
  // 显式构造函数，接受位置信息和分片互斥锁引用作为参数
  explicit Shard_naked_latch_guard(ut::Location location,
                                   Lock_mutex &shard_mutex);

 public:
  // 显式构造函数，接受位置信息和表ID，用于确定要锁定的分片
  explicit Shard_naked_latch_guard(ut::Location location,
                                   const table_id_t &table_id);

  // 显式构造函数，接受位置信息和页ID，用于确定要锁定的分片
  explicit Shard_naked_latch_guard(ut::Location location,
                                   const page_id_t &page_id);

  // 析构函数，自动解锁在构造时锁定的分片互斥锁
  ~Shard_naked_latch_guard();

 private:
  // 在构造函数中请求并锁定的分片互斥锁的引用
  Lock_mutex &m_shard_mutex;
};


/**
 * A RAII wrapper class which combines Global_shared_latch_guard and
 * Shard_naked_latch_guard to s-latch the global lock_sys latch and latch the mutex
 * protecting the specified shard for the duration of its scope.
 * The order of initialization is important: we have to take shared global latch
 * BEFORE we attempt to use hash function to compute correct shard and latch it.
 *
 * 管理全局锁和特定分片锁的RAII封装类，结合了Global_shared_latch_guard和Shard_naked_latch_guard，
 * 在其作用域期间，对全局lock_sys锁进行s-锁定，并锁定保护指定分片的互斥锁。
 * 初始化顺序至关重要：必须在尝试使用哈希函数计算正确的分片并锁定它之前获取共享的全局锁。
 */
class Shard_latch_guard {
  Global_shared_latch_guard m_global_shared_latch_guard; // 全局共享锁守护对象
  Shard_naked_latch_guard m_shard_naked_latch_guard;     // 分片裸锁守护对象

public:
  /**
   * Constructor for acquiring latches based on a table object.
   * @param location The location information for logging and debugging.
   * @param table The table object for which the latch is to be acquired.
   *
   * 根据表对象获取锁的构造函数。
   * @param location 日志和调试的位置信息。
   * @param table 需要获取锁的表对象。
   *
   * explicit: 防止隐式转换
   */
  explicit Shard_latch_guard(ut::Location location, const dict_table_t &table)
      : m_global_shared_latch_guard{location},
        m_shard_naked_latch_guard{location, table.id} {}

  /**
   * Constructor for acquiring latches based on a page ID.
   * @param location The location information for logging and debugging.
   * @param page_id The page ID for which the latch is to be acquired.
   *
   * 根据页ID获取锁的构造函数。
   * @param location 日志和调试的位置信息。
   * @param page_id 需要获取锁的页ID。
   */
  explicit Shard_latch_guard(ut::Location location, const page_id_t &page_id)
      : m_global_shared_latch_guard{location},
        m_shard_naked_latch_guard{location, page_id} {}
};
/**
 * A RAII helper which latches the mutexes protecting specified shards for the
 * duration of its scope.
 * It makes sure to take the latches in correct order and handles the case where
 * both pages are in the same shard correctly.
 * You quite probably don't want to use this class, which only takes a shard's
 * latch, without acquiring global_latch - which gives no protection from threads
 * which latch only the global_latch exclusively to prevent any activity.
 * You should use it in combination with Global_shared_latch_guard, so that you
 * first obtain an s-latch on the global_latch, or simply use the
 * Shard_latches_guard class which already combines the two for you.
 *
 * 此类是一个RAII辅助工具，在其作用域期间锁定保护指定分片的互斥锁。
 * 它确保按正确的顺序获取锁，并正确处理两个页面位于同一分片的情况。
 * 您很可能不希望在没有获取全局锁(global_latch)的情况下使用此类，
 * 因为这无法防止仅独占获取全局锁以阻止任何活动的线程。
 * 您应将其与Global_shared_latch_guard结合使用，首先在全局锁上获得共享锁，
 * 或者直接使用Shard_latches_guard类，它已经为您组合了这两种功能。
 */

class Shard_naked_latches_guard {
  /**
   * Constructor taking two mutex references for shard-level locking.
   *
   * 明确构造函数，接受两个分片互斥锁引用，用于后续的锁定操作。
   */
  explicit Shard_naked_latches_guard(Lock_mutex &shard_mutex_a, Lock_mutex &shard_mutex_b);

public:
  /**
   * Constructor taking two buffer blocks to determine the shard mutexes to lock.
   *
   * 构造函数，接受两个缓冲块引用，用于确定要锁定的分片互斥锁。
   *
   * @param block_a 第一个缓冲块引用，用于确定要锁定的分片互斥锁。
   * @param block_b 第二个缓冲块引用，用于确定要锁定的分片互斥锁。
   */
  explicit Shard_naked_latches_guard(const buf_block_t &block_a, const buf_block_t &block_b);

  /**
   * Destructor, releases the acquired latches.
   *
   * 析构函数，释放已获取的锁。
   */
  ~Shard_naked_latches_guard();

private:
  /** The "smallest" of the two shards' mutexes in the latching order */
  /** 分片互斥锁中"较小"的一个，在锁定顺序中。 */
  Lock_mutex &m_shard_mutex_1;

  /** The "largest" of the two shards' mutexes in the latching order */
  /** 分片互斥锁中"较大"的一个，在锁定顺序中。 */
  Lock_mutex &m_shard_mutex_2;

  /** The ordering on shard mutexes used to avoid deadlocks */
  /** 用于避免死锁的分片互斥锁上的排序规则。 */
  static constexpr std::less<Lock_mutex *> MUTEX_ORDER{};
};

/**
 * A RAII wrapper class which s-latches the global lock_sys shard, and mutexes
 * protecting specified shards for the duration of its scope.
 * It makes sure to take the latches in correct order and handles the case where
 * both pages are in the same shard correctly.
 * The order of initialization is important: we have to take shared global latch
 * BEFORE we attempt to use hash function to compute correct shard and latch it.
 *
 * 此类是一个RAII封装类，用于在作用域期间锁定全局lock_sys分片和保护指定分片的互斥锁。
 * 它确保以正确的顺序获取锁，并正确处理两个页面位于同一分片的情况。
 * 初始化顺序很重要：我们必须在尝试使用哈希函数计算正确的分片并锁定它之前，
 * 先获取共享的全局锁。
 */

class Shard_latches_guard {
 public:
  /**
   * Constructor for Shard_latches_guard.
   *
   * @param location 位置信息，用于日志或调试目的。
   * @param block_a 第一个要访问的缓冲区块，用于确定要锁定的分片。
   * @param block_b 第二个要访问的缓冲区块，用于确定要锁定的分片。
   */
  explicit Shard_latches_guard(ut::Location location,
                               const buf_block_t &block_a,
                               const buf_block_t &block_b)
      : m_global_shared_latch_guard{location},
        m_shard_naked_latches_guard{block_a, block_b} {}

 private:
  Global_shared_latch_guard m_global_shared_latch_guard;  ///< 用于保护全局锁系统的锁。
  Shard_naked_latches_guard m_shard_naked_latches_guard;  ///< 用于保护特定分片的锁。
};

}  // namespace locksys

#endif /* lock0guards_h */
