# Skill: Redo Log 管理 (Redo Management)

## 1. 模块职责

管理 Redo Log 的完整生命周期：
- **接收**: 从 RW 节点接收 Redo
- **持久化**: 保证 Redo 不丢失
- **索引**: 支持按 LSN 范围快速定位
- **应用**: 将 Redo 应用到数据页
- **分发**: 向 RO 节点提供 Redo 流

**实现语言**: Golang

## 2. Redo 记录格式

### 2.1 原生 InnoDB Redo 格式

```
┌─────────────────────────────────────────────────────────────────┐
│  InnoDB Redo Record                                             │
│  ─────────────────                                              │
│  type:     1 byte   (MLOG_xxx)                                  │
│  space_id: 4 bytes  (压缩编码)                                  │
│  page_no:  4 bytes  (压缩编码)                                  │
│  body:     变长     (取决于 type)                               │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 扩展后的 Redo 格式

```cpp
struct RedoRecord {
    // 原有字段
    uint8_t  type;              // Redo 类型
    uint32_t space_id;          // 表空间 ID
    uint32_t page_no;           // 页号
    
    // 新增字段 (Schema MVCC)
    uint64_t schema_version;    // 目标 schema 版本
    uint64_t lsn;               // 记录的 LSN
    
    // 可选字段 (支持 undo)
    bool     has_before_image;
    bytes    before_image;      // 修改前数据
    bytes    after_image;       // 修改后数据 / diff
};
```

## 3. Redo Store 设计

### 3.1 存储结构

```
┌─────────────────────────────────────────────────────────────────┐
│                    Redo Store 结构                               │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Redo Log Files (追加写)                                 │   │
│  │  ────────────────────────                                │   │
│  │  redo_0001.log  [LSN 0 - 10000000]                      │   │
│  │  redo_0002.log  [LSN 10000000 - 20000000]               │   │
│  │  redo_0003.log  [LSN 20000000 - current]  ← 当前活跃    │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Redo Index (LSN → 位置)                                 │   │
│  │  ──────────────────────                                  │   │
│  │  [LSN 0 - 1000000)     → file: redo_0001, offset: 0     │   │
│  │  [LSN 1000000 - 2000000) → file: redo_0001, offset: x   │   │
│  │  ...                                                     │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  Page Redo Index (可选优化)                              │   │
│  │  ─────────────────────────                               │   │
│  │  (space_id, page_no) → [LSN1, LSN2, LSN3, ...]          │   │
│  │  用于快速找到某页的所有 Redo 记录                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

### 3.2 核心类

```cpp
class RedoStore {
public:
    // === 写入接口 (来自 RW) ===
    
    WriteRedoResponse write_redo(
        uint64_t start_lsn,
        uint64_t end_lsn,
        bytes redo_data,
        bool sync
    ) {
        // 1. 追加到当前 log 文件
        current_file->append(redo_data);
        received_lsn = end_lsn;
        
        // 2. 如果需要同步，fsync
        if (sync) {
            current_file->fsync();
            durable_lsn = end_lsn;
        }
        
        // 3. 更新索引
        redo_index.add(start_lsn, end_lsn, current_file, offset);
        
        // 4. 通知 Applier
        applier->notify_new_redo();
        
        return { durable_lsn };
    }
    
    // === 读取接口 (RO 同步 / 页重建) ===
    
    bytes read_redo(uint64_t from_lsn, uint64_t to_lsn) {
        // 从索引找到位置
        auto locations = redo_index.find_range(from_lsn, to_lsn);
        
        // 读取并拼接
        bytes result;
        for (auto& loc : locations) {
            result.append(loc.file->read(loc.offset, loc.length));
        }
        return result;
    }
    
    // 读取某页的 Redo (用于页重建)
    vector<RedoRecord> get_redo_for_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no,
        uint64_t from_lsn,
        uint64_t to_lsn
    ) {
        // 方案 A: 扫描 LSN 范围，过滤
        // 方案 B: 使用 Page Redo Index (如果有)
    }
    
    // === 状态查询 ===
    
    uint64_t get_received_lsn();
    uint64_t get_durable_lsn();
    uint64_t get_applied_lsn();
    
private:
    RedoLogFile* current_file;
    vector<RedoLogFile*> files;
    RedoIndex redo_index;
    RedoApplier* applier;
    
    atomic<uint64_t> received_lsn;
    atomic<uint64_t> durable_lsn;
    atomic<uint64_t> applied_lsn;
};
```

## 4. Redo Applier

### 4.1 应用流程

```cpp
class RedoApplier {
public:
    void run() {
        while (running) {
            // 1. 等待新 Redo
            wait_for_new_redo();
            
            // 2. 获取待应用的 Redo
            uint64_t from = applied_lsn;
            uint64_t to = durable_lsn;  // 只应用已持久化的
            
            bytes redo_data = redo_store->read_redo(from, to);
            
            // 3. 解析并应用
            while (has_more_records(redo_data)) {
                RedoRecord rec = parse_next(redo_data);
                
                // 检查 schema version
                if (!is_schema_active(rec.space_id, rec.schema_version)) {
                    continue;  // 跳过已删除的 schema
                }
                
                // DDL 记录特殊处理
                if (is_ddl_redo(rec.type)) {
                    handle_ddl_redo(rec);
                    continue;
                }
                
                // 读取页
                Page* page = page_store->get_page_for_apply(
                    rec.space_id, rec.schema_version, rec.page_no);
                
                // 应用 Redo
                apply_redo_to_page(page, &rec);
                
                // 写回
                page_store->write_page(
                    rec.space_id, rec.schema_version,
                    rec.page_no, page, rec.lsn);
            }
            
            // 4. 更新 applied_lsn
            applied_lsn = to;
        }
    }
    
private:
    void apply_redo_to_page(Page* page, RedoRecord* rec) {
        switch (rec->type) {
        case MLOG_REC_INSERT:
            page_apply_insert(page, rec);
            break;
        case MLOG_REC_DELETE:
            page_apply_delete(page, rec);
            break;
        case MLOG_REC_UPDATE:
            page_apply_update(page, rec);
            break;
        case MLOG_WRITE_STRING:
            page_apply_write(page, rec->offset, rec->data);
            break;
        // ... 其他类型
        }
        
        page->lsn = rec->lsn;
    }
};
```

### 4.2 并行应用优化

```cpp
// 按 space_id 分片并行应用
class ParallelRedoApplier {
    vector<RedoApplierWorker*> workers;
    
    void dispatch_redo(RedoRecord& rec) {
        // 根据 space_id 分配到不同 worker
        int worker_id = rec.space_id % workers.size();
        workers[worker_id]->enqueue(rec);
    }
    
    void wait_all_applied(uint64_t lsn) {
        for (auto* w : workers) {
            w->wait_until_applied(lsn);
        }
    }
};
```

## 5. Redo 清理

```cpp
// Redo 日志清理策略
class RedoCleaner {
    void run() {
        while (running) {
            // 1. 获取安全清理点
            // = min(所有 RO 的 visible_lsn, checkpoint_lsn)
            uint64_t safe_lsn = get_safe_cleanup_lsn();
            
            // 2. 删除旧文件
            for (auto* file : redo_files) {
                if (file->max_lsn < safe_lsn) {
                    file->delete();
                    redo_files.remove(file);
                }
            }
            
            sleep(CLEANUP_INTERVAL);
        }
    }
};
```

## 6. IO 优化

### 6.1 批量持久化

```go
type RedoBatchWriter struct {
    buffer        *RingBuffer
    batchSize     int           // 4MB
    flushInterval time.Duration // 10ms
}

func (w *RedoBatchWriter) Write(data []byte, lsn uint64) error {
    // 写入内存缓冲
    w.buffer.Write(data)
    
    // 达到批量大小时刷盘
    if w.buffer.Size() >= w.batchSize {
        return w.flush()  // 单次 fsync
    }
    return nil
}
```

### 6.2 并行 Redo 应用

```go
type ParallelRedoApplier struct {
    workers    []*ApplyWorker
    numWorkers int  // 默认 8
}

func (p *ParallelRedoApplier) Dispatch(redo *RedoRecord) {
    // 按 space_id 分片，保证顺序
    workerID := redo.SpaceID % uint32(p.numWorkers)
    p.workers[workerID].Submit(redo)
}
```

### 6.3 增量 RO 同步

```go
// RO 节点只同步 diff，不重复传输完整 Redo
func (s *RedoStore) GetRedoStreamForRO(fromLSN uint64) <-chan *RedoRecord {
    ch := make(chan *RedoRecord, 1000)
    go func() {
        for lsn := fromLSN; ; {
            records := s.ReadFrom(lsn, 1000)
            for _, r := range records {
                ch <- r
                lsn = r.LSN + 1
            }
            if len(records) == 0 {
                time.Sleep(1 * time.Millisecond)
            }
        }
    }()
    return ch
}
```

## 7. 开发任务

- [ ] 定义 Redo 记录格式 protobuf
- [ ] 实现 RedoLogFile (追加写文件)
- [ ] 实现 RedoIndex (LSN → 位置)
- [ ] 实现 RedoStore
- [ ] 实现 RedoApplier
- [ ] **实现 RedoBatchWriter (批量持久化)**
- [ ] **实现 ParallelRedoApplier (并行应用)**
- [ ] **增量 RO 同步**
- [ ] Redo 清理机制
- [ ] RO 同步接口
- [ ] 性能测试

## 8. 参考

- 主文档 2: Redo Log 文件结构详解
- 主文档 8.3: 存储层详细设计
- InnoDB Redo 源码:
  - `storage/innobase/include/mtr0types.h` (MLOG_xxx 定义)
  - `storage/innobase/log/log0recv.cc` (Redo 应用逻辑)
