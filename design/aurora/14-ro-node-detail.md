# Skill: RO 节点详细设计 (RO Node Detail Design)

## 1. 问题背景

RO 节点需要解决以下问题：
1. **落后于 RW**: RO 应用 Redo 存在延迟，需要体现落后的 timestamp
2. **读到更新的页**: RO 可能读到比 RW 更新的页版本
3. **Read View 一致性**: 是否需要从 RW 获取 read view

**实现语言**: C++ (计算层 RO) + Golang (存储层支持)

## 2. RO 节点架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         RO 节点架构                                          │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                         RO 计算节点                                  │   │
│  │                                                                      │   │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐     │   │
│  │  │  Query Engine   │  │  Buffer Pool    │  │  Transaction    │     │   │
│  │  │                 │  │  (Cache Only)   │  │  Manager        │     │   │
│  │  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘     │   │
│  │           │                    │                    │              │   │
│  │           │ read               │ read page          │ get view     │   │
│  │           ▼                    ▼                    ▼              │   │
│  │  ┌─────────────────────────────────────────────────────────────┐  │   │
│  │  │                      RO Coordinator                          │  │   │
│  │  │  • visible_lsn 管理                                          │  │   │
│  │  │  • Read View 生成                                            │  │   │
│  │  │  • Timestamp 计算                                            │  │   │
│  │  │  • 页版本检查                                                │  │   │
│  │  └──────────────────────────┬──────────────────────────────────┘  │   │
│  │                              │                                     │   │
│  └──────────────────────────────┼─────────────────────────────────────┘   │
│                                 │                                          │
│                                 │ gRPC/RDMA                                │
│                                 ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                          存储层                                      │   │
│  │  • Redo Stream                                                       │   │
│  │  • Page Read (with max_lsn)                                         │   │
│  │  • Timestamp 映射                                                    │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 3. 扩展 Redo 格式 (携带 Timestamp)

### 3.1 Redo Record 扩展

```cpp
// 扩展的 Redo Record 格式
struct ExtendedRedoRecord {
    // 原有字段
    uint8_t  type;
    uint32_t space_id;
    uint64_t schema_version;
    uint32_t page_no;
    bytes    body;
    
    // 扩展字段
    uint64_t lsn;                    // LSN
    uint64_t timestamp_us;           // 微秒时间戳 (事务提交时间)
    uint64_t trx_id;                 // 事务 ID
    uint32_t server_id;              // 源节点 ID
    
    // 可选字段 (用于特定场景)
    uint64_t prev_lsn;               // 同一事务的前一条 Redo
    uint64_t gtid_seqno;             // GTID 序列号
};

// Redo Block Header 扩展
struct ExtendedRedoBlockHeader {
    // 原有字段
    uint32_t magic;
    uint64_t block_no;
    uint64_t first_lsn;
    uint64_t last_lsn;
    
    // 扩展字段
    uint64_t first_timestamp;        // 块内第一条记录的时间戳
    uint64_t last_timestamp;         // 块内最后一条记录的时间戳
    uint64_t min_active_trx_id;      // RW 节点当时的最小活跃事务
};
```

### 3.2 Timestamp 到 LSN 映射

```go
// 存储层维护 timestamp -> LSN 映射
type TimestampIndex struct {
    // 稀疏索引: 每秒一个采样点
    sparseIndex map[uint64]uint64  // timestamp (秒) -> LSN
    
    // 精确查找用 Redo 块头
    blockIndex  *BTree  // timestamp -> block_no
}

func (idx *TimestampIndex) LSNAtTimestamp(ts uint64) uint64 {
    // 1. 从稀疏索引找到近似位置
    sec := ts / 1000000
    approxLSN := idx.sparseIndex[sec]
    
    // 2. 从 Redo 块精确定位
    blockNo := idx.blockIndex.Floor(ts)
    block := idx.readBlock(blockNo)
    
    // 3. 扫描块内记录
    for _, rec := range block.Records {
        if rec.timestamp_us >= ts {
            return rec.lsn
        }
    }
    
    return block.Header.LastLSN
}
```

## 4. Visible LSN 管理

### 4.1 RO 节点的可见性控制

```cpp
// RO 节点的可见性管理器
class ROVisibilityManager {
public:
    // 获取当前可见 LSN
    uint64_t get_visible_lsn() {
        return visible_lsn_.load();
    }
    
    // 获取可见时间戳
    uint64_t get_visible_timestamp() {
        return visible_timestamp_.load();
    }
    
    // 推进可见性 (由 Redo 应用线程调用)
    void advance_visibility(uint64_t new_lsn, uint64_t new_ts) {
        visible_lsn_.store(new_lsn);
        visible_timestamp_.store(new_ts);
        
        // 通知等待的查询
        notify_waiters();
    }
    
    // 等待直到可见 (用于一致性读)
    bool wait_until_visible(uint64_t required_lsn, 
                            uint64_t timeout_ms) {
        auto deadline = now() + timeout_ms;
        while (visible_lsn_.load() < required_lsn) {
            if (now() >= deadline) {
                return false;  // 超时
            }
            wait(deadline - now());
        }
        return true;
    }
    
private:
    atomic<uint64_t> visible_lsn_;
    atomic<uint64_t> visible_timestamp_;
    
    // RO 落后指标
    atomic<uint64_t> rw_current_lsn_;       // RW 当前 LSN
    atomic<uint64_t> rw_current_timestamp_; // RW 当前时间戳
};
```

### 4.2 RO 落后时间计算

```cpp
// 计算 RO 落后 RW 多少时间
class ROLagCalculator {
public:
    // 获取落后时间 (秒)
    double get_lag_seconds() {
        uint64_t rw_ts = visibility_mgr_->get_rw_timestamp();
        uint64_t ro_ts = visibility_mgr_->get_visible_timestamp();
        
        return (rw_ts - ro_ts) / 1000000.0;  // 微秒转秒
    }
    
    // 获取落后 LSN
    uint64_t get_lag_lsn() {
        return visibility_mgr_->get_rw_lsn() - 
               visibility_mgr_->get_visible_lsn();
    }
    
    // 是否可接受的延迟
    bool is_acceptable_lag(double max_lag_seconds) {
        return get_lag_seconds() <= max_lag_seconds;
    }
};

// RW 定期广播状态
void RWNode::broadcast_status() {
    BroadcastStatusRequest req;
    req.set_current_lsn(current_lsn_);
    req.set_current_timestamp(current_timestamp_us_);
    req.set_min_active_trx_id(trx_sys_->min_active_id());
    
    // 广播给所有 RO
    for (auto& ro : ro_nodes_) {
        ro->UpdateRWStatus(req);
    }
}
```

## 5. 解决读到更新页的问题

### 5.1 问题场景

```
时间线:
────────────────────────────────────────────────────────────────────►
  RW: 写入页 P (LSN=100)
       ↓
  存储层: 持久化页 P (LSN=100)
       ↓
  RO: 读取页 P... 但此时 RO 的 visible_lsn = 50
       ↓
  问题: RO 读到了 LSN=100 的页，但 RO 的视图应该是 LSN=50 的状态
```

### 5.2 解决方案：读取时指定最大 LSN

```cpp
// RO 节点读取页时，指定允许的最大 LSN
buf_block_t* RONode::read_page(page_id_t page_id) {
    // 获取当前 visible_lsn
    uint64_t max_lsn = visibility_mgr_->get_visible_lsn();
    
    // 从存储层读取，限制最大 LSN
    ReadPageRequest req;
    req.set_space_id(page_id.space());
    req.set_page_no(page_id.page_no());
    req.set_max_lsn(max_lsn);  // 关键：限制最大版本
    
    auto resp = storage_client_->ReadPage(req);
    
    // 验证返回的页版本
    if (resp.page_lsn() > max_lsn) {
        // 存储层返回了更新的版本，需要回退
        // 使用 Redo 重建旧版本
        return rebuild_page_at_lsn(page_id, max_lsn);
    }
    
    return create_block(resp.page_data(), resp.page_lsn());
}
```

### 5.3 存储层支持：读取指定 LSN 版本

```go
// 存储层支持读取历史版本
func (s *PageStore) ReadPageAtLSN(key PageKey, maxLSN uint64) (*Page, error) {
    // 1. 获取当前页
    currentPage := s.readCurrentPage(key)
    
    if currentPage.LSN <= maxLSN {
        // 当前页满足要求
        return currentPage, nil
    }
    
    // 2. 需要重建旧版本
    // 找到 maxLSN 之前的基础页
    basePage, baseLSN := s.findBasePageBefore(key, maxLSN)
    
    // 3. 应用 [baseLSN, maxLSN] 范围的 Redo
    redos := s.redoStore.GetRange(key, baseLSN, maxLSN)
    page := basePage.Clone()
    for _, redo := range redos {
        if redo.LSN > maxLSN {
            break
        }
        applyRedo(page, redo)
    }
    
    return page, nil
}
```

## 6. Read View 管理

### 6.1 RO 节点的 Read View

```cpp
// RO 节点的 Read View 不需要从 RW 获取
// 而是基于 visible_lsn 对应的事务状态

class ROReadView {
public:
    // 创建 Read View (基于 visible_lsn)
    static ROReadView* create(ROVisibilityManager* vis_mgr) {
        ROReadView* view = new ROReadView();
        
        // 使用 visible_lsn 对应时刻的事务状态
        view->visible_lsn_ = vis_mgr->get_visible_lsn();
        view->visible_timestamp_ = vis_mgr->get_visible_timestamp();
        
        // 从存储层获取该 LSN 对应的活跃事务列表
        view->active_trx_ids_ = vis_mgr->get_active_trx_at_lsn(
            view->visible_lsn_);
        
        view->low_limit_id_ = vis_mgr->get_min_active_trx_at_lsn(
            view->visible_lsn_);
        view->up_limit_id_ = vis_mgr->get_max_trx_id_at_lsn(
            view->visible_lsn_);
        
        return view;
    }
    
    // 判断事务是否可见
    bool is_visible(trx_id_t trx_id) {
        // 1. 小于 low_limit，已提交，可见
        if (trx_id < low_limit_id_) {
            return true;
        }
        
        // 2. 大于等于 up_limit，未提交，不可见
        if (trx_id >= up_limit_id_) {
            return false;
        }
        
        // 3. 在活跃列表中，不可见
        if (active_trx_ids_.contains(trx_id)) {
            return false;
        }
        
        return true;
    }
    
private:
    uint64_t visible_lsn_;
    uint64_t visible_timestamp_;
    uint64_t low_limit_id_;
    uint64_t up_limit_id_;
    set<trx_id_t> active_trx_ids_;
};
```

### 6.2 活跃事务列表的传播

```cpp
// 扩展 Redo：携带事务状态快照
struct TransactionStateRedo {
    uint8_t  type = MLOG_TRX_STATE;
    uint64_t lsn;
    uint64_t timestamp;
    
    uint64_t min_active_trx_id;
    uint64_t max_trx_id;
    vector<uint64_t> active_trx_ids;  // 当前活跃事务列表
};

// RW 定期写入事务状态 Redo (如每 100ms)
void RWNode::write_trx_state_redo() {
    TransactionStateRedo redo;
    redo.lsn = current_lsn_;
    redo.timestamp = current_timestamp_;
    redo.min_active_trx_id = trx_sys_->min_active_id();
    redo.max_trx_id = trx_sys_->max_trx_id();
    redo.active_trx_ids = trx_sys_->get_active_ids();
    
    write_redo(redo);
}

// RO 节点维护事务状态历史
class ROTransactionStateHistory {
    // LSN -> 事务状态
    map<uint64_t, TransactionState> history_;
    
    TransactionState get_state_at_lsn(uint64_t lsn) {
        auto it = history_.upper_bound(lsn);
        if (it == history_.begin()) {
            return TransactionState{};
        }
        --it;
        return it->second;
    }
};
```

## 7. 一致性读模式

### 7.1 Session 一致性

```cpp
// Session 级别：保证读到自己的写
class SessionConsistency {
    uint64_t last_write_lsn_ = 0;
    
    void on_write_complete(uint64_t lsn) {
        last_write_lsn_ = max(last_write_lsn_, lsn);
    }
    
    uint64_t get_min_visible_lsn() {
        // 至少要读到自己最后写入的 LSN
        return last_write_lsn_;
    }
};
```

### 7.2 Global 一致性

```cpp
// 全局一致性：等待 RO 追上 RW
uint64_t RONode::wait_for_global_consistency(uint64_t timeout_ms) {
    // 从 RW 获取当前 LSN
    uint64_t rw_lsn = get_rw_current_lsn();
    
    // 等待 visible_lsn 追上
    if (!visibility_mgr_->wait_until_visible(rw_lsn, timeout_ms)) {
        throw TimeoutException("RO lag too high");
    }
    
    return rw_lsn;
}
```

### 7.3 Timestamp 一致性

```cpp
// 指定时间戳读取
uint64_t RONode::read_at_timestamp(uint64_t timestamp_us) {
    // 转换为 LSN
    uint64_t lsn = timestamp_index_->lsn_at_timestamp(timestamp_us);
    
    // 等待达到该 LSN
    visibility_mgr_->wait_until_visible(lsn, DEFAULT_TIMEOUT);
    
    return lsn;
}
```

## 8. 配置

```cpp
struct RONodeConfig {
    // 一致性
    ConsistencyLevel default_consistency = EVENTUAL;
    uint64_t max_acceptable_lag_ms = 100;
    
    // 等待
    uint64_t visibility_wait_timeout_ms = 5000;
    
    // 事务状态
    uint64_t trx_state_redo_interval_ms = 100;
    uint64_t trx_state_history_size = 10000;
};
```

## 9. 开发任务

- [ ] 扩展 Redo 格式 (timestamp, trx_id)
- [ ] 实现 Timestamp Index
- [ ] 实现 ROVisibilityManager
- [ ] 实现指定 max_lsn 的页读取
- [ ] 实现 ROReadView
- [ ] 实现事务状态 Redo
- [ ] 实现一致性读模式
- [ ] 集成测试

## 10. 参考

- Skill 08: RO 节点一致性
- Aurora 论文: Replica 部分
