# Skill: 计算层改造 (Compute Layer Modifications)

## 1. 模块职责

改造 MySQL/InnoDB 计算层，使其：
- 只写 Redo Log 到存储层
- 从存储层读取数据页
- 禁用 Double Write 和脏页刷新

## 2. 改造原则

```
最小化改动原则:
- 保持 MySQL 接口不变
- 保持 SQL 语义不变
- 只改造存储 IO 路径
```

## 3. 核心改造点

### 3.1 Redo Log 写入改造

**原始代码位置**: `storage/innobase/log/log0write.cc`

```cpp
// 原始: log_writer() 写入本地文件
void log_writer(log_t* log, ...) {
    // 写入 redo log 文件
    fil_io(IORequest(IORequest::WRITE), ..., log->write_lsn, ...);
}

// 改造后: 发送到存储层
void log_writer(log_t* log, ...) {
    // 构造 Redo 数据
    WriteRedoRequest req;
    req.set_start_lsn(log->write_lsn);
    req.set_end_lsn(log->write_lsn + len);
    req.set_redo_data(log_buffer_data);
    req.set_sync(sync_required);
    
    // 发送到存储层
    auto resp = storage_client->WriteRedo(req);
    
    // 等待持久化确认
    if (sync_required) {
        wait_until(resp.durable_lsn() >= log->write_lsn + len);
    }
}
```

### 3.2 页读取改造

**原始代码位置**: `storage/innobase/buf/buf0buf.cc`

```cpp
// 原始: buf_page_get() 从本地文件读取
buf_block_t* buf_page_get(
    const page_id_t& page_id,
    ...) 
{
    // 从 Buffer Pool 查找
    buf_block_t* block = buf_page_hash_get(page_id);
    if (block != NULL) {
        return block;
    }
    
    // 从文件读取
    fil_io(IORequest(IORequest::READ), page_id, ...);
}

// 改造后: 从存储层读取
buf_block_t* buf_page_get(
    const page_id_t& page_id,
    uint64_t min_lsn,  // 新增: 需要的最小 LSN
    ...) 
{
    // 从 Buffer Pool 查找
    buf_block_t* block = buf_page_hash_get(page_id);
    if (block != NULL && block->page.lsn >= min_lsn) {
        return block;
    }
    
    // 从存储层读取
    ReadPageRequest req;
    req.set_space_id(page_id.space());
    req.set_schema_version(get_current_schema_version(page_id.space()));
    req.set_page_no(page_id.page_no());
    req.set_min_lsn(min_lsn);
    
    auto resp = storage_client->ReadPage(req);
    
    // 放入 Buffer Pool
    block = buf_page_create(page_id, resp.page_data());
    return block;
}
```

### 3.3 禁用 Double Write

**原始代码位置**: `storage/innobase/buf/buf0dblwr.cc`

```cpp
// 原始: 写数据页前先写 Double Write Buffer
void buf_dblwr_write_single_page(buf_page_t* bpage, ...) {
    // 写入 double write buffer
    fil_io(IORequest(IORequest::WRITE), DBLWR_FILE, ...);
    // 写入实际位置
    fil_io(IORequest(IORequest::WRITE), bpage->id, ...);
}

// 改造后: 完全禁用
void buf_dblwr_write_single_page(buf_page_t* bpage, ...) {
    // Aurora 架构不需要 Double Write
    // 存储层通过 Redo 保证数据完整性
    return;
}

// 启动时配置
innodb_doublewrite = OFF  // 配置禁用
```

### 3.4 禁用脏页刷新

```cpp
// 原始: 后台线程刷脏页
void buf_flush_page_cleaner() {
    while (running) {
        // 选择脏页
        // 写入磁盘
    }
}

// 改造后: 禁用刷脏
void buf_flush_page_cleaner() {
    // Aurora 架构: 计算层不刷脏页
    // 存储层负责通过 Redo 应用更新页
    return;
}

// Buffer Pool 中的页:
// - 可以保留缓存加速读取
// - 不需要写回 (因为存储层会通过 Redo 更新)
// - Evict 时直接丢弃
```

### 3.5 Checkpoint 改造

```cpp
// 原始: checkpoint 推进 LSN，允许 redo 覆盖
void log_checkpoint() {
    // 等待脏页刷到磁盘
    // 更新 checkpoint LSN
}

// 改造后: 基于存储层的 applied_lsn
void log_checkpoint() {
    // 查询存储层的 applied_lsn
    auto status = storage_client->GetStatus();
    uint64_t applied_lsn = status.applied_lsn();
    
    // 可以安全截断 applied_lsn 之前的 Redo
    log->checkpoint_lsn = applied_lsn;
}
```

## 4. Storage Client

```cpp
// 计算层与存储层通信的客户端
class StorageClient {
public:
    // 单例
    static StorageClient* instance();
    
    // 初始化连接
    void init(string storage_endpoint);
    
    // Redo 写入
    WriteRedoResponse WriteRedo(const WriteRedoRequest& req);
    
    // 页读取
    ReadPageResponse ReadPage(const ReadPageRequest& req);
    
    // 批量读取
    vector<ReadPageResponse> ReadPages(const ReadPagesRequest& req);
    
    // 状态查询
    GetStatusResponse GetStatus();
    
    // 表空间操作
    CreateTablespaceResponse CreateTablespace(const CreateTablespaceRequest& req);
    void DropTablespace(const DropTablespaceRequest& req);
    
private:
    grpc::Channel channel;
    StorageService::Stub stub;
};
```

## 5. 配置参数

```ini
# Aurora 模式开关
innodb_aurora_mode = ON

# 存储层地址
aurora_storage_endpoint = "storage.cluster:9000"

# 禁用 Double Write (自动)
innodb_doublewrite = OFF

# 禁用脏页刷新 (自动)
innodb_page_cleaners = 0
innodb_max_dirty_pages_pct = 100

# Buffer Pool 配置 (仍然需要，用于读缓存)
innodb_buffer_pool_size = 4G
```

## 6. 兼容性考虑

```
需要保持兼容的功能:
- 事务 ACID
- MVCC
- 锁管理
- SQL 语义

改变行为的功能:
- 持久化方式 (Redo only)
- 崩溃恢复 (由存储层处理)
- Buffer Pool (只做缓存，不回写)
```

## 7. IO 优化

### 7.1 Redo Group Commit

```cpp
// 聚合多个事务的 Redo，减少 RPC 次数
class RedoGroupCommitter {
    struct CommitGroup {
        vector<Transaction*> txns;
        vector<byte> merged_redo;
        uint64_t start_lsn;
        uint64_t end_lsn;
    };
    
    // 配置
    size_t max_group_size = 64;
    duration max_wait_time = 1ms;
    size_t max_redo_bytes = 1 * 1024 * 1024;
    
    void flush_group() {
        // 1. LZ4 压缩
        auto compressed = lz4_compress(current_group.merged_redo);
        
        // 2. 单次 RDMA Write
        storage_client->WriteRedo(compressed);
        
        // 3. 通知所有事务
        for (auto* txn : current_group.txns) {
            txn->notify_committed();
        }
    }
};
```

### 7.2 页预读

```cpp
class PrefetchManager {
    void on_page_access(uint32_t space_id, uint32_t page_no) {
        // 检测顺序访问模式
        if (is_sequential_access(space_id, page_no)) {
            // 异步预读后续页
            prefetch_pages_async(space_id, page_no, 64);
        }
    }
    
    void prefetch_pages_async(uint32_t space_id, uint32_t start, int count) {
        ReadPagesRequest req;
        for (int i = 1; i <= count; i++) {
            req.add_pages()->set_page_no(start + i);
        }
        // 异步请求，结果放入 Buffer Pool
        storage_client->ReadPagesAsync(req);
    }
};
```

### 7.3 Buffer Pool 优化

```cpp
// 热点分区 + 大页内存
class OptimizedBufferPool {
    // 热点区 (频繁访问)
    HotPartition* hot_partition;  // 小，用 2MB 大页
    
    // 温区 (中等访问)
    WarmPartition* warm_partition;  // 大，普通内存
    
    // 访问计数，自动提升/降级
    void promote_to_hot(buf_block_t* block);
    void demote_to_warm(buf_block_t* block);
};
```

## 8. 开发任务

- [ ] 实现 StorageClient
- [ ] 改造 log_writer()
- [ ] 改造 buf_page_get()
- [ ] 禁用 Double Write
- [ ] 禁用 Page Cleaner
- [ ] 改造 Checkpoint
- [ ] 添加配置参数
- [ ] **实现 RedoGroupCommitter**
- [ ] **实现 PrefetchManager**
- [ ] **Buffer Pool 热点分区**
- [ ] RW 节点集成测试
- [ ] 性能基准测试

## 9. 参考

- 主文档 8.4: 计算层修改
- InnoDB 源码:
  - `storage/innobase/log/log0write.cc`
  - `storage/innobase/buf/buf0buf.cc`
  - `storage/innobase/buf/buf0dblwr.cc`
  - `storage/innobase/buf/buf0flu.cc`
