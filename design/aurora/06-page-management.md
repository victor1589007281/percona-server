# Skill: 页管理 (Page Management)

## 1. 模块职责

管理数据页的存储和访问：
- **页存储**: 数据页的物理存储
- **页索引**: 页到块的映射
- **多版本**: 支持 Schema MVCC 的多版本页
- **COW**: Copy-on-Write 机制

**实现语言**: Golang

## 2. 核心概念

### 2.1 页标识

```cpp
// 传统 InnoDB: 二元组
struct PageId {
    uint32_t space_id;
    uint32_t page_no;
};

// Aurora with Schema MVCC: 三元组
struct PageKey {
    uint32_t space_id;
    uint64_t schema_version;
    uint32_t page_no;
};
```

### 2.2 页索引条目

```cpp
struct PageIndexEntry {
    uint64_t lba;               // 物理块地址
    uint64_t page_lsn;          // 页的 LSN
    
    // COW 支持
    bool     is_shared;         // 是否与其他版本共享
    uint64_t shared_with;       // 共享源版本
    
    // 可选: 校验和
    uint32_t checksum;
};
```

## 3. Page Store 设计

### 3.1 核心类

```cpp
class PageStore {
public:
    // === 读取接口 ===
    
    ReadPageResponse read_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no,
        uint64_t min_lsn
    ) {
        // 1. 查找页索引
        PageKey key = {space_id, schema_version, page_no};
        PageIndexEntry* entry = page_index->get(key);
        
        if (entry == NULL) {
            return NOT_FOUND;
        }
        
        // 2. 处理 COW 共享
        if (entry->is_shared) {
            return read_page(space_id, entry->shared_with, page_no, min_lsn);
        }
        
        // 3. 读取物理块
        Page page = block_io->read(entry->lba);
        
        // 4. 检查 LSN，必要时应用 Redo
        if (page.lsn < min_lsn) {
            page = apply_pending_redo(key, page, min_lsn);
        }
        
        return page;
    }
    
    // === 写入接口 (Redo Apply 后) ===
    
    void write_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no,
        Page* page,
        uint64_t new_lsn
    ) {
        PageKey key = {space_id, schema_version, page_no};
        PageIndexEntry* entry = page_index->get(key);
        
        // COW 处理
        if (entry != NULL && entry->is_shared) {
            // 分配新块
            LBA new_lba = block_allocator->alloc();
            block_io->write(new_lba, page);
            
            // 更新索引，不再共享
            page_index->put(key, {
                .lba = new_lba,
                .page_lsn = new_lsn,
                .is_shared = false
            });
        } else {
            // 直接覆盖写
            block_io->write(entry->lba, page);
            entry->page_lsn = new_lsn;
        }
    }
    
    // === 分配接口 ===
    
    LBA allocate_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no
    ) {
        LBA lba = block_allocator->alloc();
        PageKey key = {space_id, schema_version, page_no};
        page_index->put(key, {
            .lba = lba,
            .page_lsn = 0,
            .is_shared = false
        });
        return lba;
    }
    
    // === 释放接口 (GC) ===
    
    void free_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no
    ) {
        PageKey key = {space_id, schema_version, page_no};
        PageIndexEntry* entry = page_index->remove(key);
        
        if (entry != NULL && !entry->is_shared) {
            // 只有非共享块才真正释放
            block_allocator->free(entry->lba);
        }
    }
    
private:
    PageIndex* page_index;
    BlockAllocator* block_allocator;
    BlockIO* block_io;
    RedoStore* redo_store;
};
```

## 4. Copy-on-Write (COW) 实现

### 4.1 COW 场景

```
场景 1: Instant DDL (ADD COLUMN)
────────────────────────────────────
• 所有页共享，只改元数据
• 新版本的所有页指向旧版本的物理块

场景 2: ALTER 需要重建
────────────────────────────────────
• 初始时所有页共享
• 修改时才复制 (写时复制)

场景 3: DROP + CREATE
────────────────────────────────────
• 不共享，新版本完全独立
• 旧版本数据等待 GC
```

### 4.2 COW 初始化

```cpp
void init_cow_for_new_version(
    uint32_t space_id,
    uint64_t old_version,
    uint64_t new_version,
    DDLType ddl_type
) {
    if (ddl_type == DDL_DROP_CREATE) {
        // 不共享，新版本从空开始
        return;
    }
    
    // 所有页初始化为共享
    for (page_no in all_pages(space_id, old_version)) {
        PageIndexEntry old_entry = page_index->get(
            {space_id, old_version, page_no});
        
        page_index->put({space_id, new_version, page_no}, {
            .lba = old_entry.lba,
            .page_lsn = old_entry.page_lsn,
            .is_shared = true,
            .shared_with = old_version
        });
    }
}
```

### 4.3 COW 触发

```cpp
void trigger_cow_if_needed(PageKey key) {
    PageIndexEntry* entry = page_index->get(key);
    
    if (entry->is_shared) {
        // 1. 读取当前数据
        Page page = block_io->read(entry->lba);
        
        // 2. 分配新块
        LBA new_lba = block_allocator->alloc();
        
        // 3. 复制数据
        block_io->write(new_lba, page);
        
        // 4. 更新索引
        entry->lba = new_lba;
        entry->is_shared = false;
        entry->shared_with = 0;
    }
}
```

## 5. 页索引实现

### 5.1 数据结构选择

```
方案 A: B+Tree
─────────────────
优点: 范围查询高效，有序
缺点: 写放大

方案 B: Hash Table + Sorted List
─────────────────
优点: 点查快
缺点: 范围查询需额外处理

方案 C: LSM-Tree
─────────────────
优点: 写入高效
缺点: 读取可能需要合并

推荐: B+Tree (简单，InnoDB 熟悉)
```

### 5.2 内存 + 持久化

```cpp
class PageIndex {
    // 内存索引 (热数据)
    BPlusTree<PageKey, PageIndexEntry> mem_index;
    
    // 持久化 (冷数据 + checkpoint)
    PersistentBTree<PageKey, PageIndexEntry> disk_index;
    
    PageIndexEntry* get(PageKey key) {
        // 先查内存
        auto* entry = mem_index.get(key);
        if (entry != NULL) return entry;
        
        // 再查磁盘
        return disk_index.get(key);
    }
    
    void put(PageKey key, PageIndexEntry entry) {
        mem_index.put(key, entry);
        // WAL 保证持久化
        wal.append(PUT, key, entry);
    }
    
    void checkpoint() {
        // 将内存索引写入磁盘
        disk_index.merge(mem_index);
        mem_index.clear();
    }
};
```

## 6. 块分配器

### 6.1 基于 Bitmap

```cpp
class BitmapBlockAllocator {
    // 每个 bit 表示一个块的状态
    // 0 = 空闲, 1 = 已分配
    Bitmap bitmap;
    
    LBA alloc() {
        LBA lba = bitmap.find_first_zero();
        bitmap.set(lba, 1);
        return lba;
    }
    
    void free(LBA lba) {
        bitmap.set(lba, 0);
    }
    
    vector<LBA> alloc_batch(size_t count) {
        // 尝试分配连续块 (提高 IO 效率)
        return bitmap.find_consecutive_zeros(count);
    }
};
```

### 6.2 基于 Free List

```cpp
class FreeListBlockAllocator {
    // 空闲块链表
    List<LBA> free_list;
    
    // 空闲区间 (用于连续分配)
    IntervalTree free_ranges;
    
    LBA alloc() {
        return free_list.pop_front();
    }
    
    void free(LBA lba) {
        free_list.push_back(lba);
        // 合并相邻空闲块
        free_ranges.add(lba);
    }
};
```

## 7. IO 优化

### 7.1 两级页缓存

```go
type PageCache struct {
    hotCache  *LRUCache  // 热点: 1GB, 频繁访问
    warmCache *LRUCache  // 温区: 4GB, 中等访问
    accessCount map[PageKey]*atomic.Int64
}

func (c *PageCache) Get(key PageKey) (*Page, bool) {
    // 先查热点
    if page, ok := c.hotCache.Get(key); ok {
        return page, true
    }
    // 再查温区，自动提升热点
    if page, ok := c.warmCache.Get(key); ok {
        if c.isHot(key) {
            c.hotCache.Put(key, page)
        }
        return page, true
    }
    return nil, false
}
```

### 7.2 按需 Redo 应用

```go
// 读时应用，而非后台全量应用
func (s *PageStore) ReadPage(key PageKey, minLSN uint64) (*Page, error) {
    basePage := s.readFromBlock(key)
    
    if basePage.LSN < minLSN {
        // 只应用必要的 Redo
        redos := s.redoStore.GetRedoForPage(key, basePage.LSN, minLSN)
        for _, redo := range redos {
            applyRedoToPage(basePage, redo)
        }
    }
    return basePage, nil
}
```

### 7.3 写合并

```go
type PageWriteCoalescer struct {
    pending map[PageKey]*PendingWrite
    maxPending    int           // 1000
    flushInterval time.Duration // 100ms
}

func (c *PageWriteCoalescer) ScheduleWrite(key PageKey, page *Page) {
    if existing, ok := c.pending[key]; ok {
        // 合并：同页多次修改只写一次
        existing.page = page
        existing.mergeCount++
    } else {
        c.pending[key] = &PendingWrite{page: page}
    }
}
```

### 7.4 批量 IO

```go
// 连续页批量读写
func (s *PageStore) ReadPagesContiguous(spaceID uint32, startPage, count int) []*Page {
    // 计算连续 LBA
    lbas := s.calculateContiguousLBAs(spaceID, startPage, count)
    
    // 单次大块 IO
    data := s.blockIO.ReadBatch(lbas)
    
    return s.splitToPages(data)
}
```

## 8. 开发任务

- [ ] 定义 PageKey, PageIndexEntry
- [ ] 实现 PageIndex (B+Tree)
- [ ] 实现 PageStore
- [ ] 实现 COW 机制
- [ ] 实现 BlockAllocator
- [ ] 页索引持久化
- [ ] **实现两级页缓存 (热点/温区)**
- [ ] **实现按需 Redo 应用**
- [ ] **实现写合并**
- [ ] **批量 IO 优化**
- [ ] 单元测试

## 9. 参考

- 主文档 3: IBD 数据文件结构详解
- 主文档 附录 D: Schema MVCC 三元组索引实现方案
- Skill 01: 元数据服务
- Skill 04: Schema MVCC
