# gRPC 协议定义文档

## 1. 协议概述

所有模块间通信使用 **gRPC + Protocol Buffers** 协议。

### 1.1 服务端口

| 服务 | 端口 | 提供方 |
|------|------|--------|
| StorageService | 9002 | 存储层 |
| MetadataService | 9003 | 元数据服务 |
| ComputeService | 9001 | 计算层 |
| ControlPlaneService | 9000 | 控制平面 |

### 1.2 Proto 文件结构

```
pkg/proto/
├── common.proto        # 公共类型定义
├── storage.proto       # 存储层接口
├── metadata.proto      # 元数据接口
├── compute.proto       # 计算层接口
└── controlplane.proto  # 控制平面接口
```

---

## 2. common.proto

```protobuf
syntax = "proto3";

package aurora.common;

option go_package = "github.com/aurora/pkg/proto/common";

// Redo 类型
enum RedoType {
    REDO_TYPE_UNKNOWN = 0;
    REDO_TYPE_INSERT = 1;
    REDO_TYPE_UPDATE = 2;
    REDO_TYPE_DELETE = 3;
    REDO_TYPE_PAGE_CREATE = 4;
    REDO_TYPE_PAGE_INIT = 5;
    REDO_TYPE_TRX_COMMIT = 6;
    REDO_TYPE_TRX_ROLLBACK = 7;
    REDO_TYPE_DDL = 8;
    REDO_TYPE_CHECKPOINT = 9;
    REDO_TYPE_MTR_COMMIT = 10;
}

// 实例角色
enum InstanceRole {
    INSTANCE_ROLE_UNKNOWN = 0;
    INSTANCE_ROLE_WRITER = 1;
    INSTANCE_ROLE_READER = 2;
}

// 实例状态
enum InstanceState {
    INSTANCE_STATE_UNKNOWN = 0;
    INSTANCE_STATE_CREATING = 1;
    INSTANCE_STATE_AVAILABLE = 2;
    INSTANCE_STATE_PROMOTING = 3;
    INSTANCE_STATE_STOPPING = 4;
    INSTANCE_STATE_STOPPED = 5;
    INSTANCE_STATE_FAILED = 6;
}

// 页面标识
message PageId {
    int64 space_id = 1;
    int64 page_id = 2;
}

// 错误信息
message Error {
    int32 code = 1;
    string message = 2;
}
```

---

## 3. storage.proto

```protobuf
syntax = "proto3";

package aurora.storage;

option go_package = "github.com/aurora/pkg/proto/storage";

import "google/protobuf/empty.proto";
import "common.proto";

// 存储服务
service StorageService {
    // ========== 写入接口 ==========
    
    // 写入单条 Redo
    rpc WriteRedo(WriteRedoRequest) returns (WriteRedoResponse);
    
    // 批量写入 Redo
    rpc WriteRedoBatch(WriteRedoBatchRequest) returns (WriteRedoBatchResponse);
    
    // ========== 读取接口 ==========
    
    // 读取 Page（带物化）
    rpc ReadPage(ReadPageRequest) returns (ReadPageResponse);
    
    // 批量读取 Page
    rpc ReadPageBatch(ReadPageBatchRequest) returns (ReadPageBatchResponse);
    
    // 获取 Redo 日志（流式）
    rpc GetRedoLogs(GetRedoLogsRequest) returns (stream RedoLogEntry);
    
    // ========== 管理接口 ==========
    
    // 初始化 Volume
    rpc InitializeVolume(InitializeVolumeRequest) returns (InitializeVolumeResponse);
    
    // 冻结写入
    rpc FreezeWrites(FreezeWritesRequest) returns (FreezeWritesResponse);
    
    // 解冻写入
    rpc UnfreezeWrites(UnfreezeWritesRequest) returns (google.protobuf.Empty);
    
    // 获取存储状态
    rpc GetStorageStatus(GetStorageStatusRequest) returns (StorageStatusResponse);
    
    // 健康检查
    rpc HealthCheck(HealthCheckRequest) returns (HealthCheckResponse);
}

// ========== 写入相关消息 ==========

message WriteRedoRequest {
    string volume_id = 1;
    int64 lsn = 2;
    bytes redo_data = 3;
    aurora.common.RedoType redo_type = 4;
    int64 space_id = 5;
    int64 page_id = 6;
    int64 trx_id = 7;
    int64 mtr_id = 8;
    bool mtr_start = 9;
    bool mtr_end = 10;
    uint32 checksum = 11;
}

message WriteRedoResponse {
    bool success = 1;
    int64 persisted_lsn = 2;
    string node_id = 3;
    int64 write_latency_us = 4;
}

message WriteRedoBatchRequest {
    string volume_id = 1;
    repeated WriteRedoRequest redos = 2;
    bool sync_write = 3;
}

message WriteRedoBatchResponse {
    bool success = 1;
    int64 first_lsn = 2;
    int64 last_persisted_lsn = 3;
    int32 success_count = 4;
    int32 failed_count = 5;
}

// ========== 读取相关消息 ==========

message ReadPageRequest {
    string volume_id = 1;
    int64 space_id = 2;
    int64 page_id = 3;
    int64 target_lsn = 4;       // 物化到该 LSN
    bool allow_stale = 5;       // 是否允许返回旧版本
}

message ReadPageResponse {
    bytes page_data = 1;        // 16KB
    int64 page_lsn = 2;
    bool from_cache = 3;
    bool materialized = 4;
    int64 read_latency_us = 5;
}

message ReadPageBatchRequest {
    string volume_id = 1;
    repeated aurora.common.PageId pages = 2;
    int64 target_lsn = 3;
}

message ReadPageBatchResponse {
    repeated PageData pages = 1;
}

message PageData {
    int64 space_id = 1;
    int64 page_id = 2;
    bytes data = 3;
    int64 page_lsn = 4;
    bool success = 5;
}

message GetRedoLogsRequest {
    string volume_id = 1;
    int64 start_lsn = 2;
    int64 end_lsn = 3;
    int32 max_count = 4;
    int64 space_id = 5;         // 可选：按 Page 过滤
    int64 page_id = 6;
}

message RedoLogEntry {
    int64 lsn = 1;
    aurora.common.RedoType type = 2;
    bytes data = 3;
    int64 space_id = 4;
    int64 page_id = 5;
    int64 trx_id = 6;
    int64 mtr_id = 7;
    int64 timestamp = 8;
    uint32 checksum = 9;
}

// ========== 管理相关消息 ==========

message InitializeVolumeRequest {
    string volume_id = 1;
    repeated string availability_zones = 2;
    int32 replica_count = 3;
}

message InitializeVolumeResponse {
    bool success = 1;
    repeated string node_ids = 2;
}

message FreezeWritesRequest {
    string volume_id = 1;
    int32 timeout_seconds = 2;
}

message FreezeWritesResponse {
    bool success = 1;
    int64 final_vdl = 2;
}

message UnfreezeWritesRequest {
    string volume_id = 1;
}

message GetStorageStatusRequest {
    string volume_id = 1;
}

message StorageStatusResponse {
    string volume_id = 1;
    int64 current_vdl = 2;
    int64 total_size_bytes = 3;
    int64 used_size_bytes = 4;
    repeated NodeStatus nodes = 5;
}

message NodeStatus {
    string node_id = 1;
    bool is_healthy = 2;
    int64 current_lsn = 3;
    int64 disk_used_bytes = 4;
}

message HealthCheckRequest {
    string node_id = 1;
}

message HealthCheckResponse {
    bool is_healthy = 1;
    string status_message = 2;
}
```

---

## 4. metadata.proto

```protobuf
syntax = "proto3";

package aurora.metadata;

option go_package = "github.com/aurora/pkg/proto/metadata";

import "google/protobuf/empty.proto";
import "google/protobuf/timestamp.proto";

// 元数据服务
service MetadataService {
    // ========== Volume 管理 ==========
    rpc CreateVolume(CreateVolumeRequest) returns (CreateVolumeResponse);
    rpc GetVolume(GetVolumeRequest) returns (VolumeInfo);
    rpc DeleteVolume(DeleteVolumeRequest) returns (google.protobuf.Empty);
    
    // ========== VDL 管理 ==========
    rpc UpdateVDL(UpdateVDLRequest) returns (UpdateVDLResponse);
    rpc GetVDL(GetVDLRequest) returns (GetVDLResponse);
    
    // ========== 节点 LSN 管理 ==========
    rpc UpdateNodeLSN(UpdateNodeLSNRequest) returns (google.protobuf.Empty);
    rpc GetNodeLSNs(GetNodeLSNsRequest) returns (GetNodeLSNsResponse);
    
    // ========== PG 映射 ==========
    rpc GetPageLocation(GetPageLocationRequest) returns (PageLocationResponse);
    rpc GetPageLocationBatch(GetPageLocationBatchRequest) returns (GetPageLocationBatchResponse);
    
    // ========== 实例管理 ==========
    rpc RegisterInstance(RegisterInstanceRequest) returns (RegisterInstanceResponse);
    rpc UnregisterInstance(UnregisterInstanceRequest) returns (google.protobuf.Empty);
    
    // ========== 健康检查 ==========
    rpc HealthCheck(HealthCheckRequest) returns (HealthCheckResponse);
    rpc GetLeader(google.protobuf.Empty) returns (LeaderResponse);
}

// ========== Volume 相关消息 ==========

message CreateVolumeRequest {
    string volume_id = 1;
    string cluster_id = 2;
    int64 initial_size_bytes = 3;
    repeated string availability_zones = 4;
}

message CreateVolumeResponse {
    VolumeInfo volume = 1;
}

message GetVolumeRequest {
    string volume_id = 1;
}

message DeleteVolumeRequest {
    string volume_id = 1;
}

message VolumeInfo {
    string volume_id = 1;
    string cluster_id = 2;
    int64 size_bytes = 3;
    int64 current_vdl = 4;
    int32 pg_count = 5;
    repeated ProtectionGroup pgs = 6;
    google.protobuf.Timestamp created_at = 7;
}

message ProtectionGroup {
    int32 pg_id = 1;
    repeated string node_ids = 2;   // 6 个节点
}

// ========== VDL 相关消息 ==========

message UpdateVDLRequest {
    string volume_id = 1;
    int64 new_vdl = 2;
    string instance_id = 3;
}

message UpdateVDLResponse {
    bool success = 1;
    int64 current_vdl = 2;
}

message GetVDLRequest {
    string volume_id = 1;
}

message GetVDLResponse {
    int64 vdl = 1;
    google.protobuf.Timestamp updated_at = 2;
}

// ========== 节点 LSN 相关消息 ==========

message UpdateNodeLSNRequest {
    string volume_id = 1;
    string node_id = 2;
    int64 current_lsn = 3;
}

message GetNodeLSNsRequest {
    string volume_id = 1;
}

message GetNodeLSNsResponse {
    map<string, int64> node_lsns = 1;
    int64 min_lsn = 2;
    int64 max_lsn = 3;
}

// ========== PG 映射相关消息 ==========

message GetPageLocationRequest {
    string volume_id = 1;
    int64 space_id = 2;
    int64 page_id = 3;
}

message PageLocationResponse {
    repeated string node_ids = 1;
    int64 base_lsn = 2;
    int32 pg_id = 3;
}

message GetPageLocationBatchRequest {
    string volume_id = 1;
    repeated PageIdentifier pages = 2;
}

message PageIdentifier {
    int64 space_id = 1;
    int64 page_id = 2;
}

message GetPageLocationBatchResponse {
    repeated PageLocationInfo locations = 1;
}

message PageLocationInfo {
    int64 space_id = 1;
    int64 page_id = 2;
    repeated string node_ids = 3;
}

// ========== 实例相关消息 ==========

message RegisterInstanceRequest {
    string cluster_id = 1;
    string instance_id = 2;
    int32 role = 3;             // 1=Writer, 2=Reader
    string endpoint = 4;
    int32 port = 5;
}

message RegisterInstanceResponse {
    bool success = 1;
    string volume_id = 2;
}

message UnregisterInstanceRequest {
    string cluster_id = 1;
    string instance_id = 2;
}

// ========== 健康检查相关消息 ==========

message HealthCheckRequest {}

message HealthCheckResponse {
    bool is_healthy = 1;
    bool is_leader = 2;
    string leader_id = 3;
}

message LeaderResponse {
    string leader_id = 1;
    string leader_endpoint = 2;
}
```

---

## 5. compute.proto

```protobuf
syntax = "proto3";

package aurora.compute;

option go_package = "github.com/aurora/pkg/proto/compute";

import "google/protobuf/empty.proto";

// 计算层服务
service ComputeService {
    // 健康检查
    rpc HealthCheck(HealthCheckRequest) returns (HealthCheckResponse);
    
    // 提升为 Writer
    rpc PromoteToWriter(PromoteRequest) returns (PromoteResponse);
    
    // 追赶到指定 LSN
    rpc CatchUp(CatchUpRequest) returns (CatchUpResponse);
    
    // 优雅关闭
    rpc Shutdown(ShutdownRequest) returns (google.protobuf.Empty);
    
    // 获取实例状态
    rpc GetInstanceStatus(GetInstanceStatusRequest) returns (InstanceStatusResponse);
    
    // 开始接受连接
    rpc StartAcceptingConnections(google.protobuf.Empty) returns (google.protobuf.Empty);
    
    // 停止接受连接
    rpc StopAcceptingConnections(google.protobuf.Empty) returns (google.protobuf.Empty);
}

message HealthCheckRequest {
    string instance_id = 1;
}

message HealthCheckResponse {
    bool is_healthy = 1;
    int64 current_lsn = 2;
    int64 replica_lag_ms = 3;
    int32 active_connections = 4;
    double cpu_utilization = 5;
    double memory_utilization = 6;
    int64 buffer_pool_hit_rate = 7;     // 万分比
}

message PromoteRequest {
    string instance_id = 1;
    string volume_id = 2;
}

message PromoteResponse {
    bool success = 1;
    int64 current_vdl = 2;
    string error_message = 3;
}

message CatchUpRequest {
    string instance_id = 1;
    int64 target_lsn = 2;
    int32 timeout_ms = 3;
}

message CatchUpResponse {
    bool success = 1;
    int64 reached_lsn = 2;
    string error_message = 3;
}

message ShutdownRequest {
    string instance_id = 1;
    bool force = 2;
    int32 timeout_seconds = 3;
}

message GetInstanceStatusRequest {
    string instance_id = 1;
}

message InstanceStatusResponse {
    string instance_id = 1;
    int32 role = 2;                 // 1=Writer, 2=Reader
    int32 state = 3;
    int64 current_lsn = 4;
    int32 active_connections = 5;
    int64 queries_per_second = 6;
}
```

---

## 6. controlplane.proto

```protobuf
syntax = "proto3";

package aurora.controlplane;

option go_package = "github.com/aurora/pkg/proto/controlplane";

import "google/protobuf/empty.proto";
import "google/protobuf/timestamp.proto";

// 集群服务
service ClusterService {
    rpc CreateCluster(CreateClusterRequest) returns (CreateClusterResponse);
    rpc DeleteCluster(DeleteClusterRequest) returns (google.protobuf.Empty);
    rpc GetCluster(GetClusterRequest) returns (ClusterInfo);
    rpc ListClusters(ListClustersRequest) returns (ListClustersResponse);
    rpc AddReader(AddReaderRequest) returns (AddReaderResponse);
    rpc RemoveReader(RemoveReaderRequest) returns (google.protobuf.Empty);
}

// 故障切换服务
service FailoverService {
    rpc TriggerFailover(TriggerFailoverRequest) returns (TriggerFailoverResponse);
    rpc GetFailoverStatus(GetFailoverStatusRequest) returns (FailoverStatus);
    rpc CancelFailover(CancelFailoverRequest) returns (google.protobuf.Empty);
}

// 监控服务
service MonitorService {
    rpc GetClusterStatus(GetClusterStatusRequest) returns (ClusterStatus);
    rpc GetInstanceStatus(GetInstanceStatusRequest) returns (InstanceStatus);
    rpc SubscribeEvents(SubscribeEventsRequest) returns (stream ClusterEvent);
}

// ========== 集群相关消息 ==========

message CreateClusterRequest {
    string cluster_name = 1;
    string instance_class = 2;
    int32 reader_count = 3;
    repeated string availability_zones = 4;
}

message CreateClusterResponse {
    ClusterInfo cluster = 1;
}

message DeleteClusterRequest {
    string cluster_id = 1;
}

message GetClusterRequest {
    string cluster_id = 1;
}

message ListClustersRequest {
    int32 page_size = 1;
    string page_token = 2;
}

message ListClustersResponse {
    repeated ClusterInfo clusters = 1;
    string next_page_token = 2;
}

message ClusterInfo {
    string cluster_id = 1;
    string cluster_name = 2;
    string volume_id = 3;
    ClusterState state = 4;
    InstanceInfo writer = 5;
    repeated InstanceInfo readers = 6;
    google.protobuf.Timestamp created_at = 7;
}

enum ClusterState {
    CLUSTER_STATE_UNKNOWN = 0;
    CLUSTER_STATE_CREATING = 1;
    CLUSTER_STATE_AVAILABLE = 2;
    CLUSTER_STATE_FAILING_OVER = 3;
    CLUSTER_STATE_DELETING = 4;
    CLUSTER_STATE_FAILED = 5;
}

message InstanceInfo {
    string instance_id = 1;
    int32 role = 2;
    int32 state = 3;
    string endpoint = 4;
    int32 port = 5;
}

message AddReaderRequest {
    string cluster_id = 1;
    string instance_class = 2;
    string availability_zone = 3;
}

message AddReaderResponse {
    InstanceInfo instance = 1;
}

message RemoveReaderRequest {
    string cluster_id = 1;
    string instance_id = 2;
}

// ========== 故障切换相关消息 ==========

message TriggerFailoverRequest {
    string cluster_id = 1;
    string target_instance_id = 2;
}

message TriggerFailoverResponse {
    string failover_id = 1;
    FailoverStatus status = 2;
}

message GetFailoverStatusRequest {
    string failover_id = 1;
}

message CancelFailoverRequest {
    string failover_id = 1;
}

message FailoverStatus {
    string failover_id = 1;
    string cluster_id = 2;
    FailoverState state = 3;
    string source_instance_id = 4;
    string target_instance_id = 5;
    int64 final_vdl = 6;
    google.protobuf.Timestamp started_at = 7;
    google.protobuf.Timestamp completed_at = 8;
    string error_message = 9;
}

enum FailoverState {
    FAILOVER_STATE_UNKNOWN = 0;
    FAILOVER_STATE_PENDING = 1;
    FAILOVER_STATE_FREEZING = 2;
    FAILOVER_STATE_PROMOTING = 3;
    FAILOVER_STATE_UPDATING = 4;
    FAILOVER_STATE_COMPLETED = 5;
    FAILOVER_STATE_FAILED = 6;
}

// ========== 监控相关消息 ==========

message GetClusterStatusRequest {
    string cluster_id = 1;
}

message ClusterStatus {
    string cluster_id = 1;
    ClusterState state = 2;
    int64 current_vdl = 3;
    InstanceStatus writer_status = 4;
    repeated InstanceStatus reader_statuses = 5;
}

message GetInstanceStatusRequest {
    string instance_id = 1;
}

message InstanceStatus {
    string instance_id = 1;
    int32 state = 2;
    bool is_healthy = 3;
    int64 current_lsn = 4;
    int64 replica_lag_ms = 5;
    int32 active_connections = 6;
}

message SubscribeEventsRequest {
    string cluster_id = 1;
}

message ClusterEvent {
    EventType type = 1;
    string cluster_id = 2;
    string instance_id = 3;
    string message = 4;
    google.protobuf.Timestamp timestamp = 5;
}

enum EventType {
    EVENT_TYPE_UNKNOWN = 0;
    EVENT_TYPE_INSTANCE_STATE_CHANGE = 1;
    EVENT_TYPE_FAILOVER_STARTED = 2;
    EVENT_TYPE_FAILOVER_COMPLETED = 3;
    EVENT_TYPE_REPLICATION_LAG = 4;
}
```

---

## 7. 错误码定义

| 错误码 | gRPC Status | 说明 |
|--------|-------------|------|
| `OK` | OK | 成功 |
| `NOT_FOUND` | NOT_FOUND | 资源不存在 |
| `ALREADY_EXISTS` | ALREADY_EXISTS | 资源已存在 |
| `INVALID_ARGUMENT` | INVALID_ARGUMENT | 参数错误 |
| `DEADLINE_EXCEEDED` | DEADLINE_EXCEEDED | 超时 |
| `UNAVAILABLE` | UNAVAILABLE | 服务不可用 |
| `NOT_LEADER` | FAILED_PRECONDITION | 非 Leader |
| `QUORUM_FAILED` | ABORTED | Quorum 失败 |
