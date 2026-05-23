# MySQL Undo Log 架构优化 — 支撑闪回与历史查询能力

> **目标版本**: Percona Server 8.4.7-7 LTS (MySQL 8.4 LTS 系列)
> **源码基础**: `/home/victor/base/git/others/percona-server` (branch: 8.4.7-7)
> **上游输入**: `research/mysql_undo_architecture_research.md` (技术调研报告)
> **架构师**: architect
> **日期**: 2025-07-29

---

## 1. 架构概览

### 1.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            MySQL Server Layer                                 │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                      Flashback Query Layer                              │  │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐ │  │
│  │  │  AS OF TIMESTAMP │  │ VERSIONS BETWEEN │  │  FLASHBACK TABLE    │ │  │
│  │  │  (read-only)     │  │  (read-only)     │  │  (read+write)       │ │  │
│  │  └────────┬─────────┘  └────────┬─────────┘  └──────────┬─────────┘ │  │
│  │           │                       │                       │            │  │
│  │           └───────────────────────┼───────────────────────┘            │  │
│  │                               │                                        │  │
│  └───────────────────────────────┼────────────────────────────────────────┘  │
└───────────────────────────────────┼────────────────────────────────────────────┘
                                    │
┌───────────────────────────────────┼────────────────────────────────────────────┐
│                            InnoDB Engine Layer                                │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │                   Purge Control Layer                                   │  │
│  │                                                                          │  │
│  │  ┌─────────────────────────────────┐  ┌─────────────────────────────┐   │  │
│  │  │   FlashbackReadViewManager     │  │   PurgeHoldScheduler       │   │  │
│  │  │   (H1: 多视图管理)              │  │   (H5: 时间维度延迟调度)   │   │  │
│  │  │                                 │  │                            │   │  │
│  │  │  • register_view()             │  │  • hold_until_timestamp()  │   │  │
│  │  │  • unregister_view()           │  │  • adjust_purge_throttle() │   │  │
│  │  │  • find_oldest_active_view()   │  │  • time_based_purge_lag()  │   │  │
│  │  │                                 │  │                            │   │  │
│  │  │  依赖: read0read.h (ReadView) │  │  依赖: trx0purge.h        │   │  │
│  │  └──────────────┬────────────────┘  └──────────────┬─────────────┘   │  │
│  │                 │                                     │                 │  │
│  │                 └─────────────────┬─────────────────┘                 │  │
│  │                                   │                                 │  │
│  └───────────────────────────────────┼─────────────────────────────────────┘  │
│                                      │                                         │
│  ┌───────────────────────────────────┼─────────────────────────────────────┐  │
│  │                      InnoDB Core (Existing)                            │  │
│  │                                                                          │  │
│  │  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────────┐ │  │
│  │  │  MVCC/ReadView   │  │  Undo Manager    │  │  Purge Thread Pool  │ │  │
│  │  │  (read0read.h)   │  │  (trx0undo.cc)   │  │  (trx0purge.cc)     │ │  │
│  │  └──────────────────┘  └──────────────────┘  └──────────────────────┘ │  │
│  │                                                                          │  │
│  └──────────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────────┘
                                    │
                                    ▼
┌─────────────────────────────────────────────────────────────────────────────────┐
│                          Undo Tablespace Layer                                │
│                                                                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐ │
│  │  undo_001    │  │  undo_002    │  │  undo_003    │  │  system tablespace │ │
│  │  (rollback   │  │  (rollback   │  │  (rollback   │  │  (rollback seg)   │ │
│  │   segments)  │  │   segments)  │  │   segments)  │  │                   │ │
│  └──────────────┘  └──────────────┘  └──────────────┘  └──────────────────┘ │
│                                                                                 │
│  ┌─────────────────────────────────────────────────────────────────────────────┐│
│  │  Undo Record Version Chain                                                 ││
│  │  rec_current → undo_rec_1 → undo_rec_2 → ... → oldest_purged              ││
│  └─────────────────────────────────────────────────────────────────────────────┘│
└─────────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 架构模式: Clean Architecture + DDD

采用 **Clean Architecture** 混合 **DDD** 架构模式:

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Domain Layer (领域层) — 不依赖任何外层                                  │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  FlashbackReadView (领域实体)                                   │    │
│  │  • target_timestamp: 目标时间点                                  │    │
│  │  • trx_id_bound: 对应的事务号边界                               │    │
│  │  • registered_at: 注册时间                                      │    │
│  │  • status: ACTIVE / EXPIRED / CANCELLED                        │    │
│  │                                                               │    │
│  │  PurgeHoldRequest (领域实体)                                   │    │
│  │  • hold_until: 保留截止时间                                     │    │
│  │  • tables: 涉及的表集合                                         │    │
│  │  • priority: 调度优先级                                        │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↑
                          依赖方向 (单向)
                                    │
┌─────────────────────────────────────────────────────────────────────────┐
│  Application Layer (应用层) — 编排领域服务                               │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  FlashbackQueryService                                          │    │
│  │  • build_version_at_timestamp()                                │    │
│  │  • validate_window_availability()                              │    │
│  │                                                               │    │
│  │  PurgeControlService                                           │    │
│  │  • register_flashback_hold()                                   │    │
│  │  • calculate_purge_throttle()                                  │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
                                    ↑
                                    │
┌─────────────────────────────────────────────────────────────────────────┐
│  Infrastructure Layer (基础设施层) — 实现领域接口                         │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐    │
│  │  InnoDBReadViewAdapter (ReadView 适配)                         │    │
│  │  • clone_oldest_view() → 复用现有 MVCC 基础设施               │    │
│  │                                                               │    │
│  │  UndoVersionChainAdapter (版本链适配)                          │    │
│  │  • row_vers_find_matching() 遍历                               │    │
│  │                                                               │    │
│  │  PurgeSchedulerAdapter (Purge 调度适配)                        │    │
│  │  • trx_purge_stop() / trx_purge_run()                         │    │
│  └─────────────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────────────┘
```

### 1.3 核心设计模式

| 模式 | 应用位置 | 说明 |
|------|---------|------|
| **策略模式** | `IFlashbackViewStrategy` | ReadView 扩展 vs 原生 ReadView 可切换 |
| **装饰器模式** | `PurgeThrottleDecorator` | 为 Purge 添加时间维度延迟 |
| **观察者模式** | `PurgeHoldObserver` | 监听 Flashback 生命周期，动态调整 Purge |
| **工厂模式** | `FlashbackViewFactory` | 创建不同类型的 ReadView |
| **RAII** | `FlashbackPurgeGuard` | 已实现: 构造暂停 Purge，析构恢复 |

---

## 2. 组件分解

### 2.1 模块总览

| 模块名 | 目录 | 职责 | 依赖 |
|--------|------|------|------|
| `flashback_view_manager` | storage/innobase/flashback/ | 多视图生命周期管理 | read0read.h |
| `purge_hold_scheduler` | storage/innobase/flashback/ | 时间维度 Purge 延迟调度 | trx0purge.h |
| `undo_timestamp_mapper` | storage/innobase/trx/ | 时间戳 ↔ trx_id 映射 | trx0sys.h |
| `version_chain_guard` | storage/innobase/row/ | 版本链断裂检测 | row0vers.h |
| `flashback_space_monitor` | storage/innobase/flashback/ | Undo Tablespace 空间监控 | fil0fil.h |
| `flashback_ddl_barrier` | sql/ | DDL 边界检测 | DD, MDL |

### 2.2 接口定义

#### 2.2.1 FlashbackViewManager (H1 核心)

```cpp
// storage/innobase/include/flashback_view_manager.h

/**
 * Flashback ReadView 管理器
 * 
 * 职责:
 * 1. 管理多个并发的 Flashback ReadView
 * 2. 为每个 Flashback 查询分配独立的 ReadView
 * 3. 跟踪最老的活跃 ReadView，用于 Purge 控制
 * 4. 自动过期超时的 ReadView
 * 
 * 设计原则:
 * - 线程安全: 所有公共方法加锁
 * - 无锁读取: 统计查询使用原子操作
 * - 惰性清理: ReadView 过期不立即释放，批量回收
 */
class FlashbackViewManager {
public:
    /** 单个 Flashback ReadView */
    struct FlashbackView {
        trx_id_t  view_low_limit_id;    /* ReadView 下限 (≤ 此 trx_id 可见) */
        my_time_t target_timestamp;      /* 目标时间戳 */
        trx_id_t  target_trx_id;        /* 估算的目标事务 ID */
        int64_t   registered_at;         /* 注册时间 (epoch ms) */
        int64_t   expires_at;            /* 过期时间 (epoch ms) */
        bool      is_active;             /* 是否活跃 */
        
        /* 唯一标识符 (用于查找和删除) */
        uint64_t  view_id;
    };

    /** 构造函数
     * @param max_views 最大并发视图数 (默认 256)
     * @param default_ttl_ms 默认 TTL (毫秒, 默认 5 分钟) */
    explicit FlashbackViewManager(size_t max_views = 256, 
                                  int64_t default_ttl_ms = 300000);
    
    ~FlashbackViewManager();

    /** 禁止拷贝/移动 */
    FlashbackViewManager(const FlashbackViewManager&) = delete;
    FlashbackViewManager& operator=(const FlashbackViewManager&) = delete;

    /** 注册新的 Flashback ReadView
     * 
     * @param[in] target_timestamp 目标时间戳
     * @param[in] estimated_trx_id 估算的目标事务 ID (可选, 为 0 时自动估算)
     * @return 注册的视图 ID, 0 表示失败
     * 
     * @retval > 0 成功, 返回视图 ID
     * @retval 0 失败 (已达最大视图数) */
    [[nodiscard]] uint64_t register_view(my_time_t target_timestamp,
                                         trx_id_t estimated_trx_id = 0);

    /** 注销 Flashback ReadView
     * 
     * @param view_id 要注销的视图 ID
     * @return true 成功, false 视图不存在 */
    bool unregister_view(uint64_t view_id);

    /** 获取最老的活跃 ReadView 的下限事务 ID
     * 
     * @return 最老活跃视图的 view_low_limit_id, 0 表示无活跃视图
     * 
     * 注意: 此函数被 Purge 调度器调用，用于确定 Purge 的安全边界 */
    trx_id_t get_oldest_active_trx_id() const;

    /** 检查目标时间是否在闪回窗口内
     * 
     * @param target_timestamp 目标时间戳
     * @return true 在窗口内, false 已超出窗口 */
    bool is_within_window(my_time_t target_timestamp) const;

    /** 获取当前活跃视图数 */
    size_t active_view_count() const;

    /** 获取最老可用 Undo 的时间戳
     * 
     * @return 最老可用时间戳 (unixtime), 0 表示无记录 */
    my_time_t get_oldest_undo_timestamp() const;

    /** 清理过期视图 (批量回收) */
    void cleanup_expired_views();

private:
    /** 估算目标时间戳对应的 trx_id */
    trx_id_t estimate_trx_id_at_timestamp(my_time_t timestamp);

    /** 内部: 查找视图 */
    FlashbackView* find_view_internal(uint64_t view_id);

    /** 内部: 更新最老视图追踪 */
    void update_oldest_view_tracking();

    mutable ib_mutex_t m_mutex;           /* 保护所有成员 */
    size_t m_max_views;
    int64_t m_default_ttl_ms;
    
    std::unordered_map<uint64_t, FlashbackView> m_views;  /* view_id → view */
    uint64_t m_next_view_id;
    
    /* 统计信息 (原子) */
    std::atomic<uint64_t> m_total_registered{0};
    std::atomic<uint64_t> m_total_expired{0};
    
    /* 最老视图追踪 (优化查询) */
    uint64_t m_oldest_view_id;
    my_time_t m_oldest_view_timestamp;
};
```

#### 2.2.2 PurgeHoldScheduler (H5 核心)

```cpp
// storage/innobase/include/purge_hold_scheduler.h

/**
 * Purge 延迟调度器
 * 
 * 职责:
 * 1. 维护时间维度的 Purge Hold 队列
 * 2. 根据 hold 列表动态调整 Purge 延迟
 * 3. 防止 Undo Tablespace 膨胀
 * 4. 与 FlashbackViewManager 协同工作
 * 
 * 核心算法:
 * - 计算 purge_lag = oldest_hold_timestamp - now
 * - 如果 purge_lag > 0，应用时间维度延迟
 * - 同时考虑 record count lag (现有机制)
 */
class PurgeHoldScheduler {
public:
    /** Hold 请求优先级 */
    enum class Priority : int {
        LOW = 0,      /* 常规延迟 */
        NORMAL = 1,   /* 默认优先级 */
        HIGH = 2,     /* 高优先级 (金融场景) */
        CRITICAL = 3  /* 关键操作 (正在进行的闪回) */
    };

    /** Hold 请求结构 */
    struct HoldRequest {
        uint64_t   request_id;         /* 唯一标识 */
        my_time_t  hold_until;         /* 保留截止时间 */
        Priority   priority;           /* 优先级 */
        int64_t    created_at;         /* 创建时间 (epoch ms) */
        bool       is_flashback;       /* 是否为闪回操作 */
        
        /* 关联的表 (可选, 用于细粒度控制) */
        std::vector<table_id_t> affected_tables;
    };

    /** 调度结果 */
    struct ScheduleResult {
        /** 估算的 Purge 延迟 (微秒) */
        uint64_t estimated_delay_us;
        
        /** 是否应暂停 Purge */
        bool should_pause;
        
        /** 最老的 Hold 截止时间 */
        my_time_t oldest_hold_until;
        
        /** 当前 Hold 请求数 */
        size_t active_holds;
    };

    /** 构造函数 */
    PurgeHoldScheduler();
    
    ~PurgeHoldScheduler();

    /** 添加 Hold 请求
     * 
     * @param[in] hold_until 保留截止时间
     * @param[in] priority 优先级
     * @param[in] is_flashback 是否为闪回操作
     * @param[in] affected_tables 受影响的表 (可选)
     * @return 请求 ID, 0 表示失败 */
    [[nodiscard]] uint64_t add_hold(my_time_t hold_until,
                                    Priority priority = Priority::NORMAL,
                                    bool is_flashback = false,
                                    const std::vector<table_id_t>& affected_tables = {});

    /** 移除 Hold 请求
     * 
     * @param request_id 请求 ID
     * @return true 成功, false 请求不存在 */
    bool remove_hold(uint64_t request_id);

    /** 查询调度决策 (Purge 线程调用)
     * 
     * 此函数在每次 Purge 批次执行前调用，返回是否应延迟或暂停。
     * 
     * @param[in] batch_size 当前批次大小
     * @return ScheduleResult 调度结果
     * 
     * @retval estimated_delay_us > 0 表示应延迟 (微秒)
     * @retval should_pause = true 表示应完全暂停 */
    ScheduleResult query_schedule(uint64_t batch_size) const;

    /** 检查特定表的 Hold 状态
     * 
     * @param table_id 表 ID
     * @return true 表被 Hold, false 表未被 Hold */
    bool is_table_held(table_id_t table_id) const;

    /** 获取当前空间压力等级
     * 
     * @return 0.0-1.0, 1.0 表示空间不足
     * 
     * 0.0-0.5: 正常
     * 0.5-0.8: 警告, 减少 hold 时间
     * 0.8-1.0: 危险, 优先清理 */
    double get_space_pressure() const;

    /** 设置空间压力等级 (由空间监控调用) */
    void set_space_pressure(double pressure);

    /** 获取活跃 Hold 数 */
    size_t active_hold_count() const;

    /** 获取最老的 Hold 截止时间 */
    my_time_t get_oldest_hold_until() const;

private:
    /** 内部: 重新排序优先级队列 */
    void rebuild_priority_queue();

    /** 内部: 计算基于优先级的延迟系数 */
    double calculate_priority_multiplier(Priority priority) const;

    /** 内部: 检查是否应触发紧急清理 */
    bool should_trigger_emergency_cleanup() const;

    mutable ib_mutex_t m_mutex;
    
    /* Hold 请求存储 (按 request_id 索引) */
    std::unordered_map<uint64_t, HoldRequest> m_holds;
    
    /* 优先级队列 (按 hold_until 排序, 最早优先) */
    std::priority_queue<uint64_t, std::vector<uint64_t>, 
                        HoldComparator> m_priority_queue;
    
    /* 空间压力等级 */
    double m_space_pressure;
    
    /* 统计 */
    std::atomic<uint64_t> m_total_adds{0};
    std::atomic<uint64_t> m_total_removes{0};
};
```

#### 2.2.3 UndoTimestampMapper

```cpp
// storage/innobase/include/undo_timestamp_mapper.h

/**
 * Undo 时间戳映射器
 * 
 * 职责:
 * 1. 维护 trx_id ↔ timestamp 的近似映射
 * 2. 支持按时间戳查找对应的事务边界
 * 3. 估算给定时间戳的 undo 是否已过期
 * 
 * 实现策略:
 * - 维护一个固定大小的 LRU cache: trx_id → commit_timestamp
 * - 批量插入: 事务提交时更新缓存
 * - 近似查询: 使用二分查找找最近的 trx_id
 */
class UndoTimestampMapper {
public:
    /** 单条映射记录 */
    struct MappingEntry {
        trx_id_t  trx_id;
        my_time_t commit_timestamp;
        int64_t   inserted_at;  /* 用于 LRU 淘汰 */
    };

    /** 查询结果 */
    struct QueryResult {
        trx_id_t  trx_id;           /* 估算的事务 ID */
        my_time_t timestamp;         /* 对应的时间戳 */
        bool      is_exact;         /* 是否精确匹配 */
        my_time_t oldest_undo_ts;   /* 最老可用时间戳 */
        bool      is_within_window; /* 是否在闪回窗口内 */
    };

    /** 构造函数
     * @param cache_size 缓存大小 (默认 10000) */
    explicit UndoTimestampMapper(size_t cache_size = 10000);
    
    ~UndoTimestampMapper();

    /** 注册事务提交时间 (事务提交时调用)
     * 
     * @param trx_id 事务 ID
     * @param commit_timestamp 提交时间戳 */
    void register_commit(trx_id_t trx_id, my_time_t commit_timestamp);

    /** 查询目标时间戳对应的 trx_id 边界
     * 
     * @param target_timestamp 目标时间戳
     * @return QueryResult 查询结果
     * 
     * 算法:
     * 1. 在缓存中二分查找 >= target_timestamp 的最小记录
     * 2. 返回该记录的 trx_id 作为上界 (ReadView::low_limit_id)
     * 3. 返回前一记录的 trx_id 作为下界 (ReadView::high_limit_id) */
    QueryResult query_trx_id_at_timestamp(my_time_t target_timestamp) const;

    /** 获取最老的可用 Undo 时间戳
     * 
     * @return 最老可用时间戳, 0 表示无记录 */
    my_time_t get_oldest_available_timestamp() const;

    /** 检查目标时间是否在窗口内
     * 
     * @param target_timestamp 目标时间戳
     * @return true 在窗口内 */
    bool is_timestamp_available(my_time_t target_timestamp) const;

    /** 批量注册 (用于启动时加载) */
    void register_commits_batch(const std::vector<MappingEntry>& entries);

    /** 获取缓存统计 */
    struct CacheStats {
        size_t cache_size;
        size_t hit_count;
        size_t miss_count;
        double hit_rate;
        my_time_t oldest_entry;
        my_time_t newest_entry;
    };
    CacheStats get_cache_stats() const;

private:
    /** 内部: 二分查找目标时间戳 */
    int binary_search(my_time_t target) const;

    /** 内部: 淘汰最老条目 */
    void evict_oldest();

    mutable ib_mutex_t m_mutex;
    
    /* 有序映射 (按 trx_id 升序) */
    std::map<trx_id_t, MappingEntry> m_mapping;
    
    /* LRU 淘汰队列 */
    std::list<uint64_t> m_lru_list;
    std::unordered_map<trx_id_t, std::list<uint64_t>::iterator> m_lru_index;
    
    size_t m_max_cache_size;
    
    /* 统计 */
    mutable std::atomic<size_t> m_hits{0};
    mutable std::atomic<size_t> m_misses{0};
};
```

#### 2.2.4 VersionChainGuard

```cpp
// storage/innobase/include/version_chain_guard.h

/**
 * 版本链完整性保护器
 * 
 * 职责:
 * 1. 检测版本链是否完整
 * 2. 防止在遍历时版本链被 Purge 打断
 * 3. 提供版本链断裂的优雅处理
 * 
 * 核心问题:
 * - 版本链是单向链表: rec → undo_rec_1 → undo_rec_2 → ...
 * - 如果中间某个 undo page 被 purge，链条断裂
 * - 解决方案: 在断裂处返回明确错误，而非静默失败
 */
class VersionChainGuard {
public:
    /** 版本链状态 */
    enum class ChainStatus {
        INTACT,           /* 版本链完整 */
        TRUNCATED,        /* 头部已被 truncate */
        BROKEN,           /* 中间断裂 */
        PURGED,           /* 目标版本已被 purge */
        UNKNOWN           /* 状态未知 */
    };

    /** 遍历上下文 (每次遍历创建) */
    class TraversalContext {
    public:
        TraversalContext(table_id_t table_id, trx_id_t target_trx_id);
        ~TraversalContext();
        
        /** 记录已访问的 undo record */
        void record_visit(space_id_t space, page_no_t page, offset_t offset);
        
        /** 检查特定位置是否已被 purge
         * @return true 如果已被 purge */
        bool is_purged(space_id_t space, page_no_t page, offset_t offset) const;
        
        /** 获取已访问节点数 */
        size_t visited_count() const { return m_visited.size(); }
        
        /** 标记目标版本不可达 */
        void mark_unreachable() { m_reachable = false; }
        
        bool is_reachable() const { return m_reachable; }
        
    private:
        table_id_t m_table_id;
        trx_id_t m_target_trx_id;
        int64_t m_start_time;
        
        /* 已访问的 undo record 位置集合 */
        std::unordered_set<uint64_t> m_visited;
        
        bool m_reachable;
    };

    /** 版本链检测结果 */
    struct ChainCheckResult {
        ChainStatus status;
        my_time_t oldest_available_ts;   /* 最老可用时间戳 */
        trx_id_t oldest_available_trx_id; /* 最老可用事务 ID */
        size_t estimated_missing_versions; /* 估计缺失的版本数 */
        std::string diagnostic_message;   /* 诊断消息 */
    };

    /** 构造函数
     * @param view_manager FlashbackViewManager 引用 (用于获取活跃视图) */
    explicit VersionChainGuard(FlashbackViewManager& view_manager);
    
    ~VersionChainGuard();

    /** 创建新的遍历上下文
     * 
     * @param table_id 表 ID
     * @param target_trx_id 目标事务 ID
     * @return TraversalContext 智能指针 */
    std::unique_ptr<TraversalContext> create_traversal(table_id_t table_id,
                                                        trx_id_t target_trx_id);

    /** 检查给定记录的版本链是否可访问
     * 
     * @param rec 当前记录
     * @param index 索引
     * @param target_trx_id 目标事务 ID
     * @return ChainCheckResult 检查结果 */
    ChainCheckResult check_chain(const rec_t* rec, 
                                  dict_index_t* index,
                                  trx_id_t target_trx_id);

    /** 估算版本链深度
     * 
     * @param rec 当前记录
     * @param index 索引
     * @param max_depth 最大探测深度
     * @return 估算的版本深度, -1 表示无法估算 */
    int estimate_chain_depth(const rec_t* rec, 
                             dict_index_t* index,
                             int max_depth = 100);

    /** 注册版本链引用 (遍历开始时调用)
     * 
     * 防止 Purge 线程回收这些 undo record
     * 
     * @param context 遍历上下文 */
    void register_references(TraversalContext& context);

    /** 注销版本链引用 (遍历结束时调用)
     * 
     * @param context 遍历上下文 */
    void unregister_references(TraversalContext& context);

private:
    FlashbackViewManager& m_view_manager;
    mutable ib_mutex_t m_mutex;
    
    /* 当前被引用的位置集合 */
    std::unordered_set<uint64_t> m_active_references;
    
    /* 统计 */
    std::atomic<uint64_t> m_total_checks{0};
    std::atomic<uint64_t> m_total_broken{0};
};
```

#### 2.2.5 FlashbackSpaceMonitor

```cpp
// storage/innobase/include/flashback_space_monitor.h

/**
 * Undo Tablespace 空间监控器
 * 
 * 职责:
 * 1. 监控所有 Undo Tablespace 的使用率
 * 2. 预测空间耗尽时间
 * 3. 触发空间不足告警
 * 4. 协同 PurgeHoldScheduler 调整保留策略
 * 
 * 监控指标:
 * - 每个 undo tablespace 的使用率
 * - 全局 undo 空间使用率
 * - 空间增长速率
 * - 预计耗尽时间
 */
class FlashbackSpaceMonitor {
public:
    /** 单个 Tablespace 的监控数据 */
    struct TablespaceStats {
        space_id_t     space_id;
        std::string    name;
        uint64_t       total_pages;      /* 总页数 */
        uint64_t       used_pages;       /* 已用页数 */
        uint64_t       free_pages;        /* 空闲页数 */
        double         usage_percent;    /* 使用率 */
        uint64_t       truncate_count;   /* 截断次数 */
        int64_t        last_truncate_at; /* 上次截断时间 */
        
        /* 动态指标 */
        double         growth_rate;       /* 页/秒 */
        int64_t        estimated_full_at; /* 预计满的时间 (epoch s) */
    };

    /** 全局统计 */
    struct GlobalStats {
        uint64_t       total_undo_pages;
        uint64_t       total_undo_size;   /* 字节 */
        double         overall_usage;
        size_t         active_tablespaces;
        double         max_usage;          /* 最大单表空间使用率 */
        
        /* 警告级别 */
        bool           warning_triggered;
        bool           critical_triggered;
    };

    /** 告警阈值配置 */
    struct AlertThresholds {
        double warning_percent;    /* 警告阈值, 默认 75% */
        double critical_percent;  /* 严重阈值, 默认 90% */
        uint64_t growth_rate_limit; /* 增长率限制 (页/秒) */
        int64_t  estimated_full_limit; /* 预计满时间限制 (秒) */
    };

    /** 构造函数 */
    FlashbackSpaceMonitor();

    ~FlashbackSpaceMonitor();

    /** 收集所有 Undo Tablespace 的统计
     * 
     * @return TablespaceStats 数组 */
    std::vector<TablespaceStats> collect_stats();

    /** 获取全局统计
     * 
     * @return GlobalStats 全局统计 */
    GlobalStats get_global_stats();

    /** 获取空间压力等级 (0.0-1.0)
     * 
     * @return 0.0 正常, 1.0 空间不足
     * 
     * 0.0-0.5: 绿色 (正常)
     * 0.5-0.75: 黄色 (警告)
     * 0.75-0.9: 橙色 (注意)
     * 0.9-1.0: 红色 (紧急) */
    double get_space_pressure() const;

    /** 检查是否需要调整保留窗口
     * 
     * @param current_retention 当前保留秒数
     * @return 建议的新保留秒数 (0 表示保持不变, -1 表示应暂停闪回) */
    int suggest_retention_adjustment(int64_t current_retention);

    /** 获取建议的 Undo Tablespace 数量 */
    size_t suggest_tablespace_count() const;

    /** 获取当前告警级别
     * 
     * @return "OK" / "WARNING" / "CRITICAL" */
    const char* get_alert_level() const;

    /** 更新监控数据 (定时任务调用)
     * 
     * @param interval_ms 采集间隔 (毫秒) */
    void update(size_t interval_ms);

    /** 记录截断事件 */
    void record_truncate(space_id_t space_id);

    /** 获取详细统计 (JSON 格式) */
    std::string get_stats_json() const;

private:
    /** 内部: 更新单个表空间统计 */
    TablespaceStats update_tablespace_stats(space_id_t space_id);

    /** 内部: 计算增长率 */
    double calculate_growth_rate(space_id_t space_id);

    mutable ib_mutex_t m_mutex;
    
    /* 最后更新时间 */
    int64_t m_last_update;
    
    /* 统计历史 (用于计算增长率) */
    std::unordered_map<space_id_t, std::vector<TablespaceStats>> m_history;
    
    /* 当前统计快照 */
    std::vector<TablespaceStats> m_current_stats;
    
    /* 告警阈值 */
    AlertThresholds m_thresholds;
    
    /* 告警状态 */
    bool m_warning_triggered;
    bool m_critical_triggered;
};
```

---

## 3. 数据流

### 3.1 闪回查询数据流 (SELECT ... AS OF TIMESTAMP)

```
Client
  │
  │ "SELECT * FROM employees AS OF TIMESTAMP '2025-07-29 10:30:00'"
  │
  ▼
┌──────────────────────────────────────────────────────────────────┐
│ 1. SQL Parser (sql_yacc.yy)                                       │
│    • 识别 AS OF TIMESTAMP 语法                                   │
│    • 设置 lex->flashback_query = true                            │
│    • 设置 lex->flashback_timestamp = 1751189400 (epoch)          │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 2. FlashbackViewManager::register_view()                        │
│    • 估算 target_timestamp → target_trx_id                       │
│    • 创建 FlashbackView 记录                                      │
│    • 返回 view_id                                                │
│    • m_views[view_id] = {target_trx_id, timestamp, ACTIVE}      │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 3. Query Optimizer & Execution                                   │
│    • 为 Flashback ReadView 构造 read_view_t                       │
│    • read_view_t.low_limit_id = target_trx_id                     │
│    • read_view_t.high_limit_id = oldest_active_trx_id            │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 4. ha_innobase::rnd_next()                                      │
│    • 检测 flashback_query 模式                                    │
│    • 调用 row_build_flashback_version()                           │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 5. row_build_flashback_version()                                 │
│    • 构造目标时间点的 ReadView                                    │
│    • 调用 row_vers_build_for_consistent_read()                    │
│    • 遍历 undo 版本链查找匹配版本                                 │
│    • 返回历史版本或 DB_MISSING_HISTORY                            │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 6. VersionChainGuard 保护                                        │
│    • 遍历开始: register_references(context)                       │
│    • 遍历中: is_purged() 检查                                     │
│    • 遍历结束: unregister_references(context)                    │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 7. 结果返回 Client                                               │
│    • 成功: 返回历史版本数据                                      │
│    • DB_MISSING_HISTORY: 返回错误 (历史不可用)                   │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 8. FlashbackViewManager::unregister_view()                      │
│    • 清理视图记录                                                │
│    • 更新 oldest_active_trx_id 追踪                               │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 FlashbackTable 数据流

```
Client
  │
  │ "FLASHBACK TABLE employees TO TIMESTAMP '2025-07-29 10:30:00'"
  │
  ▼
┌──────────────────────────────────────────────────────────────────┐
│ 1. 权限检查 & DDL Barrier                                       │
│    • 检查 FLASHBACK 权限                                         │
│    • 检查 DDLBarrier::check()                                    │
│    • 获取 MDL_EXCLUSIVE 锁                                       │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 2. PurgeHoldScheduler::add_hold()                               │
│    • 计算 hold_until = now + innodb_flashback_window             │
│    • 创建 HoldRequest                                            │
│    • m_holds[request_id] = {...}                                 │
│    • m_priority_queue.push(request_id)                           │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 3. FlashbackPurgeGuard (RAII)                                   │
│    • 构造: trx_purge_stop()                                      │
│    • 确保闪回期间 Purge 不清理 undo                              │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 4. FlashbackViewManager::register_view()                        │
│    • 注册目标时间点的 ReadView                                   │
│    • 记录到 m_views                                              │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 5. 全表扫描 & 版本构建                                           │
│    • 使用 UndoFlashbackEngine::execute_table()                   │
│    • 逐行: row_build_flashback_version()                         │
│    • 逐行: restore_row() 恢复历史状态                            │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 6. Space Monitor 协同                                           │
│    • FlashbackSpaceMonitor::get_space_pressure()                 │
│    • 若空间压力大: 减少保留窗口                                  │
│    • 触发 PurgeHoldScheduler 调整                                │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 7. 资源清理                                                     │
│    • FlashbackPurgeGuard 析构: trx_purge_run()                  │
│    • FlashbackViewManager::unregister_view()                    │
│    • PurgeHoldScheduler::remove_hold()                           │
│    • 释放 MDL 锁                                                │
└──────────────────────────────────────────────────────────────────┘
```

### 3.3 Purge 调度决策流程

```
trx_purge() 执行前
      │
      ▼
┌──────────────────────────────────────────────────────────────────┐
│ 1. PurgeHoldScheduler::query_schedule()                        │
│    • 获取 m_priority_queue 队首 (最老 hold)                      │
│    • 计算 time_based_lag = oldest_hold_until - now               │
└─────────────────────────────┬────────────────────────────────────┘
                              │
      ┌───────────────────────┼───────────────────────┐
      │                       │                       │
      ▼                       ▼                       ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐
│ time_based   │    │ space_       │    │ record_count_lag     │
│ _lag > 0 ?   │    │ pressure >   │    │ (现有机制)           │
│              │    │ threshold?   │    │                      │
└──────┬───────┘    └──────┬───────┘    └──────────┬───────────┘
       │                    │                       │
       ▼                    ▼                       ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐
│ YES: 计算    │    │ YES: 提高    │    │ 使用 srv_max_        │
│ delay_us =   │    │ delay_us =   │    │ purge_lag 控制       │
│ time_based   │    │ space_factor │    │                      │
│ _lag ×       │    │ × priority   │    │                      │
│ priority_    │    │ _multiplier  │    │                      │
│ multiplier   │    │              │    │                      │
└──────┬───────┘    └──────┬───────┘    └──────────┬───────────┘
       │                    │                       │
       └────────────────────┴───────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 2. FlashbackSpaceMonitor::get_space_pressure()                  │
│    • 若 pressure > 0.9: should_pause = true                     │
│    • 若 pressure > 0.75: 减少 hold 时间                         │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 3. FlashbackViewManager::get_oldest_active_trx_id()            │
│    • 获取最老活跃 ReadView 的 trx_id                            │
│    • 设置 Purge 不得清理 < 该 trx_id 的 undo                    │
└─────────────────────────────┬────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────────┐
│ 4. 执行 Purge (应用延迟后)                                      │
│    • 清理 < oldest_active_trx_id 的 undo                       │
│    • 同时遵守 record_count_lag 限制                             │
└──────────────────────────────────────────────────────────────────┘
```

---

## 4. 文件结构

### 4.1 目录树

```
percona-server/
│
├── storage/innobase/
│   ├── include/
│   │   ├── flashback_view_manager.h      ← ★ H1 核心: 多视图管理
│   │   ├── purge_hold_scheduler.h         ← ★ H5 核心: 时间延迟调度
│   │   ├── undo_timestamp_mapper.h        ← ★ 时间戳映射
│   │   ├── version_chain_guard.h          ← ★ 版本链保护
│   │   ├── flashback_space_monitor.h      ← ★ 空间监控
│   │   ├── flashback_purge_guard.h        ← 已有: RAII Purge 保护
│   │   ├── flashback_undo_engine.h        ← 已有: 闪回引擎
│   │   ├── row0vers.h                     ← 修改: 新增 row_build_flashback_version()
│   │   ├── trx0sys.h                      ← 修改: 新增 trx_sys_time_mapping()
│   │   └── trx0purge.h                    ← 修改: 新增 Purge 扩展点
│   │
│   ├── flashback/
│   │   ├── flashback_view_manager.cc      ← ★ H1 实现
│   │   ├── purge_hold_scheduler.cc        ← ★ H5 实现
│   │   ├── undo_timestamp_mapper.cc       ← ★ 时间戳映射实现
│   │   ├── version_chain_guard.cc         ← ★ 版本链保护实现
│   │   ├── flashback_space_monitor.cc     ← ★ 空间监控实现
│   │   ├── flashback_undo_engine.cc       ← 已有
│   │   └── flashback_purge_guard.cc      ← 已有
│   │
│   ├── row/
│   │   └── row0vers.cc                   ← 修改: row_build_flashback_version()
│   │
│   └── trx/
│       └── trx0sys.cc                    ← 修改: trx_sys_time_mapping()
│
├── sql/
│   ├── flashback/
│   │   ├── flashback_view_manager.h      ← SQL 层包装器
│   │   └── flashback_view_manager.cc     ← SQL 层包装器
│   │
│   ├── flashback_types.h                  ← 已有: 核心类型定义
│   ├── flashback_scheduler.h              ← 已有
│   └── flashback_ddl_barrier.h           ← 已有
│
├── share/
│   └── messages_to_clients.txt            ← 修改: 新增错误码
│
├── mysql-test/
│   └── suite/
│       └── flashback/
│           ├── t/
│           │   ├── flashback_view_manager.test  ← ★ H1 测试
│           │   ├── purge_hold_scheduler.test    ← ★ H5 测试
│           │   ├── undo_timestamp_mapper.test   ← ★ 时间映射测试
│           │   ├── version_chain_guard.test     ← ★ 链保护测试
│           │   └── flashback_space_monitor.test ← ★ 空间监控测试
│           └── r/
│               └── (对应 .result 文件)
│
└── CMakeLists.txt                         ← 修改: 新增源文件
```

### 4.2 命名约定

| 类别 | 约定 | 示例 |
|------|------|------|
| C++ 类名 | PascalCase, 前缀 `Flashback`/`Purge`/`Undo`/`Version` | `FlashbackViewManager` |
| C 函数名 | snake_case, 前缀 `flashback_` / `purge_hold_` / `undo_ts_` | `flashback_view_register()` |
| 内部结构体 | PascalCase, 后缀 `Stats`/`Request`/`Context` | `TablespaceStats` |
| 系统变量 | snake_case, 前缀 `innodb_flashback_` | `innodb_flashback_window_seconds` |
| 错误码 | 大写, 前缀 `ER_FLASHBACK_` | `ER_FLASHBACK_UNDO_PURGED` |
| 测试文件 | snake_case, 前缀 `flashback_` | `flashback_view_manager.test` |

---

## 5. 错误处理策略

### 5.1 错误分级

| 等级 | 分类 | 含义 | 处理策略 |
|------|------|------|---------|
| **E1** | 业务错误 | 用户输入不合法或超出能力范围 | 返回错误消息，不修改数据 |
| **E2** | 可恢复错误 | 运行时资源不足/超时/空间压力 | 回滚操作，释放锁，返回错误 |
| **E3** | 系统错误 | InnoDB 内部异常 | 终止操作，释放所有资源，严重告警 |
| **E4** | 不可恢复错误 | 数据损坏/断言失败 | 触发 server 崩溃处理 |

### 5.2 新增错误码

```sql
-- share/messages_to_clients.txt 新增

ER_FLASHBACK_VIEW_LIMIT 3820
  eng "Maximum flashback views (%lu) reached. Please wait for active "
      "queries to complete before starting new flashback operations."

ER_FLASHBACK_TIMESTAMP_UNAVAILABLE 3821
  eng "Target timestamp %s is beyond the undo log retention window. "
      "The oldest available undo timestamp is %s. "
      "Consider increasing innodb_flashback_window_seconds."

ER_FLASHBACK_PURGE_HOLD_FAILED 3822
  eng "Failed to register purge hold request. "
      "Undo tablespace may be under space pressure."

ER_FLASHBACK_VERSION_CHAIN_BROKEN 3823
  eng "Version chain is broken for row at position (%u, %u, %u). "
      "The undo record has been purged. Historical data is not available."

ER_FLASHBACK_ESTIMATE_TRX_ID_FAILED 3824
  eng "Failed to estimate transaction ID for timestamp %s. "
      "The timestamp-to-transaction mapping cache may be empty."

ER_FLASHBACK_WINDOW_EXCEEDED 3825
  eng "Flashback window limit exceeded. Current retention is %lu seconds, "
      "but requested flashback extends to %lu seconds ago. "
      "Either reduce the flashback window or increase "
      "innodb_flashback_window_seconds."

ER_FLASHBACK_DDL_BARRIER_TRIGGERED 3826
  eng "DDL operation detected on table '%s' after target timestamp %s. "
      "Flashback to this time point is not safe. "
      "Please specify an earlier timestamp."

ER_FLASHBACK_SPACE_CRITICAL 3827
  eng "Undo tablespace space is critically low (%s). "
      "Flashback operations are temporarily disabled. "
      "Please consider truncating undo tablespaces."
```

### 5.3 错误处理流程

```
错误发生
  │
  ├── E1 业务错误
  │   └─ my_error(ER_FLASHBACK_*, MYF(0))
  │       └─ return error to client
  │
  ├── E2 可恢复错误
  │   ├─ 清理已分配资源
  │   │   ├─ VersionChainGuard::unregister_references()
  │   │   ├─ FlashbackViewManager::unregister_view()
  │   │   └─ PurgeHoldScheduler::remove_hold()
  │   ├─ 恢复 Purge (若 FlashbackPurgeGuard 已析构)
  │   └─ my_error(ER_FLASHBACK_*, MYF(0))
  │
  ├── E3 系统错误
  │   ├─ 清理所有已分配资源 (同 E2)
  │   ├─ push_warning + my_error
  │   └─ log_error() 写入错误日志
  │
  └── E4 不可恢复错误
      └─ ut_error 或 ut_ad 失败 → 触发 crash
```

---

## 6. 关键约束清单

### 6.1 约束定义

| 编号 | 约束描述 | 违反后果 | 缓解措施 |
|------|---------|---------|---------|
| **C1** | Flashback ReadView 不得被 Purge 回收 | 版本链断裂，`DB_MISSING_HISTORY` | `FlashbackViewManager::get_oldest_active_trx_id()` 阻断 Purge |
| **C2** | Purge 延迟必须考虑时间维度 | 时间窗口短的闪回无法完成 | `PurgeHoldScheduler::query_schedule()` 综合时间 + 记录数 |
| **C3** | Undo Tablespace 空间不得耗尽 | 新事务无法分配 undo，`ER_ROLLBACK_NO_UNDO_LOG_SPACE` | `FlashbackSpaceMonitor::get_space_pressure()` 动态调整 |
| **C4** | trx_id ↔ timestamp 映射必须准确 | 闪回时间点不精确 | `UndoTimestampMapper` LRU 缓存 + 二分查找 |
| **C5** | 版本链遍历必须原子完成 | 部分遍历时链断裂 | `VersionChainGuard::register_references()` 引用计数 |
| **C6** | Flashback 操作必须可中断 | 长闪回阻塞业务 | 检查 `thd->killed`，支持分批执行 |
| **C7** | DDL 屏障必须在闪回前检查 | DDL 后闪回数据不一致 | `DDLBarrier::check()` 前置检查 |
| **C8** | 空间压力高时必须降级 | 闪回影响核心事务 | `FlashbackSpaceMonitor` 触发降级策略 |
| **C9** | ReadView 数量必须有上限 | 资源耗尽 | `FlashbackViewManager` 限制 `max_views` |
| **C10** | Hold 请求必须按时过期 | Hold 永久保留，undo 膨胀 | `PurgeHoldScheduler` 自动清理过期请求 |

### 6.2 约束验证矩阵

| 约束 | 验证时机 | 验证方法 | 失败动作 |
|------|---------|---------|---------|
| C1 | Purge 调度前 | `get_oldest_active_trx_id()` | 跳过该事务 |
| C2 | Purge 批次前 | `query_schedule()` | 计算延迟或暂停 |
| C3 | 闪回开始前 | `get_space_pressure()` | 拒绝或降级 |
| C4 | 视图注册时 | `query_trx_id_at_timestamp()` | 返回精确度警告 |
| C5 | 遍历开始/结束 | RAII `TraversalContext` | 自动注销 |
| C6 | 每批处理后 | `thd->killed` 检查 | 中断并清理 |
| C7 | 闪回执行前 | `DDLBarrier::check()` | 拒绝执行 |
| C8 | 空间采集周期 | `update()` → 告警 | 减少保留窗口 |
| C9 | 视图注册时 | `active_view_count() < max_views` | 拒绝注册 |
| C10 | 调度查询时 | `cleanup_expired_holds()` | 自动移除 |

---

## 7. 系统变量

### 7.1 H1 相关变量

| 变量名 | 类型 | 默认值 | 范围 | 描述 |
|--------|------|--------|------|------|
| `innodb_flashback_enabled` | BOOL | ON | ON/OFF | 闪回功能总开关 |
| `innodb_flashback_window_seconds` | ULONG | 900 | 60-86400 | 闪回时间窗口 (秒) |
| `innodb_flashback_max_views` | ULONG | 256 | 16-4096 | 最大并发 ReadView 数 |
| `innodb_flashback_view_ttl_seconds` | ULONG | 300 | 30-3600 | 单个 ReadView TTL |

### 7.2 H5 相关变量

| 变量名 | 类型 | 默认值 | 范围 | 描述 |
|--------|------|--------|------|------|
| `innodb_purge_hold_enabled` | BOOL | ON | ON/OFF | Purge Hold 调度开关 |
| `innodb_purge_hold_max_requests` | ULONG | 1024 | 16-65536 | 最大 Hold 请求数 |
| `innodb_purge_hold_time_multiplier` | DOUBLE | 1.5 | 0.1-10.0 | 时间延迟乘数 |

### 7.3 空间管理变量

| 变量名 | 类型 | 默认值 | 范围 | 描述 |
|--------|------|--------|------|------|
| `innodb_undo_space_warning_threshold` | DOUBLE | 0.75 | 0.1-0.99 | 空间警告阈值 |
| `innodb_undo_space_critical_threshold` | DOUBLE | 0.90 | 0.5-0.99 | 空间严重阈值 |
| `innodb_undo_auto_truncate` | BOOL | ON | ON/OFF | 自动截断开关 |

---

## 8. 核心算法详解

### 8.1 时间戳 → trx_id 映射算法

```
算法: binary_search_timestamp_to_trx_id(target_timestamp)

输入: target_timestamp (目标时间戳)
输出: QueryResult { trx_id, timestamp, is_exact, oldest_undo_ts, is_within_window }

1. 获取 m_mapping 的 lock (m_mutex)
2. 如果 m_mapping 为空:
   - return { TRX_ID_MAX, 0, false, 0, false }
3. 执行二分查找:
   - left = m_mapping.begin()
   - right = m_mapping.end()
   - while left < right:
     - mid = (left + right) / 2
     - if m_mapping[mid].commit_timestamp < target_timestamp:
       - left = mid + 1
     - else:
       - right = mid
4. if left == m_mapping.end():
   - // 目标时间在所有记录之后
   - return { newest_trx_id, newest_ts, true, oldest_ts, true }
5. if m_mapping[left].commit_timestamp == target_timestamp:
   - return { m_mapping[left].trx_id, timestamp, true, oldest_ts, true }
6. else:
   - // 近似匹配
   - prev = left - 1
   - if prev >= m_mapping.begin():
     - return { m_mapping[left].trx_id, timestamp, false, oldest_ts, true }
   - else:
     - return { m_mapping[left].trx_id, timestamp, false, oldest_ts, false }
```

### 8.2 Purge Hold 调度算法

```
算法: query_schedule(batch_size)

输入: batch_size (当前批次大小)
输出: ScheduleResult { estimated_delay_us, should_pause, oldest_hold_until, active_holds }

1. 获取 m_mutex
2. 获取当前时间 now = current_time()
3. 获取 active_holds = m_holds.size()
4. 获取 oldest_hold_until = m_priority_queue.top().hold_until (如果非空)
5. 获取 space_pressure = m_space_pressure

6. 计算 time_based_lag:
   - if oldest_hold_until > now:
     - time_based_lag = oldest_hold_until - now (秒)
   - else:
     - time_based_lag = 0

7. 计算 delay_us:
   - base_delay = time_based_lag * 1000000 (转换为微秒)
   - priority_factor = 计算优先级系数 (HIGH=2.0, NORMAL=1.0, LOW=0.5)
   - space_factor = 1.0 + space_pressure (0.0 时为 1.0, 0.9 时为 1.9)
   - estimated_delay_us = base_delay * priority_factor * space_factor

8. 计算 should_pause:
   - if space_pressure > 0.90:
     - should_pause = true
   - else if estimated_delay_us > MAX_DELAY_US (60000000):  // 60 秒
     - should_pause = true
   - else:
     - should_pause = false

9. return ScheduleResult { estimated_delay_us, should_pause, oldest_hold_until, active_holds }
```

### 8.3 版本链断裂检测算法

```
算法: check_chain(rec, index, target_trx_id)

输入: rec (当前记录), index (索引), target_trx_id (目标事务 ID)
输出: ChainCheckResult { status, oldest_available_ts, estimated_missing, message }

1. 获取 rec 的 trx_id 和 roll_ptr
2. 获取 oldest_active_trx_id = FlashbackViewManager::get_oldest_active_trx_id()

3. 遍历版本链:
   - current_trx_id = rec.trx_id
   - while current_trx_id > target_trx_id:
     - 获取 roll_ptr 指向的 undo record
     - if undo record 不存在 (page 已 purge):
       - return { BROKEN, oldest_ts, estimated_missing, "Undo page purged" }
     - 检查 current_trx_id > oldest_active_trx_id:
       - if true: return { TRUNCATED, oldest_ts, missing++, "Head truncated" }
     - current_trx_id = undo_record.prev_trx_id
     - 移动到上一个版本
   - if current_trx_id <= target_trx_id:
     - return { INTACT, oldest_ts, 0, "Chain intact" }

4. return { UNKNOWN, oldest_ts, 0, "Unknown state" }
```

### 8.4 Crash Recovery 后 UndoTimestampMapper 状态恢复

> **M3 补充设计**: 描述从 undo log 重建 `trx_id↔timestamp` 映射的流程
> **状态**: 增强版 — 结合 Percona Server 8.4 源码精确化

#### 8.4.1 问题背景

当 MySQL 崩溃重启后, `UndoTimestampMapper` 内存中的 `trx_id↔timestamp` 映射全部丢失。
为保证闪回功能在重启后仍可正常工作, 必须从持久化数据中重建映射。

**约束关联**: C4 (trx_id ↔ timestamp 映射必须准确)

**核心挑战**:
- Undo log header 仅存储 `trx_id` 和 `trx_no`, **不直接存储 commit_timestamp**
- 需要组合多个数据源才能恢复精确的时间戳信息
- `trx_t::commit_lsn` 仅在内存事务结构中存在, 崩溃后不可直接获取
- Insert undo log 没有 history list 节点, 需要特殊处理

#### 8.4.2 可用数据源分析

| 数据源 | 信息 | 精确度 | 可用性 |
|--------|------|--------|--------|
| Undo Log Header | `TRX_UNDO_TRX_ID` (偏移0, 8字节), `TRX_UNDO_TRX_NO` (偏移8, 8字节) | 精确 (事务ID) | ✅ 始终可用 |
| Undo Log Header GTID | `TRX_UNDO_FLAG_GTID` — 如果 set, header 中包含 GTID 信息 | 精确 (事务标识) | ✅ 若事务有 GTID |
| Binlog Gtid_log_event | `original_commit_timestamp` / `immediate_commit_timestamp` | 精确 (微秒级) | ✅ 若 binlog 开启 |
| trx_t::commit_lsn | 事务提交时的 LSN | 近似 (LSN→时间需估算) | ❌ 崩溃后内存丢失 |
| Undo History List | 按 `trx_no` 排序的已提交 update undo | 有序 (非时间) | ✅ 始终可用 |
| InnoDB Redo Log | 提交 LSN 与写入时间的间接关系 | 近似 | ✅ 始终可用 |

**关键源码常量** (`storage/innobase/include/trx0undo.h`):
```cpp
constexpr uint32_t TRX_UNDO_TRX_ID = 0;       // 事务ID, 8字节
constexpr uint32_t TRX_UNDO_TRX_NO = 8;       // 事务编号, 8字节
constexpr uint32_t TRX_UNDO_DEL_MARKS = 16;   // delete mark 标志
constexpr uint32_t TRX_UNDO_FLAGS = 20;       // 标志位 (GTID, XID 等)
constexpr uint32_t TRX_UNDO_FLAG_GTID = 0x02; // 包含 GTID 信息
```

#### 8.4.3 恢复流程总览

```
MySQL 启动 (srv_start / innobase_start_or_create_for_mysql)
  │
  ├── 1. InnoDB 常规恢复 (redo apply, rollback uncommitted)
  │     └── trx_recovery_rollback_phase() → trx_recovery_rollback_completed()
  │
  ├── 2. trx_undo_lists_init() 初始化 rollback segment
  │      └── 扫描所有 rseg, 构建 history list 和 active undo list
  │
  ├── 3. trx_purge_sys_start() 启动 Purge 系统
  │      └── purge_sys->view 创建 Purge ReadView
  │
  ├── 4. UndoTimestampMapper::recover_from_crash() ◄── 新增
  │      ├── 阶段 A: 从 binlog 提取已提交事务时间戳 (若 binlog 开启)
  │      ├── 阶段 B: 遍历 undo history list + insert undo, 提取 trx_id/trx_no
  │      ├── 阶段 C: 交叉关联 binlog 时间戳 + LSN 估算, 补全映射
  │      └── 阶段 D: 批量注册到 LRU 缓存, 标记恢复完成
  │
  └── 5. 恢复完成, 闪回功能可用, 开始接收查询
```

#### 8.4.4 恢复状态机

```
┌─────────────────────────────────────────────────────────────────┐
│                        RecoveryState                             │
│                                                                  │
│  NOT_STARTED ────────▶ RECOVERING ────────▶ COMPLETE             │
│                             │                  │                 │
│                             │                  │                 │
│                             ▼                  ▼                 │
│                        PARTIAL          DEGRADED                 │
│                     (部分条目恢复)      (仅估算, 无精确时间戳)      │
│                                                                  │
│  状态含义:                                                        │
│  - NOT_STARTED: 恢复尚未开始                                      │
│  - RECOVERING: 正在执行阶段 A-D                                    │
│  - COMPLETE: 全部条目有精确时间戳                                  │
│  - PARTIAL: 部分条目有精确时间戳, 部分为估算                       │
│  - DEGRADED: 无精确时间戳, 全部为线性估算                          │
│                                                                  │
│  查询行为:                                                        │
│  - NOT_STARTED / RECOVERING: 拒绝闪回查询, 返回                    │
│    "Flashback not ready, recovery in progress"                   │
│  - COMPLETE: 正常服务                                             │
│  - PARTIAL / DEGRADED: 允许查询但附加 WARNING                      │
│    "Flashback data may be approximate after crash recovery"       │
└─────────────────────────────────────────────────────────────────┘
```

#### 8.4.5 阶段 A: 从 Binlog 提取提交时间戳

```
函数: extract_commit_timestamps_from_binlog()

前提条件:
  - binlog 开启 (opt_log_bin = true)
  - binlog_format = ROW (推荐)

步骤:
1. 获取当前 binlog 位置 (MYSQL_BIN_LOG::get_current_log())
   - 若 crash 前 binlog 已 flush, 使用 binlog_index 定位
   - 否则从最新 binlog 文件开始扫描

2. 从最近的 binlog 文件开始反向扫描:
   - 使用 Log_event::read_log_event() 解析每个事件
   - 跳过非事务事件 (Query_event, Format_desc_event 等)
   - 对每个 Gtid_log_event:
     * 读取 original_commit_timestamp (8 字节, 微秒级)
     * 记录: binlog_ts_map[gtid.sidno, gtid.gno] = timestamp

3. 时间窗口裁剪:
   - 仅保留 timestamp >= (now - flashback_window_seconds) 的条目
   - 避免加载过期的历史事务

4. 返回 binlog_ts_map: std::unordered_map<std::string, my_time_t>
   注意: binlog 不直接包含 trx_id, 需要通过 GTID 或 Xid_event
        与 InnoDB 的 trx_id 建立关联

复杂度:
  - 时间: O(N), N = binlog 事件数 (裁剪后通常 < 100万)
  - 空间: O(M), M = 事务数
```

**关键源码位置参考**:
- `sql/binlog.cc`: `MYSQL_BIN_LOG::find_first_log()` — 定位 binlog 文件
- `sql/log_event.h`: `Gtid_log_event` — 包含 `original_commit_timestamp`
- `sql/log_event.h` L3982-3995: 构造函数中的 timestamp 参数

**binlog → trx_id 关联策略**:
```
方案 1 (精确): 通过 GTID 关联
  - 如果 undo log header 设置了 TRX_UNDO_FLAG_GTID
  - 从 header 中解析 GTID, 与 binlog Gtid_log_event 的 GTID 匹配
  - 匹配成功 → 获得精确 commit_timestamp

方案 2 (近似): 通过事务顺序关联
  - binlog 事务按提交时间排序, undo history 按 trx_no 排序
  - 假设两者事务顺序一致, 按序号一一对应
  - 精度: 取决于事务并发度和 binlog group commit 粒度
```

#### 8.4.6 阶段 B: 遍历 Undo Log 提取事务元信息

```
函数: scan_undo_logs()

输入: 无 (使用全局 trx_sys 和 purge_sys)
输出: std::vector<PendingEntry> — 按 trx_no 排序的事务元信息

数据结构:
  struct PendingEntry {
    trx_id_t  trx_id;         // 从 undo header 读取
    trx_id_t  trx_no;         // 从 undo header 读取
    my_time_t timestamp;      // 时间戳 (初始为 0, 待填充)
    bool      has_gtid;       // 是否包含 GTID 信息
    char      gtid[256];      // GTID 字符串 (若有, 用于 binlog 关联)
    bool      is_insert_undo; // true = insert undo, false = update undo
  };

步骤:

1. 遍历所有 rollback segment (trx_sys->rw_rsegs 和 trx_sys->ur_rsegs):
   for each rseg in trx_sys:
     └── rseg 类型: TRX_RSEG_TYPE (rw 读写 / ur 只读)

2. 对每个 rseg, 遍历其 history list (仅 update undo):
   a. 获取 history list 头节点 (从 rseg->last_page_no 开始)
   b. 使用 flst_read_addr() 读取每个 history list 节点
   c. 对每个节点, 读取 undo log header:
      - TRX_UNDO_TRX_ID  (偏移 0, 8 字节) → entry.trx_id
      - TRX_UNDO_TRX_NO  (偏移 8, 8 字节) → entry.trx_no
      - TRX_UNDO_FLAGS   (偏移 20, 1 字节) → 检查 TRX_UNDO_FLAG_GTID
      - 若有 GTID: 解析并存储 entry.gtid, entry.has_gtid = true
   d. 当到达 history list 尾部 (FLST_NULL) 时停止

3. 遍历 insert undo (insert undo 不在 history list 中):
   - insert undo 仍在 rseg 的 active undo list 中
   - 这些事务在 crash recovery 后已被 rollback 或已提交
   - 仅需要已提交的 insert undo (但 insert undo 通常在提交时被释放)
   - 实际上: insert undo 在事务提交后立即被释放, 不会持久化到 history
   - 结论: insert undo 对恢复无贡献, 仅扫描 update undo

4. 按 trx_no 升序排序所有 PendingEntry
   (trx_no 单调递增, 排序后即为提交时间序)

5. 返回结果

时间窗口裁剪优化:
  - 在扫描过程中, 如果 entry.trx_no 对应的 trx_id 已经超出
    flashback_window (通过估算判断), 可以提前停止扫描
  - 减少不必要的 IO 操作
```

**关键源码位置参考**:
- `trx0undo.h` L527-530: `TRX_UNDO_TRX_ID` 和 `TRX_UNDO_TRX_NO` 偏移
- `trx0rseg.h` L149: `trx_rseg_mem_create()` — rseg 内存结构
- `trx0purge.cc`: purge_sys 的 history list 遍历逻辑
- `fil0fil.h`: `flst_read_addr()` — 文件链表读取

#### 8.4.7 阶段 C: 交叉关联时间戳

```
函数: associate_timestamps(entries, binlog_ts_map)

输入:
  - entries: 阶段 B 的 PendingEntry 列表 (按 trx_no 排序)
  - binlog_ts_map: 阶段 A 的 gtid→timestamp 映射

输出: entries 被原地修改, timestamp 字段被填充

策略 (按优先级):

┌───────────────────────────────────────────────────────────────────┐
│ 优先级 1: GTID 精确匹配 (最高精度)                                 │
│                                                                   │
│ 对每个 entry with has_gtid == true:                               │
│   - 在 binlog_ts_map 中查找对应的 GTID                            │
│   - 如果找到: entry.timestamp = binlog_ts_map[gtid]               │
│   - 标记: entry.source = EXACT_BINLOG                             │
└───────────────────────────────────────────────────────────────────┘
         │ (未匹配)
         ▼
┌───────────────────────────────────────────────────────────────────┐
│ 优先级 2: 顺序映射 (中等精度)                                      │
│                                                                   │
│ 对 entry without GTID 或 binlog 中未找到:                          │
│   - 利用 binlog 事务列表和 undo 列表的顺序对应关系                  │
│   - 将 binlog 中无 GTID 的事务按顺序映射到 undo entry              │
│   - 时间戳精度: 受 binlog group commit 影响, 通常 < 100ms 误差    │
│   - 标记: entry.source = ORDERED_BINLOG                           │
└───────────────────────────────────────────────────────────────────┘
         │ (无 binlog 数据)
         ▼
┌───────────────────────────────────────────────────────────────────┐
│ 优先级 3: LSN 估算 (低精度)                                        │
│                                                                   │
│ 如果 binlog 不可用 (未开启或文件缺失):                             │
│   - 利用 trx_no 的单调性 + innodb_flashback_window_seconds         │
│   - 假设事务均匀分布在时间窗口内                                   │
│   - 线性插值:                                                     │
│       now = current_unixtime()                                    │
│       window = flashback_window_sec                               │
│       min_no = entries[0].trx_no                                  │
│       max_no = entries[last].trx_no                               │
│       span = max_no - min_no                                      │
│       if span > 0:                                                │
│         time_per_trx = window / span                              │
│         for each entry:                                           │
│           entry.timestamp = now - (max_no - entry.trx_no)         │
│                            * time_per_trx                         │
│   - 标记: entry.source = LSN_ESTIMATE                             │
└───────────────────────────────────────────────────────────────────┘
```

**时间戳来源枚举**:
```cpp
enum class TimestampSource : uint8_t {
    UNKNOWN = 0,         // 未设置
    EXACT_BINLOG = 1,    // binlog GTID 精确匹配
    ORDERED_BINLOG = 2,  // binlog 顺序映射
    LSN_ESTIMATE = 3,    // LSN 线性估算
    TRX_NO_ESTIMATE = 4, // trx_no 线性估算 (无 LSN 时的降级)
};
```

#### 8.4.8 阶段 D: 批量注册到内存映射

```
函数: UndoTimestampMapper::recover_from_crash()

调用时机: innobase_start_or_create_for_mysql() 中,
          trx_purge_sys_start() 之后, start_accepting_connections() 之前

步骤:

1. 日志: "[Flashback] Recovering UndoTimestampMapper from undo logs..."
   记录当前状态: m_state = RecoveryState::RECOVERING

2. 调用阶段 A:
   binlog_ts_map = extract_commit_timestamps_from_binlog()
   日志: "[Flashback] Binlog scan: N transactions with timestamps"

3. 调用阶段 B:
   undo_entries = scan_undo_logs()
   日志: "[Flashback] Undo scan: M entries from history list"

4. 调用阶段 C:
   associate_timestamps(undo_entries, binlog_ts_map)

5. 统计恢复质量:
   exact_count = count entries with source == EXACT_BINLOG
   ordered_count = count entries with source == ORDERED_BINLOG
   estimated_count = count entries with source == LSN_ESTIMATE

   if exact_count > 0:
       m_state = RecoveryState::COMPLETE
   else if ordered_count > 0:
       m_state = RecoveryState::PARTIAL
   else:
       m_state = RecoveryState::DEGRADED

6. 转换为 MappingEntry 并批量注册:
   vector<MappingEntry> final_entries;
   for each entry in undo_entries:
       if entry.timestamp > 0:  // 过滤无效条目
           final_entries.push_back({entry.trx_id, entry.timestamp, now()})

   register_commits_batch(final_entries)

7. 验证:
   - oldest_ts = get_oldest_available_timestamp()
   - newest_ts = get_newest_timestamp()
   - 日志: "[Flashback] Recovery complete. State: {m_state}. "
           "Loaded {N} entries, exact={exact_count}, "
           "ordered={ordered_count}, estimated={estimated_count}. "
           "Time range: {oldest_ts} ~ {newest_ts}"

8. 标记恢复完成: m_recovery_complete.store(true)

9. 如果 m_state == DEGRADED:
   - 写入错误日志: "[Flashback] WARNING: Crash recovery in degraded mode. "
                   "Timestamp accuracy is limited. Consider enabling binlog."
   - 但闪回功能仍可用 (近似模式)
```

#### 8.4.9 新增接口定义

```cpp
// storage/innobase/include/undo_timestamp_mapper.h

class UndoTimestampMapper {
public:
    // ... (现有接口保持不变, 见 §2.2.3) ...

    /** 恢复状态枚举 */
    enum class RecoveryState : uint8_t {
        NOT_STARTED = 0,   // 恢复尚未开始
        RECOVERING = 1,    // 正在恢复
        COMPLETE = 2,      // 全部条目有精确时间戳
        PARTIAL = 3,       // 部分精确, 部分估算
        DEGRADED = 4,      // 全部为估算
    };

    /** 时间戳来源 (用于查询精度判断) */
    enum class TimestampSource : uint8_t {
        UNKNOWN = 0,
        EXACT_BINLOG = 1,
        ORDERED_BINLOG = 2,
        LSN_ESTIMATE = 3,
        TRX_NO_ESTIMATE = 4,
    };

    /** Crash Recovery 后从 undo log 重建映射
     *
     * 此函数在 InnoDB 启动时调用 (srv_start 之后), 用于恢复
     * 内存中的 trx_id↔timestamp 映射。
     *
     * @param[in] binlog_enabled 是否开启 binlog
     * @param[in] flashback_window_sec 闪回窗口大小 (秒)
     * @return true 成功 (至少部分恢复), false 完全失败
     *
     * @note 此函数是阻塞操作, 应在启动阶段完成后再接受查询
     * @note 即使返回 false, 闪回功能仍可能以近似模式工作 */
    [[nodiscard]] bool recover_from_crash(bool binlog_enabled,
                                          uint64_t flashback_window_sec);

    /** 获取当前恢复状态
     *
     * @return RecoveryState 当前恢复状态 */
    RecoveryState get_recovery_state() const;

    /** 检查闪回查询是否可以安全执行
     *
     * @return true 可以执行, false 恢复尚未完成
     *
     * 如果返回 true 但状态为 PARTIAL/DEGRADED, 调用方应附加警告 */
    bool is_ready_for_flashback() const;

private:
    /** 阶段 A: 从 binlog 提取提交时间戳
     * @return GTID→timestamp 映射 */
    std::unordered_map<std::string, my_time_t>
    extract_commit_timestamps_from_binlog();

    /** 阶段 B: 遍历 undo history list 提取事务元信息
     * @return 按 trx_no 排序的 PendingEntry 列表 */
    struct PendingEntry {
        trx_id_t     trx_id;
        trx_id_t     trx_no;
        my_time_t    timestamp;       // 0 表示待填充
        bool         has_gtid;
        char         gtid[256];       // GTID 字符串
        TimestampSource source;       // 时间戳来源
    };
    std::vector<PendingEntry> scan_undo_logs();

    /** 阶段 C: 交叉关联时间戳 (原地修改 entries) */
    void associate_timestamps(std::vector<PendingEntry>& entries,
                              const std::unordered_map<std::string, my_time_t>& binlog_ts);

    /** 阶段 C-降级: 按 trx_no 线性估算时间戳 */
    void estimate_timestamps_by_order(std::vector<PendingEntry>& entries,
                                      uint64_t flashback_window_sec);

    /** 当前恢复状态 */
    std::atomic<RecoveryState> m_recovery_state{RecoveryState::NOT_STARTED};

    /** 恢复完成标志 (用于快速检查) */
    std::atomic<bool> m_recovery_complete{false};
};
```

#### 8.4.10 调用时机与集成点

```
集成位置: storage/innobase/srv/srv0start.cc
         → innobase_start_or_create_for_mysql()

修改点 (伪代码):

  // === 现有代码 ===
  trx_sys_init_at_db_start();    // 初始化事务系统
  trx_purge_sys_start();          // 启动 Purge

  // === 新增: Crash Recovery 后恢复时间映射 ===
  if (innodb_flashback_enabled) {
    ib::info(ER_IB_MSG_FLASHBACK_RECOVERY)
        << "Recovering UndoTimestampMapper from undo logs...";

    bool ok = undo_ts_mapper->recover_from_crash(
        opt_log_bin,                              // binlog 是否开启
        innodb_flashback_window_seconds           // 闪回窗口
    );

    if (!ok) {
      ib::warn(ER_IB_WARN_FLASHBACK_RECOVERY_FAILED)
          << "UndoTimestampMapper recovery failed. "
          << "Flashback queries will operate in approximate mode.";
    }

    ib::info(ER_IB_MSG_FLASHBACK_RECOVERY_DONE)
        << "UndoTimestampMapper recovery complete. "
        << "State: " << undo_ts_mapper->get_recovery_state();
  }

  // === 现有代码继续 ===
  // ... start_accepting_connections() ...
```

**集成约束**:
- 必须在 `trx_purge_sys_start()` 之后调用, 因为需要 history list 已初始化
- 必须在 `start_accepting_connections()` 之前调用, 避免用户查询读到不完整映射
- 如果恢复时间过长 (> 60s), 考虑异步模式: 先启动服务器, 后台恢复
  期间闪回查询返回 "Flashback not ready"

#### 8.4.11 性能与内存考虑

| 指标 | 预期值 | 说明 |
|------|--------|------|
| 恢复时间 (有 binlog) | < 30 秒 (100万 undo entry) | 主要耗时在 binlog 解析 |
| 恢复时间 (无 binlog) | < 5 秒 (100万 undo entry) | 仅扫描 undo, 线性估算 |
| 内存占用 | ~50 MB (10万条目) | 每个 PendingEntry 约 300 字节 |
| 启动延迟增加 | < 5% | 恢复过程在启动阶段完成 |
| 恢复期间闪回查询 | 阻塞或返回错误 | 恢复完成前不提供服务 |

**优化策略**:
1. **增量恢复**: 仅恢复最近 N 小时的事务 (由 `innodb_flashback_window_seconds` 决定),
   扫描 undo 时提前跳过过期条目
2. **异步恢复 (可选)**: 如果恢复时间 > 30 秒, 先启动服务器, 后台线程继续恢复
   - 恢复期间 `is_ready_for_flashback()` 返回 false
   - 闪回查询返回 `ER_FLASHBACK_RECOVERY_IN_PROGRESS`
3. **缓存预热**: 恢复完成后, 仅加载最近 2×flashback_window 的条目到 LRU 缓存
   避免加载过多历史数据占用内存

#### 8.4.12 降级与容错

```
降级场景矩阵:

┌──────┬─────────────────────────┬───────────────────────────────┬────────────┐
│ 场景 │ 原因                   │ 降级行为                      │ 闪回可用性 │
├──────┼─────────────────────────┼───────────────────────────────┼────────────┤
│ S1   │ Binlog 未开启           │ 使用 trx_no 线性估算           │ ⚠️ 近似   │
│ S2   │ Binlog 文件缺失/损坏    │ 部分精确 + 剩余部分估算        │ ⚠️ 部分   │
│ S3   │ Undo history 被 purge   │ 仅恢复剩余部分                  │ ⚠️ 窗口   │
│      │ 截断                   │                               │   缩小    │
│ S4   │ 恢复超时 (> 60s)       │ 异步继续, 先启动服务            │ ⏳ 延迟   │
│ S5   │ Undo page 损坏          │ 跳过损坏 page, 记录错误日志    │ ⚠️ 部分   │
│ S6   │ 完全无可用数据          │ m_state = DEGRADED            │ ❌ 不可用 │
└──────┴─────────────────────────┴───────────────────────────────┴────────────┘

容错机制:
  1. 每个阶段独立失败不影响其他阶段 (阶段 A 失败 → 跳过, 进入 B+C)
  2. undo 扫描时使用 mtr (mini-transaction) 读取 page, 如果 page 损坏则
     跳过该 page 并继续, 不会导致启动失败
  3. 最终至少提供 "近似映射" 而非完全不可用 (除非无 undo 数据)
  4. 闪回查询时检查 m_recovery_state:
     - COMPLETE: 正常服务
     - PARTIAL: 正常服务 + WARNING (仅对估算条目可能不准)
     - DEGRADED: 正常服务 + WARNING (全部为估算)
     - RECOVERING: 返回 ER_FLASHBACK_RECOVERY_IN_PROGRESS
  5. 信息持久化 (可选): 在 innodb_data_home_dir 下创建
     flashback_recovery_info.txt, 记录最后一次恢复的时间和条目数,
     供运维排查使用
```

#### 8.4.13 边界情况处理

| 边界情况 | 处理策略 |
|----------|---------|
| 正常关闭后重启 (非 crash) | undo history 已被 purge 清理, 仅保留窗口内数据, 正常恢复 |
| 首次启动 (新实例) | 无 undo history, m_state = NOT_STARTED, 闪回不可用 (预期行为) |
| binlog 和 undo 事务数不一致 | 以 undo 为准, binlog 仅作为时间戳补充来源 |
| 大事务 (跨越多个 undo page) | 仅读取 undo header (第一页), 不遍历数据页 |
| XA 事务 | undo header 含 `TRX_UNDO_FLAG_XID`, 不参与闪回 (XA 事务不产生 binlog GTID) |
| DDL 事务 | undo header 可能有特殊标记, 跳过不参与时间映射 |

---

## 9. 实现计划

### Phase 1: H1 核心基础设施 (4-6 周)

**交付物**:
- [ ] `flashback_view_manager.h/cc`: FlashbackViewManager 实现
- [ ] `undo_timestamp_mapper.h/cc`: UndoTimestampMapper 实现
- [ ] `row0vers.cc`: 新增 `row_build_flashback_version()`
- [ ] `trx0sys.cc`: 新增时间戳映射函数
- [ ] 系统变量: `innodb_flashback_enabled`, `innodb_flashback_window_seconds`
- [ ] MTR 测试: 视图注册、注销、时间映射

**验收标准**:
```sql
SELECT * FROM t AS OF TIMESTAMP NOW() - INTERVAL 5 MINUTE;
-- 在窗口内返回历史数据
-- 超出窗口返回 ER_FLASHBACK_TIMESTAMP_UNAVAILABLE
```

### Phase 2: H5 Purge 延迟调度 (4-6 周)

**交付物**:
- [ ] `purge_hold_scheduler.h/cc`: PurgeHoldScheduler 实现
- [ ] `version_chain_guard.h/cc`: VersionChainGuard 实现
- [ ] 修改 `trx0purge.cc`: 集成时间维度调度
- [ ] 系统变量: `innodb_purge_hold_enabled`, `innodb_purge_hold_max_requests`
- [ ] MTR 测试: Hold 调度、版本链保护

**验收标准**:
```sql
-- 闪回期间 Purge 被正确延迟
-- 闪回完成后 Purge 恢复正常
-- 空间压力高时自动降级
```

### Phase 3: 空间管理与监控 (2-4 周)

**交付物**:
- [ ] `flashback_space_monitor.h/cc`: FlashbackSpaceMonitor 实现
- [ ] `DDLBarrier` 增强: DDL 边界检测
- [ ] 系统变量: `innodb_undo_space_warning_threshold`
- [ ] `INFORMATION_SCHEMA.INNODB_FLASHBACK_STATUS` 表
- [ ] MTR 测试: 空间监控、告警触发

**验收标准**:
```sql
-- 空间使用率达到警告阈值时触发告警
-- 空间使用率达到严重阈值时拒绝新闪回
-- 自动截断恢复正常空间使用
```

### Phase 4: 集成与优化 (2-4 周)

**交付物**:
- [ ] 集成测试: 完整闪回流程
- [ ] 性能测试: 不同窗口大小的性能基准
- [ ] 竞态测试: Purge 与 Flashback 并发
- [ ] 文档: 架构文档、运维手册
- [ ] 性能优化: 批量操作、缓存优化

**验收标准**:
```sql
-- 闪回查询 < 100ms (1000 行表)
-- 全表闪回 < 10s (10000 行表)
-- 并发闪回不导致 undo 膨胀失控
```

---

## 10. 风险缓解

| 风险 | 等级 | 缓解方案 | 实施阶段 |
|------|------|---------|---------|
| R1: Undo 膨胀失控 | 🔴 25/25 | `FlashbackSpaceMonitor` 动态调整 + 空间告警 | Phase 3 |
| R2: ReadView 数量过多 | 🟡 16/25 | `max_views` 限制 + TTL 过期 | Phase 1 |
| R3: 时间映射不准确 | 🟡 12/25 | LRU 缓存 + 近似匹配警告 | Phase 1 |
| R4: 版本链断裂 | 🟡 15/25 | `VersionChainGuard` 引用计数 | Phase 2 |
| R5: DDL 兼容性问题 | 🟡 12/25 | `DDLBarrier` 前置检查 | Phase 3 |
| R6: 长事务阻塞 Purge | 🟡 14/25 | 分批处理 + `thd->killed` 检查 | Phase 4 |

---

## 11. 设计验收检查清单

### 11.1 模块接口完整性

- [ ] `FlashbackViewManager`: 8 个公共方法, 职责单一
- [ ] `PurgeHoldScheduler`: 6 个公共方法, 职责单一
- [ ] `UndoTimestampMapper`: 5 个公共方法, 职责单一
- [ ] `VersionChainGuard`: 5 个公共方法, 职责单一
- [ ] `FlashbackSpaceMonitor`: 6 个公共方法, 职责单一

### 11.2 依赖方向

- [ ] Domain Layer → Infrastructure Layer (单向)
- [ ] Infrastructure Layer 不依赖 Domain 实现细节
- [ ] 所有模块通过接口交互
- [ ] 无循环依赖

### 11.3 文件结构

- [ ] 所有新增文件有明确位置
- [ ] 修改文件已标注
- [ ] 测试文件目录结构完整
- [ ] CMake 构建配置已规划

### 11.4 约束覆盖

- [ ] C1: `get_oldest_active_trx_id()` → Purge 边界
- [ ] C2: `query_schedule()` → 时间 + 记录数延迟
- [ ] C3: `get_space_pressure()` → 空间压力检测
- [ ] C4: `query_trx_id_at_timestamp()` → 时间映射
- [ ] C5: `register_references()` → 引用计数
- [ ] C6: `thd->killed` → 可中断
- [ ] C7: `DDLBarrier::check()` → DDL 检查
- [ ] C8: 降级策略 → 空间压力
- [ ] C9: `max_views` 限制 → ReadView 上限
- [ ] C10: 过期清理 → Hold 过期

---

## 附录 A: 关键源码行号索引

| 函数/结构 | 文件 | 位置 | 用途 |
|-----------|------|------|------|
| `trx_purge_stop()` | `trx0purge.cc` | L2526 | 暂停 Purge |
| `trx_purge_run()` | `trx0purge.cc` | L2585 | 恢复 Purge |
| `row_vers_build_for_consistent_read()` | `row0vers.cc` | L1255 | 版本链构建 |
| `purge_sys->view` | `trx0purge.cc` | L269 | ReadView 引用 |
| `purge_iter_t` | `trx0purge.h` | L118 | Purge 迭代器 |
| `srv_max_purge_lag` | `trx0purge.cc` | L74 | 现有延迟参数 |
| `purge_queue` | `trx0purge.cc` | L80 | 优先级队列 |

## 附录 B: 与现有 Flashback 设计的关系

本设计 (DESIGN_UNDO_ARCHITECTURE.md) 是 **DESIGN.md** (MySQL Flashback 技术设计) 的 **底层基础设施**:

```
┌─────────────────────────────────────────────────────────────┐
│                    DESIGN.md (上层应用)                       │
│                                                              │
│  SQL Layer                                                   │
│  • FLASHBACK TABLE 语法                                       │
│  • AS OF TIMESTAMP 查询                                       │
│  • VERSIONS BETWEEN 查询                                      │
│  • BinlogFlashbackEngine (长窗口)                             │
└─────────────────────────────┬───────────────────────────────┘
                              │ 依赖
                              ▼
┌─────────────────────────────────────────────────────────────┐
│        DESIGN_UNDO_ARCHITECTURE.md (本文件, 底层基础设施)     │
│                                                              │
│  InnoDB Layer                                                │
│  • FlashbackViewManager (H1: 多视图管理)                     │
│  • PurgeHoldScheduler (H5: 时间延迟调度)                    │
│  • UndoTimestampMapper (时间 ↔ trx_id 映射)                  │
│  • VersionChainGuard (版本链保护)                            │
│  • FlashbackSpaceMonitor (空间监控)                          │
│                                                              │
│  依赖 InnoDB 现有组件:                                        │
│  • row_vers_build_for_consistent_read()                      │
│  • trx_purge_stop/run()                                      │
│  • ReadView 机制                                             │
└─────────────────────────────────────────────────────────────┘
```

**设计原则**: 本设计遵循 **依赖反转原则 (DIP)**:
- 上层 (Flashback SQL) 依赖下层 (Undo 架构)
- 下层不依赖上层，专注于 Undo 管理和 Purge 调度
- 同一套基础设施可被其他需要 Undo 历史的功能复用

---

*本文档基于 Percona Server (InnoDB) 源码分析生成，核心技术依据来自 `research/mysql_undo_architecture_research.md`。*
