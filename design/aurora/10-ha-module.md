# Skill: 高可用模块 (HA Module)

## 1. 模块职责

实现存算分离架构下的高可用能力：
- **故障检测**: 检测 RW/RO/存储节点故障
- **自动 Failover**: RW 故障时自动选主
- **拓扑管理**: 动态调整集群拓扑
- **脑裂防护**: 防止多主写入

**实现语言**: Golang

## 2. 架构概览

```
┌─────────────────────────────────────────────────────────────────┐
│                         HA 架构                                  │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    Arbitration Service                   │   │
│  │                    (Etcd / Consul)                       │   │
│  │  • 选主仲裁                                              │   │
│  │  • 租约管理                                              │   │
│  │  • 元数据存储                                            │   │
│  └─────────────────────────────────────────────────────────┘   │
│           │              │              │                       │
│           ▼              ▼              ▼                       │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐              │
│  │  HA Agent   │ │  HA Agent   │ │  HA Agent   │              │
│  │  (RW Node)  │ │  (RO Node)  │ │  (RO Node)  │              │
│  └─────────────┘ └─────────────┘ └─────────────┘              │
│         │                                                       │
│         ▼                                                       │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    Storage Layer                         │   │
│  │                    (多副本容错)                          │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 3. 核心组件

### 3.1 HA Agent (每个计算节点部署)

```go
package ha

type HAAgent struct {
    nodeID       string
    role         NodeRole  // RW / RO / CANDIDATE
    
    // 外部依赖
    arbitrator   ArbitrationClient  // Etcd/Consul
    storageClient StorageClient
    
    // 内部状态
    leaseID      string
    leaseTTL     time.Duration
    
    // 健康检查
    healthChecker *HealthChecker
    
    // Failover 控制
    failoverMgr   *FailoverManager
}

func NewHAAgent(config *HAConfig) *HAAgent {
    return &HAAgent{
        nodeID:       config.NodeID,
        role:         RoleCandidate,
        arbitrator:   NewEtcdClient(config.EtcdEndpoints),
        leaseTTL:     config.LeaseTTL,
        healthChecker: NewHealthChecker(config),
    }
}

func (a *HAAgent) Start() error {
    // 1. 注册节点
    if err := a.registerNode(); err != nil {
        return err
    }
    
    // 2. 尝试成为 RW
    go a.campaignLoop()
    
    // 3. 启动健康检查
    go a.healthCheckLoop()
    
    // 4. 启动租约续期
    go a.leaseKeepaliveLoop()
    
    return nil
}
```

### 3.2 仲裁服务接口

```go
package ha

type ArbitrationClient interface {
    // 租约管理
    CreateLease(ttl time.Duration) (leaseID string, err error)
    KeepAliveLease(leaseID string) error
    RevokeLease(leaseID string) error
    
    // 选主
    Campaign(leaseID, nodeID string) (isLeader bool, err error)
    Resign(leaseID string) error
    GetLeader() (nodeID string, err error)
    WatchLeader(ctx context.Context) <-chan LeaderEvent
    
    // 节点注册
    RegisterNode(nodeID string, info NodeInfo) error
    ListNodes() ([]NodeInfo, error)
    WatchNodes(ctx context.Context) <-chan NodeEvent
    
    // 分布式锁 (Fencing)
    Lock(key string, leaseID string) error
    Unlock(key string) error
}

type NodeInfo struct {
    NodeID     string
    Role       NodeRole
    Endpoint   string
    Health     HealthStatus
    LSN        uint64  // 当前 LSN (用于选主)
    RegisterAt time.Time
}
```

### 3.3 健康检查器

```go
package ha

type HealthChecker struct {
    config     *HealthConfig
    targets    []HealthTarget
    results    map[string]*HealthResult
    mu         sync.RWMutex
}

type HealthTarget struct {
    Name     string
    Type     TargetType  // MYSQL, STORAGE, NETWORK
    Endpoint string
    Interval time.Duration
    Timeout  time.Duration
}

func (h *HealthChecker) Check(target HealthTarget) *HealthResult {
    switch target.Type {
    case TargetMySQL:
        return h.checkMySQL(target)
    case TargetStorage:
        return h.checkStorage(target)
    case TargetNetwork:
        return h.checkNetwork(target)
    }
    return nil
}

func (h *HealthChecker) checkMySQL(target HealthTarget) *HealthResult {
    // 1. TCP 连接检查
    conn, err := net.DialTimeout("tcp", target.Endpoint, target.Timeout)
    if err != nil {
        return &HealthResult{Healthy: false, Error: err}
    }
    defer conn.Close()
    
    // 2. MySQL 协议握手
    // 3. 执行简单查询 (SELECT 1)
    // 4. 检查 InnoDB 状态
    
    return &HealthResult{Healthy: true}
}

func (h *HealthChecker) checkStorage(target HealthTarget) *HealthResult {
    // 调用存储层 GetStatus RPC
    resp, err := storageClient.GetStatus(context.Background(), 
        &GetStatusRequest{})
    if err != nil {
        return &HealthResult{Healthy: false, Error: err}
    }
    
    return &HealthResult{
        Healthy: true,
        Extra: map[string]interface{}{
            "applied_lsn": resp.AppliedLsn,
            "durable_lsn": resp.DurableLsn,
        },
    }
}
```

## 4. Failover 流程

### 4.1 RW 故障检测

```go
func (a *HAAgent) detectRWFailure() bool {
    // 多重检测，避免误判
    checks := []func() bool{
        a.checkRWHeartbeat,     // 心跳超时
        a.checkRWLease,         // 租约失效
        a.checkStorageRWStatus, // 存储层视角
    }
    
    failCount := 0
    for _, check := range checks {
        if !check() {
            failCount++
        }
    }
    
    // 多数检测失败才认定故障
    return failCount >= 2
}
```

### 4.2 选主流程

```go
func (a *HAAgent) campaignLoop() {
    for {
        select {
        case <-a.ctx.Done():
            return
        default:
        }
        
        // 只有 RO 或 Candidate 才参与选主
        if a.role == RoleRW {
            time.Sleep(a.config.CampaignInterval)
            continue
        }
        
        // 检查是否需要选主
        leader, err := a.arbitrator.GetLeader()
        if err == nil && leader != "" {
            // 有 Leader，等待
            time.Sleep(a.config.CampaignInterval)
            continue
        }
        
        // 尝试成为 Leader
        if err := a.tryBecomeRW(); err != nil {
            log.Warnf("Campaign failed: %v", err)
        }
        
        time.Sleep(a.config.CampaignInterval)
    }
}

func (a *HAAgent) tryBecomeRW() error {
    // 1. 获取本节点 LSN
    myLSN := a.getLocalLSN()
    
    // 2. 获取所有候选节点
    nodes, _ := a.arbitrator.ListNodes()
    
    // 3. 检查是否有更高 LSN 的节点
    for _, node := range nodes {
        if node.Role == RoleRO && node.LSN > myLSN {
            // 有更适合的候选者，放弃
            return fmt.Errorf("node %s has higher LSN", node.NodeID)
        }
    }
    
    // 4. 尝试获取 Leader 租约
    isLeader, err := a.arbitrator.Campaign(a.leaseID, a.nodeID)
    if err != nil {
        return err
    }
    
    if isLeader {
        // 5. 获得 Leader，执行切换
        return a.promoteToRW()
    }
    
    return nil
}
```

### 4.3 提升为 RW

```go
func (a *HAAgent) promoteToRW() error {
    log.Infof("Promoting node %s to RW", a.nodeID)
    
    // 1. Fencing: 确保旧 RW 无法写入
    if err := a.fenceOldRW(); err != nil {
        return fmt.Errorf("fencing failed: %w", err)
    }
    
    // 2. 等待存储层 Redo 全部应用
    if err := a.waitStorageApply(); err != nil {
        return fmt.Errorf("wait storage apply failed: %w", err)
    }
    
    // 3. 通知 MySQL 切换为 RW 模式
    if err := a.switchMySQLToRW(); err != nil {
        return fmt.Errorf("switch MySQL failed: %w", err)
    }
    
    // 4. 更新节点角色
    a.role = RoleRW
    a.arbitrator.RegisterNode(a.nodeID, NodeInfo{
        NodeID: a.nodeID,
        Role:   RoleRW,
    })
    
    // 5. 通知其他节点
    a.broadcastRoleChange()
    
    log.Infof("Node %s is now RW", a.nodeID)
    return nil
}
```

## 5. Fencing (脑裂防护)

### 5.1 Fencing 机制

```go
// 确保旧 RW 无法继续写入
func (a *HAAgent) fenceOldRW() error {
    // 方案 1: 存储层 Fencing
    // 存储层只接受持有有效租约的节点写入
    err := a.storageClient.AcquireWriteLock(context.Background(),
        &AcquireWriteLockRequest{
            NodeID:  a.nodeID,
            LeaseID: a.leaseID,
        })
    if err != nil {
        return err
    }
    
    // 方案 2: 网络 Fencing (可选)
    // 通过 STONITH 隔离旧节点
    
    // 方案 3: 旧 RW 自检
    // 旧 RW 检测到租约失效后自动降级
    
    return nil
}
```

### 5.2 存储层写入检查

```go
// 存储层在接收 WriteRedo 时检查
func (s *StorageServer) WriteRedo(ctx context.Context, 
    req *WriteRedoRequest) (*WriteRedoResponse, error) {
    
    // 1. 检查写入者是否持有有效租约
    if !s.validateWriteLease(req.NodeID, req.LeaseID) {
        return nil, status.Error(codes.PermissionDenied, 
            "invalid write lease")
    }
    
    // 2. 正常处理写入
    return s.doWriteRedo(req)
}

func (s *StorageServer) validateWriteLease(nodeID, leaseID string) bool {
    s.writeLockMu.RLock()
    defer s.writeLockMu.RUnlock()
    
    return s.currentWriter == nodeID && s.currentLease == leaseID
}
```

## 6. 拓扑管理

### 6.1 集群拓扑

```go
type ClusterTopology struct {
    mu      sync.RWMutex
    rwNode  *NodeInfo
    roNodes []*NodeInfo
    version uint64
}

func (t *ClusterTopology) GetRW() *NodeInfo {
    t.mu.RLock()
    defer t.mu.RUnlock()
    return t.rwNode
}

func (t *ClusterTopology) UpdateRW(node *NodeInfo) {
    t.mu.Lock()
    defer t.mu.Unlock()
    t.rwNode = node
    t.version++
}

func (t *ClusterTopology) AddRO(node *NodeInfo) {
    t.mu.Lock()
    defer t.mu.Unlock()
    t.roNodes = append(t.roNodes, node)
    t.version++
}

func (t *ClusterTopology) RemoveRO(nodeID string) {
    t.mu.Lock()
    defer t.mu.Unlock()
    for i, n := range t.roNodes {
        if n.NodeID == nodeID {
            t.roNodes = append(t.roNodes[:i], t.roNodes[i+1:]...)
            break
        }
    }
    t.version++
}
```

### 6.2 动态扩缩容

```go
// 添加 RO 节点
func (a *HAAgent) AddRONode(endpoint string) error {
    // 1. 验证节点可达
    if err := a.healthChecker.CheckEndpoint(endpoint); err != nil {
        return err
    }
    
    // 2. 注册到仲裁服务
    nodeID := generateNodeID()
    a.arbitrator.RegisterNode(nodeID, NodeInfo{
        NodeID:   nodeID,
        Role:     RoleRO,
        Endpoint: endpoint,
    })
    
    // 3. 初始化 RO 节点
    // (RO 节点会自动从存储层同步数据)
    
    return nil
}

// 移除节点
func (a *HAAgent) RemoveNode(nodeID string) error {
    node, _ := a.arbitrator.GetNode(nodeID)
    
    if node.Role == RoleRW {
        return fmt.Errorf("cannot remove RW node directly")
    }
    
    // 从仲裁服务注销
    return a.arbitrator.UnregisterNode(nodeID)
}
```

## 7. 配置

```go
type HAConfig struct {
    // 节点配置
    NodeID       string
    Endpoint     string
    
    // 仲裁服务
    EtcdEndpoints []string
    
    // 租约配置
    LeaseTTL          time.Duration  // 默认 10s
    LeaseRenewInterval time.Duration // 默认 3s
    
    // 健康检查
    HealthCheckInterval time.Duration // 默认 1s
    HealthCheckTimeout  time.Duration // 默认 3s
    FailureThreshold    int           // 默认 3
    
    // Failover
    CampaignInterval    time.Duration // 默认 1s
    FailoverTimeout     time.Duration // 默认 30s
}
```

## 8. 监控指标

```go
var (
    haRoleGauge = prometheus.NewGaugeVec(
        prometheus.GaugeOpts{
            Name: "aurora_ha_role",
            Help: "Current HA role (1=RW, 0=RO)",
        },
        []string{"node_id"},
    )
    
    haFailoverTotal = prometheus.NewCounterVec(
        prometheus.CounterOpts{
            Name: "aurora_ha_failover_total",
            Help: "Total failover count",
        },
        []string{"from_node", "to_node"},
    )
    
    haLeaseRenewLatency = prometheus.NewHistogram(
        prometheus.HistogramOpts{
            Name:    "aurora_ha_lease_renew_latency_seconds",
            Help:    "Lease renew latency",
            Buckets: prometheus.ExponentialBuckets(0.001, 2, 10),
        },
    )
)
```

## 9. 开发任务

- [ ] 实现 HAAgent 框架
- [ ] 实现 Etcd ArbitrationClient
- [ ] 实现健康检查器
- [ ] 实现选主流程
- [ ] 实现 Fencing 机制
- [ ] 实现拓扑管理
- [ ] 存储层写锁支持
- [ ] 监控指标
- [ ] 集成测试
- [ ] 故障注入测试

## 10. 参考

- 主文档: mysql-sidecar-agent-design.md (HA 部分)
- Etcd 官方文档
- Raft 论文
