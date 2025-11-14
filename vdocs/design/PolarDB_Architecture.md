# PolarDB 云原生数据库架构设计与技术解析

## 1. 概述

PolarDB 是阿里云推出的云原生关系型数据库，采用**存储与计算分离**的架构设计。它兼容 MySQL、PostgreSQL 和 Oracle，提供高性能、高可用性和弹性扩展能力。相比传统 MySQL，PolarDB 在高并发场景下性能可提升 6 倍以上，存储容量最高可达 100TB。

## 2. 架构设计

### 2.1 整体架构

```mermaid
graph TB
    subgraph "**客户端层**"
        A[**应用程序**]
    end
    
    subgraph "**计算层 - Compute Nodes**"
        B[**主节点 Primary**]
        C[**只读节点 1**]
        D[**只读节点 2**]
        E[**只读节点 N**]
    end
    
    subgraph "**存储层 - PolarFS**"
        F[**共享分布式存储**]
        G[**ChunkServer 1**]
        H[**ChunkServer 2**]
        I[**ChunkServer 3**]
    end
    
    subgraph "**管理与监控层**"
        J[**集群管理器**]
        K[**监控告警系统**]
        L[**备份恢复服务**]
    end
    
    A -->|SQL请求| B
    A -->|读请求| C
    A -->|读请求| D
    A -->|读请求| E
    
    B -->|Redo Log| F
    C -->|读数据| F
    D -->|读数据| F
    E -->|读数据| F
    
    F --> G
    F --> H
    F --> I
    
    J -.->|管理| B
    J -.->|管理| C
    J -.->|管理| D
    J -.->|管理| E
    K -.->|监控| F
    L -.->|备份| F
    
    style A fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style D fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style G fill:#f5f5dc,stroke:#333,stroke-width:2px,color:#000
    style H fill:#f5f5dc,stroke:#333,stroke-width:2px,color:#000
    style I fill:#f5f5dc,stroke:#333,stroke-width:2px,color:#000
    style J fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
    style K fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
    style L fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
```

### 2.2 核心特性

- **存储计算分离**：计算节点和存储节点独立扩展
- **一写多读**：1 个主节点 + 最多 15 个只读节点
- **共享存储**：所有计算节点共享同一份数据
- **RDMA 网络**：计算层与存储层通过 RDMA 实现低延迟通信

## 3. 功能设计与模块划分

### 3.1 计算层（Compute Layer）

```mermaid
graph LR
    subgraph "**计算节点内部架构**"
        A[**SQL Parser<br/>SQL解析器**]
        B[**Optimizer<br/>查询优化器**]
        C[**Executor<br/>执行引擎**]
        D[**Buffer Pool<br/>缓冲池**]
        E[**Transaction<br/>Manager<br/>事务管理**]
        F[**LogIndex<br/>日志索引**]
    end
    
    A --> B
    B --> C
    C --> D
    C --> E
    E --> F
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
```

**计算层保留的 MySQL 核心功能：**

| **功能模块** | **保留能力** | **优化改进** |
|------------|------------|------------|
| **SQL 解析** | ✅ 完全兼容 MySQL 语法 | 支持分布式查询优化 |
| **查询优化器** | ✅ 保留 MySQL 优化器 | 增加并行查询优化 |
| **事务管理** | ✅ 完整 ACID 支持 | 分布式事务优化 |
| **存储引擎接口** | ✅ InnoDB 兼容 | 适配共享存储 |
| **连接管理** | ✅ 连接池管理 | 支持更高并发 |
| **权限管理** | ✅ 完整用户权限系统 | 云原生身份认证 |
| **复制机制** | ⚠️ 改为物理复制 | 基于 Redo Log 复制 |

### 3.2 存储层（Storage Layer - PolarFS）

```mermaid
graph TB
    subgraph "**PolarFS 分布式文件系统**"
        A[**元数据服务器**]
        B[**ChunkServer 集群**]
        C[**三副本机制**]
        D[**RDMA 网络**]
    end
    
    subgraph "**存储功能**"
        E[**数据页存储**]
        F[**Redo Log 持久化**]
        G[**快照与备份**]
        H[**数据恢复**]
    end
    
    A --> B
    B --> C
    C --> D
    
    B --> E
    B --> F
    B --> G
    B --> H
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
    style G fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
    style H fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
```

## 4. 数据交互流程

### 4.1 写入流程

```mermaid
sequenceDiagram
    participant **C** as **客户端**
    participant **P** as **主节点**
    participant **BP** as **Buffer Pool**
    participant **RL** as **Redo Log**
    participant **PFS** as **PolarFS存储**
    participant **RO** as **只读节点**
    
    **C**->>**P**: **1. 发送 INSERT/UPDATE**
    **P**->>**P**: **2. SQL 解析与优化**
    **P**->>**BP**: **3. 修改 Buffer Pool**
    **P**->>**RL**: **4. 写入 Redo Log**
    **RL**->>**PFS**: **5. Redo Log 持久化**
    **PFS**-->>**P**: **6. 持久化确认**
    **P**-->>**C**: **7. 返回成功**
    
    Note over **PFS**,**RO**: **异步过程**
    **PFS**->>**RO**: **8. Redo Log 同步**
    **RO**->>**RO**: **9. Replay Redo Log**
    
    rect rgb(255, 250, 205)
    Note over **P**,**PFS**: **关键优化：只需同步 Redo Log，不需要同步数据页**
    end
```

### 4.2 读取流程

```mermaid
sequenceDiagram
    participant **C** as **客户端**
    participant **RO** as **只读节点**
    participant **BP** as **Buffer Pool**
    participant **LI** as **LogIndex**
    participant **PFS** as **PolarFS存储**
    
    **C**->>**RO**: **1. 发送 SELECT 查询**
    **RO**->>**BP**: **2. 检查 Buffer Pool**
    
    alt **缓存命中**
        **BP**-->>**RO**: **3a. 返回缓存数据**
    else **缓存未命中**
        **RO**->>**LI**: **3b. 查询 LogIndex**
        **LI**-->>**RO**: **4. 返回最新 LSN**
        **RO**->>**PFS**: **5. 读取数据页**
        **PFS**-->>**RO**: **6. 返回数据页**
        **RO**->>**RO**: **7. 应用增量 Redo Log**
        **RO**->>**BP**: **8. 更新 Buffer Pool**
    end
    
    **RO**-->>**C**: **9. 返回查询结果**
    
    rect rgb(255, 250, 205)
    Note over **RO**,**PFS**: **LogIndex 优化：快速定位数据页的最新版本**
    end
```

## 5. 技术创新点

### 5.1 LogIndex 机制

**LogIndex** 是 PolarDB 的核心创新，用于解决只读节点的数据一致性问题。

```mermaid
graph LR
    subgraph "**传统方式**"
        A[**扫描全部<br/>Redo Log**]
        B[**性能瓶颈**]
    end
    
    subgraph "**LogIndex 优化**"
        C[**Page ID + LSN<br/>索引映射**]
        D[**快速定位**]
        E[**并行回放**]
    end
    
    A --> B
    C --> D
    D --> E
    
    style A fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style C fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style D fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style E fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
```

**LogIndex 工作原理：**
- 维护 `Page ID -> LSN` 的映射关系
- 只读节点只需回放与查询相关的 Redo Log
- 支持并行回放，大幅提升性能

### 5.2 Redo Log 改动

| **改动项** | **传统 MySQL** | **PolarDB** | **优势** |
|----------|--------------|-----------|--------|
| **日志格式** | 物理日志 | 物理日志（优化） | 减少日志量 |
| **写入位置** | 本地磁盘 | 共享存储（PolarFS） | 主从共享 |
| **同步方式** | 异步/半同步 | 基于 RDMA 的低延迟同步 | 微秒级延迟 |
| **回放机制** | 串行回放 | 并行回放（基于 LogIndex） | 10x+ 性能提升 |

## 6. 高可用架构

### 6.1 故障检测与切换

```mermaid
graph TB
    subgraph "**故障检测机制**"
        A[**心跳检测<br/>每秒检测**]
        B[**健康检查<br/>3次失败触发**]
        C[**网络分区检测**]
    end
    
    subgraph "**故障切换流程**"
        D[**检测到主节点故障**]
        E[**选择最新的只读节点**]
        F[**提升为新主节点**]
        G[**更新 DNS/VIP**]
        H[**客户端重连**]
    end
    
    subgraph "**数据一致性保证**"
        I[**Redo Log LSN 比较**]
        J[**选择 LSN 最大的节点**]
        K[**确保无数据丢失**]
    end
    
    A --> B
    B --> C
    C --> D
    
    D --> E
    E --> I
    I --> J
    J --> F
    F --> G
    G --> H
    H --> K
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style G fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style H fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style I fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style J fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style K fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
```

**故障切换时间：**
- **检测时间**：< 10 秒
- **切换时间**：< 30 秒
- **总 RTO**：< 1 分钟

## 7. Serverless 实现

```mermaid
graph TB
    subgraph "**资源池**"
        A[**计算资源池**]
        B[**空闲计算节点**]
        C[**活跃计算节点**]
    end
    
    subgraph "**自动伸缩**"
        D[**负载监控**]
        E[**扩容触发<br/>CPU > 70%**]
        F[**缩容触发<br/>CPU < 30%**]
    end
    
    subgraph "**快速启动**"
        G[**预热节点池**]
        H[**秒级启动<br/>&lt; 10秒**]
        I[**连接共享存储**]
    end
    
    D --> E
    D --> F
    E --> G
    G --> H
    H --> I
    I --> C
    
    A --> B
    B --> C
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
    style G fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
    style H fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style I fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
```

**Serverless 核心能力：**
1. **按需分配**：根据负载自动分配计算资源
2. **秒级启动**：计算节点无状态，连接共享存储即可启动
3. **按量计费**：按实际使用的 CPU/内存/时长计费

## 8. PITR（时间点恢复）

```mermaid
graph LR
    subgraph "**备份策略**"
        A[**全量快照<br/>每天1次**]
        B[**增量 Redo Log<br/>持续归档**]
    end
    
    subgraph "**恢复流程**"
        C[**1. 选择恢复时间点**]
        D[**2. 定位最近快照**]
        E[**3. 恢复快照数据**]
        F[**4. 应用 Redo Log**]
        G[**5. 恢复到目标时间**]
    end
    
    A --> C
    B --> C
    C --> D
    D --> E
    E --> F
    F --> G
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
    style G fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
```

**PITR 能力：**
- **恢复粒度**：秒级
- **保留时长**：7-730 天可配置
- **恢复速度**：TB 级数据 < 1 小时

## 9. Binlog 订阅解决方案

### 9.1 实现方案

```mermaid
graph TB
    subgraph "**Binlog 生成**"
        A[**主节点**]
        B[**Redo Log**]
        C[**Binlog 转换模块**]
        D[**Binlog 文件**]
    end
    
    subgraph "**订阅方式**"
        E[**Canal/Maxwell**]
        F[**MySQL Binlog 协议**]
        G[**DTS 数据传输服务**]
    end
    
    A --> B
    B --> C
    C --> D
    D --> E
    D --> F
    D --> G
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
    style G fill:#e8e1ff,stroke:#333,stroke-width:2px,color:#000
```

**解决方案：**
1. **兼容模式**：生成标准 MySQL Binlog，支持主流订阅工具
2. **DTS 服务**：阿里云提供的数据传输服务，原生支持 PolarDB
3. **CDC 能力**：Change Data Capture，实时捕获数据变更

## 10. 计算层快速启动

### 10.1 启动优化

```mermaid
graph LR
    subgraph "**传统 MySQL 启动**"
        A[**加载数据文件**]
        B[**恢复 Redo Log**]
        C[**构建索引**]
        D[**启动时间<br/>分钟级**]
    end
    
    subgraph "**PolarDB 快速启动**"
        E[**无本地数据**]
        F[**连接 PolarFS**]
        G[**加载元数据**]
        H[**启动时间<br/>&lt; 10秒**]
    end
    
    A --> B
    B --> C
    C --> D
    
    E --> F
    F --> G
    G --> H
    
    style A fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style C fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style D fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style E fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style F fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style G fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style H fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
```

**快速启动原因：**
1. **无状态设计**：计算节点不存储数据
2. **共享存储**：直接连接 PolarFS，无需加载数据
3. **元数据预热**：只需加载必要的元数据

## 11. 主从数据同步

### 11.1 同步机制

```mermaid
graph TB
    subgraph "**主节点**"
        A[**执行事务**]
        B[**生成 Redo Log**]
        C[**写入 PolarFS**]
    end
    
    subgraph "**共享存储 PolarFS**"
        D[**Redo Log 持久化**]
        E[**数据页**]
    end
    
    subgraph "**只读节点**"
        F[**读取 Redo Log**]
        G[**Replay 到 Buffer Pool**]
        H[**读取数据页**]
    end
    
    A --> B
    B --> C
    C --> D
    D --> F
    F --> G
    E --> H
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style F fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style G fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style H fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
```

**同步的数据：**
1. **Redo Log**：事务日志（主要同步内容）
2. **数据页**：通过共享存储访问，无需同步
3. **元数据**：表结构、索引信息等

**同步延迟：**
- **物理复制延迟**：微秒级（< 100μs）
- **逻辑复制延迟**：毫秒级（传统 binlog）

## 12. 使用场景

### 12.1 适用场景

```mermaid
graph TB
    subgraph "**高并发场景**"
        A[**电商大促**]
        B[**社交应用**]
        C[**游戏业务**]
    end
    
    subgraph "**弹性扩展场景**"
        D[**业务波动大**]
        E[**Serverless 应用**]
        F[**成本优化需求**]
    end
    
    subgraph "**高可用场景**"
        G[**金融业务**]
        H[**核心系统**]
        I[**SLA > 99.99%**]
    end
    
    subgraph "**混合负载**"
        J[**OLTP + OLAP**]
        K[**读写分离**]
        L[**实时分析**]
    end
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style D fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style F fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style G fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style H fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style I fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style J fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style K fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style L fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
```

## 13. 资源成本分析

### 13.1 成本对比

| **资源维度** | **传统 MySQL** | **PolarDB** | **优势分析** |
|------------|--------------|-----------|------------|
| **CPU** | 固定配置，利用率低 | 按需分配，弹性伸缩 | **节省 30-50%** |
| **内存** | 每个实例独立 Buffer Pool | 主从共享 Buffer Pool（优化） | **节省 20-40%** |
| **磁盘** | 主从各自存储完整数据 | 共享存储，单份数据 | **节省 50-70%** |
| **网络** | 传输完整数据页 | 只传输 Redo Log | **减少 80%+ 流量** |
| **运维** | 人工管理 | 自动化管理 | **降低 60% 运维成本** |

### 13.2 成本优化图

```mermaid
graph LR
    subgraph "**传统架构成本**"
        A[**CPU<br/>100%**]
        B[**内存<br/>100%**]
        C[**磁盘<br/>100%**]
        D[**总成本<br/>100%**]
    end
    
    subgraph "**PolarDB 成本**"
        E[**CPU<br/>50-70%**]
        F[**内存<br/>60-80%**]
        G[**磁盘<br/>30-50%**]
        H[**总成本<br/>40-60%**]
    end
    
    A -.->|优化| E
    B -.->|优化| F
    C -.->|优化| G
    D ==>|节省| H
    
    style A fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style B fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style C fill:#ffd7d7,stroke:#333,stroke-width:2px,color:#000
    style D fill:#ffd7d7,stroke:#333,stroke-width:3px,color:#000
    style E fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style F fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style G fill:#d7ffd7,stroke:#333,stroke-width:2px,color:#000
    style H fill:#d7ffd7,stroke:#333,stroke-width:3px,color:#000
```

## 14. 核心问题解答总结

### 14.1 问题汇总

| **问题** | **PolarDB 解决方案** | **技术关键** |
|---------|-------------------|------------|
| **1. 计算层保留能力** | 完整保留 MySQL 核心功能（SQL解析、事务、存储引擎） | InnoDB 兼容 + 共享存储适配 |
| **2. Binlog 订阅** | 生成标准 Binlog + DTS 服务 | Redo Log 转 Binlog |
| **3. 主从同步** | 基于 Redo Log 的物理复制 | 共享存储 + LogIndex |
| **4. Redo 改动** | RDMA 低延迟 + 并行回放 + LogIndex | 微秒级同步延迟 |
| **5. 高可用** | 秒级故障检测 + 自动切换 + LSN 保证一致性 | RTO < 1分钟 |
| **6. Serverless** | 无状态计算节点 + 资源池化 + 秒级启动 | 按量计费 |
| **7. PITR** | 全量快照 + Redo Log 归档 | 秒级恢复粒度 |
| **8. 快速启动** | 无本地数据 + 共享存储 | < 10秒启动 |
| **9. 资源成本** | 存储节省 50-70%，总成本节省 40-60% | 共享存储 + 弹性伸缩 |

## 15. 参考资料

1. **官方文档**：
   - [PolarDB 产品文档](https://www.aliyun.com/product/polardb)
   - [PolarDB 技术白皮书](https://developer.aliyun.com/article/polardb)

2. **学术论文**：
   - POLARDB Meets Computational Storage: Efficiently Support Analytical Workloads in Cloud-Native Relational Database (FAST 2020)

3. **技术博客**：
   - [PolarDB 存储计算分离架构解析](https://developer.aliyun.com)
   - [PolarDB LogIndex 技术详解](https://developer.aliyun.com)

---

**文档版本**：v1.0  
**最后更新**：2025-11-04  
**作者**：云原生数据库技术团队

