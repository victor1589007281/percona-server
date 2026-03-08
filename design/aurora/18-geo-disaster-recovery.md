# Skill: 跨城容灾 (Geo Disaster Recovery)

## 1. 模块职责

提供跨城、跨存储集群的容灾能力：
- **跨城复制**: 异步复制到远程数据中心
- **跨集群容灾**: 支持不同存储集群间的数据同步
- **灾难切换**: 灾难时快速切换到备集群

**实现语言**: Golang

## 2. 架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         跨城容灾架构                                         │
│                                                                             │
│  主数据中心 (北京)                        备数据中心 (上海)                  │
│  ─────────────────                        ─────────────────                 │
│                                                                             │
│  ┌──────────────────┐                    ┌──────────────────┐              │
│  │  Aurora Cluster  │                    │  Aurora Cluster  │              │
│  │  (Primary)       │                    │  (Standby)       │              │
│  │                  │                    │                  │              │
│  │  ┌────┐ ┌────┐  │                    │  ┌────┐ ┌────┐  │              │
│  │  │ RW │ │ RO │  │                    │  │ RO │ │ RO │  │              │
│  │  └────┘ └────┘  │                    │  └────┘ └────┘  │              │
│  │       │         │                    │       ▲         │              │
│  │       ▼         │                    │       │         │              │
│  │  ┌──────────┐   │    Redo Stream     │  ┌──────────┐   │              │
│  │  │ Storage  │───┼──────────────────►│  │ Storage  │   │              │
│  │  │ Cluster  │   │    (异步)          │  │ Cluster  │   │              │
│  │  └──────────┘   │                    │  └──────────┘   │              │
│  └──────────────────┘                    └──────────────────┘              │
│                                                                             │
│  同步策略:                                                                  │
│  • 本地: 同步 Quorum 写入 (RPO=0)                                          │
│  • 跨城: 异步复制 (RPO=秒级)                                               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 3. 跨城复制

### 3.1 复制拓扑

```go
type GeoReplicationTopology struct {
    // 主集群
    PrimaryCluster *ClusterInfo
    
    // 备集群列表 (支持多个)
    StandbyClusters []*StandbyClusterInfo
}

type ClusterInfo struct {
    ID        string
    Region    string
    Endpoint  string
    StorageEndpoint string
}

type StandbyClusterInfo struct {
    ClusterInfo
    
    // 复制配置
    ReplicationMode ReplicationMode  // ASYNC, SEMI_SYNC
    MaxLagSeconds   int              // 最大允许延迟
    
    // 状态
    CurrentLSN      uint64
    CurrentGTID     string
    LagSeconds      float64
}

type ReplicationMode int
const (
    ReplicationAsync    ReplicationMode = iota  // 异步
    ReplicationSemiSync                         // 半同步
)
```

### 3.2 Redo 流复制

```go
type GeoReplicator struct {
    primaryStorage   *StorageClient
    standbyStorages  []*StorageClient
    
    // 配置
    config          *GeoReplicationConfig
    
    // 状态
    lastReplicatedLSN uint64
}

func (r *GeoReplicator) Run() {
    for {
        // 1. 从主集群拉取 Redo
        redoBatch, err := r.primaryStorage.FetchRedo(
            r.lastReplicatedLSN,
            r.config.BatchSize,
        )
        if err != nil {
            log.Errorf("Fetch redo failed: %v", err)
            time.Sleep(r.config.RetryInterval)
            continue
        }
        
        if len(redoBatch.Records) == 0 {
            time.Sleep(r.config.PollInterval)
            continue
        }
        
        // 2. 发送到所有备集群
        var wg sync.WaitGroup
        for _, standby := range r.standbyStorages {
            wg.Add(1)
            go func(s *StorageClient) {
                defer wg.Done()
                r.replicateToStandby(s, redoBatch)
            }(standby)
        }
        wg.Wait()
        
        // 3. 更新进度
        r.lastReplicatedLSN = redoBatch.LastLSN
    }
}

func (r *GeoReplicator) replicateToStandby(
    standby *StorageClient,
    batch *RedoBatch) error {
    
    // 压缩传输 (减少跨城带宽)
    compressed := zstd.Compress(batch.Data)
    
    // 发送到备集群
    return standby.WriteRedoRemote(&WriteRedoRemoteRequest{
        SourceClusterID: r.config.PrimaryClusterID,
        StartLSN:        batch.StartLSN,
        EndLSN:          batch.EndLSN,
        CompressedData:  compressed,
        GTID:            batch.GTID,
    })
}
```

### 3.3 半同步复制

```go
// 半同步: 至少一个备集群确认
type SemiSyncReplicator struct {
    GeoReplicator
    
    // 等待确认的事务
    pendingConfirms map[uint64]*PendingConfirm
    mu              sync.Mutex
}

type PendingConfirm struct {
    LSN        uint64
    WaitCh     chan struct{}
    Confirmed  bool
    ConfirmBy  string  // 哪个备集群确认
}

func (r *SemiSyncReplicator) WaitForRemoteConfirm(lsn uint64, timeout time.Duration) error {
    r.mu.Lock()
    pending := &PendingConfirm{
        LSN:    lsn,
        WaitCh: make(chan struct{}),
    }
    r.pendingConfirms[lsn] = pending
    r.mu.Unlock()
    
    defer func() {
        r.mu.Lock()
        delete(r.pendingConfirms, lsn)
        r.mu.Unlock()
    }()
    
    // 等待确认或超时
    select {
    case <-pending.WaitCh:
        return nil
    case <-time.After(timeout):
        // 超时降级为异步
        return ErrSemiSyncTimeout
    }
}

func (r *SemiSyncReplicator) OnRemoteConfirm(clusterID string, lsn uint64) {
    r.mu.Lock()
    defer r.mu.Unlock()
    
    // 确认所有 <= lsn 的事务
    for pendingLSN, pending := range r.pendingConfirms {
        if pendingLSN <= lsn && !pending.Confirmed {
            pending.Confirmed = true
            pending.ConfirmBy = clusterID
            close(pending.WaitCh)
        }
    }
}
```

## 4. 备集群管理

### 4.1 备集群状态

```go
type StandbyCluster struct {
    info          *StandbyClusterInfo
    storageClient *StorageClient
    
    // 健康检查
    lastHealthCheck time.Time
    isHealthy       bool
    
    // 复制状态
    appliedLSN      uint64
    appliedGTID     string
    lagMonitor      *LagMonitor
}

func (s *StandbyCluster) UpdateStatus() {
    // 获取备集群状态
    status, err := s.storageClient.GetStatus()
    if err != nil {
        s.isHealthy = false
        return
    }
    
    s.isHealthy = true
    s.appliedLSN = status.AppliedLSN
    s.appliedGTID = status.GTID
    s.lastHealthCheck = time.Now()
    
    // 计算延迟
    s.lagMonitor.UpdateLag(s.appliedLSN)
}

type LagMonitor struct {
    primaryLSN      uint64
    standbyLSN      uint64
    primaryThroughput float64  // LSN/秒
}

func (m *LagMonitor) GetLagSeconds() float64 {
    lsnGap := m.primaryLSN - m.standbyLSN
    if m.primaryThroughput == 0 {
        return 0
    }
    return float64(lsnGap) / m.primaryThroughput
}
```

### 4.2 备集群存储层

```go
// 备集群存储层：接收并应用远程 Redo
type StandbyStorageLayer struct {
    *StorageLayer
    
    // 复制源
    sourceClusterID string
    
    // 冲突检测
    conflictChecker *ConflictChecker
}

func (s *StandbyStorageLayer) WriteRedoRemote(req *WriteRedoRemoteRequest) error {
    // 1. 解压
    data := zstd.Decompress(req.CompressedData)
    
    // 2. 验证源
    if req.SourceClusterID != s.sourceClusterID {
        return ErrInvalidSource
    }
    
    // 3. 检测 LSN 间隙
    if req.StartLSN > s.appliedLSN+1 {
        return ErrLSNGap
    }
    
    // 4. 写入并应用
    return s.WriteAndApplyRedo(data, req.StartLSN, req.EndLSN)
}
```

## 5. 灾难切换

### 5.1 切换流程

```go
type GeoFailoverOrchestrator struct {
    primaryCluster  *ClusterInfo
    standbyClusters []*StandbyCluster
    
    // 仲裁
    globalArbitrator *GlobalArbitrator
}

// 计划内切换 (无数据丢失)
func (o *GeoFailoverOrchestrator) PlannedSwitchover(
    targetClusterID string) error {
    
    // 1. 停止主集群写入
    if err := o.stopPrimaryWrites(); err != nil {
        return err
    }
    
    // 2. 等待备集群追上
    target := o.getStandbyCluster(targetClusterID)
    if err := o.waitStandbyCatchUp(target); err != nil {
        return err
    }
    
    // 3. 切换角色
    if err := o.swapRoles(o.primaryCluster, target); err != nil {
        return err
    }
    
    return nil
}

// 灾难切换 (可能有数据丢失)
func (o *GeoFailoverOrchestrator) DisasterFailover(
    targetClusterID string) (*FailoverResult, error) {
    
    // 1. 确认主集群不可用
    if o.isPrimaryAlive() {
        return nil, ErrPrimaryStillAlive
    }
    
    // 2. 选择数据最新的备集群
    target := o.selectBestStandby()
    if target == nil {
        target = o.getStandbyCluster(targetClusterID)
    }
    
    // 3. 计算数据丢失
    dataLoss := o.calculateDataLoss(target)
    
    // 4. 获取全局仲裁确认
    if err := o.globalArbitrator.ConfirmFailover(target.info.ID); err != nil {
        return nil, err
    }
    
    // 5. 提升备集群为主
    if err := o.promoteStandby(target); err != nil {
        return nil, err
    }
    
    return &FailoverResult{
        NewPrimary:    target.info.ID,
        DataLossLSN:   dataLoss.LSN,
        DataLossGTID:  dataLoss.GTID,
    }, nil
}

func (o *GeoFailoverOrchestrator) waitStandbyCatchUp(
    target *StandbyCluster) error {
    
    primaryLSN := o.getPrimaryLSN()
    
    timeout := time.After(5 * time.Minute)
    ticker := time.NewTicker(1 * time.Second)
    
    for {
        select {
        case <-timeout:
            return ErrCatchUpTimeout
        case <-ticker.C:
            if target.appliedLSN >= primaryLSN {
                return nil
            }
            log.Infof("Standby catching up: %d/%d", 
                target.appliedLSN, primaryLSN)
        }
    }
}
```

### 5.2 全局仲裁

```go
// 跨城仲裁服务 (需要独立部署在第三方位置)
type GlobalArbitrator struct {
    // 三地部署: 北京、上海、广州各一个仲裁节点
    nodes []*ArbitrationNode
}

func (a *GlobalArbitrator) ConfirmFailover(newPrimaryID string) error {
    // 需要多数仲裁节点同意
    votes := make(chan bool, len(a.nodes))
    
    for _, node := range a.nodes {
        go func(n *ArbitrationNode) {
            vote, _ := n.VoteForFailover(newPrimaryID)
            votes <- vote
        }(node)
    }
    
    // 收集投票
    agreeCount := 0
    for i := 0; i < len(a.nodes); i++ {
        if <-votes {
            agreeCount++
        }
    }
    
    // 多数同意
    if agreeCount > len(a.nodes)/2 {
        return nil
    }
    
    return ErrFailoverRejected
}
```

## 6. 数据一致性

### 6.1 冲突检测

```go
// 切换后检测是否有冲突事务
type ConflictDetector struct {
}

func (d *ConflictDetector) DetectConflicts(
    oldPrimaryGTID string,
    newPrimaryGTID string) []ConflictTransaction {
    
    oldSet, _ := ParseGTIDSet(oldPrimaryGTID)
    newSet, _ := ParseGTIDSet(newPrimaryGTID)
    
    // 找到在旧主有但新主没有的事务
    // 这些事务可能丢失
    missing := oldSet.Subtract(newSet)
    
    var conflicts []ConflictTransaction
    for _, gtid := range missing.All() {
        conflicts = append(conflicts, ConflictTransaction{
            GTID:   gtid,
            Status: TransactionLost,
        })
    }
    
    return conflicts
}
```

### 6.2 数据修复

```go
// 灾难后数据修复
type DataRepairer struct {
    primaryStorage *StorageClient
    backupStorage  BackupStorage
}

// 尝试从备份恢复丢失的事务
func (r *DataRepairer) RepairFromBackup(
    lostTransactions []ConflictTransaction) error {
    
    for _, tx := range lostTransactions {
        // 从备份查找事务数据
        txData, err := r.backupStorage.FindTransaction(tx.GTID)
        if err != nil {
            log.Warnf("Cannot find transaction %s in backup", tx.GTID)
            continue
        }
        
        // 重放事务 (如果不冲突)
        if !r.wouldConflict(txData) {
            r.replayTransaction(txData)
        }
    }
    
    return nil
}
```

## 7. 监控

### 7.1 复制延迟监控

```go
var (
    geoReplicationLag = prometheus.NewGaugeVec(
        prometheus.GaugeOpts{
            Name: "aurora_geo_replication_lag_seconds",
            Help: "Replication lag to standby cluster",
        },
        []string{"source_cluster", "target_cluster"},
    )
    
    geoReplicationThroughput = prometheus.NewCounterVec(
        prometheus.CounterOpts{
            Name: "aurora_geo_replication_bytes_total",
            Help: "Bytes replicated to standby cluster",
        },
        []string{"source_cluster", "target_cluster"},
    )
)
```

### 7.2 健康检查

```go
func (m *GeoMonitor) CheckHealth() *GeoHealthReport {
    report := &GeoHealthReport{}
    
    // 检查主集群
    report.PrimaryStatus = m.checkClusterHealth(m.primaryCluster)
    
    // 检查所有备集群
    for _, standby := range m.standbyClusters {
        status := StandbyStatus{
            ClusterID: standby.info.ID,
            Health:    m.checkClusterHealth(standby.info),
            LagSeconds: standby.lagMonitor.GetLagSeconds(),
            AppliedLSN: standby.appliedLSN,
        }
        report.StandbyStatuses = append(report.StandbyStatuses, status)
    }
    
    // 检查网络连通性
    report.NetworkStatus = m.checkCrossRegionNetwork()
    
    return report
}
```

## 8. 配置

```go
type GeoDisasterRecoveryConfig struct {
    // 复制
    ReplicationMode        ReplicationMode `default:"async"`
    SemiSyncTimeout        time.Duration   `default:"500ms"`
    MaxAllowedLagSeconds   int             `default:"60"`
    
    // 带宽控制
    MaxReplicationBandwidth int64          `default:"104857600"`  // 100MB/s
    CompressionEnabled      bool           `default:"true"`
    
    // 切换
    PlannedSwitchoverTimeout time.Duration `default:"5m"`
    DisasterFailoverTimeout  time.Duration `default:"2m"`
    
    // 仲裁
    GlobalArbitratorEndpoints []string
}
```

## 9. 开发任务

- [ ] 实现 Redo 流复制
- [ ] 实现半同步复制
- [ ] 实现备集群管理
- [ ] 实现计划内切换
- [ ] 实现灾难切换
- [ ] 实现全局仲裁
- [ ] 实现冲突检测
- [ ] 跨城网络优化
- [ ] 监控指标
- [ ] 演练工具

## 10. 参考

- Aurora Global Database 文档
- MySQL Group Replication
