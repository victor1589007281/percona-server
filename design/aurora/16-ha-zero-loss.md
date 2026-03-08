# Skill: HA 零丢失快速切换 (HA Zero-Loss Fast Failover)

## 1. 问题背景

HA 切换需要满足：
1. **零数据丢失**: RPO = 0，所有已提交事务不丢失
2. **快速切换**: RTO < 30s，最小化业务中断
3. **脑裂防护**: 严格保证只有一个 RW

**实现语言**: Golang (HA Agent) + C++ (计算层配合)

## 2. 零丢失架构

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    零丢失 HA 架构                                            │
│                                                                             │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    写入确认流程                                      │   │
│  │                                                                      │   │
│  │  Client ──► RW ──► 存储层 ──► 持久化 (多副本) ──► 确认 ──► Client   │   │
│  │                        │                                             │   │
│  │                        │ 同步复制到多个存储节点                       │   │
│  │                        ▼                                             │   │
│  │              Quorum Write (3/5 或 2/3)                               │   │
│  │                                                                      │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  关键保证:                                                                  │
│  • 事务只有在 Redo 持久化到 Quorum 后才返回 commit                         │
│  • 新 RW 必须从 Quorum 恢复最新数据                                        │
│  • Fencing 确保旧 RW 无法继续写入                                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 3. Quorum 写入

### 3.1 多副本存储

```go
type QuorumWriter struct {
    replicas    []*StorageReplica
    totalCount  int  // 总副本数 (如 3)
    quorumCount int  // 确认数 (如 2)
}

func (w *QuorumWriter) Write(redo []byte, lsn uint64) error {
    // 1. 并行写入所有副本
    results := make(chan WriteResult, w.totalCount)
    
    for _, replica := range w.replicas {
        go func(r *StorageReplica) {
            err := r.Write(redo, lsn)
            results <- WriteResult{Replica: r, Error: err}
        }(replica)
    }
    
    // 2. 等待 Quorum 确认
    successCount := 0
    var firstError error
    
    for i := 0; i < w.totalCount; i++ {
        result := <-results
        if result.Error == nil {
            successCount++
            if successCount >= w.quorumCount {
                // Quorum 达成，可以返回成功
                return nil
            }
        } else if firstError == nil {
            firstError = result.Error
        }
    }
    
    // 3. Quorum 未达成
    return fmt.Errorf("quorum not reached: %d/%d, error: %v",
        successCount, w.quorumCount, firstError)
}
```

### 3.2 存储层副本一致性

```go
type StorageReplica struct {
    id         string
    endpoint   string
    durableLSN uint64  // 已持久化的最大 LSN
}

// 存储层保证副本间一致性
type ReplicaConsistencyManager struct {
    replicas []*StorageReplica
}

func (m *ReplicaConsistencyManager) GetMaxDurableLSN() uint64 {
    // 获取 Quorum 副本中的最大持久化 LSN
    var lsns []uint64
    for _, r := range m.replicas {
        lsns = append(lsns, r.durableLSN)
    }
    
    sort.Sort(sort.Reverse(sort.IntSlice(lsns)))
    
    // 返回第 quorum 个 LSN (保证 quorum 副本都有)
    return lsns[m.quorumCount-1]
}
```

## 4. 快速 Failover 流程

### 4.1 故障检测

```go
type FailureDetector struct {
    rwNode        *NodeInfo
    checkInterval time.Duration
    
    // 检测阈值
    heartbeatTimeout   time.Duration  // 心跳超时
    consecutiveFailure int            // 连续失败次数
}

func (d *FailureDetector) Run() {
    failureCount := 0
    
    for {
        time.Sleep(d.checkInterval)
        
        // 多路检测
        checks := []func() bool{
            d.checkHeartbeat,       // 心跳检测
            d.checkStorageView,     // 存储层视角
            d.checkArbitrator,      // 仲裁服务视角
        }
        
        failedChecks := 0
        for _, check := range checks {
            if !check() {
                failedChecks++
            }
        }
        
        // 多数检测失败
        if failedChecks >= 2 {
            failureCount++
            if failureCount >= d.consecutiveFailure {
                d.triggerFailover()
                return
            }
        } else {
            failureCount = 0
        }
    }
}

func (d *FailureDetector) checkStorageView() bool {
    // 询问存储层，RW 最后写入时间
    status := d.storageClient.GetRWStatus()
    return time.Since(status.LastWriteTime) < d.heartbeatTimeout
}
```

### 4.2 Fencing (旧 RW 隔离)

```go
// Fencing 三重保障
type FencingManager struct {
    arbitrator *ArbitrationClient
    storage    *StorageClient
}

func (f *FencingManager) FenceOldRW(oldRW *NodeInfo) error {
    // 1. 仲裁层 Fencing: 撤销旧 RW 的租约
    if err := f.arbitrator.RevokeLease(oldRW.LeaseID); err != nil {
        return err
    }
    
    // 2. 存储层 Fencing: 更新写入 Epoch
    // 只有持有新 Epoch 的节点才能写入
    newEpoch := f.storage.IncrementEpoch()
    
    // 3. 网络层 Fencing (可选): STONITH
    // f.stonith.FenceNode(oldRW.Endpoint)
    
    return nil
}

// 存储层检查 Epoch
func (s *StorageServer) WriteRedo(req *WriteRedoRequest) error {
    // 验证 Epoch
    if req.Epoch < s.currentEpoch {
        return ErrEpochTooOld  // 拒绝旧 Epoch 的写入
    }
    
    // 正常写入
    return s.doWriteRedo(req)
}
```

### 4.3 新 RW 选举与恢复

```go
type FailoverOrchestrator struct {
    candidates []*RONode
    storage    *StorageClient
    arbitrator *ArbitrationClient
}

func (o *FailoverOrchestrator) ExecuteFailover() (*NodeInfo, error) {
    startTime := time.Now()
    
    // Phase 1: Fencing (确保旧 RW 无法写入)
    if err := o.fencingMgr.FenceOldRW(o.oldRW); err != nil {
        return nil, err
    }
    phase1Duration := time.Since(startTime)
    
    // Phase 2: 选择新 RW (选 LSN 最大的)
    newRW := o.selectNewRW()
    phase2Duration := time.Since(startTime) - phase1Duration
    
    // Phase 3: 恢复 (等待存储层完成 Redo 应用)
    if err := o.waitStorageRecovery(newRW); err != nil {
        return nil, err
    }
    phase3Duration := time.Since(startTime) - phase1Duration - phase2Duration
    
    // Phase 4: 激活新 RW
    if err := o.activateNewRW(newRW); err != nil {
        return nil, err
    }
    totalDuration := time.Since(startTime)
    
    log.Infof("Failover completed: fence=%v, select=%v, recover=%v, total=%v",
        phase1Duration, phase2Duration, phase3Duration, totalDuration)
    
    return newRW, nil
}

func (o *FailoverOrchestrator) selectNewRW() *RONode {
    var bestCandidate *RONode
    var bestLSN uint64
    
    for _, ro := range o.candidates {
        // 获取 RO 节点已应用的 LSN
        appliedLSN := ro.GetAppliedLSN()
        
        if appliedLSN > bestLSN {
            bestLSN = appliedLSN
            bestCandidate = ro
        }
    }
    
    return bestCandidate
}

func (o *FailoverOrchestrator) waitStorageRecovery(newRW *RONode) error {
    // 获取存储层持久化的最大 LSN
    maxDurableLSN := o.storage.GetMaxDurableLSN()
    
    // 等待存储层将所有 Redo 应用完成
    timeout := time.After(30 * time.Second)
    ticker := time.NewTicker(100 * time.Millisecond)
    
    for {
        select {
        case <-timeout:
            return ErrRecoveryTimeout
        case <-ticker.C:
            appliedLSN := o.storage.GetAppliedLSN()
            if appliedLSN >= maxDurableLSN {
                return nil  // 恢复完成
            }
        }
    }
}
```

## 5. RTO 优化

### 5.1 并行化

```go
func (o *FailoverOrchestrator) FastFailover() (*NodeInfo, error) {
    var wg sync.WaitGroup
    errCh := make(chan error, 3)
    
    // 并行执行
    wg.Add(3)
    
    // 1. Fencing
    go func() {
        defer wg.Done()
        if err := o.fencingMgr.FenceOldRW(o.oldRW); err != nil {
            errCh <- err
        }
    }()
    
    // 2. 选择新 RW
    var newRW *RONode
    go func() {
        defer wg.Done()
        newRW = o.selectNewRW()
    }()
    
    // 3. 通知存储层准备切换
    go func() {
        defer wg.Done()
        o.storage.PrepareFailover()
    }()
    
    wg.Wait()
    
    // 检查错误
    select {
    case err := <-errCh:
        return nil, err
    default:
    }
    
    // 激活新 RW (必须在 Fencing 完成后)
    return o.activateNewRW(newRW)
}
```

### 5.2 预热 RO 节点

```go
// RO 节点预热，减少切换后的冷启动
type ROPrewarmer struct {
    roNode *RONode
}

func (p *ROPrewarmer) Prewarm() {
    // 1. 保持 Redo 应用尽可能实时
    p.roNode.SetReplicationMode(SYNC)  // 尽量同步
    
    // 2. 预加载热点数据到 Buffer Pool
    hotPages := p.getHotPages()
    for _, page := range hotPages {
        p.roNode.PrefetchPage(page)
    }
    
    // 3. 预编译常用 SQL
    for _, sql := range p.getFrequentSQLs() {
        p.roNode.PreparePlan(sql)
    }
}
```

### 5.3 RTO 时间分解

```
目标 RTO < 30s 分解:
─────────────────────────────────────────────────────────
Phase 1: 故障检测        5-10s   (多次检测 + 确认)
Phase 2: Fencing         1-2s    (并行撤销租约 + 更新 Epoch)
Phase 3: 选举新 RW       < 1s    (选 LSN 最大的 RO)
Phase 4: 存储层恢复      5-15s   (等待 Redo 应用完成)
Phase 5: 激活新 RW       1-2s    (切换模式 + 通知)
─────────────────────────────────────────────────────────
总计:                    13-30s
```

## 6. 数据验证

### 6.1 切换后验证

```go
func (o *FailoverOrchestrator) VerifyAfterFailover(newRW *NodeInfo) error {
    // 1. 验证 GTID Set 一致性
    storageGTID := o.storage.GetGTIDSet()
    newRWGTID := newRW.GetGTIDSet()
    
    if !storageGTID.Equal(newRWGTID) {
        return fmt.Errorf("GTID mismatch: storage=%v, newRW=%v",
            storageGTID, newRWGTID)
    }
    
    // 2. 验证 LSN 一致性
    storageLSN := o.storage.GetMaxDurableLSN()
    newRWLSN := newRW.GetAppliedLSN()
    
    if newRWLSN < storageLSN {
        return fmt.Errorf("LSN gap: storage=%d, newRW=%d",
            storageLSN, newRWLSN)
    }
    
    // 3. 验证数据完整性 (抽样)
    return o.verifyDataIntegrity(newRW)
}
```

## 7. 监控指标

```go
var (
    failoverDuration = prometheus.NewHistogramVec(
        prometheus.HistogramOpts{
            Name:    "aurora_failover_duration_seconds",
            Help:    "Failover duration by phase",
            Buckets: prometheus.LinearBuckets(1, 2, 15),
        },
        []string{"phase"},
    )
    
    replicationLag = prometheus.NewGauge(
        prometheus.GaugeOpts{
            Name: "aurora_replication_lag_seconds",
            Help: "Replication lag between RW and RO",
        },
    )
    
    fencingSuccess = prometheus.NewCounter(
        prometheus.CounterOpts{
            Name: "aurora_fencing_success_total",
            Help: "Successful fencing operations",
        },
    )
)
```

## 8. 配置

```go
type HAZeroLossConfig struct {
    // 故障检测
    HeartbeatInterval     time.Duration `default:"1s"`
    HeartbeatTimeout      time.Duration `default:"3s"`
    ConsecutiveFailure    int           `default:"3"`
    
    // Quorum
    TotalReplicas         int           `default:"3"`
    QuorumCount           int           `default:"2"`
    
    // Failover
    FailoverTimeout       time.Duration `default:"30s"`
    RecoveryTimeout       time.Duration `default:"20s"`
    
    // 预热
    EnablePrewarm         bool          `default:"true"`
    PrewarmHotPageCount   int           `default:"10000"`
}
```

## 9. 开发任务

- [ ] 实现 Quorum 写入
- [ ] 实现多路故障检测
- [ ] 实现 Fencing (仲裁 + 存储 Epoch)
- [ ] 实现快速 Failover 流程
- [ ] 实现 RO 预热
- [ ] 切换后验证
- [ ] 监控指标
- [ ] 故障注入测试

## 10. 参考

- Skill 10: HA 模块
- Raft 论文: Leader Election
- Aurora 论文: Failover 部分
