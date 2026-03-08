# Skill: Binlog 模块 (Binlog Module)

## 1. 模块职责

在存算分离架构下管理 Binlog：
- **Binlog 存储**: 存储层管理 Binlog
- **Binlog 同步**: 支持外部 MySQL 从库
- **Binlog 订阅**: 支持 CDC 场景
- **Binlog 与 Redo 协调**: 保证事务一致性

**实现语言**: Golang (存储层 Binlog 管理)

## 2. 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                    Binlog 架构                                   │
│                                                                 │
│  ┌─────────────────┐                                           │
│  │   RW Node       │                                           │
│  │                 │                                           │
│  │  MySQL + InnoDB │                                           │
│  │      │          │                                           │
│  │      ▼          │                                           │
│  │  Binlog Cache   │                                           │
│  └────────┬────────┘                                           │
│           │                                                     │
│           │ WriteBinlog (与 WriteRedo 原子)                    │
│           ▼                                                     │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    存储层                                 │   │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐      │   │
│  │  │ Redo Store  │  │ Binlog Store│  │ Page Store  │      │   │
│  │  └─────────────┘  └─────────────┘  └─────────────┘      │   │
│  └─────────────────────────────────────────────────────────┘   │
│           │                                                     │
│           │ Binlog Stream                                      │
│           ▼                                                     │
│  ┌─────────────────┐  ┌─────────────────┐                     │
│  │  外部 MySQL 从库 │  │   CDC Consumer  │                     │
│  │  (Replica)      │  │   (Kafka, etc)  │                     │
│  └─────────────────┘  └─────────────────┘                     │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 3. 核心组件

### 3.1 Binlog Store

```go
package binlog

type BinlogStore struct {
    config      *BinlogConfig
    
    // 存储
    files       []*BinlogFile
    currentFile *BinlogFile
    index       *BinlogIndex
    
    // 同步
    mu          sync.RWMutex
    
    // 元数据
    gtidSet     *GTIDSet
    
    // 与 Redo 协调
    redoStore   *RedoStore
}

type BinlogFile struct {
    Name      string
    StartPos  uint64
    EndPos    uint64
    StartGTID string
    EndGTID   string
    
    // 物理存储
    LBAs      []uint64  // 块地址列表
}

func NewBinlogStore(config *BinlogConfig, redoStore *RedoStore) *BinlogStore {
    return &BinlogStore{
        config:    config,
        index:     NewBinlogIndex(),
        gtidSet:   NewGTIDSet(),
        redoStore: redoStore,
    }
}
```

### 3.2 Binlog 写入

```go
// 原子写入 Redo + Binlog
type AtomicWriteRequest struct {
    RedoData   []byte
    RedoLSN    uint64
    BinlogData []byte
    BinlogPos  uint64
    GTID       string
}

func (s *StorageServer) AtomicWrite(ctx context.Context,
    req *AtomicWriteRequest) (*AtomicWriteResponse, error) {
    
    // 1. 开始原子操作
    txn := s.beginTransaction()
    defer txn.Rollback()
    
    // 2. 写入 Redo
    if err := s.redoStore.WriteRedoTxn(txn, req.RedoData, req.RedoLSN); err != nil {
        return nil, err
    }
    
    // 3. 写入 Binlog
    if err := s.binlogStore.WriteBinlogTxn(txn, req.BinlogData, 
        req.BinlogPos, req.GTID); err != nil {
        return nil, err
    }
    
    // 4. 提交
    if err := txn.Commit(); err != nil {
        return nil, err
    }
    
    return &AtomicWriteResponse{
        DurableLSN:    req.RedoLSN,
        DurableBinlog: req.BinlogPos,
    }, nil
}

func (b *BinlogStore) WriteBinlogTxn(txn *Transaction, 
    data []byte, pos uint64, gtid string) error {
    
    b.mu.Lock()
    defer b.mu.Unlock()
    
    // 1. 检查位置连续性
    if pos != b.currentFile.EndPos {
        return fmt.Errorf("binlog position gap: expected %d, got %d",
            b.currentFile.EndPos, pos)
    }
    
    // 2. 分配块并写入
    lba := b.allocateBlock()
    if err := txn.WriteBlock(lba, data); err != nil {
        return err
    }
    
    // 3. 更新索引
    b.currentFile.EndPos = pos + uint64(len(data))
    b.currentFile.EndGTID = gtid
    b.currentFile.LBAs = append(b.currentFile.LBAs, lba)
    
    // 4. 更新 GTID Set
    b.gtidSet.Add(gtid)
    
    // 5. 检查是否需要轮转
    if b.shouldRotate() {
        b.rotate()
    }
    
    return nil
}
```

### 3.3 Binlog 索引

```go
type BinlogIndex struct {
    // GTID → (file, pos)
    gtidIndex map[string]BinlogPosition
    
    // position → (file, offset)
    posIndex  *BTree
    
    // file → metadata
    fileIndex map[string]*BinlogFile
}

type BinlogPosition struct {
    File   string
    Pos    uint64
    LBA    uint64
}

func (idx *BinlogIndex) LookupByGTID(gtid string) (*BinlogPosition, error) {
    pos, ok := idx.gtidIndex[gtid]
    if !ok {
        return nil, fmt.Errorf("GTID not found: %s", gtid)
    }
    return &pos, nil
}

func (idx *BinlogIndex) LookupByPos(file string, pos uint64) (*BinlogPosition, error) {
    key := fmt.Sprintf("%s:%d", file, pos)
    entry := idx.posIndex.Get(key)
    if entry == nil {
        return nil, fmt.Errorf("position not found: %s:%d", file, pos)
    }
    return entry.(*BinlogPosition), nil
}
```

## 4. Binlog 同步服务

### 4.1 MySQL 复制协议支持

```go
type BinlogDumpServer struct {
    binlogStore *BinlogStore
    
    // 连接管理
    connections map[uint32]*ReplicaConnection
    mu          sync.RWMutex
}

type ReplicaConnection struct {
    ServerID   uint32
    GTID       string
    File       string
    Pos        uint64
    
    sendCh     chan *BinlogEvent
    ctx        context.Context
    cancel     context.CancelFunc
}

// 处理 COM_BINLOG_DUMP_GTID
func (s *BinlogDumpServer) HandleBinlogDumpGTID(
    conn *mysql.Conn, gtidSet string) error {
    
    // 1. 解析起始 GTID
    startGTID, err := ParseGTIDSet(gtidSet)
    if err != nil {
        return err
    }
    
    // 2. 查找起始位置
    pos, err := s.binlogStore.FindPositionByGTID(startGTID)
    if err != nil {
        return err
    }
    
    // 3. 注册连接
    replica := &ReplicaConnection{
        ServerID: conn.ConnectionID(),
        GTID:     gtidSet,
        File:     pos.File,
        Pos:      pos.Pos,
        sendCh:   make(chan *BinlogEvent, 1000),
    }
    replica.ctx, replica.cancel = context.WithCancel(context.Background())
    
    s.mu.Lock()
    s.connections[replica.ServerID] = replica
    s.mu.Unlock()
    
    // 4. 发送 Binlog 事件
    go s.streamBinlog(replica, conn)
    
    return nil
}

func (s *BinlogDumpServer) streamBinlog(
    replica *ReplicaConnection, conn *mysql.Conn) {
    
    defer func() {
        s.mu.Lock()
        delete(s.connections, replica.ServerID)
        s.mu.Unlock()
    }()
    
    for {
        select {
        case <-replica.ctx.Done():
            return
        default:
        }
        
        // 1. 读取 Binlog 事件
        events, err := s.binlogStore.ReadFrom(replica.File, replica.Pos, 
            s.config.BatchSize)
        if err != nil {
            log.Errorf("Read binlog failed: %v", err)
            return
        }
        
        if len(events) == 0 {
            // 等待新事件
            time.Sleep(s.config.PollInterval)
            continue
        }
        
        // 2. 发送给从库
        for _, event := range events {
            if err := conn.WriteBinlogEvent(event); err != nil {
                log.Errorf("Send binlog failed: %v", err)
                return
            }
            replica.Pos = event.NextPos
        }
    }
}
```

### 4.2 CDC 订阅接口

```go
// gRPC 服务
service BinlogService {
    // 订阅 Binlog 流
    rpc Subscribe(SubscribeRequest) returns (stream BinlogEvent);
    
    // 查询位置
    rpc GetPosition(GetPositionRequest) returns (GetPositionResponse);
    
    // 查询 GTID Set
    rpc GetGTIDSet(GetGTIDSetRequest) returns (GetGTIDSetResponse);
}

message SubscribeRequest {
    oneof start_point {
        string gtid = 1;          // GTID 模式
        BinlogPosition pos = 2;   // File + Pos 模式
    }
    repeated string filter_tables = 3;  // 表过滤
    repeated string filter_events = 4;  // 事件类型过滤
}

message BinlogEvent {
    uint32 type = 1;
    uint64 timestamp = 2;
    bytes data = 3;
    string gtid = 4;
    string file = 5;
    uint64 pos = 6;
}
```

```go
func (s *BinlogServer) Subscribe(req *SubscribeRequest, 
    stream BinlogService_SubscribeServer) error {
    
    // 1. 确定起始位置
    var pos *BinlogPosition
    if req.GetGtid() != "" {
        pos, _ = s.binlogStore.FindPositionByGTID(req.GetGtid())
    } else {
        pos = req.GetPos()
    }
    
    // 2. 创建过滤器
    filter := NewEventFilter(req.FilterTables, req.FilterEvents)
    
    // 3. 流式发送
    for {
        events, err := s.binlogStore.ReadFrom(pos.File, pos.Pos, 100)
        if err != nil {
            return err
        }
        
        for _, event := range events {
            if filter.Match(event) {
                if err := stream.Send(event); err != nil {
                    return err
                }
            }
            pos.Pos = event.NextPos
        }
        
        if len(events) == 0 {
            time.Sleep(100 * time.Millisecond)
        }
    }
}
```

## 5. Binlog 与 Redo 协调

### 5.1 XA 两阶段提交

```
事务提交顺序:
─────────────────────────────────────────────────────────────►
  1. InnoDB Prepare (写 Redo)
  2. 写 Binlog
  3. InnoDB Commit (写 Redo Commit 标记)

在存算分离架构:
─────────────────────────────────────────────────────────────►
  1. Redo Prepare → 存储层
  2. Binlog → 存储层 (原子)
  3. Redo Commit → 存储层

恢复时:
  如果 Binlog 存在 → 提交事务
  如果 Binlog 不存在 → 回滚事务
```

### 5.2 崩溃恢复

```go
func (s *StorageLayer) RecoverXA() error {
    // 1. 扫描未完成的 Redo 事务
    preparedTxns := s.redoStore.GetPreparedTransactions()
    
    for _, txn := range preparedTxns {
        // 2. 检查 Binlog 是否存在
        binlogExists := s.binlogStore.HasTransaction(txn.XID)
        
        if binlogExists {
            // 3. Binlog 存在，提交事务
            s.redoStore.CommitTransaction(txn.XID)
        } else {
            // 4. Binlog 不存在，回滚事务
            s.redoStore.RollbackTransaction(txn.XID)
        }
    }
    
    return nil
}
```

## 6. Binlog 清理

```go
type BinlogPurger struct {
    store *BinlogStore
    
    // 保留策略
    retentionTime   time.Duration  // 时间保留
    retentionSize   uint64         // 大小保留
    minReplicaGTID  string         // 最慢从库 GTID
}

func (p *BinlogPurger) Run() {
    for {
        // 1. 收集所有从库的位置
        minGTID := p.collectReplicaPositions()
        
        // 2. 计算安全清理点
        safeGTID := p.calculateSafePurgePoint(minGTID)
        
        // 3. 清理旧文件
        p.purgeBeforeGTID(safeGTID)
        
        time.Sleep(p.config.PurgeInterval)
    }
}

func (p *BinlogPurger) purgeBeforeGTID(gtid string) {
    files := p.store.ListFilesBefore(gtid)
    
    for _, file := range files {
        // 检查时间/大小保留策略
        if p.shouldKeep(file) {
            continue
        }
        
        log.Infof("Purging binlog file: %s", file.Name)
        p.store.DeleteFile(file.Name)
    }
}
```

## 7. 协议消息

```protobuf
// Binlog 写入
message WriteBinlogRequest {
    uint64 db_id = 1;
    bytes  binlog_data = 2;
    uint64 binlog_pos = 3;
    string gtid = 4;
    uint64 redo_lsn = 5;  // 关联的 Redo LSN
}

message WriteBinlogResponse {
    bool   success = 1;
    uint64 durable_pos = 2;
}

// Binlog 读取
message ReadBinlogRequest {
    uint64 db_id = 1;
    string file = 2;
    uint64 pos = 3;
    uint32 max_events = 4;
}

message ReadBinlogResponse {
    repeated BinlogEventData events = 1;
    string next_file = 2;
    uint64 next_pos = 3;
}

message BinlogEventData {
    uint32 type = 1;
    uint64 timestamp = 2;
    bytes  data = 3;
    string gtid = 4;
    uint64 next_pos = 5;
}
```

## 8. IO 优化

### 8.1 Binlog 压缩

```go
type CompressedBinlogWriter struct {
    compressor *zstd.Encoder
}

func (w *CompressedBinlogWriter) Write(event *BinlogEvent) error {
    data, _ := proto.Marshal(event)
    
    // ZSTD 压缩，压缩率约 60-70%
    compressed := w.compressor.EncodeAll(data, nil)
    
    return w.file.Write(compressed)
}
```

### 8.2 流式推送 (Server Push)

```go
type BinlogStreamer struct {
    subscribers map[string]*Subscriber
}

// 新 Binlog 时主动推送，避免轮询
func (s *BinlogStreamer) OnNewBinlog(event *BinlogEvent) {
    for _, sub := range s.subscribers {
        select {
        case sub.ch <- event:  // 非阻塞推送
        default:
        }
    }
}
```

### 8.3 与 Redo 批量提交

```go
// Redo + Binlog 一次 fsync
func (s *StorageServer) AtomicWrite(req *AtomicWriteRequest) error {
    txn := s.beginTransaction()
    
    // 写入同一批次
    s.redoStore.WriteRedoTxn(txn, req.RedoData)
    s.binlogStore.WriteBinlogTxn(txn, req.BinlogData)
    
    // 单次提交
    return txn.Commit()  // 一次 fsync
}
```

## 9. 开发任务

- [ ] 实现 BinlogStore
- [ ] 实现 Binlog 索引
- [ ] 实现原子 Redo+Binlog 写入
- [ ] MySQL 复制协议支持
- [ ] CDC 订阅 gRPC 服务
- [ ] XA 恢复逻辑
- [ ] **实现 ZSTD 压缩**
- [ ] **实现流式推送**
- [ ] **Redo+Binlog 批量提交**
- [ ] Binlog 清理
- [ ] 监控指标
- [ ] 集成测试

## 10. 参考

- MySQL 复制协议文档
- MySQL Binlog 格式文档
- 主文档 5: Binlog 文件结构详解
