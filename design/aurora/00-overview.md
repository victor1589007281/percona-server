# Aurora-like 存算分离架构 - 概述

## 1. 项目目标

将 MySQL/InnoDB 改造为类 Aurora 的存算分离架构：
- 计算层只写 Redo Log
- 存储层负责 Redo 应用和数据页管理
- 支持一写多读 (1 RW + N RO)
- DDL 期间 RO 查询无阻塞 (Schema MVCC)
- 自动 HA 故障切换
- 支持外部 MySQL 从库和 CDC

## 2. 技术选型

| 组件 | 语言 | 说明 |
|------|------|------|
| 计算层 | C++ | MySQL/InnoDB 改造 |
| 存储层 | Golang | 全部存储服务 |
| 元数据服务 | Golang | 表空间、页索引管理 |
| HA 模块 | Golang | 故障检测、选主 |
| Binlog 模块 | Golang | CDC、复制支持 |
| 协议 | gRPC + RDMA | 控制面 + 数据面 |

## 3. 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      仲裁服务 (Etcd/Consul)                      │
│                      • 选主  • 租约  • 元数据                    │
└───────────────────────────────┬─────────────────────────────────┘
                                │
┌───────────────────────────────┼─────────────────────────────────┐
│                         计算层 (C++)                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │   RW Node    │  │   RO Node    │  │   RO Node    │          │
│  │ + HA Agent   │  │ + HA Agent   │  │ + HA Agent   │          │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘          │
│         │                 │                 │                   │
│         │ gRPC/RDMA       │ gRPC/RDMA       │ gRPC/RDMA        │
└─────────┼─────────────────┼─────────────────┼───────────────────┘
          │                 │                 │
          ▼                 ▼                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                      存储层 (Golang)                             │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    元数据服务                             │   │
│  │  • 表空间注册表 (db.table → space_id, schema_v)         │   │
│  │  • 页索引 (space_id, schema_v, page_no → LBA)           │   │
│  │  • 块分配器                                              │   │
│  └─────────────────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │                    数据服务                               │   │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐        │   │
│  │  │ Redo Store  │ │ Page Store  │ │ Binlog Store│        │   │
│  │  └─────────────┘ └─────────────┘ └─────────────┘        │   │
│  └─────────────────────────────────────────────────────────┘   │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │              Schema MVCC  |  GC Manager                   │   │
│  └─────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
          │
          ▼
┌─────────────────────────────────────────────────────────────────┐
│                    分布式块存储 (Block Storage)                  │
└─────────────────────────────────────────────────────────────────┘
          │
          ▼
┌─────────────────┐  ┌─────────────────┐
│  外部 MySQL 从库 │  │   CDC Consumer  │
│   (Binlog)      │  │  (Kafka, etc)   │
└─────────────────┘  └─────────────────┘
```

## 4. 核心 Skill 模块

| # | 模块 | 语言 | 文件 | 职责 |
|---|------|------|------|------|
| 00 | Overview | - | `00-overview.md` | 整体架构 |
| 01 | 元数据服务 | Go | `01-metadata-service.md` | 表空间、页索引、块分配 |
| 02 | 存储层 | Go | `02-storage-layer.md` | Redo/Page Store |
| 03 | 计算层 | C++ | `03-compute-layer.md` | MySQL/InnoDB 改造 |
| 04 | Schema MVCC | Go | `04-schema-mvcc.md` | DDL 多版本 |
| 05 | Redo 管理 | Go | `05-redo-management.md` | Redo 接收/应用 |
| 06 | 页管理 | Go | `06-page-management.md` | 页存储、COW |
| 07 | 通信协议 | Go+C++ | `07-protocol.md` | gRPC + RDMA |
| 08 | RO 一致性 | Go | `08-ro-consistency.md` | Schema Snapshot |
| 09 | GC 机制 | Go | `09-gc-mechanism.md` | 版本清理 |
| 10 | HA 模块 | Go | `10-ha-module.md` | 故障检测、Failover |
| 11 | Binlog 模块 | Go | `11-binlog-module.md` | CDC、复制 |
| 12 | IO 优化 | - | `12-io-optimization.md` | 全局 IO 优化 |
| 13 | 块对齐 | Go | `13-block-alignment.md` | 原子写、无 Double Write |
| 14 | RO 节点详设 | Go+C++ | `14-ro-node-detail.md` | timestamp、Read View |
| 15 | Binlog-Redo 合并 | Go+C++ | `15-binlog-redo-merge.md` | 统一日志、GTID |
| 16 | HA 零丢失 | Go | `16-ha-zero-loss.md` | Quorum、快速切换 |
| 17 | 备份/PITR | Go | `17-backup-snapshot-pitr.md` | 快照、恢复 |
| 18 | 跨城容灾 | Go | `18-geo-disaster-recovery.md` | 异地复制、灾难切换 |

## 5. 关键设计决策

### 5.1 语言选择
- **计算层 (C++)**: 保持 MySQL/InnoDB 原有语言，最小改动
- **存储层 (Golang)**: 高并发、GC 友好、开发效率高

### 5.2 协议选择
- **控制面 (gRPC)**: 简单可靠，适合低频操作
- **数据面 (RDMA)**: 零拷贝、低延迟，适合大数据传输 (Redo/Page)

### 5.3 核心映射关系
```
(db_name, table_name, snapshot_lsn)
    ↓
(space_id, schema_version)
    ↓
(space_id, schema_version, page_no)
    ↓
LBA (块地址)
```

## 6. 开发路线图

### Phase 1: 基础设施 (并行)
- [ ] 07-protocol: gRPC 服务定义
- [ ] 01-metadata: 元数据服务框架
- [ ] 06-page: 页索引、块分配器

### Phase 2: 存储层核心
- [ ] 05-redo: Redo Store
- [ ] 02-storage: 存储层整合
- [ ] 11-binlog: Binlog Store

### Phase 3: 计算层改造
- [ ] 03-compute: log_writer() 改造
- [ ] 03-compute: buf_page_get() 改造
- [ ] 07-protocol: RDMA 支持

### Phase 4: Schema MVCC
- [ ] 04-schema: Schema Version 管理
- [ ] 08-ro: Schema Snapshot
- [ ] 09-gc: Schema GC

### Phase 5: HA 与生产就绪
- [ ] 10-ha: HA Agent
- [ ] 10-ha: Failover 流程
- [ ] 10-ha: Fencing 机制
- [ ] 监控、告警、运维工具

### Phase 6: IO 优化 (贯穿各阶段)
- [ ] Redo Group Commit + LZ4 压缩
- [ ] 页预读 + 两级缓存
- [ ] 并行 Redo 应用 + 写合并
- [ ] RDMA Pipeline
- [ ] 元数据索引分片
- [ ] Binlog 压缩 + 流式推送

### Phase 7: 数据保护
- [ ] 逻辑快照
- [ ] 全量/增量备份
- [ ] PITR (时间点恢复)
- [ ] Quorum 写入 (零丢失)
- [ ] 快速 Failover (RTO < 30s)

### Phase 8: 跨城容灾
- [ ] 跨城异步复制
- [ ] 半同步复制 (可选)
- [ ] 全局仲裁
- [ ] 灾难切换
- [ ] 数据修复

## 7. 参考文档

- 主设计文档: `mysql-storage-architecture-distributed-block-design.md`
- AWS Aurora 论文: "Amazon Aurora: Design Considerations..."
- PolarDB 文档: DDL Synchronization
- RDMA 编程指南
