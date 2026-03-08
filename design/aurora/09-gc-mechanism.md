# Skill: GC 机制 (Garbage Collection)

## 1. 模块职责

清理不再需要的数据：
- **Schema GC**: 清理已删除的 Schema 版本
- **页版本 GC**: 清理旧版本的数据页
- **Redo GC**: 清理已应用的 Redo Log

**实现语言**: Golang

## 2. GC 依赖关系

```
┌─────────────────────────────────────────────────────────────────┐
│                    GC 依赖关系                                   │
│                                                                 │
│  Schema GC 依赖:                                                │
│  • cluster_min_active_snapshot_lsn (所有节点最小活跃快照)       │
│  • 只有当 drop_lsn < min_snapshot 时才能 GC                     │
│                                                                 │
│  页版本 GC 依赖:                                                │
│  • Schema GC (schema 版本被 GC 后，相关页才能删)               │
│  • COW 引用计数 (共享页需要检查引用)                           │
│                                                                 │
│  Redo GC 依赖:                                                  │
│  • applied_lsn (已应用的 Redo 可以删除)                        │
│  • checkpoint_lsn (恢复需要的最小 LSN)                         │
│  • RO visible_lsn (RO 可能需要重建历史版本)                    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 3. Schema GC

### 3.1 GC 安全条件

```cpp
// Schema 版本 V 可以被 GC 的条件:
bool can_gc_schema_version(SchemaVersion* v) {
    // 1. 必须是已删除的版本
    if (v->drop_lsn == 0) {
        return false;
    }
    
    // 2. 所有节点的最小活跃快照都已超过删除点
    uint64_t min_snapshot = get_cluster_min_active_snapshot_lsn();
    if (v->drop_lsn >= min_snapshot) {
        return false;  // 还有查询可能看到这个版本
    }
    
    return true;
}
```

### 3.2 GC 流程

```cpp
class SchemaGC {
    void run() {
        while (running) {
            // 1. 收集集群最小活跃快照
            uint64_t min_snapshot = collect_cluster_min_snapshot();
            
            // 2. 遍历所有已删除的 schema 版本
            auto dropped_versions = schema_store->list_dropped_versions();
            
            for (auto& v : dropped_versions) {
                if (v.drop_lsn < min_snapshot) {
                    // 3. 安全删除
                    gc_schema_version(v);
                }
            }
            
            sleep(GC_INTERVAL);  // 如 10 秒
        }
    }
    
    void gc_schema_version(SchemaVersion& v) {
        log("GC schema version: table=%s, space=%d, version=%d",
            v.table_name, v.space_id, v.schema_version);
        
        // 1. 删除所有页
        gc_pages_for_schema(v.space_id, v.schema_version);
        
        // 2. 删除 schema 版本记录
        schema_store->remove(v.space_id, v.schema_version);
    }
    
    uint64_t collect_cluster_min_snapshot() {
        uint64_t min_lsn = UINT64_MAX;
        
        // 从 RW 节点获取
        min_lsn = min(min_lsn, rw_node->get_min_active_snapshot());
        
        // 从所有 RO 节点获取
        for (auto& ro : ro_nodes) {
            min_lsn = min(min_lsn, ro->get_min_active_snapshot());
        }
        
        return min_lsn;
    }
};
```

## 4. 页版本 GC

### 4.1 页 GC 流程

```cpp
void gc_pages_for_schema(uint32_t space_id, uint64_t schema_version) {
    // 遍历该 schema 版本的所有页
    auto pages = page_index->list_pages(space_id, schema_version);
    
    for (auto& page_entry : pages) {
        // 检查 COW 共享
        if (page_entry.is_shared) {
            // 共享页，减少引用计数
            decrement_page_ref(page_entry.shared_with, page_entry.page_no);
        } else {
            // 独立页，直接释放
            block_allocator->free(page_entry.lba);
        }
        
        // 删除索引条目
        page_index->remove({space_id, schema_version, page_entry.page_no});
    }
}

void decrement_page_ref(uint64_t schema_version, uint32_t page_no) {
    // 引用计数减 1
    ref_count[{schema_version, page_no}]--;
    
    // 如果引用为 0 且版本已被 GC，释放块
    if (ref_count[{schema_version, page_no}] == 0 &&
        is_schema_gc_ed(schema_version)) {
        
        auto entry = page_index->get({space_id, schema_version, page_no});
        block_allocator->free(entry.lba);
    }
}
```

### 4.2 COW 引用计数

```cpp
// 页的引用计数 (用于 COW)
class PageRefCounter {
    // (space_id, schema_version, page_no) → ref_count
    HashMap<PageKey, atomic<int>> ref_counts;
    
    void increment(PageKey key) {
        ref_counts[key]++;
    }
    
    void decrement(PageKey key) {
        if (--ref_counts[key] == 0) {
            // 可以释放物理块
            schedule_free(key);
        }
    }
};
```

## 5. Redo GC

### 5.1 安全清理点

```cpp
uint64_t get_safe_redo_cleanup_lsn() {
    uint64_t safe_lsn = UINT64_MAX;
    
    // 1. 存储层已应用的 LSN
    safe_lsn = min(safe_lsn, storage_applied_lsn);
    
    // 2. 所有 RO 节点的 visible_lsn (可能需要重建历史页)
    for (auto& ro : ro_nodes) {
        safe_lsn = min(safe_lsn, ro->visible_lsn);
    }
    
    // 3. Schema GC 可能需要的 LSN
    // (某些历史版本页重建可能需要旧 Redo)
    safe_lsn = min(safe_lsn, cluster_min_active_snapshot_lsn);
    
    return safe_lsn;
}
```

### 5.2 Redo 清理

```cpp
class RedoGC {
    void run() {
        while (running) {
            uint64_t safe_lsn = get_safe_redo_cleanup_lsn();
            
            // 删除 safe_lsn 之前的 Redo 文件
            for (auto& file : redo_files) {
                if (file.max_lsn < safe_lsn) {
                    log("GC redo file: %s, max_lsn=%d", 
                        file.name, file.max_lsn);
                    file.delete();
                    redo_files.remove(file);
                }
            }
            
            sleep(REDO_GC_INTERVAL);  // 如 60 秒
        }
    }
};
```

## 6. GC 策略配置

```cpp
struct GCConfig {
    // Schema GC
    int schema_gc_interval_sec = 10;
    int schema_max_retained_versions = 100;
    int schema_max_age_sec = 3600;
    
    // Redo GC
    int redo_gc_interval_sec = 60;
    uint64_t redo_min_retained_size = 1GB;
    int redo_min_retained_time_sec = 3600;
    
    // 页 GC
    bool page_gc_enabled = true;
    int page_gc_batch_size = 1000;
};
```

## 7. 监控指标

```cpp
// GC 相关监控
struct GCMetrics {
    // Schema GC
    uint64_t schema_versions_gc_total;
    uint64_t schema_versions_pending;
    uint64_t schema_gc_duration_ms;
    
    // 页 GC
    uint64_t pages_gc_total;
    uint64_t blocks_freed_total;
    uint64_t space_reclaimed_bytes;
    
    // Redo GC
    uint64_t redo_files_gc_total;
    uint64_t redo_bytes_gc_total;
    uint64_t oldest_retained_lsn;
    
    // 水位
    uint64_t cluster_min_active_snapshot_lsn;
    uint64_t safe_redo_cleanup_lsn;
};
```

## 8. 故障处理

### 8.1 GC 被阻塞

```cpp
// 检测 GC 阻塞
void check_gc_health() {
    uint64_t min_snapshot = get_cluster_min_active_snapshot_lsn();
    uint64_t current_lsn = get_current_lsn();
    
    // 如果差距太大，可能有长查询阻塞 GC
    if (current_lsn - min_snapshot > GC_LAG_THRESHOLD) {
        alert("GC lagging: min_snapshot=%d, current=%d, gap=%d",
              min_snapshot, current_lsn, current_lsn - min_snapshot);
        
        // 可选: 强制 kill 长查询
        if (config.force_gc_enabled) {
            kill_oldest_query();
        }
    }
}
```

### 8.2 空间不足

```cpp
// 紧急 GC
void emergency_gc() {
    // 1. 降低 GC 阈值
    force_gc_schema_versions(config.emergency_gc_threshold);
    
    // 2. 强制清理 Redo
    force_gc_redo(config.emergency_redo_retention);
    
    // 3. 通知应用层
    notify_storage_pressure();
}
```

## 9. 开发任务

- [ ] 实现 SchemaGC
- [ ] 实现页版本 GC
- [ ] 实现 COW 引用计数
- [ ] 实现 RedoGC
- [ ] 集群 snapshot 收集
- [ ] GC 配置管理
- [ ] 监控指标
- [ ] 紧急 GC 机制
- [ ] 集成测试

## 10. 参考

- 主文档 8.7.6: Schema 版本垃圾回收
- Skill 04: Schema MVCC
- Skill 05: Redo 管理
- Skill 06: 页管理
