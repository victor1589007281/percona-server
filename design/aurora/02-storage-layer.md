# Skill: 存储层 (Storage Layer)

## 1. 模块职责

存储层是 Aurora 架构的核心，负责：
- **Redo 接收与持久化**: 接收计算层的 Redo Log
- **Redo 应用**: 将 Redo 应用到数据页
- **页服务**: 提供按 LSN 版本的页读取

**实现语言**: Golang

## 2. 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         存储层                                   │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    API Gateway                           │   │
│  │  WriteRedo / ReadPage / GetStatus / ...                  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                              │                                  │
│         ┌────────────────────┼────────────────────┐            │
│         │                    │                    │            │
│         ▼                    ▼                    ▼            │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────┐      │
│  │ Redo Store  │     │ Page Store  │     │ Metadata    │      │
│  │             │────►│             │     │ Service     │      │
│  │ • 接收      │     │ • 存储      │     │             │      │
│  │ • 持久化    │     │ • 读取      │     │ • 表空间    │      │
│  │ • 应用      │     │ • 多版本    │     │ • 页索引    │      │
│  └─────────────┘     └─────────────┘     └─────────────┘      │
│         │                    │                    │            │
│         └────────────────────┼────────────────────┘            │
│                              │                                  │
│                              ▼                                  │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    Block IO Layer                        │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 3. 核心组件

### 3.1 Redo Store

```cpp
class RedoStore {
public:
    // 写入 Redo (来自 RW 节点)
    WriteRedoResponse write_redo(
        uint64_t start_lsn,
        uint64_t end_lsn,
        bytes redo_data,
        bool sync
    );
    
    // 读取 Redo (RO 节点同步)
    bytes read_redo(uint64_t from_lsn, uint64_t to_lsn);
    
    // 获取 LSN 水位
    uint64_t get_received_lsn();
    uint64_t get_durable_lsn();
    uint64_t get_applied_lsn();
    
private:
    // Redo 日志存储 (追加写)
    RedoLogFile redo_files;
    
    // LSN 到 LBA 的索引
    RedoIndex redo_index;
    
    // 后台应用线程
    RedoApplier applier;
};
```

### 3.2 Page Store

```cpp
class PageStore {
public:
    // 读取页 (指定最小 LSN)
    ReadPageResponse read_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no,
        uint64_t min_lsn
    );
    
    // 写入页 (Redo 应用后)
    void write_page(
        uint32_t space_id,
        uint64_t schema_version,
        uint32_t page_no,
        bytes page_data,
        uint64_t page_lsn
    );
    
private:
    // 页索引 (来自 MetadataService)
    MetadataService* metadata;
    
    // 页缓存
    PageCache cache;
    
    // 块 IO
    BlockIO block_io;
};
```

### 3.3 Redo Applier (后台线程)

```cpp
class RedoApplier {
public:
    void run() {
        while (running) {
            // 获取待应用的 Redo
            auto redo_batch = redo_store->get_pending_redo();
            
            for (auto& redo : redo_batch) {
                // 解析 Redo 记录
                RedoRecord rec = parse_redo(redo);
                
                // 检查 schema version 是否有效
                if (!metadata->is_schema_active(rec.space_id, 
                                                 rec.schema_version)) {
                    continue;  // 跳过已删除的 schema
                }
                
                // 读取当前页
                Page page = page_store->read_page_internal(
                    rec.space_id, rec.schema_version, rec.page_no);
                
                // 应用 Redo
                apply_redo_to_page(&page, &rec);
                
                // 写回
                page_store->write_page(
                    rec.space_id, rec.schema_version, 
                    rec.page_no, page.data, rec.lsn);
            }
            
            // 更新 applied_lsn
            update_applied_lsn(redo_batch.back().lsn);
        }
    }
};
```

## 4. LSN 水位管理

```
┌─────────────────────────────────────────────────────────────────┐
│                    LSN 水位                                      │
│                                                                 │
│  received_lsn: 已接收的最大 LSN (来自 RW)                        │
│  durable_lsn:  已持久化的最大 LSN (fsync 完成)                   │
│  applied_lsn:  已应用到页的最大 LSN                              │
│                                                                 │
│  关系: applied_lsn <= durable_lsn <= received_lsn               │
│                                                                 │
│  ────────────────────────────────────────────────────►  时间    │
│         │              │              │                         │
│    applied_lsn    durable_lsn   received_lsn                   │
│         │              │              │                         │
│    页数据可读      Redo 已持久     Redo 在内存                   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 5. 读取页的流程

```cpp
ReadPageResponse read_page(space_id, schema_v, page_no, min_lsn) {
    // 1. 查找页索引
    PageIndexEntry entry = metadata->lookup_page(
        space_id, schema_v, page_no);
    
    if (entry == NULL) {
        return NOT_FOUND;
    }
    
    // 2. 读取物理块
    Page page = block_io->read(entry.lba);
    
    // 3. 检查 LSN
    if (page.lsn >= min_lsn) {
        // 页已经足够新
        return page;
    }
    
    // 4. 页太旧，需要应用更多 Redo
    auto pending_redo = redo_store->get_redo_for_page(
        space_id, schema_v, page_no, page.lsn, min_lsn);
    
    for (auto& redo : pending_redo) {
        apply_redo_to_page(&page, &redo);
    }
    
    return page;
}
```

## 6. 段 (Segment) 管理

```
存储层按 Segment 组织数据，每个 Segment 管理一定范围的页：

┌─────────────────────────────────────────────────────────────────┐
│  Segment 0                                                      │
│  ├── page_range: space 0-10, page 0-100000                     │
│  ├── redo_log_blocks: [LBA 1000-2000]                          │
│  ├── page_blocks: [LBA 10000-50000]                            │
│  ├── received_lsn: 123456789                                   │
│  ├── durable_lsn: 123456700                                    │
│  └── applied_lsn: 123400000                                    │
├─────────────────────────────────────────────────────────────────┤
│  Segment 1                                                      │
│  ├── ...                                                        │
└─────────────────────────────────────────────────────────────────┘

每个 Segment 可以有多个副本 (replication)
```

## 7. 开发任务

- [ ] 定义存储层 gRPC 服务
- [ ] 实现 RedoStore (接收、持久化)
- [ ] 实现 RedoApplier (后台应用)
- [ ] 实现 PageStore (读取、写入)
- [ ] LSN 水位管理
- [ ] Segment 管理
- [ ] 页缓存 (可选优化)
- [ ] 集成测试

## 8. 参考

- 主文档 8.3: 存储层详细设计
- 主文档 8.6: 完整架构偏移总图
- Skill 05: Redo 管理
- Skill 06: 页管理
