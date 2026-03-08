# Skill: 元数据服务 (Metadata Service)

## 1. 模块职责

元数据服务是存储层的核心组件，负责：
- **表空间管理**: 替代文件系统的文件/目录操作
- **页索引**: (space_id, schema_v, page_no) → LBA 映射
- **块分配**: 管理 LBA 空间的分配和回收

**实现语言**: Golang

## 2. 定位

```
元数据服务 ≠ 通用文件系统
元数据服务 = MySQL 专用的 "表空间 → 块" 映射层
```

**它替代了什么**:
- 文件系统的目录/文件管理
- InnoDB 的 FSP_HDR、Extent Descriptor
- 文件偏移计算

**它不需要什么**:
- POSIX 兼容
- 文件权限管理
- 硬链接/软链接
- mmap 支持

## 3. 核心数据结构

### 3.1 表空间注册表 (Tablespace Registry)

```go
type TablespaceInfo struct {
    uint32_t space_id;          // 表空间 ID
    uint64_t schema_version;    // Schema 版本
    string   db_name;           // 数据库名
    string   table_name;        // 表名
    
    uint64_t create_lsn;        // 创建时的 LSN
    uint64_t drop_lsn;          // 删除时的 LSN (0=未删除)
    
    TableState state;           // ACTIVE / DROPPED / PENDING_GC
};

// 索引
// 主键: (space_id, schema_version)
// 二级索引: (db_name, table_name) → list of versions
```

### 3.2 页索引 (Page Index)

```cpp
struct PageIndexEntry {
    uint64_t lba;               // 物理块地址
    uint64_t page_lsn;          // 页的 LSN
    bool     is_shared;         // 是否与其他版本共享 (COW)
    uint64_t shared_with;       // 如果共享，指向源版本
};

// 索引: (space_id, schema_version, page_no) → PageIndexEntry
// 实现: B+Tree 或 LSM-Tree
```

### 3.3 块分配器 (Block Allocator)

```cpp
class BlockAllocator {
    // 空闲块管理 (可用 bitmap 或 free list)
    FreeList free_blocks;
    
    // 分配一个块
    LBA alloc();
    
    // 批量分配 (提高效率)
    vector<LBA> alloc_batch(size_t count);
    
    // 释放块
    void free(LBA lba);
    
    // 释放块 (带引用计数，用于 COW)
    void free_if_no_ref(LBA lba);
};
```

## 4. 核心 API

### 4.1 表空间操作

```cpp
// 创建表空间
CreateTablespaceResponse create_tablespace(
    string db_name,
    string table_name,
    uint64_t initial_pages
);

// 删除表空间 (标记删除，等待 GC)
void drop_tablespace(
    uint32_t space_id,
    uint64_t schema_version,
    uint64_t drop_lsn
);

// 查找表空间 (根据 snapshot 找可见版本)
TablespaceInfo lookup_tablespace(
    string db_name,
    string table_name,
    uint64_t snapshot_lsn
);

// 重命名表空间
void rename_tablespace(
    uint32_t space_id,
    uint64_t schema_version,
    string new_db_name,
    string new_table_name
);
```

### 4.2 页索引操作

```cpp
// 查找页的 LBA
LBA lookup_page(
    uint32_t space_id,
    uint64_t schema_version,
    uint32_t page_no
);

// 分配新页
LBA allocate_page(
    uint32_t space_id,
    uint64_t schema_version,
    uint32_t page_no
);

// 更新页 LBA (写入后)
void update_page(
    uint32_t space_id,
    uint64_t schema_version,
    uint32_t page_no,
    LBA new_lba,
    uint64_t new_lsn
);

// 释放页 (GC 时调用)
void free_page(
    uint32_t space_id,
    uint64_t schema_version,
    uint32_t page_no
);
```

## 5. 实现要点

### 5.1 元数据持久化

```
元数据本身也存储在块存储中：
- 预留固定的 LBA 区域 (如 LBA 0-9999)
- 元数据变更写 WAL，保证原子性
- 定期 checkpoint，加速恢复
```

### 5.2 内存缓存

```cpp
class MetadataCache {
    // 表空间信息缓存
    LRUCache<TablespaceKey, TablespaceInfo> tablespace_cache;
    
    // 热点页索引缓存
    LRUCache<PageKey, PageIndexEntry> page_index_cache;
    
    // 写入时 invalidate
    void invalidate(TablespaceKey key);
};
```

### 5.3 并发控制

```cpp
// 表空间级别的锁
mutex tablespace_locks[NUM_LOCK_BUCKETS];

// 页索引的细粒度锁
ReadWriteLock page_index_lock;

// 块分配器的锁
mutex allocator_lock;
```

## 6. 与其他模块的交互

```
┌──────────────────┐
│   计算层请求      │
│  (gRPC 调用)     │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│   元数据服务      │◄──── 本文档
└────────┬─────────┘
         │
    ┌────┴────┐
    │         │
    ▼         ▼
┌────────┐ ┌────────┐
│Page    │ │Redo    │
│Store   │ │Store   │
└────────┘ └────────┘
```

## 7. IO 优化

### 7.1 索引分片

```go
// 按 space_id 分片，减少锁竞争
type ShardedPageIndex struct {
    shards    []*PageIndexShard
    numShards int  // 默认 16
}

func (s *ShardedPageIndex) Get(key PageKey) (*PageIndexEntry, bool) {
    shard := s.shards[key.SpaceID % s.numShards]
    shard.mu.RLock()
    defer shard.mu.RUnlock()
    return shard.index.Get(key)
}
```

### 7.2 批量 WAL 写入

```go
func (s *ShardedPageIndex) BatchPut(entries []PageIndexEntry) error {
    // 按 shard 分组
    groups := make(map[int][]PageIndexEntry)
    for _, e := range entries {
        shardID := e.Key.SpaceID % s.numShards
        groups[shardID] = append(groups[shardID], e)
    }
    
    // 并行更新各 shard (每个 shard 批量 WAL)
    var wg sync.WaitGroup
    for shardID, group := range groups {
        wg.Add(1)
        go func(id int, entries []PageIndexEntry) {
            defer wg.Done()
            s.shards[id].BatchUpdate(entries)  // 单次 fsync
        }(shardID, group)
    }
    wg.Wait()
    return nil
}
```

### 7.3 缓存预热

```go
func (s *MetadataService) WarmupCache() {
    // 启动时加载热点表空间到内存
    hotTablespaces := s.loadHotTablespaces()
    for _, ts := range hotTablespaces {
        s.tablespaceCache.Put(ts.Key(), ts)
        
        // 预加载页索引
        pages := s.loadPageIndex(ts.SpaceID)
        for _, p := range pages {
            s.pageIndexCache.Put(p.Key, p)
        }
    }
}
```

## 8. 开发任务

- [ ] 定义 TablespaceInfo protobuf
- [ ] 实现 BlockAllocator (bitmap 版本)
- [ ] 实现 PageIndex (B+Tree 版本)
- [ ] 实现 TablespaceRegistry
- [ ] 元数据 WAL 和 checkpoint
- [ ] 内存缓存层
- [ ] **实现索引分片 (ShardedPageIndex)**
- [ ] **批量 WAL 写入**
- [ ] **缓存预热**
- [ ] 单元测试

## 9. 参考

- 主文档 附录 E: 元数据服务职责与定位详解
- 主文档 7.3: 元数据存储结构
- 主文档 8.5: 存储层元数据管理
