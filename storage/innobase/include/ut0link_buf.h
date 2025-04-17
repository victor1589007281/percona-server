/*****************************************************************************

Copyright (c) 2017, 2022, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

*****************************************************************************/

/**************************************************/ /**
 @file include/ut0link_buf.h

 Link buffer - concurrent data structure which allows:
         - concurrent addition of links
         - single-threaded tracking of connected path created by links
         - limited size of window with holes (missing links)

 Created 2017-08-30 Paweł Olchawa
 *******************************************************/

#ifndef ut0link_buf_h
#define ut0link_buf_h

#include <atomic>
#include <cstdint>

#include "ut0counter.h"
#include "ut0dbg.h"
#include "ut0new.h"
#include "ut0ut.h"

/** Concurrent data structure, which allows to track concurrently
performed operations which locally might be dis-ordered.
并发数据结构，用于跟踪可能局部无序的并发操作

This data structure is informed about finished concurrent operations
and tracks up to which point in a total order all operations have
been finished (there are no holes).
该数据结构接收已完成并发操作信息，并跟踪总顺序中所有操作已完成的位置(无空洞)

It also allows to limit the last period in which there might be holes.
These holes refer to unfinished concurrent operations, which precede
in the total order some operations that are already finished.
它还允许限制可能存在空洞的最后时间段，这些空洞指在总顺序中位于某些已完成操作之前的未完成操作

Threads might concurrently report finished operations (lock-free).
线程可以并发报告已完成操作(无锁)

Threads might ask for maximum currently known position in total order,
up to which all operations are already finished (lock-free).
线程可以查询当前已知的最大位置(无锁)

Single thread might track the reported finished operations and update
maximum position in total order, up to which all operations are done.
单线程可以跟踪已报告完成的操作并更新最大位置

You might look at current usages of this data structure in log0buf.cc.
可以参考log0buf.cc中的实际使用案例
*/
template <typename Position = uint64_t>
class Link_buf {
 public:
  /** Type used to express distance between two positions.
  It could become a parameter of template if it was useful.
  However there is no such need currently.
  用于表示两个位置之间距离的类型，如果需要可以成为模板参数，但目前无此必要
  */
  typedef Position Distance;

  /** Constructs the link buffer. Allocated memory for the links.
  Initializes the tail pointer with 0.
  构造链接缓冲区，分配链接内存并初始化尾部指针为0
  
  @param[in]    capacity        number of slots in the ring buffer
                               环形缓冲区的槽位数
  */
  explicit Link_buf(size_t capacity);

  // 默认构造函数
  Link_buf();

  // 移动构造函数
  Link_buf(Link_buf &&rhs);

  // 删除拷贝构造函数
  Link_buf(const Link_buf &rhs) = delete;

  // 移动赋值运算符
  Link_buf &operator=(Link_buf &&rhs);

  // 删除拷贝赋值运算符
  Link_buf &operator=(const Link_buf &rhs) = delete;

  /** Destructs the link buffer. Deallocates memory for the links.
  析构函数，释放链接内存
  */
  ~Link_buf();

  /** Add a directed link between two given positions. It is user's
  responsibility to ensure that there is space for the link. This is
  because it can be useful to ensure much earlier that there is space.
  添加两个位置间的有向链接，用户需确保有足够空间
  
  @param[in]    from    position where the link starts
                       链接起始位置
  @param[in]    to      position where the link ends (from -> to)
                       链接结束位置
  */
  void add_link(Position from, Position to);

  /** Add a directed link between two given positions. It is user's
  responsibility to ensure that there is space for the link. This is
  because it can be useful to ensure much earlier that there is space.
  In addition, advances the tail pointer in the buffer if possible.
  添加链接并可能推进尾部指针
  
  @param[in]    from    position where the link starts
                       链接起始位置
  @param[in]    to      position where the link ends (from -> to)
                       链接结束位置
  */
  void add_link_advance_tail(Position from, Position to);

  /** Advances the tail pointer in the buffer by following connected
  path created by links. Starts at current position of the pointer.
  Stops when the provided function returns true.
  沿着链接路径推进尾部指针
  
  @param[in]    stop_condition  function used as a stop condition;
                                  (lsn_t prev, lsn_t next) -> bool;
                                  returns false if we should follow
                                  the link prev->next, true to stop
                               停止条件函数
  @param[in]    max_retry       max fails to retry
                               最大重试次数
  @return true if and only if the pointer has been advanced
          返回是否成功推进指针
  */
  template <typename Stop_condition>
  bool advance_tail_until(Stop_condition stop_condition,
                          uint32_t max_retry = 1);

  /** Advances the tail pointer in the buffer without additional
  condition for stop. Stops at missing outgoing link.
  推进尾部指针直到遇到缺失的链接
  
  @see advance_tail_until()
  @return true if and only if the pointer has been advanced
          返回是否成功推进指针
  */
  bool advance_tail();

  /** @return capacity of the ring buffer
  返回环形缓冲区容量
  */
  size_t capacity() const;

  /** @return the tail pointer
  返回尾部指针
  */
  Position tail() const;

  /** Checks if there is space to add link at given position.
  User has to use this function before adding the link, and
  should wait until the free space exists.
  检查指定位置是否有空间添加链接
  
  @param[in]    position        position to check
                               要检查的位置
  @return true if and only if the space is free
          返回是否有足够空间
  */
  bool has_space(Position position);

  /** Validates (using assertions) that there are no links set
  in the range [begin, end).
  验证[begin,end)范围内没有设置链接
  
  @param[in]    begin        range start
                             范围起始
  @param[in]    end          range end
                             范围结束
  */
  void validate_no_links(Position begin, Position end);

  /** Validates (using assertions) that there no links at all.
  验证完全没有设置任何链接
  */
  void validate_no_links();

 private:
  /** Translates position expressed in original unit to position
  in the m_links (which is a ring buffer).
  将原始位置转换为环形缓冲区中的位置
  
  @param[in]    position        position in original unit
                               原始位置
  @return position in the m_links
          返回环形缓冲区中的位置
  */
  size_t slot_index(Position position) const;

  /** Computes next position by looking into slots array and
  following single link which starts in provided position.
  通过查找槽位数组计算下一个位置
  
  @param[in]    position        position to start
                               起始位置
  @param[out]   next            computed next position
                               计算得到的下一个位置
  @return false if there was no link, true otherwise
          返回是否找到有效链接
  */
  bool next_position(Position position, Position &next);

  /** Deallocated memory, if it was allocated.
  释放已分配的内存
  */
  void free();

  /** Capacity of the buffer.
  缓冲区容量
  */
  size_t m_capacity;

  /** Pointer to the ring buffer (unaligned).
  指向环形缓冲区的指针(未对齐)
  */
  std::atomic<Distance> *m_links;

  /** Tail pointer in the buffer (expressed in original unit).
  缓冲区中的尾部指针(以原始单位表示)
  */
  alignas(ut::INNODB_CACHE_LINE_SIZE) std::atomic<Position> m_tail;
};

template <typename Position>
Link_buf<Position>::Link_buf(size_t capacity)
    : m_capacity(capacity), m_tail(0) {
  if (capacity == 0) {
    m_links = nullptr;
    return;
  }

  ut_a((capacity & (capacity - 1)) == 0);

  m_links = ut::new_arr_withkey<std::atomic<Distance>>(UT_NEW_THIS_FILE_PSI_KEY,
                                                       ut::Count{capacity});

  for (size_t i = 0; i < capacity; ++i) {
    m_links[i].store(0);
  }
}

template <typename Position>
Link_buf<Position>::Link_buf() : Link_buf(0) {}

template <typename Position>
Link_buf<Position>::Link_buf(Link_buf &&rhs)
    : m_capacity(rhs.m_capacity), m_tail(rhs.m_tail.load()) {
  m_links = rhs.m_links;
  rhs.m_links = nullptr;
}

template <typename Position>
Link_buf<Position> &Link_buf<Position>::operator=(Link_buf &&rhs) {
  free();

  m_capacity = rhs.m_capacity;

  m_tail.store(rhs.m_tail.load());

  m_links = rhs.m_links;
  rhs.m_links = nullptr;

  return *this;
}

template <typename Position>
Link_buf<Position>::~Link_buf() {
  free();
}

template <typename Position>
void Link_buf<Position>::free() {
  if (m_links != nullptr) {
    ut::delete_arr(m_links);
    m_links = nullptr;
  }
}

template <typename Position>
inline void Link_buf<Position>::add_link(Position from, Position to) {
  ut_ad(to > from);
  ut_ad(to - from <= std::numeric_limits<Distance>::max());

  const auto index = slot_index(from);

  auto &slot = m_links[index];

  slot.store(to);
}

template <typename Position>
inline bool Link_buf<Position>::next_position(Position position,
                                              Position &next) {
  const auto index = slot_index(position);

  auto &slot = m_links[index];

  next = slot.load(std::memory_order_relaxed);

  return next <= position;
}

template <typename Position>
// 添加链接并可能推进尾部指针
inline void Link_buf<Position>::add_link_advance_tail(Position from,
                                                      Position to) {
  // 断言检查：结束位置必须大于起始位置
  ut_ad(to > from);
  // 断言检查：距离必须在有效范围内
  ut_ad(to - from <= std::numeric_limits<Distance>::max());

  // 获取当前尾部指针位置
  auto position = m_tail.load(std::memory_order_acquire);

  // 断言检查：当前尾部指针不能超过起始位置
  ut_ad(position <= from);

  // 如果当前尾部指针正好等于起始位置
  if (position == from) {
    /* can advance m_tail directly and exclusively, and it is unlock */
    // 可以直接且独占地推进尾部指针到结束位置
    m_tail.store(to, std::memory_order_release);
  } else {
    // 计算起始位置对应的槽位索引
    auto index = slot_index(from);
    auto &slot = m_links[index];

    /* add link */
    // 在槽位中存储结束位置
    slot.store(to, std::memory_order_release);

    // 定义停止条件：当遇到比起始位置大的位置时停止
    auto stop_condition = [&](Position prev_pos, Position) {
      return (prev_pos > from);
    };

    // 尝试沿着链接路径推进尾部指针
    advance_tail_until(stop_condition);
  }
}

template <typename Position>
template <typename Stop_condition>

/**
 * 推进尾部指针直到满足停止条件
 * 
 * 该函数在多线程环境下安全地推进尾部指针，沿着已建立的链接路径前进，
 * 直到遇到停止条件或无法继续前进为止。
 * 
 * @tparam Stop_condition 停止条件函数类型
 * @param stop_condition 停止条件函数，当返回true时停止推进
 * @param max_retry 最大重试次数
 * @return true 表示成功推进了尾部指针，false 表示没有推进
 */
template <typename Stop_condition>
bool Link_buf<Position>::advance_tail_until(Stop_condition stop_condition,
                                            uint32_t max_retry) {
  /* multi threaded aware */
  // 获取当前尾部指针位置作为起点
  auto position = m_tail.load(std::memory_order_acquire);
  auto from = position; // 记录原始位置用于比较

  uint32_t retry = 0;
  while (true) {
    // 计算当前position对应的槽位索引
    auto index = slot_index(position);
    auto &slot = m_links[index];

    // 加载该槽位的值(下一个position)
    auto next_load = slot.load(std::memory_order_acquire);

    if (next_load >= position + m_capacity) {
      /* either we wrapped and tail was advanced meanwhile,
      or there is link start_lsn -> end_lsn of length >= m_capacity */
      // 处理环形缓冲区环绕或超长链接的情况
      position = m_tail.load(std::memory_order_acquire);
      if (position != from) {
        from = position;
        continue; // 如果尾部指针已改变，重新开始
      }
    }

    // 检查是否应该停止推进
    if (next_load <= position || stop_condition(position, next_load)) {
      /* nothing to advance for now */
      return false; // 没有可推进的链接或满足停止条件
    }

    /* try to lock as storing the end */
    // 尝试原子性地获取槽位控制权
    if (slot.compare_exchange_strong(next_load, position,
                                     std::memory_order_acq_rel)) {
      /* it could happen, that after thread read position = m_tail.load(),
      it got scheduled out for longer; when it comes back it might still
      see the link going forward in that slot but m_tail could have been
      already advanced forward (as we do not reset slots when traversing
      them); thread needs to re-check if m_tail is still behind the slot. */
      position = m_tail.load(std::memory_order_acquire);
      if (position == from) {
        /* confirmed. can advance m_tail exclusively */
        position = next_load;// 安全地推进position
        break;
      }
    }

    retry++;
    if (retry > max_retry) {
      /* give up */
      return false;
    }

    UT_RELAX_CPU(); // 避免忙等待消耗CPU
    position = m_tail.load(std::memory_order_acquire);
    if (position == from) {
      /* no progress? */
      return false;
    }
    from = position;
  }

  // 沿着链接路径继续推进
  while (true) {
    Position next;

    bool stop = next_position(position, next);

    if (stop || stop_condition(position, next)) {
      break; // 遇到停止条件或无效链接
    }

    position = next; // 继续前进
  }

  // 验证原始尾部指针未被修改
  ut_a(from == m_tail.load(std::memory_order_acquire));

  /* unlock */
  // 原子性地更新尾部指针
  m_tail.store(position, std::memory_order_release);

  // 返回是否实际推进了指针
  if (position == from) {
    return false;
  }

  return true;
}

template <typename Position>
inline bool Link_buf<Position>::advance_tail() {
  auto stop_condition = [](Position, Position) { return false; };

  return advance_tail_until(stop_condition);
}

template <typename Position>
inline size_t Link_buf<Position>::capacity() const {
  return m_capacity;
}

template <typename Position>
inline Position Link_buf<Position>::tail() const {
  return m_tail.load(std::memory_order_acquire);
}

template <typename Position>

/**
 * 检查在指定位置是否有空间可以添加链接
 * 
 * 该函数用于确保在添加新链接前，环形缓冲区中有足够的空间。
 * 如果当前空间不足，会尝试推进尾部指针以释放空间。
 * 
 * @param position 要检查的位置(通常是一个LSN值)
 * @return true 表示有足够空间可以添加链接，false 表示空间不足
 */
inline bool Link_buf<Position>::has_space(Position position) {
  // 获取当前尾部指针位置
  auto tail = m_tail.load(std::memory_order_acquire);
  
  // 检查当前位置是否在尾部+容量的范围内
  if (tail + m_capacity > position) {
    return true;  // 有足够空间
  }

  // 如果没有足够空间，尝试推进尾部指针
  auto stop_condition = [](Position, Position) { return false; };
  advance_tail_until(stop_condition, 0);

  // 再次检查空间情况
  tail = m_tail.load(std::memory_order_acquire);
  return tail + m_capacity > position;
}

template <typename Position>
inline size_t Link_buf<Position>::slot_index(Position position) const {
  return position & (m_capacity - 1);
}

template <typename Position>
void Link_buf<Position>::validate_no_links(Position begin, Position end) {
  const auto tail = m_tail.load();

  /* After m_capacity iterations we would have all slots tested. */

  end = std::min(end, begin + m_capacity);

  for (; begin < end; ++begin) {
    const size_t index = slot_index(begin);

    const auto &slot = m_links[index];

    ut_a(slot.load() <= tail);
  }
}

template <typename Position>
void Link_buf<Position>::validate_no_links() {
  validate_no_links(0, m_capacity);
}

#endif /* ut0link_buf_h */
