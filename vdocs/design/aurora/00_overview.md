# Aurora-Like 分布式数据库功能设计文档

## 1. 项目概述

本项目基于 Aurora 架构设计实现一个云原生分布式数据库系统，采用**Log-is-Database**架构，实现存储与计算分离。

### 1.1 技术栈

| 模块 | 开发语言 | 版本 | 说明 |
|------|----------|------|------|
| **计算层** | C++ | MySQL 8.4.3-3 | 基于 Percona Server 改造 |
| **存储层** | Golang | 1.21+ | 独立开发 |
| **元数据服务** | Golang | 1.21+ | 基于 Raft 协议 |
| **控制平面** | Golang | 1.21+ | 集群管理 |
| **模块通信** | gRPC | 1.60+ | Protocol Buffers |

### 1.2 文档索引

| 文档 | 说明 |
|------|------|
| [01_compute_layer.md](./01_compute_layer.md) | 计算层设计（C++ MySQL） |
| [02_storage_layer.md](./02_storage_layer.md) | 存储层设计（Golang） |
| [03_metadata_service.md](./03_metadata_service.md) | 元数据服务设计（Golang） |
| [04_control_plane.md](./04_control_plane.md) | 控制平面设计（Golang） |
| [05_grpc_protocol.md](./05_grpc_protocol.md) | gRPC 协议定义 |
| [06_physical_format.md](./06_physical_format.md) | 物理文件格式设计 |
| [07_cross_region.md](./07_cross_region.md) | 跨城容灾设计 |
| [08_dts.md](./08_dts.md) | 数据传输服务（DTS）设计 |
| [09_backup_pitr.md](./09_backup_pitr.md) | 备份恢复与 PITR 设计 |
| [10_replication_protocol.md](./10_replication_protocol.md) | 复制协议设计（Quorum/Multi-Raft） |
| [11_network_layer.md](./11_network_layer.md) | 网络层设计（TCP/RDMA） |

---

## 2. 系统架构

### 2.1 整体架构图

```mermaid
graph TB
    subgraph "客户端层"
        Client[MySQL Client]
    end
    
    subgraph "控制平面（Golang）"
        CP1[API Gateway]
        CP2[Cluster Manager]
        CP3[Failover Controller]
        CP4[Monitor Service]
    end
    
    subgraph "计算层（C++ MySQL 8.4.3-3）"
        Writer[Writer Instance]
        Reader1[Reader Instance 1]
        Reader2[Reader Instance 2]
    end
    
    subgraph "存储层（Golang）"
        SN1[Storage Node 1<br/>AZ-a]
        SN2[Storage Node 2<br/>AZ-a]
        SN3[Storage Node 3<br/>AZ-b]
        SN4[Storage Node 4<br/>AZ-b]
        SN5[Storage Node 5<br/>AZ-c]
        SN6[Storage Node 6<br/>AZ-c]
    end
    
    subgraph "元数据服务（Golang + Raft）"
        Meta1[Meta Node 1<br/>Leader]
        Meta2[Meta Node 2<br/>Follower]
        Meta3[Meta Node 3<br/>Follower]
    end
    
    subgraph "增值服务（Golang）"
        Backup[备份服务]
        DTS[数据传输服务]
        CrossRegion[跨城容灾]
    end
    
    Client --> Writer
    Client --> Reader1
    Client --> Reader2
    
    CP1 --> CP2
    CP2 --> CP3
    CP2 --> CP4
    CP4 --> Writer
    CP3 --> Writer
    
    Writer -->|Redo Log| SN1
    Writer -->|Redo Log| SN3
    Writer -->|Redo Log| SN5
    
    Reader1 -->|Read Page| SN1
    Reader2 -->|Read Page| SN3
    
    Writer --> Meta1
    SN1 --> Meta1
    
    Meta1 --> Meta2
    Meta1 --> Meta3
    
    SN1 --> Backup
    Writer --> DTS
    Writer --> CrossRegion
    
    style Writer fill:#ffe1e1,stroke:#333,stroke-width:3px,color:#000
    style Reader1 fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style Reader2 fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style Meta1 fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style CP1 fill:#e1e1ff,stroke:#333,stroke-width:2px,color:#000
    style Backup fill:#ffe1ff,stroke:#333,stroke-width:2px,color:#000
    style DTS fill:#e1ffff,stroke:#333,stroke-width:2px,color:#000
    style CrossRegion fill:#ffffe1,stroke:#333,stroke-width:2px,color:#000
```

### 2.2 模块职责

| 模块 | 职责 | 关键功能 |
|------|------|----------|
| **计算层** | SQL 处理与事务管理 | SQL 解析、查询优化、Redo 生成、Buffer Pool |
| **存储层** | 数据持久化与物化 | Redo 接收、Page 物化、Quorum 写入、Coalescing |
| **元数据服务** | 元数据管理 | Volume 管理、LSN 追踪、PG 映射、强一致性 |
| **控制平面** | 集群管理 | 健康监控、故障切换、实例管理、调度 |
| **备份服务** | 数据保护 | 自动备份、快照管理、PITR 恢复 |
| **DTS** | 数据传输 | 数据迁移、实时同步、CDC 订阅 |
| **跨城容灾** | 异地容灾 | Binlog 同步、容灾切换、全局管理 |

### 2.3 Quorum 配置

| 参数 | 值 | 说明 |
|------|-----|------|
| **N** | 6 | 总副本数 |
| **Vw** | 4 | 写入 Quorum |
| **Vr** | 3 | 读取 Quorum |
| **AZ** | 3 | 可用区数量 |

---

## 3. 模块间通信

### 3.1 gRPC 服务端口

| 服务 | 端口 | 提供方 | 说明 |
|------|------|--------|------|
| `StorageService` | 9002 | 存储层 | Redo 写入、Page 读取 |
| `MetadataService` | 9003 | 元数据服务 | VDL 管理、PG 映射 |
| `ComputeService` | 9001 | 计算层 | 健康检查、角色切换 |
| `ControlPlaneService` | 9000 | 控制平面 | 集群管理 API |
| `CrossRegionService` | 9010 | 跨城服务 | Binlog 同步、容灾切换 |
| `DTSService` | 9020 | DTS | 数据迁移、同步任务 |
| `BackupService` | 9030 | 备份服务 | 快照管理、PITR |

### 3.2 模块间调用关系

```mermaid
graph LR
    subgraph "调用方"
        C1[计算层]
        C2[控制平面]
        C3[存储层]
        C4[DTS]
        C5[跨城容灾]
        C6[备份服务]
    end
    
    subgraph "被调用方"
        S1[StorageService<br/>:9002]
        S2[MetadataService<br/>:9003]
        S3[ComputeService<br/>:9001]
        S4[S3/OSS<br/>对象存储]
    end
    
    C1 -->|WriteRedo / ReadPage| S1
    C1 -->|UpdateVDL / GetVDL| S2
    C2 -->|HealthCheck / Promote| S3
    C2 -->|FreezeWrites| S1
    C3 -->|UpdateNodeLSN| S2
    C4 -->|GetBinlog| C1
    C5 -->|ReplicateBinlog| S1
    C6 -->|UploadSnapshot| S4
    C6 -->|DownloadRedo| S4
    
    style S1 fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style S2 fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style S3 fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style S4 fill:#e1e1ff,stroke:#333,stroke-width:2px,color:#000
```

---

## 4. 核心场景时序图

### 4.1 写入流程（Write Path）

```mermaid
sequenceDiagram
    participant Client as MySQL Client
    participant Writer as Writer Instance<br/>(C++ MySQL)
    participant Storage as Storage Layer<br/>(6 Nodes, Golang)
    participant Meta as Metadata Service<br/>(Golang)

    Client->>Writer: 1. INSERT INTO users VALUES(...)
    Writer->>Writer: 2. SQL 解析与优化
    Writer->>Writer: 3. 修改 Buffer Pool
    Writer->>Writer: 4. 生成 Redo Log, 分配 LSN
    
    par 并行写入 6 个存储节点
        Writer->>Storage: 5a. gRPC WriteRedo LSN data → Node1
        Writer->>Storage: 5b. gRPC WriteRedo LSN data → Node2
        Writer->>Storage: 5c. gRPC WriteRedo LSN data → Node3
        Writer->>Storage: 5d. gRPC WriteRedo LSN data → Node4
        Writer->>Storage: 5e. gRPC WriteRedo LSN data → Node5
        Writer->>Storage: 5f. gRPC WriteRedo LSN data → Node6
    end
    
    Storage-->>Writer: 6a. ACK from Node1
    Storage-->>Writer: 6b. ACK from Node2
    Storage-->>Writer: 6c. ACK from Node3
    Storage-->>Writer: 6d. ACK from Node4
    Note over Writer: 收到 4 个 ACK Quorum 达成
    
    Writer->>Meta: 7. gRPC UpdateVDL volume_id new_vdl
    Meta-->>Writer: 8. VDL 更新确认
    
    Writer-->>Client: 9. Query OK 1 row affected
```

### 4.2 读取流程（Read Path）

```mermaid
sequenceDiagram
    participant Client as MySQL Client
    participant Reader as Reader Instance<br/>(C++ MySQL)
    participant Buffer as Buffer Pool
    participant Meta as Metadata Service<br/>(Golang)
    participant Storage as Storage Node<br/>(Golang)

    Client->>Reader: 1. SELECT FROM users WHERE id=100
    Reader->>Reader: 2. SQL 解析与优化
    Reader->>Buffer: 3. 查找 Page 是否在 Buffer Pool
    
    alt Page 在 Buffer Pool 中
        Buffer-->>Reader: 4a. 返回缓存的 Page
    else Page 不在 Buffer Pool
        Reader->>Meta: 4b. gRPC GetPageLocation page_id
        Meta-->>Reader: 5. 返回 Storage Node 列表
        Reader->>Storage: 6. gRPC ReadPage page_id target_lsn
        Storage->>Storage: 7. 物化 Page Base Page 加 Redo
        Storage-->>Reader: 8. 返回 16KB Page 数据
        Reader->>Buffer: 9. 将 Page 加入 Buffer Pool
    end
    
    Reader->>Reader: 10. 执行查询 提取结果
    Reader-->>Client: 11. 返回查询结果
```

### 4.3 事务提交流程（Commit Path）

```mermaid
sequenceDiagram
    participant Client as MySQL Client
    participant Writer as Writer Instance
    participant Storage as Storage Layer
    participant Meta as Metadata Service

    Client->>Writer: 1. BEGIN
    Client->>Writer: 2. INSERT INTO orders...
    Writer->>Writer: 3. 生成 INSERT Redo LSN=1000
    Writer->>Storage: 4. WriteRedo LSN=1000
    Storage-->>Writer: 5. Quorum ACK
    
    Client->>Writer: 6. UPDATE inventory...
    Writer->>Writer: 7. 生成 UPDATE Redo LSN=1010
    Writer->>Storage: 8. WriteRedo LSN=1010
    Storage-->>Writer: 9. Quorum ACK
    
    Client->>Writer: 10. COMMIT
    Writer->>Writer: 11. 生成 COMMIT Redo LSN=1020
    Writer->>Storage: 12. WriteRedo LSN=1020
    Storage-->>Writer: 13. Quorum ACK 4/6
    
    Writer->>Meta: 14. gRPC UpdateVDL VDL=1020
    Meta-->>Writer: 15. 确认
    
    Writer-->>Client: 16. Query OK COMMIT successful
```

### 4.4 Reader 同步流程

```mermaid
sequenceDiagram
    participant Writer as Writer Instance
    participant Storage as Storage Layer
    participant Reader as Reader Instance
    participant Buffer as Reader Buffer Pool

    Writer->>Storage: 1. WriteRedo LSN=1000 data
    Storage->>Storage: 2. 持久化 Redo
    Storage-->>Writer: 3. ACK
    
    loop Reader 持续同步
        Reader->>Storage: 4. gRPC GetRedoLogs from_lsn to_lsn
        Storage-->>Reader: 5. 返回 Redo 日志列表
        
        loop 应用每条 Redo
            alt Page 在 Buffer 中
                Reader->>Buffer: 6a. 应用 Redo 到缓存 Page
            else Page 不在 Buffer 中
                Reader->>Reader: 6b. 标记 Page 失效
            end
        end
        
        Reader->>Reader: 7. 更新 read_point
    end
```

### 4.5 故障切换流程（Failover）

```mermaid
sequenceDiagram
    participant Monitor as Monitor Service<br/>(Golang)
    participant Failover as Failover Controller<br/>(Golang)
    participant OldWriter as Old Writer<br/>崩溃
    participant NewWriter as Reader → Writer
    participant Storage as Storage Layer
    participant Meta as Metadata Service

    Monitor->>OldWriter: 1. gRPC HealthCheck
    Note over OldWriter: 无响应 崩溃
    Monitor->>Monitor: 2. 连续 3 次失败 确认故障
    Monitor->>Failover: 3. ReportFailure writer_id
    
    Failover->>Storage: 4. gRPC FreezeWrites volume_id
    Storage->>Storage: 5. 停止接受新 Redo
    Storage-->>Failover: 6. FreezeResponse final_vdl=50000
    
    Failover->>Failover: 7. 选择 LSN 最高的 Reader
    Failover->>NewWriter: 8. gRPC CatchUp target_lsn=50000
    NewWriter->>Storage: 9. GetRedoLogs current 50000
    Storage-->>NewWriter: 10. 返回增量 Redo
    NewWriter->>NewWriter: 11. 应用增量 Redo
    NewWriter-->>Failover: 12. CatchUp 完成
    
    Failover->>NewWriter: 13. gRPC PromoteToWriter
    NewWriter->>NewWriter: 14. 切换为 Writer 模式
    NewWriter-->>Failover: 15. Promote 成功
    
    Failover->>Meta: 16. UpdateWriter new_writer_id
    Failover->>Storage: 17. gRPC UnfreezeWrites
    
    Failover->>NewWriter: 18. StartAcceptingConnections
    Note over NewWriter: 开始接受 MySQL 连接
```

### 4.6 跨城容灾同步流程

```mermaid
sequenceDiagram
    participant Writer_A as Writer 主区域
    participant BinlogGen as Binlog Generator
    participant Sender as Replica Sender
    participant Network as 跨域专线
    participant Receiver as Replica Receiver
    participant Headless_B as Headless Writer<br/>灾备区域
    participant Storage_B as 存储层 灾备区域

    Writer_A->>Writer_A: 执行 DML 事务
    Writer_A->>Writer_A: 生成 Redo Log
    Writer_A->>BinlogGen: 提交事务
    
    BinlogGen->>BinlogGen: Redo 转换为 Binlog
    BinlogGen->>BinlogGen: 分配 GTID
    BinlogGen->>Sender: 写入 Binlog Buffer
    
    loop 批量发送
        Sender->>Sender: 收集 Binlog 批次
        Sender->>Sender: LZ4 压缩
        Sender->>Network: gRPC ReplicateBinlog
        Network->>Receiver: 跨域传输
        
        Receiver->>Receiver: 解压 写入 Relay Log
        Receiver->>Headless_B: 转换为 Redo 应用
        Headless_B->>Storage_B: gRPC WriteRedo
        Storage_B-->>Headless_B: Quorum ACK
        
        Receiver-->>Sender: ACK GTID
    end
```

### 4.7 PITR 恢复流程

```mermaid
sequenceDiagram
    participant User as 用户
    participant API as Backup API
    participant RestoreMgr as Restore Manager
    participant Meta as Metadata
    participant S3 as S3 存储
    participant NewCluster as 新集群

    User->>API: RestoreToPointInTime 目标时间
    API->>RestoreMgr: 创建恢复任务
    
    RestoreMgr->>Meta: 时间转 LSN
    Meta-->>RestoreMgr: target_lsn
    
    RestoreMgr->>Meta: 查找最近快照
    Meta-->>RestoreMgr: snapshot_id lsn
    
    RestoreMgr->>Meta: 查找 Redo 归档段
    Meta-->>RestoreMgr: Redo 段列表
    
    RestoreMgr->>RestoreMgr: 创建新集群
    
    par 恢复快照
        RestoreMgr->>S3: 下载 Page 快照
        S3-->>RestoreMgr: Page 数据
        RestoreMgr->>NewCluster: 写入 Page
    end
    
    loop 回放 Redo
        RestoreMgr->>S3: 下载 Redo 段
        S3-->>RestoreMgr: Redo 数据
        RestoreMgr->>NewCluster: 应用 Redo 直到 target_lsn
    end
    
    RestoreMgr->>NewCluster: 启动集群
    NewCluster-->>RestoreMgr: 就绪
    RestoreMgr-->>User: 返回新集群信息
```

### 4.8 数据迁移流程（DTS）

```mermaid
sequenceDiagram
    participant User as 用户
    participant DTS as DTS 服务
    participant Source as 源数据库
    participant Target as 目标 Aurora

    User->>DTS: 创建迁移任务
    DTS-->>User: task_id
    
    User->>DTS: 启动任务
    
    Note over DTS,Source: 阶段1: 结构迁移
    DTS->>Source: SHOW CREATE TABLE
    Source-->>DTS: DDL 语句
    DTS->>Target: CREATE TABLE
    
    Note over DTS,Source: 阶段2: 全量迁移
    loop 分页抽取
        DTS->>Source: SELECT chunk
        Source-->>DTS: 数据批次
        DTS->>Target: LOAD DATA
    end
    
    Note over DTS,Source: 阶段3: 增量同步
    DTS->>Source: 连接 Binlog
    loop 增量同步
        Source-->>DTS: Binlog Event
        DTS->>Target: 应用变更
    end
    
    User->>DTS: 完成切换
    DTS-->>User: 迁移完成
```

---

## 5. 数据流图

### 5.1 Redo Log 数据流

```mermaid
graph LR
    subgraph "计算层"
        SQL[SQL 执行] --> Buffer[Buffer 修改]
        Buffer --> RedoGen[Redo 生成]
        RedoGen --> RedoBuf[Redo Buffer]
    end
    
    subgraph "网络层"
        RedoBuf --> gRPC[gRPC WriteRedo]
    end
    
    subgraph "存储层"
        gRPC --> SN1[Node 1]
        gRPC --> SN2[Node 2]
        gRPC --> SN3[Node 3]
        gRPC --> SN4[Node 4]
        gRPC --> SN5[Node 5]
        gRPC --> SN6[Node 6]
        
        SN1 --> WAL1[WAL File]
        SN2 --> WAL2[WAL File]
        SN3 --> WAL3[WAL File]
        SN4 --> WAL4[WAL File]
        SN5 --> WAL5[WAL File]
        SN6 --> WAL6[WAL File]
    end
    
    style RedoGen fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style gRPC fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
```

### 5.2 VDL 计算

```mermaid
graph TB
    subgraph "6 个存储节点的 LSN 状态"
        N1[Node1: LSN=1000]
        N2[Node2: LSN=995]
        N3[Node3: LSN=1000]
        N4[Node4: LSN=998]
        N5[Node5: LSN=990]
        N6[Node6: LSN=1000]
    end
    
    subgraph "VDL 计算过程"
        Sort[排序: 1000 1000 1000 998 995 990]
        VDL[VDL = 第 4 大值 = 998]
    end
    
    N1 --> Sort
    N2 --> Sort
    N3 --> Sort
    N4 --> Sort
    N5 --> Sort
    N6 --> Sort
    Sort --> VDL
    
    style VDL fill:#fff3e1,stroke:#333,stroke-width:3px,color:#000
```

---

## 6. 部署架构

### 6.1 典型部署拓扑

```mermaid
graph TB
    subgraph "AZ-a"
        CP1[控制平面节点 1]
        Writer[Writer Instance]
        SN1[Storage Node 1]
        SN2[Storage Node 2]
        Meta1[Metadata Node 1]
    end
    
    subgraph "AZ-b"
        CP2[控制平面节点 2]
        Reader1[Reader Instance 1]
        SN3[Storage Node 3]
        SN4[Storage Node 4]
        Meta2[Metadata Node 2]
    end
    
    subgraph "AZ-c"
        CP3[控制平面节点 3]
        Reader2[Reader Instance 2]
        SN5[Storage Node 5]
        SN6[Storage Node 6]
        Meta3[Metadata Node 3]
    end
    
    subgraph "增值服务"
        Backup[备份服务]
        DTS[DTS 服务]
    end
    
    subgraph "灾备区域"
        DR[跨城容灾节点]
    end
    
    style Writer fill:#ffe1e1,stroke:#333,stroke-width:3px,color:#000
    style Reader1 fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style Reader2 fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style Meta1 fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style DR fill:#e1e1ff,stroke:#333,stroke-width:2px,color:#000
```

### 6.2 最小部署配置

| 组件 | 最小数量 | 推荐数量 | 说明 |
|------|----------|----------|------|
| Writer | 1 | 1 | 单写入点 |
| Reader | 0 | 2+ | 按读负载扩展 |
| Storage Node | 6 | 6 | 固定 6 副本 |
| Metadata Node | 3 | 3 | Raft 需要奇数 |
| Control Plane | 1 | 3 | 高可用建议 3 个 |
| 备份服务 | 1 | 2 | 高可用建议 2 个 |
| DTS 服务 | 0 | 1+ | 按需部署 |
| 跨城容灾 | 0 | 1 | 按需部署 |

---

## 7. 关键设计决策

### 7.1 设计原则

| 原则 | 说明 |
|------|------|
| **Log-is-Database** | Redo Log 是唯一的持久化数据源，Page 是派生数据 |
| **Quorum Write** | 写入 4/6 节点即可确认，容忍 2 节点故障 |
| **Lazy Materialization** | Page 按需物化，减少不必要的 I/O |
| **Shared Storage** | 所有计算节点共享同一存储层 |
| **Separation of Concerns** | 计算、存储、元数据分离 |

### 7.2 性能目标

| 指标 | 目标值 |
|------|--------|
| 写入延迟 (P99) | < 5ms |
| 读取延迟 (P99) | < 2ms (Buffer 命中), < 10ms (需物化) |
| 主从复制延迟 | < 20ms |
| Failover 时间 | < 30s |
| 存储吞吐量 | > 100K IOPS / 节点 |
| 跨城同步延迟 | < 100ms |
| PITR 恢复时间 | < 10min（取决于数据量） |

---

## 8. 代码仓库结构

```
aurora/
├── cmd/                          # 各服务入口
│   ├── storage-node/             # 存储节点 (Golang)
│   ├── metadata-service/         # 元数据服务 (Golang)
│   ├── control-plane/            # 控制平面 (Golang)
│   ├── backup-service/           # 备份服务 (Golang)
│   ├── dts-service/              # DTS 服务 (Golang)
│   └── cross-region/             # 跨城容灾 (Golang)
├── pkg/                          # 公共包
│   ├── proto/                    # gRPC 定义
│   ├── redo/                     # Redo 处理
│   ├── page/                     # Page 处理
│   └── quorum/                   # Quorum 协议
├── internal/                     # 内部包
│   ├── storage/                  # 存储层实现
│   ├── metadata/                 # 元数据实现
│   ├── control/                  # 控制平面实现
│   ├── backup/                   # 备份实现
│   ├── dts/                      # DTS 实现
│   └── crossregion/              # 跨城容灾实现
├── mysql-plugin/                 # MySQL 插件 (C++)
│   ├── storage_engine/           # 存储引擎适配
│   └── redo_sender/              # Redo 发送模块
└── docs/                         # 文档
```

---

## 附录 A：术语表

| 术语 | 说明 |
|------|------|
| **VDL** | Volume Durable LSN，已持久化的最高 LSN |
| **VCL** | Volume Complete LSN，已完成的最高 LSN |
| **MTR** | Mini Transaction，最小事务单元 |
| **PG** | Protection Group，保护组（10GB 数据单元） |
| **Quorum** | 法定人数，达成一致的最小节点数 |
| **Coalescing** | 合并，将 Redo 应用到 Base Page 生成新 Page |
| **PITR** | Point-In-Time Recovery，时间点恢复 |
| **DTS** | Data Transmission Service，数据传输服务 |
| **CDC** | Change Data Capture，变更数据捕获 |
| **GTID** | Global Transaction Identifier，全局事务标识 |
