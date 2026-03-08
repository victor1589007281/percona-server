# Skill: IO 优化 (IO Optimization)

## 1. 模块职责

从全局视角优化整个存算分离架构的 IO 性能：
- **减少 IO 次数**: 批量、合并、缓存
- **减少 IO 延迟**: RDMA、异步、预读
- **减少 IO 数据量**: 压缩、增量、去重
- **提高 IO 并行度**: 多路、Pipeline

## 2. IO 路径全景图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           IO 路径全景图                                      │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                         计算层 (RW)                                  │   │
│  │                                                                      │   │
│  │  事务提交 ──► Redo Buffer ──► 批量发送 ──► 压缩 ──► RDMA Write     │   │
│  │                  ↑                                                   │   │
│  │              Group Commit                                            │   │
│  │                                                                      │   │
│  │  页读取 ◄── Buffer Pool ◄── 预读队列 ◄── RDMA Read ◄── 存储层      │   │
│  │                  ↑                                                   │   │
│  │              热点缓存                                                │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              │                                              │
│                              │ RDMA / gRPC                                  │
│                              ▼                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                          存储层                                      │   │
│  │                                                                      │   │
│  │  Redo 接收 ──► 内存队列 ──► 批量持久化 ──► 后台应用                 │   │
│  │                                   │                                  │   │
│  │                                   ▼                                  │   │
│  │  页读取 ◄── 页缓存 ◄── Redo on-demand 应用 ◄── 块存储              │   │
│  │                                                                      │   │
│  │  索引 ──► B+Tree (内存 + LSM 持久化) ──► 批量写入                   │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                              │                                              │
│                              │ 批量 IO                                      │
│                              ▼                                              │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                       分布式块存储                                   │   │
│  │                                                                      │   │
│  │  多路并行 ──► 条带化写入 ──► 本地 SSD 缓存                          │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 3. 分模块 IO 优化

### 3.1 计算层 (03-compute-layer)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| Redo 批量写入 | Group Commit | 减少 RPC 次数 10-100x |
| Redo 压缩 | LZ4 实时压缩 | 减少网络传输 50-70% |
| 页预读 | 顺序扫描检测 + 异步预读 | 减少读延迟 |
| Buffer Pool 优化 | 热点分区 + 大页内存 | 提高缓存命中率 |

```cpp
// Redo Group Commit 优化
class RedoGroupCommitter {
    // 聚合多个事务的 Redo
    struct CommitGroup {
        vector<Transaction*> txns;
        vector<byte> merged_redo;
        uint64_t start_lsn;
        uint64_t end_lsn;
    };
    
    // 配置
    size_t max_group_size = 64;           // 最多聚合 64 个事务
    duration max_wait_time = 1ms;         // 最多等待 1ms
    size_t max_redo_bytes = 1 * 1024 * 1024;  // 最多 1MB
    
    void commit(Transaction* txn) {
        // 1. 加入当前 group
        current_group.txns.push_back(txn);
        current_group.merged_redo.append(txn->redo);
        
        // 2. 检查是否应该提交
        if (should_flush()) {
            flush_group();
        }
    }
    
    bool should_flush() {
        return current_group.txns.size() >= max_group_size ||
               current_group.merged_redo.size() >= max_redo_bytes ||
               time_since_first_txn() >= max_wait_time;
    }
    
    void flush_group() {
        // 压缩
        auto compressed = lz4_compress(current_group.merged_redo);
        
        // 单次 RDMA Write
        storage_client->WriteRedo(current_group.start_lsn,
                                   current_group.end_lsn,
                                   compressed);
        
        // 通知所有事务
        for (auto* txn : current_group.txns) {
            txn->notify_committed();
        }
    }
};
```

```cpp
// 页预读优化
class PrefetchManager {
    // 检测顺序扫描模式
    struct AccessPattern {
        uint32_t space_id;
        uint32_t last_page;
        int direction;  // 1 = forward, -1 = backward
        int sequential_count;
    };
    
    unordered_map<uint32_t, AccessPattern> patterns;
    
    void on_page_access(uint32_t space_id, uint32_t page_no) {
        auto& p = patterns[space_id];
        
        // 检测顺序访问
        if (page_no == p.last_page + p.direction) {
            p.sequential_count++;
        } else {
            p.direction = (page_no > p.last_page) ? 1 : -1;
            p.sequential_count = 1;
        }
        p.last_page = page_no;
        
        // 触发预读
        if (p.sequential_count >= 4) {
            // 预读后续 16-64 页
            int prefetch_count = min(64, p.sequential_count * 4);
            prefetch_pages_async(space_id, page_no, p.direction, 
                                 prefetch_count);
        }
    }
    
    void prefetch_pages_async(uint32_t space_id, uint32_t start,
                              int direction, int count) {
        // 批量读取请求
        ReadPagesRequest req;
        for (int i = 1; i <= count; i++) {
            req.add_pages()->set_page_no(start + i * direction);
        }
        
        // 异步发送，不等待结果
        storage_client->ReadPagesAsync(req, [this](auto resp) {
            for (auto& page : resp.pages()) {
                buffer_pool->insert_prefetched(page);
            }
        });
    }
};
```

### 3.2 存储层 - Redo 管理 (05-redo-management)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| 内存队列缓冲 | Ring Buffer | 减少磁盘 IO |
| 批量持久化 | fsync 合并 | 减少 fsync 次数 |
| 并行应用 | 按 space_id 分片 | 提高应用吞吐 |
| 增量传输 (RO) | 只传 diff | 减少 RO 同步带宽 |

```go
// Redo 批量持久化
type RedoBatchWriter struct {
    buffer      *RingBuffer    // 内存环形缓冲
    file        *os.File
    
    // 配置
    batchSize   int           // 批量大小 (字节)
    flushInterval time.Duration // 刷盘间隔
    
    // 水位
    receivedLSN uint64
    durableLSN  uint64
}

func (w *RedoBatchWriter) Write(data []byte, lsn uint64) error {
    // 1. 写入内存缓冲
    w.buffer.Write(data)
    atomic.StoreUint64(&w.receivedLSN, lsn)
    
    // 2. 检查是否需要刷盘
    if w.buffer.Size() >= w.batchSize {
        return w.flush()
    }
    return nil
}

func (w *RedoBatchWriter) flushLoop() {
    ticker := time.NewTicker(w.flushInterval)
    for range ticker.C {
        if w.buffer.Size() > 0 {
            w.flush()
        }
    }
}

func (w *RedoBatchWriter) flush() error {
    // 1. 批量写入文件
    data := w.buffer.ReadAll()
    if _, err := w.file.Write(data); err != nil {
        return err
    }
    
    // 2. 单次 fsync
    if err := w.file.Sync(); err != nil {
        return err
    }
    
    // 3. 更新水位
    atomic.StoreUint64(&w.durableLSN, w.receivedLSN)
    return nil
}
```

```go
// Redo 并行应用
type ParallelRedoApplier struct {
    workers    []*ApplyWorker
    numWorkers int
}

func NewParallelRedoApplier(numWorkers int) *ParallelRedoApplier {
    p := &ParallelRedoApplier{
        numWorkers: numWorkers,
        workers:    make([]*ApplyWorker, numWorkers),
    }
    
    for i := 0; i < numWorkers; i++ {
        p.workers[i] = NewApplyWorker(i)
        go p.workers[i].Run()
    }
    return p
}

func (p *ParallelRedoApplier) Dispatch(redo *RedoRecord) {
    // 按 space_id 分片，保证同一表空间的 Redo 顺序
    workerID := redo.SpaceID % uint32(p.numWorkers)
    p.workers[workerID].Submit(redo)
}

func (p *ParallelRedoApplier) WaitUntil(lsn uint64) {
    // 等待所有 worker 都应用到指定 LSN
    var wg sync.WaitGroup
    for _, w := range p.workers {
        wg.Add(1)
        go func(worker *ApplyWorker) {
            defer wg.Done()
            worker.WaitUntil(lsn)
        }(w)
    }
    wg.Wait()
}
```

### 3.3 存储层 - 页管理 (06-page-management)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| 页缓存 | LRU + 热点分区 | 减少块存储读取 |
| 按需 Redo 应用 | 读时应用 | 减少后台 IO |
| 写合并 | 同页多次修改合并 | 减少写 IO |
| 大块读写 | 连续页批量 IO | 提高 IO 效率 |

```go
// 页缓存优化
type PageCache struct {
    // 两级缓存
    hotCache  *LRUCache  // 热点缓存 (小，频繁访问)
    warmCache *LRUCache  // 温数据缓存 (大，中等访问)
    
    // 访问计数
    accessCount map[PageKey]*atomic.Int64
}

func (c *PageCache) Get(key PageKey) (*Page, bool) {
    // 1. 先查热点缓存
    if page, ok := c.hotCache.Get(key); ok {
        c.recordAccess(key)
        return page, true
    }
    
    // 2. 再查温缓存
    if page, ok := c.warmCache.Get(key); ok {
        c.recordAccess(key)
        // 提升到热点缓存
        if c.isHot(key) {
            c.hotCache.Put(key, page)
        }
        return page, true
    }
    
    return nil, false
}

func (c *PageCache) recordAccess(key PageKey) {
    count := c.accessCount[key]
    if count == nil {
        count = &atomic.Int64{}
        c.accessCount[key] = count
    }
    count.Add(1)
}

func (c *PageCache) isHot(key PageKey) bool {
    count := c.accessCount[key]
    return count != nil && count.Load() > 10
}
```

```go
// 按需 Redo 应用 (读时应用)
func (s *PageStore) ReadPage(key PageKey, minLSN uint64) (*Page, error) {
    // 1. 尝试从缓存读取
    if page, ok := s.cache.Get(key); ok {
        if page.LSN >= minLSN {
            return page, nil
        }
    }
    
    // 2. 从块存储读取基础页
    basePage, err := s.readFromBlock(key)
    if err != nil {
        return nil, err
    }
    
    // 3. 如果基础页 LSN 不够，应用 Redo
    if basePage.LSN < minLSN {
        // 只获取需要的 Redo
        redos := s.redoStore.GetRedoForPage(key, basePage.LSN, minLSN)
        
        // 应用 Redo
        for _, redo := range redos {
            applyRedoToPage(basePage, redo)
        }
        basePage.LSN = minLSN
    }
    
    // 4. 放入缓存
    s.cache.Put(key, basePage)
    
    return basePage, nil
}
```

```go
// 写合并优化
type PageWriteCoalescer struct {
    pending map[PageKey]*PendingWrite
    mu      sync.Mutex
    
    flushInterval time.Duration
    maxPending    int
}

type PendingWrite struct {
    page      *Page
    redoCount int
    firstTime time.Time
}

func (c *PageWriteCoalescer) ScheduleWrite(key PageKey, page *Page) {
    c.mu.Lock()
    defer c.mu.Unlock()
    
    if existing, ok := c.pending[key]; ok {
        // 合并：用新页替换，只写一次
        existing.page = page
        existing.redoCount++
    } else {
        c.pending[key] = &PendingWrite{
            page:      page,
            redoCount: 1,
            firstTime: time.Now(),
        }
    }
    
    // 检查是否需要刷盘
    if len(c.pending) >= c.maxPending {
        c.flush()
    }
}

func (c *PageWriteCoalescer) flush() {
    // 批量写入
    var batch []*BatchWriteItem
    for key, pw := range c.pending {
        batch = append(batch, &BatchWriteItem{
            Key:  key,
            Page: pw.page,
        })
    }
    
    // 并行写入块存储
    c.blockStore.BatchWrite(batch)
    
    c.pending = make(map[PageKey]*PendingWrite)
}
```

### 3.4 协议层 (07-protocol)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| RDMA 零拷贝 | 直接内存访问 | 减少 CPU 拷贝开销 |
| 请求流水线 | Pipeline | 隐藏网络延迟 |
| 连接多路复用 | 多 QP | 提高并发 |
| 智能路由 | 就近读取 | 减少网络跳数 |

```go
// RDMA 请求流水线
type RDMAPipeline struct {
    conn      *RDMAConnection
    inflight  chan *PipelineRequest
    maxDepth  int
}

type PipelineRequest struct {
    req      interface{}
    respCh   chan interface{}
    doneCh   chan struct{}
}

func (p *RDMAPipeline) Submit(req interface{}) <-chan interface{} {
    pr := &PipelineRequest{
        req:    req,
        respCh: make(chan interface{}, 1),
        doneCh: make(chan struct{}),
    }
    
    // 非阻塞提交
    select {
    case p.inflight <- pr:
        // 异步发送
        go p.send(pr)
    default:
        // 队列满，同步等待
        p.inflight <- pr
        go p.send(pr)
    }
    
    return pr.respCh
}

func (p *RDMAPipeline) send(pr *PipelineRequest) {
    // 发送 RDMA 请求
    p.conn.PostSend(pr.req)
    
    // 等待完成
    p.conn.WaitCompletion()
    
    // 返回结果
    pr.respCh <- p.conn.GetResponse()
    close(pr.doneCh)
}

// 批量读取多页
func (c *StorageClient) ReadPagesParallel(requests []ReadPageRequest) []Page {
    pipeline := c.getPipeline()
    
    // 并行提交所有请求
    var respChs []<-chan interface{}
    for _, req := range requests {
        respChs = append(respChs, pipeline.Submit(req))
    }
    
    // 收集结果
    var pages []Page
    for _, ch := range respChs {
        resp := <-ch
        pages = append(pages, resp.(Page))
    }
    
    return pages
}
```

### 3.5 元数据服务 (01-metadata-service)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| 内存索引 | 全内存 B+Tree | 消除磁盘查找 |
| 索引分片 | 按 space_id 分片 | 减少锁竞争 |
| 批量更新 | WAL 合并 | 减少持久化次数 |
| 缓存预热 | 启动时加载 | 减少冷启动延迟 |

```go
// 分片索引
type ShardedPageIndex struct {
    shards    []*PageIndexShard
    numShards int
}

type PageIndexShard struct {
    index *BPlusTree
    mu    sync.RWMutex
    wal   *WAL
}

func (s *ShardedPageIndex) getShardID(spaceID uint32) int {
    return int(spaceID) % s.numShards
}

func (s *ShardedPageIndex) Get(key PageKey) (*PageIndexEntry, bool) {
    shard := s.shards[s.getShardID(key.SpaceID)]
    shard.mu.RLock()
    defer shard.mu.RUnlock()
    
    return shard.index.Get(key)
}

func (s *ShardedPageIndex) BatchPut(entries []PageIndexEntry) error {
    // 按 shard 分组
    groups := make(map[int][]PageIndexEntry)
    for _, e := range entries {
        shardID := s.getShardID(e.Key.SpaceID)
        groups[shardID] = append(groups[shardID], e)
    }
    
    // 并行更新各 shard
    var wg sync.WaitGroup
    var errMu sync.Mutex
    var firstErr error
    
    for shardID, group := range groups {
        wg.Add(1)
        go func(id int, entries []PageIndexEntry) {
            defer wg.Done()
            
            shard := s.shards[id]
            shard.mu.Lock()
            defer shard.mu.Unlock()
            
            // 批量写 WAL
            if err := shard.wal.AppendBatch(entries); err != nil {
                errMu.Lock()
                if firstErr == nil {
                    firstErr = err
                }
                errMu.Unlock()
                return
            }
            
            // 更新内存索引
            for _, e := range entries {
                shard.index.Put(e.Key, e)
            }
        }(shardID, group)
    }
    
    wg.Wait()
    return firstErr
}
```

### 3.6 Binlog 模块 (11-binlog-module)

| 优化点 | 技术 | 收益 |
|--------|------|------|
| 与 Redo 合并提交 | 原子批量 | 减少 fsync |
| 压缩 | ZSTD 压缩 | 减少存储和带宽 |
| 流式传输 | Server Push | 减少 RO 轮询 |

```go
// Binlog 压缩
type CompressedBinlogWriter struct {
    file       *os.File
    compressor *zstd.Encoder
    
    uncompressedSize int64
    compressedSize   int64
}

func (w *CompressedBinlogWriter) Write(event *BinlogEvent) error {
    // 序列化
    data, _ := proto.Marshal(event)
    w.uncompressedSize += int64(len(data))
    
    // 压缩写入
    compressed := w.compressor.EncodeAll(data, nil)
    w.compressedSize += int64(len(compressed))
    
    // 写入文件
    return binary.Write(w.file, binary.LittleEndian, compressed)
}

func (w *CompressedBinlogWriter) CompressionRatio() float64 {
    return float64(w.compressedSize) / float64(w.uncompressedSize)
}
```

```go
// Binlog 流式推送
type BinlogStreamer struct {
    store       *BinlogStore
    subscribers map[string]*Subscriber
    mu          sync.RWMutex
}

type Subscriber struct {
    id       string
    position uint64
    ch       chan *BinlogEvent
}

func (s *BinlogStreamer) Subscribe(id string, startPos uint64) <-chan *BinlogEvent {
    sub := &Subscriber{
        id:       id,
        position: startPos,
        ch:       make(chan *BinlogEvent, 1000),
    }
    
    s.mu.Lock()
    s.subscribers[id] = sub
    s.mu.Unlock()
    
    return sub.ch
}

// 新 Binlog 写入时主动推送
func (s *BinlogStreamer) OnNewBinlog(event *BinlogEvent) {
    s.mu.RLock()
    defer s.mu.RUnlock()
    
    for _, sub := range s.subscribers {
        if event.Position >= sub.position {
            select {
            case sub.ch <- event:
            default:
                // 缓冲区满，跳过 (订阅者会重新拉取)
            }
        }
    }
}
```

## 4. 全局 IO 优化配置

```go
type IOConfig struct {
    // 计算层
    RedoGroupCommit struct {
        MaxGroupSize   int           `default:"64"`
        MaxWaitTime    time.Duration `default:"1ms"`
        MaxRedoBytes   int           `default:"1048576"`  // 1MB
    }
    
    Prefetch struct {
        Enabled           bool `default:"true"`
        SequentialThreshold int  `default:"4"`
        MaxPrefetchPages  int  `default:"64"`
    }
    
    // 存储层
    RedoBatch struct {
        BatchSize     int           `default:"4194304"`  // 4MB
        FlushInterval time.Duration `default:"10ms"`
    }
    
    RedoApply struct {
        NumWorkers int `default:"8"`
    }
    
    PageCache struct {
        HotCacheSize  int `default:"1073741824"`   // 1GB
        WarmCacheSize int `default:"4294967296"`   // 4GB
    }
    
    WriteCoalesce struct {
        Enabled       bool          `default:"true"`
        MaxPending    int           `default:"1000"`
        FlushInterval time.Duration `default:"100ms"`
    }
    
    // 协议层
    RDMA struct {
        Enabled        bool `default:"true"`
        PipelineDepth  int  `default:"32"`
        NumConnections int  `default:"4"`
    }
    
    // 元数据
    Index struct {
        NumShards int `default:"16"`
    }
    
    // 压缩
    Compression struct {
        RedoEnabled   bool   `default:"true"`
        RedoAlgorithm string `default:"lz4"`
        BinlogEnabled bool   `default:"true"`
        BinlogAlgorithm string `default:"zstd"`
    }
}
```

## 5. IO 优化效果预估

| 优化项 | 场景 | 预期提升 |
|--------|------|----------|
| Redo Group Commit | 高并发 OLTP | 写入吞吐 5-10x |
| LZ4 压缩 | Redo 传输 | 带宽减少 50-70% |
| RDMA | 页读取 | 延迟减少 50%+ |
| 页预读 | 顺序扫描 | 延迟减少 80%+ |
| 并行 Redo 应用 | 多表写入 | 应用吞吐 4-8x |
| 写合并 | 热点页更新 | 写 IO 减少 50%+ |
| 页缓存 | 热点读取 | 命中率 90%+ |
| 索引分片 | 高并发元数据 | 吞吐 4-8x |

## 6. 监控指标

```go
var (
    // 计算层
    redoGroupSizeHist = prometheus.NewHistogram(...)
    redoCompressRatio = prometheus.NewGauge(...)
    prefetchHitRate   = prometheus.NewGauge(...)
    
    // 存储层
    redoBatchSizeHist   = prometheus.NewHistogram(...)
    redoApplyLatency    = prometheus.NewHistogram(...)
    pageCacheHitRate    = prometheus.NewGauge(...)
    writeCoalesceRatio  = prometheus.NewGauge(...)
    
    // 协议层
    rdmaLatencyHist     = prometheus.NewHistogram(...)
    rdmaThroughput      = prometheus.NewCounter(...)
    
    // 总体
    iopsRead  = prometheus.NewCounter(...)
    iopsWrite = prometheus.NewCounter(...)
    ioLatencyRead  = prometheus.NewHistogram(...)
    ioLatencyWrite = prometheus.NewHistogram(...)
)
```

## 7. 开发任务

- [ ] 计算层 Group Commit
- [ ] 计算层预读管理器
- [ ] 存储层批量 Redo 写入
- [ ] 存储层并行 Redo 应用
- [ ] 页缓存优化
- [ ] 写合并优化
- [ ] RDMA Pipeline
- [ ] 元数据索引分片
- [ ] Binlog 压缩和流式推送
- [ ] 监控指标集成
- [ ] 性能基准测试

## 8. 参考

- 各模块 Skill 文档
- Aurora 论文: IO 优化部分
- RDMA 最佳实践
