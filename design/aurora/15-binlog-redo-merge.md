# Skill: Binlog 与 Redo 合并设计 (Binlog-Redo Merge)

## 1. 问题背景

将 Binlog 与 Redo 合并面临以下挑战：
1. **格式差异**: Redo 是物理日志，Binlog 是逻辑日志
2. **崩溃恢复**: 合并后崩溃恢复流程变化
3. **GTID 维护**: GTID 集合如何持久化和恢复
4. **外部复制**: 如何为外部 MySQL 从库提供标准 Binlog

**实现语言**: C++ (计算层) + Golang (存储层)

## 2. 设计方案

### 2.1 统一日志格式 (Unified Log)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    统一日志架构                                              │
│                                                                             │
│  不是真正合并，而是：                                                       │
│  • Redo 和 Binlog 共享同一个 LSN 序列                                       │
│  • 同一事务的 Redo 和 Binlog 原子写入                                       │
│  • 使用 XID 关联 Redo 和 Binlog                                            │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    计算层                                            │   │
│  │                                                                      │   │
│  │  事务执行 ──► Redo Buffer ──► Binlog Buffer ──► 统一提交             │   │
│  │                     │              │                │                │   │
│  │                     │              │                │                │   │
│  │                     ▼              ▼                ▼                │   │
│  │              ┌──────────────────────────────┐                        │   │
│  │              │     Unified Commit Request   │                        │   │
│  │              │  • redo_data                 │                        │   │
│  │              │  • binlog_data               │                        │   │
│  │              │  • gtid                      │                        │   │
│  │              │  • xid                       │                        │   │
│  │              └──────────────────────────────┘                        │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                 │                                           │
│                                 │ 原子写入                                  │
│                                 ▼                                           │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    存储层                                            │   │
│  │                                                                      │   │
│  │  ┌─────────────────────────────────────────────────────────────┐    │   │
│  │  │  Unified Log Store                                           │    │   │
│  │  │  • Redo Records (物理日志)                                   │    │   │
│  │  │  • Binlog Events (逻辑日志)                                  │    │   │
│  │  │  • 共享 LSN 序列                                             │    │   │
│  │  │  • GTID Index                                                │    │   │
│  │  └─────────────────────────────────────────────────────────────┘    │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 扩展的 Redo 格式

```cpp
// 事务提交记录 (包含 Binlog 引用)
struct TrxCommitRedo {
    uint8_t  type = MLOG_TRX_COMMIT;
    uint64_t lsn;
    uint64_t timestamp_us;
    
    // 事务标识
    uint64_t trx_id;
    bytes    xid;  // XA 事务 ID (如果有)
    
    // GTID 信息
    bytes    gtid_uuid;     // 16 bytes UUID
    uint64_t gtid_seqno;    // 序列号
    
    // Binlog 位置
    uint64_t binlog_lsn;    // Binlog 数据在统一日志中的位置
    uint32_t binlog_length; // Binlog 数据长度
    
    // 可选：内联小 Binlog
    bool     binlog_inline;
    bytes    binlog_data;   // 小于阈值时内联
};

// Binlog 记录 (作为 Redo 的一种类型)
struct BinlogRedo {
    uint8_t  type = MLOG_BINLOG_DATA;
    uint64_t lsn;
    
    // 关联的事务
    uint64_t trx_id;
    bytes    gtid;
    
    // Binlog 事件数据
    bytes    binlog_events;  // 可能包含多个 Binlog Event
};
```

### 2.3 统一提交协议

```go
// 存储层：统一提交
message UnifiedCommitRequest {
    uint64 db_id = 1;
    
    // Redo 数据
    bytes  redo_data = 2;
    uint64 redo_start_lsn = 3;
    uint64 redo_end_lsn = 4;
    
    // Binlog 数据
    bytes  binlog_data = 5;
    
    // GTID
    bytes  gtid_uuid = 6;
    uint64 gtid_seqno = 7;
    
    // 事务标识
    uint64 trx_id = 8;
    bytes  xid = 9;
    
    // 选项
    bool   sync = 10;
}

message UnifiedCommitResponse {
    bool   success = 1;
    uint64 durable_lsn = 2;
    uint64 binlog_pos = 3;
    string error = 4;
}
```

```go
func (s *StorageServer) UnifiedCommit(ctx context.Context,
    req *UnifiedCommitRequest) (*UnifiedCommitResponse, error) {
    
    // 1. 分配 LSN
    lsn := s.allocateLSN()
    
    // 2. 构造统一日志记录
    records := []LogRecord{
        // Redo 部分
        {Type: REDO, LSN: lsn, Data: req.RedoData},
        
        // Binlog 部分 (如果有)
        {Type: BINLOG, LSN: lsn, Data: req.BinlogData, GTID: req.Gtid},
        
        // 提交标记
        {Type: COMMIT, LSN: lsn, TrxID: req.TrxId, GTID: req.Gtid},
    }
    
    // 3. 原子写入
    if err := s.unifiedLogStore.WriteBatch(records); err != nil {
        return nil, err
    }
    
    // 4. 更新 GTID Set
    s.gtidSet.Add(req.GtidUuid, req.GtidSeqno)
    
    // 5. 同步 (如果需要)
    if req.Sync {
        s.unifiedLogStore.Sync()
    }
    
    return &UnifiedCommitResponse{
        Success:    true,
        DurableLsn: lsn,
        BinlogPos:  lsn,  // Binlog 位置 = LSN
    }, nil
}
```

## 3. GTID 管理

### 3.1 GTID Set 存储

```go
type GTIDSet struct {
    // UUID -> 已执行的序列号范围
    // 格式: uuid:1-100,200-300
    executed map[UUID][]Interval
    
    // 持久化 (定期 checkpoint)
    store    *GTIDStore
    
    mu       sync.RWMutex
}

type Interval struct {
    Start uint64
    End   uint64
}

func (g *GTIDSet) Add(uuid UUID, seqno uint64) {
    g.mu.Lock()
    defer g.mu.Unlock()
    
    intervals := g.executed[uuid]
    g.executed[uuid] = mergeInterval(intervals, seqno)
    
    // 异步持久化
    g.store.MarkDirty()
}

func (g *GTIDSet) String() string {
    // 生成 MySQL 格式的 GTID Set 字符串
    // 如: "uuid1:1-100,uuid2:1-50"
    var parts []string
    for uuid, intervals := range g.executed {
        for _, iv := range intervals {
            parts = append(parts, fmt.Sprintf("%s:%d-%d", 
                uuid, iv.Start, iv.End))
        }
    }
    return strings.Join(parts, ",")
}
```

### 3.2 GTID 持久化

```go
// GTID Set 作为特殊 Redo 记录持久化
type GTIDCheckpointRedo struct {
    Type     uint8  = MLOG_GTID_CHECKPOINT
    LSN      uint64
    GTIDSet  []GTIDRange
}

type GTIDRange struct {
    UUID   [16]byte
    Ranges []Interval
}

// 定期 checkpoint
func (g *GTIDStore) Checkpoint() error {
    g.mu.RLock()
    defer g.mu.RUnlock()
    
    // 写入 GTID 检查点
    redo := GTIDCheckpointRedo{
        LSN:     g.currentLSN,
        GTIDSet: g.gtidSet.ToRanges(),
    }
    
    return g.logStore.Write(redo)
}
```

## 4. 崩溃恢复

### 4.1 恢复流程

```go
func (s *StorageLayer) RecoverUnifiedLog() error {
    // 1. 扫描统一日志，找到最后有效位置
    lastValidLSN := s.unifiedLogStore.FindLastValidLSN()
    
    // 2. 恢复 GTID Set
    gtidSet := s.recoverGTIDSet(lastValidLSN)
    
    // 3. 识别未完成的事务
    incompleteTrxs := s.findIncompleteTrxs(lastValidLSN)
    
    // 4. 回滚未完成事务
    for _, trx := range incompleteTrxs {
        s.rollbackTrx(trx)
    }
    
    // 5. 设置恢复点
    s.durableLSN = lastValidLSN
    s.gtidSet = gtidSet
    
    return nil
}

func (s *StorageLayer) recoverGTIDSet(maxLSN uint64) *GTIDSet {
    // 1. 找到最近的 GTID checkpoint
    checkpoint := s.findLastGTIDCheckpoint(maxLSN)
    gtidSet := NewGTIDSet(checkpoint)
    
    // 2. 重放 checkpoint 之后的提交记录
    commits := s.scanCommitsAfter(checkpoint.LSN, maxLSN)
    for _, commit := range commits {
        gtidSet.Add(commit.GTID)
    }
    
    return gtidSet
}

func (s *StorageLayer) findIncompleteTrxs(maxLSN uint64) []*IncompleteTrx {
    // 扫描找到有 Prepare 但没有 Commit 的事务
    preparedTrxs := make(map[uint64]*IncompleteTrx)
    
    s.unifiedLogStore.Scan(0, maxLSN, func(rec LogRecord) {
        switch rec.Type {
        case MLOG_TRX_PREPARE:
            preparedTrxs[rec.TrxID] = &IncompleteTrx{
                TrxID: rec.TrxID,
                State: PREPARED,
            }
        case MLOG_TRX_COMMIT:
            delete(preparedTrxs, rec.TrxID)
        case MLOG_TRX_ROLLBACK:
            delete(preparedTrxs, rec.TrxID)
        }
    })
    
    // 检查是否有对应的 Binlog (XA 决策)
    var result []*IncompleteTrx
    for _, trx := range preparedTrxs {
        if s.hasBinlogForTrx(trx.TrxID, maxLSN) {
            // 有 Binlog，应该提交
            trx.Decision = COMMIT
        } else {
            // 没有 Binlog，应该回滚
            trx.Decision = ROLLBACK
        }
        result = append(result, trx)
    }
    
    return result
}
```

## 5. Binlog 服务 (外部从库)

### 5.1 标准 Binlog 协议适配

```go
// 从统一日志提取 Binlog，提供给外部从库
type BinlogAdapter struct {
    unifiedLog *UnifiedLogStore
    gtidSet    *GTIDSet
}

// 实现 MySQL Binlog Dump 协议
func (a *BinlogAdapter) HandleBinlogDump(conn *mysql.Conn, 
    startGTID string) error {
    
    // 1. 解析起始 GTID
    requestedGTID, _ := ParseGTIDSet(startGTID)
    
    // 2. 计算需要发送的 GTID 范围
    missingGTIDs := a.gtidSet.Subtract(requestedGTID)
    
    // 3. 找到起始 LSN
    startLSN := a.findLSNForGTID(missingGTIDs.First())
    
    // 4. 流式发送 Binlog
    a.unifiedLog.StreamBinlog(startLSN, func(rec LogRecord) error {
        if rec.Type != BINLOG {
            return nil  // 跳过非 Binlog 记录
        }
        
        // 转换为标准 Binlog Event
        events := a.convertToMySQLBinlogEvents(rec)
        for _, event := range events {
            if err := conn.WriteBinlogEvent(event); err != nil {
                return err
            }
        }
        return nil
    })
    
    return nil
}

func (a *BinlogAdapter) convertToMySQLBinlogEvents(rec LogRecord) []*BinlogEvent {
    // 统一日志的 Binlog 数据已经是标准格式
    // 只需要调整 position (统一日志 LSN -> Binlog Position)
    events := ParseBinlogEvents(rec.Data)
    
    for _, event := range events {
        // 使用 LSN 作为 Binlog Position
        event.Header.LogPos = rec.LSN
        event.Header.NextPos = rec.LSN + uint64(len(event.Data))
    }
    
    return events
}
```

### 5.2 GTID 查询接口

```go
// gRPC 接口
service GTIDService {
    // 获取已执行的 GTID Set
    rpc GetExecutedGTIDSet(GetGTIDRequest) returns (GetGTIDResponse);
    
    // 等待 GTID 执行完成
    rpc WaitForGTID(WaitGTIDRequest) returns (WaitGTIDResponse);
    
    // 查找 GTID 对应的 LSN
    rpc FindLSNByGTID(FindLSNRequest) returns (FindLSNResponse);
}

message GetGTIDResponse {
    string gtid_set = 1;  // MySQL 格式: "uuid:1-100,..."
    uint64 last_lsn = 2;
}
```

## 6. 依赖关系调整

### 6.1 计算层改造

```cpp
// 原有: 先写 Redo，再写 Binlog
// 改造后: 统一提交

class UnifiedCommitter {
public:
    void begin_transaction(trx_t* trx) {
        // 分配 GTID
        trx->gtid = allocate_gtid();
    }
    
    void commit_transaction(trx_t* trx) {
        // 1. 收集 Redo
        bytes redo_data = trx->redo_buffer.data();
        
        // 2. 收集 Binlog
        bytes binlog_data = trx->binlog_buffer.data();
        
        // 3. 统一提交
        UnifiedCommitRequest req;
        req.set_redo_data(redo_data);
        req.set_binlog_data(binlog_data);
        req.set_gtid(trx->gtid);
        req.set_trx_id(trx->id);
        
        auto resp = storage_client_->UnifiedCommit(req);
        
        // 4. 更新状态
        trx->commit_lsn = resp.durable_lsn();
    }
};
```

### 6.2 禁用本地 Binlog

```ini
# 计算层配置
log_bin = OFF                    # 禁用本地 Binlog
aurora_unified_log = ON          # 启用统一日志
aurora_gtid_mode = ON            # 启用 GTID
```

## 7. 配置

```go
type UnifiedLogConfig struct {
    // 日志
    LogBlockSize     int `default:"4096"`
    SyncMode         string `default:"fsync"`  // fsync, async
    
    // Binlog
    BinlogInlineThreshold int `default:"4096"`  // 内联阈值
    
    // GTID
    GTIDCheckpointInterval time.Duration `default:"10s"`
    
    // 外部复制
    MaxReplicationConnections int `default:"100"`
}
```

## 8. 开发任务

- [ ] 设计统一日志格式
- [ ] 实现 UnifiedCommit RPC
- [ ] 实现 GTID Set 管理
- [ ] 实现崩溃恢复
- [ ] 实现 Binlog 适配器
- [ ] 计算层改造
- [ ] GTID 服务
- [ ] 外部复制测试

## 9. 参考

- Skill 11: Binlog 模块
- MySQL GTID 文档
- Aurora 论文: 日志存储部分
