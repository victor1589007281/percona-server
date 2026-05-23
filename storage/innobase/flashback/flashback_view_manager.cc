/**
  @file storage/innobase/flashback/flashback_view_manager.cc
  @brief Flashback View Manager — 多 ReadView 生命周期管理实现

  为闪回查询 (AS OF TIMESTAMP) 提供 ReadView 注册/注销机制。

  线程安全:
  - 所有公开方法通过 m_mutex 保证线程安全
  - 与 InnoDB 内部 MVCC 机制兼容 (通过 row_build_flashback_read_view 构造 ReadView)

  约束遵守:
  - C1: 本模块不涉及 binlog 写入，纯读操作
  - C9: 仅持有内部 mutex，不阻塞并发写入
*/

#include "my_config.h"

#include "flashback_view_manager.h"

#include "read0types.h"
#include "trx0trx.h"    /* TRX_ID_MAX */
#include "ut0dbg.h"

/* ================================================================
 * 构造函数 — 初始化配置参数
 * ================================================================ */

/** 构造函数
 *
 * 初始化视图管理器，设置最大视图数量和 TTL。
 *
 * WHY: 参数使用默认值 fallback，避免调用者必须指定所有参数。
 *      默认 max_views=64 足够应对绝大多数并发闪回查询场景。
 *      默认 TTL=300 秒 (5 分钟) 为客户端提供足够的容错时间。
 *
 * @param[in] max_views  最大并发视图数量 (0 表示使用默认值 64)
 * @param[in] ttl_sec    视图 TTL (秒) (0 表示使用默认值 300)
 */
FlashbackViewManager::FlashbackViewManager(ulint max_views, ulint ttl_sec)
    : m_max_views(max_views > 0 ? max_views
                                : FLASHBACK_VIEW_MANAGER_DEFAULT_MAX_VIEWS),
      m_ttl(ttl_sec > 0 ? std::chrono::seconds(ttl_sec)
                        : std::chrono::seconds(
                              FLASHBACK_VIEW_MANAGER_DEFAULT_TTL_SEC)),
      m_next_view_id(1) {
  ut_ad(m_max_views > 0);
  ut_ad(m_ttl.count() > 0);
}

/* ================================================================
 * 析构函数 — 清理所有已注册视图
 * ================================================================ */

/** 析构函数
 *
 * 注销所有已注册的 ReadView，释放资源。
 *
 * WHY: 确保管理器销毁时不会遗留未关闭的 ReadView，
 *      避免影响 Purge 线程的判断 (get_oldest_active_trx_id 返回错误值)。
 */
FlashbackViewManager::~FlashbackViewManager() {
  std::lock_guard<std::mutex> lock(m_mutex);
  for (auto &pair : m_views) {
    pair.second.view.close();
  }
  m_views.clear();
}

/* ================================================================
 * 核心接口: register_view
 * ================================================================ */

/** 注册一个新的闪回 ReadView
 *
 * 实现步骤:
 * 1. 获取锁
 * 2. 清理过期视图 (自动维护)
 * 3. 检查数量上限，如超限则返回 INVALID_VIEW_ID
 * 4. 生成 view_id，构造 ReadView
 * 5. 注册到 m_views 映射
 * 6. 返回 view_id
 *
 * WHY: 在注册前自动清理过期视图，是一种 "lazy cleanup" 策略。
 *      这比单独启动后台清理线程更简单，且不会影响正常性能
 *      (清理操作只在注册时触发，且只扫描过期条目)。
 *
 * @param[in] target_trx_id  目标事务 ID
 * @return  视图句柄 (INVALID_VIEW_ID 表示失败)
 */
FlashbackViewManager::view_id_t FlashbackViewManager::register_view(
    trx_id_t target_trx_id) {
  ut_ad(target_trx_id > 0);

  std::lock_guard<std::mutex> lock(m_mutex);

  /* 步骤 1: 自动清理过期视图 */
  cleanup_expired_locked();

  /* 步骤 2: 检查数量上限 */
  if (m_views.size() >= m_max_views) {
    /* WHY: 达到上限时返回 INVALID_VIEW_ID，而非抛出异常。
       这符合 MySQL 的错误处理惯例: 通过返回值而非异常报告错误。
       调用者应检查返回值并报告 ER_FLASHBACK_VIEW_LIMIT 给客户端。 */
    return INVALID_VIEW_ID;
  }

  /* 步骤 3: 生成唯一 view_id */
  view_id_t vid = generate_view_id();

  /* 步骤 4: 构造 ReadView
     WHY: 使用 row_build_flashback_read_view 构造一个针对目标 trx_id 的
     ReadView。此函数是 ReadView 的 friend，可以直接设置内部字段。
     我们通过该函数初始化 view 对象，避免直接访问私有成员。 */
  ViewEntry entry;
  entry.target_trx_id = target_trx_id;
  entry.registered_at = std::chrono::steady_clock::now();

  row_build_flashback_read_view(target_trx_id, entry.view);

  /* 步骤 5: 注册到映射 */
  m_views.emplace(vid, std::move(entry));

  return vid;
}

/* ================================================================
 * 核心接口: unregister_view
 * ================================================================ */

/** 注销一个已注册的闪回 ReadView
 *
 * WHY: 注销操作是幂等的 — 如果 view_id 不存在或已注销，直接返回。
 *      这避免客户端重复注销时产生错误，也简化了错误处理逻辑。
 *
 * @param[in] view_id  要注销的视图句柄
 */
void FlashbackViewManager::unregister_view(view_id_t view_id) {
  if (view_id == INVALID_VIEW_ID) {
    return;
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  unregister_view_locked(view_id);
}

/* ================================================================
 * 核心接口: get_oldest_active_trx_id
 * ================================================================ */

/** 获取所有活跃视图中的最小 up_limit_id
 *
 * 遍历所有已注册的 ReadView，返回其中最小的 m_up_limit_id。
 * 如果没有活跃视图，返回 TRX_ID_MAX (表示不阻塞任何 Purge)。
 *
 * WHY: Purge 线程通过此值判断哪些 undo 记录仍被闪回查询需要。
 *      只有 undo 记录的事务 ID < 此值时，才可以安全清理。
 *
 * @return  最小 up_limit_id; 无活跃视图时返回 TRX_ID_MAX
 */
trx_id_t FlashbackViewManager::get_oldest_active_trx_id() const {
  std::lock_guard<std::mutex> lock(m_mutex);

  trx_id_t oldest = TRX_ID_MAX;

  for (const auto &pair : m_views) {
    trx_id_t up_limit = pair.second.view.up_limit_id();
    if (up_limit < oldest) {
      oldest = up_limit;
    }
  }

  return oldest;
}

/* ================================================================
 * 辅助方法: active_view_count
 * ================================================================ */

/** 获取当前活跃视图数量 */
ulint FlashbackViewManager::active_view_count() const {
  std::lock_guard<std::mutex> lock(m_mutex);
  return static_cast<ulint>(m_views.size());
}

/* ================================================================
 * 辅助方法: cleanup_expired
 * ================================================================ */

/** 清理所有已过期的视图
 *
 * 遍历 m_views，注销那些注册时间超过 TTL 的视图。
 * 供外部显式调用或在 register_view 时自动触发。
 *
 * @return  清理的视图数量
 */
ulint FlashbackViewManager::cleanup_expired() {
  std::lock_guard<std::mutex> lock(m_mutex);
  return cleanup_expired_locked();
}

/* ================================================================
 * 内部辅助: cleanup_expired_locked (调用者需已持有 m_mutex)
 * ================================================================ */

/** 内部过期清理 (调用者需已持有 m_mutex)
 *
 * WHY: 使用 erase-remove_if 风格的遍历清理。
 *      由于 std::unordered_map 不支持迭代时安全删除，
 *      我们使用手动迭代 + erase 的方式。
 *
 * @return  清理的视图数量
 */
ulint FlashbackViewManager::cleanup_expired_locked() {
  auto now = std::chrono::steady_clock::now();
  ulint cleaned = 0;

  for (auto it = m_views.begin(); it != m_views.end();) {
    auto elapsed = now - it->second.registered_at;
    if (elapsed >= m_ttl) {
      it->second.view.close();
      it = m_views.erase(it);
      cleaned++;
    } else {
      ++it;
    }
  }

  return cleaned;
}

/* ================================================================
 * 内部辅助: unregister_view_locked (调用者需已持有 m_mutex)
 * ================================================================ */

/** 内部注销函数 (调用者需已持有 m_mutex) */
void FlashbackViewManager::unregister_view_locked(view_id_t view_id) {
  auto it = m_views.find(view_id);
  if (it == m_views.end()) {
    /* 视图不存在，幂等返回 */
    return;
  }

  /* 关闭 ReadView */
  it->second.view.close();
  m_views.erase(it);
}

/* ================================================================
 * 内部辅助: generate_view_id
 * ================================================================ */

/** 生成唯一的视图 ID
 *
 * WHY: 使用单调递增的计数器生成 view_id。
 *      从 1 开始 (0 保留为 INVALID_VIEW_ID)。
 *      如果计数器溢出到 0，自动跳回 1。
 *      在 64 位计数器下，溢出几乎不可能发生。
 *
 * @return  新的 view_id
 */
FlashbackViewManager::view_id_t FlashbackViewManager::generate_view_id() {
  view_id_t vid = m_next_view_id++;
  /* 溢出保护: 0 是 INVALID_VIEW_ID，跳过 */
  if (m_next_view_id == INVALID_VIEW_ID) {
    m_next_view_id = 1;
  }
  return vid;
}
