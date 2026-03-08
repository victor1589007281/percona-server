# Aurora-like 存算分离架构 Skills

本目录包含 MySQL Aurora-like 存算分离架构的模块化设计文档，用于指导后续功能开发。

## 技术选型

| 组件 | 实现语言 | 通信协议 |
|------|----------|----------|
| 计算层 (MySQL/InnoDB 改造) | C++ | - |
| 存储层 (全部服务) | Golang | gRPC + RDMA |
| HA 模块 | Golang | gRPC |
| Binlog 模块 | Golang | gRPC + MySQL Protocol |

## 目录结构

```
aurora/
├── README.md                 # 本文件
├── 00-overview.md           # 整体架构概述
├── 01-metadata-service.md   # 元数据服务 (Go)
├── 02-storage-layer.md      # 存储层 (Go)
├── 03-compute-layer.md      # 计算层改造 (C++)
├── 04-schema-mvcc.md        # Schema 多版本 (Go)
├── 05-redo-management.md    # Redo Log 管理 (Go)
├── 06-page-management.md    # 页管理 (Go)
├── 07-protocol.md           # 通信协议 (gRPC + RDMA)
├── 08-ro-consistency.md     # RO 节点一致性 (Go)
├── 09-gc-mechanism.md       # GC 机制 (Go)
├── 10-ha-module.md          # 高可用模块 (Go)
├── 11-binlog-module.md      # Binlog 模块 (Go)
├── 12-io-optimization.md    # IO 优化 (全局)
├── 13-block-alignment.md    # 块对齐与原子写 (Go)
├── 14-ro-node-detail.md     # RO 节点详细设计 (Go+C++)
├── 15-binlog-redo-merge.md  # Binlog-Redo 合并 (Go+C++)
├── 16-ha-zero-loss.md       # HA 零丢失快速切换 (Go)
├── 17-backup-snapshot-pitr.md  # 备份/快照/PITR (Go)
└── 18-geo-disaster-recovery.md # 跨城容灾 (Go)
```

## Skill 概览

| # | Skill | 语言 | 职责 | 依赖 |
|---|-------|------|------|------|
| 00 | Overview | - | 整体架构和开发路线图 | - |
| 01 | Metadata Service | Go | 表空间管理、页索引、块分配 | - |
| 02 | Storage Layer | Go | Redo 存储、页存储、数据服务 | 01 |
| 03 | Compute Layer | C++ | MySQL/InnoDB 改造点 | 07 |
| 04 | Schema MVCC | Go | DDL 多版本、无阻塞查询 | 01, 06 |
| 05 | Redo Management | Go | Redo 接收、持久化、应用 | 01, 06 |
| 06 | Page Management | Go | 页存储、COW、多版本 | 01 |
| 07 | Protocol | Go+C++ | gRPC + RDMA 接口 | - |
| 08 | RO Consistency | Go | Schema Snapshot、可见性 | 04, 05 |
| 09 | GC Mechanism | Go | Schema GC、页版本清理 | 04, 05, 06 |
| 10 | HA Module | Go | 故障检测、Failover、Fencing | 07 |
| 11 | Binlog Module | Go | CDC、MySQL 复制支持 | 02, 05 |
| 12 | IO Optimization | - | 全局 IO 优化策略 | 全部 |
| 13 | Block Alignment | Go | 块对齐、原子写、无 Double Write | 02, 05, 06 |
| 14 | RO Node Detail | Go+C++ | visible_lsn、timestamp、Read View | 03, 08 |
| 15 | Binlog-Redo Merge | Go+C++ | 统一日志、GTID、崩溃恢复 | 05, 11 |
| 16 | HA Zero Loss | Go | Quorum 写、快速 Failover | 10 |
| 17 | Backup/PITR | Go | 快照、备份、时间点恢复 | 02, 05 |
| 18 | Geo DR | Go | 跨城复制、灾难切换 | 02, 10, 16 |

## 开发顺序建议

```
Phase 1: 基础设施 (并行)
─────────────────────────
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ 07-protocol  │  │01-metadata   │  │ 06-page-mgmt │
│ (Go + C++)   │  │   (Go)       │  │    (Go)      │
└──────────────┘  └──────────────┘  └──────────────┘

Phase 2: 存储层核心
─────────────────────────
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ 05-redo-mgmt │  │02-storage    │  │ 11-binlog    │
│    (Go)      │  │   (Go)       │  │    (Go)      │
└──────────────┘  └──────────────┘  └──────────────┘

Phase 3: 计算层改造
─────────────────────────
┌──────────────┐
│ 03-compute   │
│   (C++)      │
└──────────────┘

Phase 4: Schema MVCC
─────────────────────────
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ 04-schema    │  │08-ro         │  │ 09-gc        │
│   (Go)       │  │  (Go)        │  │   (Go)       │
└──────────────┘  └──────────────┘  └──────────────┘

Phase 5: 高可用
─────────────────────────
┌──────────────┐
│ 10-ha-module │
│    (Go)      │
└──────────────┘
```

## 快速导航

### 我想了解整体架构
→ 阅读 [00-overview.md](00-overview.md)

### 我要开发存储层 (Golang)
→ 阅读 [02-storage-layer.md](02-storage-layer.md)
→ 然后 [01-metadata-service.md](01-metadata-service.md)
→ 然后 [05-redo-management.md](05-redo-management.md)
→ 然后 [06-page-management.md](06-page-management.md)

### 我要改造 MySQL 代码 (C++)
→ 阅读 [03-compute-layer.md](03-compute-layer.md)

### 我要实现通信协议 (gRPC + RDMA)
→ 阅读 [07-protocol.md](07-protocol.md)

### 我要实现 DDL 无阻塞
→ 阅读 [04-schema-mvcc.md](04-schema-mvcc.md)
→ 然后 [08-ro-consistency.md](08-ro-consistency.md)

### 我要实现高可用 (Golang)
→ 阅读 [10-ha-module.md](10-ha-module.md)

### 我要实现 Binlog/CDC (Golang)
→ 阅读 [11-binlog-module.md](11-binlog-module.md)

### 我要实现 GC
→ 阅读 [09-gc-mechanism.md](09-gc-mechanism.md)

### 我要做 IO 优化
→ 阅读 [12-io-optimization.md](12-io-optimization.md)
→ 阅读 [13-block-alignment.md](13-block-alignment.md) (原子写)

### 我要细化 RO 节点
→ 阅读 [14-ro-node-detail.md](14-ro-node-detail.md)

### 我要合并 Binlog 和 Redo
→ 阅读 [15-binlog-redo-merge.md](15-binlog-redo-merge.md)

### 我要实现零丢失 HA
→ 阅读 [16-ha-zero-loss.md](16-ha-zero-loss.md)

### 我要实现备份和 PITR
→ 阅读 [17-backup-snapshot-pitr.md](17-backup-snapshot-pitr.md)

### 我要实现跨城容灾
→ 阅读 [18-geo-disaster-recovery.md](18-geo-disaster-recovery.md)

## 协议说明

### gRPC (控制面)
- 用于低频、小数据操作
- GetStatus, CreateTablespace, Schema 查询等
- 标准 protobuf 定义

### RDMA (数据面)
- 用于高频、大数据操作
- WriteRedo, ReadPage
- 零拷贝、低延迟
- 支持 InfiniBand / RoCE

## 主设计文档

完整设计请参考:
- [mysql-storage-architecture-distributed-block-design.md](../mysql-storage-architecture-distributed-block-design.md)

## 贡献指南

1. 每个 Skill 文档应包含:
   - 实现语言标注
   - 模块职责
   - 核心数据结构 (Go/C++ 代码示例)
   - 关键算法/流程
   - 开发任务 checklist
   - 参考文档

2. 更新时同步更新:
   - 主设计文档
   - 相关 Skill 的依赖引用
   - 本 README 的概览表

3. 命名规范:
   - 文件名: `NN-skill-name.md`
   - 编号两位数，便于排序
