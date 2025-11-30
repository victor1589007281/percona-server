# 控制平面设计文档

## 1. 模块概述

控制平面使用 **Golang** 开发，负责集群管理、监控和故障切换。

### 1.1 核心职责

| 职责 | 说明 |
|------|------|
| 集群管理 | 实例创建、删除、配置变更 |
| 健康监控 | 实时监控所有组件状态 |
| 故障切换 | 检测故障并自动切换 Writer |
| 调度服务 | Reader 分配、负载均衡 |
| API 网关 | 提供管理 API |

### 1.2 模块架构图

```mermaid
graph TB
    subgraph "控制平面（Golang）"
        subgraph "API 层"
            GRPC[**gRPC Server**<br/>:9000]
            REST[**REST Gateway**<br/>:8080]
        end
        
        subgraph "核心服务"
            ClusterMgr[**Cluster Manager**<br/>集群管理]
            Monitor[**Monitor Service**<br/>健康监控]
            Failover[**Failover Controller**<br/>故障切换]
            Scheduler[**Scheduler**<br/>调度服务]
        end
        
        subgraph "存储"
            StateStore[**State Store**<br/>etcd 客户端]
        end
    end
    
    subgraph "外部依赖"
        etcd[(etcd 集群)]
        Compute[计算层实例]
        Storage[存储层节点]
        Meta[元数据服务]
    end
    
    GRPC --> ClusterMgr
    GRPC --> Monitor
    GRPC --> Failover
    REST --> GRPC
    
    ClusterMgr --> StateStore
    Monitor --> StateStore
    Failover --> StateStore
    
    StateStore --> etcd
    
    Monitor --> Compute
    Failover --> Compute
    Failover --> Storage
    ClusterMgr --> Meta
    
    style GRPC fill:#e1e1ff,stroke:#333,stroke-width:2px,color:#000
    style Failover fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style Monitor fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
```

---

## 2. 内部时序图

### 2.1 健康检查流程

```mermaid
sequenceDiagram
    participant Monitor as Monitor Service
    participant Writer as Writer Instance
    participant Reader as Reader Instance
    participant Storage as Storage Node
    participant State as State Store

    loop 每 5 秒
        par 并行检查所有组件
            Monitor->>Writer: 1a. gRPC: HealthCheck()
            Monitor->>Reader: 1b. gRPC: HealthCheck()
            Monitor->>Storage: 1c. gRPC: HealthCheck()
        end
        
        Writer-->>Monitor: 2a. HealthCheckResponse
        Reader-->>Monitor: 2b. HealthCheckResponse
        Storage-->>Monitor: 2c. HealthCheckResponse
        
        Monitor->>Monitor: 3. 分析健康状态
        
        alt 发现异常
            Monitor->>State: 4a. 更新状态
            Monitor->>Monitor: 5a. 发送告警
            
            alt Writer 故障
                Monitor->>Monitor: 6a. 触发 Failover
            end
        else 正常
            Monitor->>State: 4b. 更新心跳时间
        end
    end
```

### 2.2 故障切换完整流程

```mermaid
sequenceDiagram
    participant Monitor as Monitor Service
    participant Failover as Failover Controller
    participant OldWriter as Old Writer
    participant Reader1 as Reader 1
    participant Reader2 as Reader 2
    participant Storage as Storage Layer
    participant Meta as Metadata Service
    participant DNS as DNS Manager

    Note over Monitor,OldWriter: 阶段 1：故障检测
    Monitor->>OldWriter: 1. HealthCheck()
    Note over OldWriter: 无响应
    Monitor->>Monitor: 2. 连续 3 次失败
    Monitor->>Failover: 3. TriggerFailover(writer_id)
    
    Note over Failover,Storage: 阶段 2：冻结存储层
    Failover->>Storage: 4. gRPC: FreezeWrites(volume_id)
    Storage->>Storage: 5. 停止接受新 Redo
    Storage-->>Failover: 6. final_vdl = 50000
    
    Note over Failover,Reader2: 阶段 3：选择新 Writer
    par 查询 Reader 状态
        Failover->>Reader1: 7a. GetInstanceStatus()
        Failover->>Reader2: 7b. GetInstanceStatus()
    end
    Reader1-->>Failover: 8a. lsn = 49800
    Reader2-->>Failover: 8b. lsn = 49900
    
    Failover->>Failover: 9. 选择 LSN 最高的 Reader2
    
    Note over Failover,Reader2: 阶段 4：提升 Reader
    Failover->>Reader2: 10. gRPC: CatchUp(target_lsn=50000)
    Reader2->>Storage: 11. GetRedoLogs(49900, 50000)
    Storage-->>Reader2: 12. 返回增量 Redo
    Reader2->>Reader2: 13. 应用 Redo
    Reader2-->>Failover: 14. CatchUp 完成
    
    Failover->>Reader2: 15. gRPC: PromoteToWriter()
    Reader2->>Reader2: 16. 切换为 Writer 模式
    Reader2-->>Failover: 17. Promote 成功
    
    Note over Failover,DNS: 阶段 5：更新元数据
    Failover->>Meta: 18. UpdateWriter(reader2)
    Failover->>Storage: 19. UnfreezeWrites()
    Failover->>DNS: 20. UpdateEndpoint(new_writer_ip)
    
    Failover->>Reader2: 21. StartAcceptingConnections()
    Note over Reader2: 新 Writer 开始服务
```

### 2.3 创建集群流程

```mermaid
sequenceDiagram
    participant Client as Admin Client
    participant API as API Gateway
    participant Cluster as Cluster Manager
    participant Meta as Metadata Service
    participant Compute as Compute Service
    participant Storage as Storage Layer
    participant State as State Store

    Client->>API: 1. CreateCluster(config)
    API->>Cluster: 2. 创建集群请求
    
    Cluster->>State: 3. 检查集群是否存在
    State-->>Cluster: 4. 不存在
    
    Note over Cluster,Meta: 创建 Volume
    Cluster->>Meta: 5. gRPC: CreateVolume(volume_config)
    Meta-->>Cluster: 6. volume_id
    
    Note over Cluster,Storage: 初始化存储
    Cluster->>Storage: 7. gRPC: InitializeVolume(volume_id)
    Storage-->>Cluster: 8. 初始化成功
    
    Note over Cluster,Compute: 创建 Writer
    Cluster->>Compute: 9. gRPC: CreateInstance(writer_config)
    Compute-->>Cluster: 10. writer_endpoint
    
    Note over Cluster,Compute: 创建 Readers
    par 并行创建
        Cluster->>Compute: 11a. CreateInstance(reader_config)
        Cluster->>Compute: 11b. CreateInstance(reader_config)
    end
    Compute-->>Cluster: 12a. reader1_endpoint
    Compute-->>Cluster: 12b. reader2_endpoint
    
    Cluster->>State: 13. 保存集群信息
    Cluster-->>API: 14. 返回集群信息
    API-->>Client: 15. CreateClusterResponse
```

### 2.4 添加 Reader 流程

```mermaid
sequenceDiagram
    participant Client as Admin Client
    participant Cluster as Cluster Manager
    participant Meta as Metadata Service
    participant Compute as Compute Service
    participant Storage as Storage Layer

    Client->>Cluster: 1. AddReader(cluster_id, config)
    
    Cluster->>Meta: 2. GetCluster(cluster_id)
    Meta-->>Cluster: 3. 返回集群信息（volume_id）
    
    Cluster->>Compute: 4. CreateInstance(reader_config)
    Note over Compute: 配置 volume_id
    Compute->>Compute: 5. 启动 MySQL 实例
    Compute->>Storage: 6. 开始同步 Redo
    Compute-->>Cluster: 7. reader_endpoint
    
    Cluster->>Meta: 8. RegisterInstance(reader)
    Meta-->>Cluster: 9. 注册成功
    
    Cluster-->>Client: 10. AddReaderResponse
```

---

## 3. 核心组件设计

### 3.1 Failover Controller

```go
// internal/failover/controller.go

type FailoverController struct {
    computeClient  ComputeServiceClient
    storageClient  StorageServiceClient
    metadataClient MetadataServiceClient
    stateStore     StateStore
}

type FailoverContext struct {
    FailoverID   string
    ClusterID    string
    OldWriterID  string
    NewWriterID  string
    FinalVDL     int64
    State        FailoverState
    StartTime    time.Time
}

// 执行 Failover
func (c *FailoverController) ExecuteFailover(ctx context.Context, clusterID, oldWriterID string) (*FailoverContext, error) {
    fo := &FailoverContext{
        FailoverID:  generateID(),
        ClusterID:   clusterID,
        OldWriterID: oldWriterID,
        State:       StateStarted,
        StartTime:   time.Now(),
    }
    
    // 1. 冻结存储层
    finalVDL, err := c.freezeStorage(ctx, clusterID)
    if err != nil {
        return nil, err
    }
    fo.FinalVDL = finalVDL
    fo.State = StateFrozen
    
    // 2. 选择新 Writer
    newWriter, err := c.selectNewWriter(ctx, clusterID, finalVDL)
    if err != nil {
        c.unfreezeStorage(ctx, clusterID)
        return nil, err
    }
    fo.NewWriterID = newWriter
    
    // 3. 提升 Reader
    if err := c.promoteReader(ctx, newWriter, finalVDL); err != nil {
        c.unfreezeStorage(ctx, clusterID)
        return nil, err
    }
    fo.State = StatePromoted
    
    // 4. 更新元数据
    if err := c.updateMetadata(ctx, fo); err != nil {
        return nil, err
    }
    
    // 5. 解冻存储层
    c.unfreezeStorage(ctx, clusterID)
    
    // 6. 更新 DNS
    c.updateDNS(ctx, fo)
    
    fo.State = StateCompleted
    return fo, nil
}

// 选择新 Writer
func (c *FailoverController) selectNewWriter(ctx context.Context, clusterID string, targetVDL int64) (string, error) {
    readers := c.getClusterReaders(ctx, clusterID)
    
    var bestReader string
    var bestLSN int64 = -1
    
    for _, reader := range readers {
        status, err := c.computeClient.GetInstanceStatus(ctx, reader)
        if err != nil {
            continue
        }
        if status.IsHealthy && status.CurrentLsn > bestLSN {
            bestLSN = status.CurrentLsn
            bestReader = reader
        }
    }
    
    if bestReader == "" {
        return "", ErrNoHealthyReader
    }
    return bestReader, nil
}
```

### 3.2 Monitor Service

```go
// internal/monitor/service.go

type MonitorService struct {
    computeClient ComputeServiceClient
    storageClient StorageServiceClient
    failover      *FailoverController
    
    instances     map[string]*InstanceHealth
    mu            sync.RWMutex
}

type InstanceHealth struct {
    InstanceID       string
    IsHealthy        bool
    ConsecutiveFails int
    LastCheck        time.Time
    CurrentLSN       int64
}

// 启动监控
func (s *MonitorService) Start(ctx context.Context) {
    ticker := time.NewTicker(5 * time.Second)
    defer ticker.Stop()
    
    for {
        select {
        case <-ctx.Done():
            return
        case <-ticker.C:
            s.checkAll(ctx)
        }
    }
}

// 检查所有实例
func (s *MonitorService) checkAll(ctx context.Context) {
    s.mu.RLock()
    instances := s.getInstances()
    s.mu.RUnlock()
    
    for _, inst := range instances {
        go s.checkInstance(ctx, inst)
    }
}

// 检查单个实例
func (s *MonitorService) checkInstance(ctx context.Context, inst *InstanceHealth) {
    resp, err := s.computeClient.HealthCheck(ctx, &pb.HealthCheckRequest{
        InstanceId: inst.InstanceID,
    })
    
    s.mu.Lock()
    defer s.mu.Unlock()
    
    if err != nil || !resp.IsHealthy {
        inst.ConsecutiveFails++
        if inst.ConsecutiveFails >= 3 && inst.IsHealthy {
            inst.IsHealthy = false
            // 触发告警或 Failover
            if inst.Role == RoleWriter {
                go s.failover.TriggerFailover(inst.InstanceID)
            }
        }
    } else {
        inst.ConsecutiveFails = 0
        inst.IsHealthy = true
        inst.CurrentLSN = resp.CurrentLsn
    }
    inst.LastCheck = time.Now()
}
```

### 3.3 Cluster Manager

```go
// internal/cluster/manager.go

type ClusterManager struct {
    metadataClient MetadataServiceClient
    computeClient  ComputeServiceClient
    storageClient  StorageServiceClient
    stateStore     StateStore
}

// 创建集群
func (m *ClusterManager) CreateCluster(ctx context.Context, req *CreateClusterRequest) (*ClusterInfo, error) {
    clusterID := generateClusterID()
    volumeID := generateVolumeID()
    
    // 1. 创建 Volume
    if err := m.createVolume(ctx, volumeID, req.AZs); err != nil {
        return nil, err
    }
    
    // 2. 初始化存储
    if err := m.initializeStorage(ctx, volumeID); err != nil {
        return nil, err
    }
    
    // 3. 创建 Writer
    writer, err := m.createInstance(ctx, clusterID, volumeID, RoleWriter, req.Config)
    if err != nil {
        return nil, err
    }
    
    // 4. 创建 Readers
    var readers []*InstanceInfo
    for i := 0; i < req.ReaderCount; i++ {
        reader, err := m.createInstance(ctx, clusterID, volumeID, RoleReader, req.Config)
        if err != nil {
            continue
        }
        readers = append(readers, reader)
    }
    
    // 5. 保存集群信息
    cluster := &ClusterInfo{
        ClusterID: clusterID,
        VolumeID:  volumeID,
        Writer:    writer,
        Readers:   readers,
        State:     ClusterStateAvailable,
    }
    m.stateStore.SaveCluster(ctx, cluster)
    
    return cluster, nil
}
```

---

## 4. gRPC 接口

### 4.1 Control Plane Service

```protobuf
service ClusterService {
    // 集群管理
    rpc CreateCluster(CreateClusterRequest) returns (CreateClusterResponse);
    rpc DeleteCluster(DeleteClusterRequest) returns (google.protobuf.Empty);
    rpc GetCluster(GetClusterRequest) returns (ClusterInfo);
    rpc ListClusters(ListClustersRequest) returns (ListClustersResponse);
    
    // 实例管理
    rpc AddReader(AddReaderRequest) returns (AddReaderResponse);
    rpc RemoveReader(RemoveReaderRequest) returns (google.protobuf.Empty);
}

service FailoverService {
    // 故障切换
    rpc TriggerFailover(TriggerFailoverRequest) returns (TriggerFailoverResponse);
    rpc GetFailoverStatus(GetFailoverStatusRequest) returns (FailoverStatus);
    rpc CancelFailover(CancelFailoverRequest) returns (google.protobuf.Empty);
}

service MonitorService {
    // 监控
    rpc GetClusterStatus(GetClusterStatusRequest) returns (ClusterStatus);
    rpc GetInstanceStatus(GetInstanceStatusRequest) returns (InstanceStatus);
    rpc SubscribeEvents(SubscribeEventsRequest) returns (stream ClusterEvent);
}

message CreateClusterRequest {
    string cluster_name = 1;
    string instance_class = 2;
    int32 reader_count = 3;
    repeated string availability_zones = 4;
}

message ClusterInfo {
    string cluster_id = 1;
    string cluster_name = 2;
    string volume_id = 3;
    InstanceInfo writer = 4;
    repeated InstanceInfo readers = 5;
    ClusterState state = 6;
}

message TriggerFailoverRequest {
    string cluster_id = 1;
    string target_instance_id = 2;  // 可选，指定提升的 Reader
}

message FailoverStatus {
    string failover_id = 1;
    FailoverState state = 2;
    string source_instance_id = 3;
    string target_instance_id = 4;
    int64 final_vdl = 5;
}
```

---

## 5. 配置参数

```yaml
server:
  grpc_port: 9000
  rest_port: 8080

etcd:
  endpoints:
    - etcd1:2379
    - etcd2:2379
    - etcd3:2379
  dial_timeout: 5s

monitor:
  check_interval: 5s
  timeout: 3s
  fail_threshold: 3

failover:
  freeze_timeout: 30s
  promote_timeout: 60s
  catchup_timeout: 30s

compute_client:
  timeout: 10s
  retry_count: 3

storage_client:
  timeout: 5s
  retry_count: 3
```

---

## 6. 监控指标

| 指标 | 类型 | 说明 |
|------|------|------|
| `control_plane_clusters_total` | Gauge | 集群总数 |
| `control_plane_instances_total` | Gauge | 实例总数 |
| `control_plane_failovers_total` | Counter | Failover 总数 |
| `control_plane_failover_duration_seconds` | Histogram | Failover 耗时 |
| `control_plane_health_check_failures_total` | Counter | 健康检查失败数 |
| `control_plane_api_requests_total` | Counter | API 请求总数 |
