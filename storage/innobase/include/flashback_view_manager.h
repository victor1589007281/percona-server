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

/** @file include/flashback_view_manager.h
 Flashback View Manager — 多 ReadView 生命周期管理

 为闪回查询 (AS OF TIMESTAMP) 提供 ReadView 注册/注销机制，
 管理多个一致性读视图的生命周期。

 核心职责:
 1. register_view(): 为指定目标 trx_id 注册一个新的 ReadView
 2. unregister_view(): 注销已注册的 ReadView
 3. get_oldest_active_trx_id(): 返回所有活跃视图中的最小 trx_id
    (用于防止 Undo Purge 清理仍被需要的历史数据)
 4. TTL 过期清理: 自动清理超时的视图

 设计理由:
 - 多个并发闪回查询可能需要不同的 ReadView (不同的目标时间点)
 - Purge 线程需要知道最老的活跃 trx_id 以决定何时可以安全清理 Undo
 - 视图数量有上限 (防止资源耗尽)
 - 视图有 TTL (防止客户端忘记注销导致泄漏)

 线程安全:
 - register_view / unregister_view 使用 mutex 保护，线程安全
 - get_oldest_active_trx_id 使用 mutex 保护，线程安全
 - 与 InnoDB 的 MVCC (MVCC::view_add/view_close) 兼容

 约束遵守:
 - C1: 本模块不涉及 binlog 写入 (纯读操作)
 - C9: 不阻塞并发写入 — 只持有内部 mutex，不持有任何表级或行级锁
*/

#ifndef flashback_view_manager_h
#define flashback_view_manager_h

#include <chrono>
#include <mutex>
#include <unordered_map>

#include "read0types.h" /* ReadView, row_build_flashback_read_view */
#include "trx0types.h"  /* trx_id_t */
#include "univ.i"

/* ================================================================
 * 常量定义
 * ================================================================ */

/** 默认最大并发视图数量 */
static constexpr ulint FLASHBACK_VIEW_MANAGER_DEFAULT_MAX_VIEWS = 64;

/** 默认视图 TTL (秒)，超时后自动清理 */
static constexpr ulint FLASHBACK_VIEW_MANAGER_DEFAULT_TTL_SEC = 300;

/* ================================================================
 * FlashbackViewManager
 * ================================================================ */

/** 闪回 ReadView 管理器
 *
 * 管理多个闪回查询的 ReadView 生命周期。
 * 每个视图用一个 view_id (调用者提供的句柄) 标识。
 *
 * WHY: 闪回查询 (SELECT ... AS OF TIMESTAMP) 需要构造一个特定的 ReadView
 *      来读取目标时间点的历史版本。当多个并发闪回查询针对不同的时间点时，
 *      需要同时维护多个 ReadView。此管理器提供:
 *      - 视图注册/注销 (带数量限制)
 *      - 最老活跃 trx_id 查询 (供 Purge 线程使用)
 *      - TTL 自动清理 (防止视图泄漏)
 *
 * 线程安全: 是。所有公开方法内部持有 m_mutex。
 * 可重入性: 否。同一线程不能递归调用 register_view。
 */
class FlashbackViewManager {
 public:
  /** 视图句柄类型，由调用者用于标识已注册的视图 */
  using view_id_t = uint64_t;

  /** 无效视图句柄 */
  static constexpr view_id_t INVALID_VIEW_ID = 0;

  /** 构造函数
   *
   * @param[in] max_views  最大并发视图数量 (0 表示使用默认值 64)
   * @param[in] ttl_sec    视图 TTL (秒)，超时后自动清理 (0 表示使用默认值 300)
   */
  explicit FlashbackViewManager(ulint max_views = 0, ulint ttl_sec = 0);

  /** 析构函数: 注销所有已注册的视图 */
  ~FlashbackViewManager();

  /** 禁止拷贝 */
  FlashbackViewManager(const FlashbackViewManager &) = delete;
  FlashbackViewManager &operator=(const FlashbackViewManager &) = delete;

  /** 禁止移动 — 管理器不应该被转移所有权 */
  FlashbackViewManager(FlashbackViewManager &&) = delete;
  FlashbackViewManager &operator=(FlashbackViewManager &&) = delete;

  /* ================================================================
   * 核心接口: register / unregister / query
   * ================================================================ */

  /** 注册一个新的闪回 ReadView
   *
   * 为指定的目标 trx_id 构造并注册一个 ReadView。
   * 如果已达到 max_views 上限，返回 INVALID_VIEW_ID。
   *
   * WHY: 闪回查询需要 ReadView 来确定哪些事务的更改对当前查询可见。
   *      对于 AS OF TIMESTAMP 查询，目标 trx_id 标识了 "在此 trx 之前
   *      提交的事务可见" 的边界。
   *
   * @param[in] target_trx_id  目标事务 ID (所有 >= 此 ID 的事务不可见)
   * @return  视图句柄 (INVALID_VIEW_ID 表示注册失败，如已达上限)
   */
  view_id_t register_view(trx_id_t target_trx_id);

  /** 注销一个已注册的闪回 ReadView
   *
   * 释放与指定 view_id 关联的 ReadView 资源。
   * 如果 view_id 无效或不存在，无操作 (幂等)。
   *
   * @param[in] view_id  要注销的视图句柄
   */
  void unregister_view(view_id_t view_id);

  /** 获取所有活跃视图中的最小 up_limit_id
   *
   * 返回值用于通知 Purge 线程: 所有 undo 记录的 trx_id >= 此值
   * 可能被某些闪回查询需要，不应被清理。
   *
   * WHY: Purge 线程需要知道系统中最老的活跃 ReadView 的可见性边界，
   *      以避免清理仍被需要的 undo 历史。
   *
   * @return  最小 up_limit_id; 如果没有活跃视图，返回 TRX_ID_MAX
   */
  trx_id_t get_oldest_active_trx_id() const;

  /** 获取当前活跃视图数量
   * @return 活跃视图数量
   */
  ulint active_view_count() const;

  /** 获取最大并发视图数量
   * @return max_views 限制
   */
  ulint max_views() const { return m_max_views; }

  /** 清理所有已过期的视图 (基于 TTL)
   *
   * 遍历所有已注册的视图，注销那些注册时间超过 TTL 的视图。
   * 通常在定期维护调用时执行，或者在 register_view 时发现
   * 达到上限时自动触发。
   *
   * @return  清理的视图数量
   */
  ulint cleanup_expired();

  /* ================================================================
   * 内部结构 (公开用于调试)
   * ================================================================ */

  /** 单个视图的元数据 */
  struct ViewEntry {
    /** ReadView 对象 (由 InnoDB MVCC 机制管理生命周期) */
    ReadView view;

    /** 注册时间 (steady_clock 时间戳) */
    std::chrono::steady_clock::time_point registered_at;

    /** 目标 trx_id */
    trx_id_t target_trx_id;
  };

 private:
  /** 生成唯一的视图 ID
   * @return 新的 view_id (保证在当前管理器生命周期内不重复)
   */
  view_id_t generate_view_id();

  /** 内部注销函数 (调用者需已持有 m_mutex) */
  void unregister_view_locked(view_id_t view_id);

  /** 最大并发视图数量 */
  ulint m_max_views;

  /** 视图 TTL (秒) */
  std::chrono::seconds m_ttl;

  /** 保护 m_views 和 m_next_view_id 的互斥锁 */
  mutable std::mutex m_mutex;

  /** 视图存储: view_id → ViewEntry */
  std::unordered_map<view_id_t, ViewEntry> m_views;

  /** 下一个可用的视图 ID (单调递增) */
  view_id_t m_next_view_id;
};

#endif /* flashback_view_manager_h */
