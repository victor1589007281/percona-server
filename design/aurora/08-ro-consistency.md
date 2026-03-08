# Skill: RO 节点一致性 (RO Consistency)

## 1. 模块职责

保证 RO 节点的读取一致性：
- **Schema Snapshot**: 查询看到一致的 Schema 视图
- **可见性控制**: 控制查询可见的数据 LSN
- **Redo 同步**: RO 节点跟踪 RW 的 Redo 进度

**实现语言**: Golang

## 2. 一致性模型

### 2.1 读一致性级别

```
┌─────────────────────────────────────────────────────────────────┐
│                    RO 读一致性级别                               │
│                                                                 │
│  EVENTUAL (最终一致)                                            │
│  ─────────────────                                              │
│  • RO 可能读到旧数据                                           │
│  • 最低延迟                                                     │
│  • 适合: 容忍延迟的分析查询                                    │
│                                                                 │
│  SESSION (会话一致)                                             │
│  ────────────────                                               │
│  • 同一会话内，读到自己写的数据                                 │
│  • 需要跟踪 session 的 write LSN                               │
│  • 适合: 读写分离场景                                          │
│                                                                 │
│  GLOBAL (全局一致) - 默认                                       │
│  ────────────────                                               │
│  • 读取时等待 RO 追上指定 LSN                                  │
│  • 延迟可能较高                                                 │
│  • 适合: 需要强一致的场景                                      │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 LSN 水位

```cpp
// RO 节点维护的 LSN 水位
class RONode {
    uint64_t applied_lsn;       // 已应用到本地缓存的 LSN
    uint64_t visible_lsn;       // 对用户查询可见的 LSN
    uint64_t rw_current_lsn;    // RW 节点当前的 LSN
    
    // 关系: visible_lsn <= applied_lsn <= rw_current_lsn
};
```

## 3. Schema Snapshot

### 3.1 获取快照

```cpp
SchemaSnapshot* get_schema_snapshot_for_query() {
    // 获取当前可见 LSN
    uint64_t snapshot_lsn = get_visible_lsn();
    
    // 创建 Schema Snapshot
    SchemaSnapshot* ss = new SchemaSnapshot();
    ss->snapshot_lsn = snapshot_lsn;
    
    return ss;
}

// 查询使用 snapshot
void execute_query(Query* query) {
    // 1. 获取 Schema Snapshot
    SchemaSnapshot* ss = get_schema_snapshot_for_query();
    
    // 2. 解析表名，找到可见的 schema 版本
    for (table in query->tables) {
        SchemaVersion* v = find_visible_table(ss, table.db, table.name);
        if (v == NULL) {
            throw TableNotFoundError(table);
        }
        table.space_id = v->space_id;
        table.schema_version = v->schema_version;
    }
    
    // 3. 执行查询，使用指定的 schema 版本
    execute_with_schema(query, ss);
    
    // 4. 释放 snapshot
    release_schema_snapshot(ss);
}
```

### 3.2 可见性判断

```cpp
SchemaVersion* find_visible_table(
    SchemaSnapshot* ss,
    string db_name,
    string table_name
) {
    // 从 Schema Version 表中查找
    auto versions = schema_store->get_versions(db_name, table_name);
    
    for (auto& v : versions) {
        // 可见条件:
        // 1. create_lsn <= snapshot_lsn (表已创建)
        // 2. drop_lsn == 0 OR drop_lsn > snapshot_lsn (表未删除)
        
        if (v.create_lsn <= ss->snapshot_lsn &&
            (v.drop_lsn == 0 || v.drop_lsn > ss->snapshot_lsn)) {
            return &v;
        }
    }
    
    return NULL;  // 表不存在
}
```

## 4. RO 节点 Redo 同步

### 4.1 同步架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    RO Redo 同步架构                              │
│                                                                 │
│  ┌─────────────────┐                                           │
│  │   RW Node       │                                           │
│  │                 │                                           │
│  │  write_lsn=1500 │                                           │
│  └────────┬────────┘                                           │
│           │ WriteRedo                                          │
│           ▼                                                     │
│  ┌─────────────────┐                                           │
│  │   Storage Layer │  received_lsn=1500                        │
│  │                 │  durable_lsn=1450                         │
│  │                 │  applied_lsn=1400                         │
│  └────────┬────────┘                                           │
│           │                                                     │
│           │ Redo Stream                                        │
│           ▼                                                     │
│  ┌─────────────────┐                                           │
│  │   RO Node       │                                           │
│  │                 │                                           │
│  │  applied_lsn=1350                                           │
│  │  visible_lsn=1350                                           │
│  └─────────────────┘                                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 4.2 Redo 应用线程

```cpp
class RORedoApplier {
    void run() {
        while (running) {
            // 1. 从存储层拉取 Redo
            auto redo_stream = storage_client->GetRedoStream(
                applied_lsn, BATCH_SIZE);
            
            // 2. 解析并处理
            for (auto& redo : redo_stream) {
                if (is_ddl_redo(redo)) {
                    // DDL 分发到 Schema Worker
                    schema_worker->enqueue(redo);
                } else {
                    // DML: 更新本地缓存 (如果有)
                    invalidate_buffer_pool_if_cached(redo);
                }
                
                applied_lsn = redo.lsn;
            }
            
            // 3. 更新 visible_lsn
            // (可能需要等待 Schema Worker 完成某些 DDL)
            update_visible_lsn();
        }
    }
    
    void update_visible_lsn() {
        // visible_lsn = min(applied_lsn, schema_worker.completed_lsn)
        visible_lsn = min(applied_lsn, 
                          schema_worker->get_completed_lsn());
    }
};
```

### 4.3 Buffer Pool 缓存失效

```cpp
void invalidate_buffer_pool_if_cached(RedoRecord& redo) {
    PageId page_id = {redo.space_id, redo.page_no};
    
    // 检查 Buffer Pool 是否缓存了这个页
    buf_block_t* block = buf_page_hash_get(page_id);
    
    if (block != NULL) {
        // 方案 A: 直接失效
        buf_page_invalidate(block);
        
        // 方案 B: 应用 Redo 更新缓存 (更激进)
        // apply_redo_to_cached_page(block, redo);
    }
}
```

## 5. 活跃快照跟踪

### 5.1 跟踪器

```cpp
class ActiveSnapshotTracker {
    // 活跃查询的 snapshot LSN (最小堆)
    MinHeap<uint64_t> active_snapshots;
    mutex lock;
    
    void on_query_start(uint64_t snapshot_lsn) {
        lock_guard<mutex> guard(lock);
        active_snapshots.push(snapshot_lsn);
    }
    
    void on_query_end(uint64_t snapshot_lsn) {
        lock_guard<mutex> guard(lock);
        active_snapshots.remove(snapshot_lsn);
    }
    
    uint64_t get_min_active_snapshot() {
        lock_guard<mutex> guard(lock);
        if (active_snapshots.empty()) {
            return current_visible_lsn;  // 无活跃查询
        }
        return active_snapshots.top();
    }
};
```

### 5.2 上报到存储层

```cpp
// 定期上报，用于 Schema GC
void report_active_snapshot() {
    while (running) {
        uint64_t min_snapshot = tracker.get_min_active_snapshot();
        
        ReportActiveSnapshotRequest req;
        req.set_node_id(node_id);
        req.set_min_active_snapshot_lsn(min_snapshot);
        
        storage_client->ReportActiveSnapshot(req);
        
        sleep(REPORT_INTERVAL);  // 如 1 秒
    }
}
```

## 6. 等待一致性读

```cpp
// GLOBAL 一致性: 等待 RO 追上指定 LSN
Page* read_page_with_consistency(
    uint32_t space_id,
    uint64_t schema_version,
    uint32_t page_no,
    uint64_t required_lsn,
    ConsistencyLevel level
) {
    if (level == EVENTUAL) {
        // 直接读，不等待
        return storage_client->ReadPage(
            space_id, schema_version, page_no, 0);
    }
    
    if (level == GLOBAL) {
        // 等待 visible_lsn >= required_lsn
        while (visible_lsn < required_lsn) {
            wait_for_lsn_advance(required_lsn, TIMEOUT);
        }
    }
    
    return storage_client->ReadPage(
        space_id, schema_version, page_no, required_lsn);
}
```

## 7. 开发任务

- [ ] 实现 SchemaSnapshot
- [ ] 实现可见性判断
- [ ] RO Redo 同步线程
- [ ] Buffer Pool 缓存失效
- [ ] ActiveSnapshotTracker
- [ ] 一致性级别支持
- [ ] 上报活跃快照
- [ ] 集成测试

## 8. 参考

- 主文档 8.7: RW/RO 节点一致性与 DDL 多版本设计
- Skill 04: Schema MVCC
- Skill 09: GC 机制
