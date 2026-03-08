# Skill: 通信协议 (Protocol)

## 1. 模块职责

定义计算层与存储层之间的通信协议：
- **gRPC 服务定义**: 标准 RPC 接口
- **RDMA 支持**: 高性能数据传输
- **消息格式**: Request/Response 结构
- **错误处理**: 错误码和重试策略

**实现语言**: Golang (存储层), C++ (计算层客户端)

## 2. 双协议架构

```
┌─────────────────────────────────────────────────────────────────┐
│                    双协议架构                                    │
│                                                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    计算层 (C++)                          │   │
│  │  ┌─────────────────────────────────────────────────┐    │   │
│  │  │              StorageClient                       │    │   │
│  │  │  ┌───────────────┐  ┌───────────────┐           │    │   │
│  │  │  │ gRPC Client   │  │ RDMA Client   │           │    │   │
│  │  │  │ (控制面)      │  │ (数据面)      │           │    │   │
│  │  │  └───────────────┘  └───────────────┘           │    │   │
│  │  └─────────────────────────────────────────────────┘    │   │
│  └─────────────────────────────────────────────────────────┘   │
│           │                        │                            │
│           │ TCP/IP                 │ RDMA (InfiniBand/RoCE)    │
│           ▼                        ▼                            │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    存储层 (Golang)                       │   │
│  │  ┌───────────────┐  ┌───────────────┐                   │   │
│  │  │ gRPC Server   │  │ RDMA Server   │                   │   │
│  │  │ (控制面)      │  │ (数据面)      │                   │   │
│  │  └───────────────┘  └───────────────┘                   │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                 │
│  协议选择:                                                      │
│  • 控制面 (小数据): gRPC (GetStatus, CreateTablespace, ...)    │
│  • 数据面 (大数据): RDMA (ReadPage, WriteRedo)                 │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## 3. gRPC 服务定义

### 3.1 主服务

```protobuf
syntax = "proto3";
package aurora.storage;

// 主服务
service StorageService {
    // Redo 写入 (支持 RDMA 模式)
    rpc WriteRedo(WriteRedoRequest) returns (WriteRedoResponse);
    
    // 页读取 (支持 RDMA 模式)
    rpc ReadPage(ReadPageRequest) returns (ReadPageResponse);
    rpc ReadPages(ReadPagesRequest) returns (stream ReadPageResponse);
    
    // 状态查询
    rpc GetStatus(GetStatusRequest) returns (GetStatusResponse);
    
    // 表空间管理
    rpc CreateTablespace(CreateTablespaceRequest) returns (CreateTablespaceResponse);
    rpc DropTablespace(DropTablespaceRequest) returns (DropTablespaceResponse);
    rpc LookupTablespace(LookupTablespaceRequest) returns (LookupTablespaceResponse);
    
    // RDMA 设置
    rpc SetupRDMA(SetupRDMARequest) returns (SetupRDMAResponse);
}

// Schema MVCC 扩展服务
service SchemaMVCCService {
    rpc GetVisibleSchema(GetVisibleSchemaRequest) returns (GetVisibleSchemaResponse);
    rpc ReportActiveSnapshot(ReportActiveSnapshotRequest) returns (ReportActiveSnapshotResponse);
    rpc ListSchemaVersions(ListSchemaVersionsRequest) returns (ListSchemaVersionsResponse);
}

// Binlog 服务
service BinlogService {
    rpc WriteBinlog(WriteBinlogRequest) returns (WriteBinlogResponse);
    rpc ReadBinlog(ReadBinlogRequest) returns (stream BinlogEvent);
    rpc Subscribe(SubscribeRequest) returns (stream BinlogEvent);
    rpc GetGTIDSet(GetGTIDSetRequest) returns (GetGTIDSetResponse);
}

// HA 服务
service HAService {
    rpc AcquireWriteLock(AcquireWriteLockRequest) returns (AcquireWriteLockResponse);
    rpc ReleaseWriteLock(ReleaseWriteLockRequest) returns (ReleaseWriteLockResponse);
    rpc GetClusterStatus(GetClusterStatusRequest) returns (GetClusterStatusResponse);
}
```

### 3.2 消息定义

```protobuf
// ========== Redo 写入 ==========
message WriteRedoRequest {
    uint64 db_id = 1;
    uint64 start_lsn = 2;
    uint64 end_lsn = 3;
    bytes  redo_data = 4;       // gRPC 模式
    bool   sync = 5;
    
    // RDMA 模式
    bool   use_rdma = 10;
    uint64 rdma_mr_key = 11;    // Memory Region Key
    uint64 rdma_offset = 12;    // 数据偏移
    uint64 rdma_length = 13;    // 数据长度
}

message WriteRedoResponse {
    bool   success = 1;
    uint64 durable_lsn = 2;
    string error = 3;
}

// ========== 页读取 ==========
message ReadPageRequest {
    uint64 db_id = 1;
    uint32 space_id = 2;
    uint64 schema_version = 3;
    uint32 page_no = 4;
    uint64 min_lsn = 5;
    
    // RDMA 模式
    bool   use_rdma = 10;
    uint64 rdma_mr_key = 11;
    uint64 rdma_offset = 12;    // 写入目标偏移
}

message ReadPageResponse {
    bool   success = 1;
    bytes  page_data = 2;       // gRPC 模式
    uint64 page_lsn = 3;
    uint64 schema_version = 4;
    string error = 5;
    
    // RDMA 模式
    bool   rdma_completed = 10; // RDMA 写入已完成
}

// ========== RDMA 设置 ==========
message SetupRDMARequest {
    string client_id = 1;
    string local_ip = 2;
    uint32 local_port = 3;
    uint32 qp_num = 4;          // Queue Pair Number
    uint32 psn = 5;             // Packet Sequence Number
    bytes  gid = 6;             // Global ID
}

message SetupRDMAResponse {
    bool   success = 1;
    uint32 server_qp_num = 2;
    uint32 server_psn = 3;
    bytes  server_gid = 4;
    uint64 server_mr_key = 5;   // Server Memory Region Key
    string error = 6;
}

// ========== 状态查询 ==========
message GetStatusRequest {
    uint64 db_id = 1;
}

message GetStatusResponse {
    uint64 received_lsn = 1;
    uint64 durable_lsn = 2;
    uint64 applied_lsn = 3;
    
    repeated SegmentStatus segments = 4;
    
    // RDMA 状态
    bool   rdma_enabled = 10;
    uint64 rdma_bytes_sent = 11;
    uint64 rdma_bytes_recv = 12;
}
```

## 4. RDMA 实现

### 4.1 RDMA 服务端 (Golang)

```go
package rdma

import (
    "github.com/Mellanox/rdma-core-go"
)

type RDMAServer struct {
    ctx        *rdma.Context
    pd         *rdma.ProtectionDomain
    cq         *rdma.CompletionQueue
    
    // 连接管理
    connections map[string]*RDMAConnection
    mu          sync.RWMutex
    
    // 内存区域
    memoryPool  *MemoryPool
}

type RDMAConnection struct {
    clientID   string
    qp         *rdma.QueuePair
    localMR    *rdma.MemoryRegion
    remoteMR   RemoteMemoryInfo
}

type RemoteMemoryInfo struct {
    Addr   uint64
    Length uint64
    RKey   uint32
}

func NewRDMAServer(config *RDMAConfig) (*RDMAServer, error) {
    // 1. 打开 RDMA 设备
    devices, err := rdma.GetDeviceList()
    if err != nil {
        return nil, err
    }
    
    ctx, err := devices[0].Open()
    if err != nil {
        return nil, err
    }
    
    // 2. 分配 Protection Domain
    pd, err := ctx.AllocPD()
    if err != nil {
        return nil, err
    }
    
    // 3. 创建 Completion Queue
    cq, err := ctx.CreateCQ(config.CQSize, nil)
    if err != nil {
        return nil, err
    }
    
    // 4. 初始化内存池
    memoryPool := NewMemoryPool(pd, config.PoolSize)
    
    return &RDMAServer{
        ctx:         ctx,
        pd:          pd,
        cq:          cq,
        connections: make(map[string]*RDMAConnection),
        memoryPool:  memoryPool,
    }, nil
}

// 建立 RDMA 连接
func (s *RDMAServer) SetupConnection(req *SetupRDMARequest) (*RDMAConnection, error) {
    // 1. 创建 Queue Pair
    qpAttr := rdma.QPInitAttr{
        SendCQ:  s.cq,
        RecvCQ:  s.cq,
        Cap: rdma.QPCap{
            MaxSendWR:  256,
            MaxRecvWR:  256,
            MaxSendSGE: 1,
            MaxRecvSGE: 1,
        },
        QPType: rdma.RC,  // Reliable Connection
    }
    
    qp, err := s.pd.CreateQP(qpAttr)
    if err != nil {
        return nil, err
    }
    
    // 2. 分配本地 Memory Region
    buf := s.memoryPool.Alloc(16 * 1024 * 1024)  // 16MB
    mr, err := s.pd.RegMR(buf, rdma.IBV_ACCESS_LOCAL_WRITE|
                               rdma.IBV_ACCESS_REMOTE_WRITE|
                               rdma.IBV_ACCESS_REMOTE_READ)
    if err != nil {
        return nil, err
    }
    
    // 3. 交换连接信息并转换 QP 状态
    if err := s.connectQP(qp, req); err != nil {
        return nil, err
    }
    
    conn := &RDMAConnection{
        clientID: req.ClientId,
        qp:       qp,
        localMR:  mr,
        remoteMR: RemoteMemoryInfo{
            RKey: req.RemoteMrKey,
        },
    }
    
    s.mu.Lock()
    s.connections[req.ClientId] = conn
    s.mu.Unlock()
    
    return conn, nil
}

// RDMA Read (从客户端读取数据)
func (s *RDMAServer) RDMARead(conn *RDMAConnection, 
    remoteOffset, localOffset, length uint64) error {
    
    // 发起 RDMA Read 请求
    wr := rdma.SendWR{
        Opcode:     rdma.IBV_WR_RDMA_READ,
        SendFlags:  rdma.IBV_SEND_SIGNALED,
        SGList: []rdma.SGE{{
            Addr:   conn.localMR.Addr() + localOffset,
            Length: uint32(length),
            LKey:   conn.localMR.LKey(),
        }},
        WRRemote: rdma.WRRemote{
            RemoteAddr: conn.remoteMR.Addr + remoteOffset,
            RKey:       conn.remoteMR.RKey,
        },
    }
    
    if err := conn.qp.PostSend(&wr); err != nil {
        return err
    }
    
    // 等待完成
    return s.waitCompletion()
}

// RDMA Write (向客户端写入数据)
func (s *RDMAServer) RDMAWrite(conn *RDMAConnection,
    localOffset, remoteOffset, length uint64) error {
    
    wr := rdma.SendWR{
        Opcode:     rdma.IBV_WR_RDMA_WRITE,
        SendFlags:  rdma.IBV_SEND_SIGNALED,
        SGList: []rdma.SGE{{
            Addr:   conn.localMR.Addr() + localOffset,
            Length: uint32(length),
            LKey:   conn.localMR.LKey(),
        }},
        WRRemote: rdma.WRRemote{
            RemoteAddr: conn.remoteMR.Addr + remoteOffset,
            RKey:       conn.remoteMR.RKey,
        },
    }
    
    if err := conn.qp.PostSend(&wr); err != nil {
        return err
    }
    
    return s.waitCompletion()
}
```

### 4.2 RDMA 客户端 (C++)

```cpp
// 计算层 RDMA 客户端
class RDMAClient {
public:
    RDMAClient(const std::string& server_addr) {
        // 1. 打开设备
        device_list = ibv_get_device_list(&num_devices);
        context = ibv_open_device(device_list[0]);
        
        // 2. 分配 PD 和 CQ
        pd = ibv_alloc_pd(context);
        cq = ibv_create_cq(context, CQ_SIZE, nullptr, nullptr, 0);
        
        // 3. 创建 QP
        struct ibv_qp_init_attr qp_attr = {};
        qp_attr.send_cq = cq;
        qp_attr.recv_cq = cq;
        qp_attr.qp_type = IBV_QPT_RC;
        qp_attr.cap.max_send_wr = 256;
        qp_attr.cap.max_recv_wr = 256;
        qp = ibv_create_qp(pd, &qp_attr);
        
        // 4. 分配并注册内存
        buffer = aligned_alloc(4096, BUFFER_SIZE);
        mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE,
            IBV_ACCESS_LOCAL_WRITE | 
            IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_READ);
    }
    
    // 通过 gRPC 交换连接信息
    bool Connect(const std::string& server_addr) {
        // 调用 gRPC SetupRDMA
        SetupRDMARequest req;
        req.set_client_id(client_id);
        req.set_qp_num(qp->qp_num);
        req.set_psn(local_psn);
        req.set_gid(local_gid);
        
        SetupRDMAResponse resp;
        grpc_stub->SetupRDMA(req, &resp);
        
        // 保存服务端信息
        remote_qp_num = resp.server_qp_num();
        remote_psn = resp.server_psn();
        remote_mr_key = resp.server_mr_key();
        
        // 转换 QP 状态到 RTS
        return modify_qp_to_rts();
    }
    
    // 写 Redo (零拷贝)
    bool WriteRedoRDMA(const void* data, size_t len, uint64_t lsn) {
        // 1. 数据已在注册的内存中，直接发起 RDMA Write
        struct ibv_send_wr wr = {};
        struct ibv_sge sge = {};
        
        sge.addr = (uint64_t)data;
        sge.length = len;
        sge.lkey = mr->lkey;
        
        wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.wr.rdma.remote_addr = remote_buffer_addr;
        wr.wr.rdma.rkey = remote_mr_key;
        wr.imm_data = htonl(len);  // 通过 immediate data 传递长度
        
        struct ibv_send_wr* bad_wr;
        if (ibv_post_send(qp, &wr, &bad_wr) != 0) {
            return false;
        }
        
        // 2. 等待完成
        return poll_completion();
    }
    
    // 读页 (零拷贝)
    bool ReadPageRDMA(uint32_t space_id, uint32_t page_no, 
                      void* dest, uint64_t min_lsn) {
        // 通过 gRPC 请求，服务端会用 RDMA Write 写入数据
        ReadPageRequest req;
        req.set_space_id(space_id);
        req.set_page_no(page_no);
        req.set_min_lsn(min_lsn);
        req.set_use_rdma(true);
        req.set_rdma_mr_key(mr->rkey);
        req.set_rdma_offset((uint64_t)dest - (uint64_t)buffer);
        
        ReadPageResponse resp;
        grpc_stub->ReadPage(req, &resp);
        
        return resp.rdma_completed();
    }
    
private:
    struct ibv_context* context;
    struct ibv_pd* pd;
    struct ibv_cq* cq;
    struct ibv_qp* qp;
    struct ibv_mr* mr;
    void* buffer;
    
    uint32_t remote_qp_num;
    uint32_t remote_psn;
    uint32_t remote_mr_key;
    uint64_t remote_buffer_addr;
};
```

## 5. 协议选择策略

```go
type TransportSelector struct {
    rdmaEnabled bool
    rdmaClient  *RDMAClient
    grpcClient  *GRPCClient
    
    // 阈值配置
    rdmaThreshold uint64  // 超过此大小使用 RDMA
}

func (s *TransportSelector) WriteRedo(req *WriteRedoRequest) error {
    if s.rdmaEnabled && len(req.RedoData) > int(s.rdmaThreshold) {
        // 大数据走 RDMA
        return s.rdmaClient.WriteRedo(req)
    }
    // 小数据走 gRPC
    return s.grpcClient.WriteRedo(req)
}

func (s *TransportSelector) ReadPage(req *ReadPageRequest) (*ReadPageResponse, error) {
    if s.rdmaEnabled {
        // 页读取始终走 RDMA (16KB)
        return s.rdmaClient.ReadPage(req)
    }
    return s.grpcClient.ReadPage(req)
}
```

## 6. 错误码

```protobuf
enum ErrorCode {
    OK = 0;
    
    // 通用错误
    UNKNOWN_ERROR = 1;
    INVALID_ARGUMENT = 2;
    NOT_FOUND = 3;
    ALREADY_EXISTS = 4;
    
    // 存储层错误
    STORAGE_FULL = 100;
    STORAGE_IO_ERROR = 101;
    STORAGE_CORRUPTION = 102;
    
    // LSN 相关
    LSN_OUT_OF_RANGE = 200;
    LSN_GAP_DETECTED = 201;
    
    // Schema MVCC 相关
    SCHEMA_VERSION_NOT_FOUND = 300;
    SCHEMA_VERSION_DROPPED = 301;
    SNAPSHOT_TOO_OLD = 302;
    
    // RDMA 相关
    RDMA_NOT_SUPPORTED = 400;
    RDMA_CONNECTION_FAILED = 401;
    RDMA_MR_INVALID = 402;
    
    // HA 相关
    WRITE_LOCK_DENIED = 500;
    NOT_LEADER = 501;
}
```

## 7. 性能优化

### 7.1 批量操作

```go
// 批量写入 Redo (减少 RTT)
func (c *StorageClient) WriteBatchRedo(batch []*WriteRedoRequest) error {
    // 合并小请求
    merged := mergeRedoRequests(batch)
    
    if c.rdmaEnabled && merged.Size > c.rdmaThreshold {
        return c.rdmaWriteBatch(merged)
    }
    return c.grpcWriteBatch(merged)
}
```

### 7.2 连接池

```go
type ConnectionPool struct {
    grpcConns []*grpc.ClientConn
    rdmaConns []*RDMAConnection
    
    grpcIdx atomic.Int32
    rdmaIdx atomic.Int32
}

func (p *ConnectionPool) GetGRPCConn() *grpc.ClientConn {
    idx := p.grpcIdx.Add(1) % int32(len(p.grpcConns))
    return p.grpcConns[idx]
}
```

## 8. IO 优化

### 8.1 RDMA 请求流水线

```go
type RDMAPipeline struct {
    conn      *RDMAConnection
    inflight  chan *PipelineRequest
    maxDepth  int  // 32
}

func (p *RDMAPipeline) Submit(req interface{}) <-chan interface{} {
    pr := &PipelineRequest{req: req, respCh: make(chan interface{}, 1)}
    
    // 非阻塞提交，隐藏网络延迟
    p.inflight <- pr
    go p.send(pr)
    
    return pr.respCh
}
```

### 8.2 批量页读取

```go
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
        pages = append(pages, (<-ch).(Page))
    }
    return pages
}
```

### 8.3 智能路由

```go
// 就近读取，减少网络跳数
type SmartRouter struct {
    topology map[string][]string  // segment -> storage nodes
    latency  map[string]time.Duration
}

func (r *SmartRouter) SelectNode(segmentID string) string {
    nodes := r.topology[segmentID]
    
    // 选择延迟最低的节点
    var best string
    var bestLatency time.Duration = time.Hour
    
    for _, node := range nodes {
        if r.latency[node] < bestLatency {
            best = node
            bestLatency = r.latency[node]
        }
    }
    return best
}
```

## 9. 开发任务

- [ ] 定义 proto 文件
- [ ] 实现 gRPC Server (Golang)
- [ ] 实现 gRPC Client (C++)
- [ ] 实现 RDMA Server (Golang)
- [ ] 实现 RDMA Client (C++)
- [ ] 协议选择器
- [ ] 连接池
- [ ] **实现 RDMA Pipeline**
- [ ] **批量页读取**
- [ ] **智能路由**
- [ ] 监控指标
- [ ] 性能测试

## 10. 参考

- gRPC 官方文档
- RDMA 编程指南
- Mellanox rdma-core 文档
- 主文档 8.8: 协议设计
