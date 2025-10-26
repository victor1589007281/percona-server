# MyRocks 引擎 MVCC 实现机制源码分析

## 1. 概述

MyRocks 是 RocksDB 作为 MySQL 存储引擎的实现，它基于 LSM-Tree（Log-Structured Merge Tree）数据结构，通过**基于序列号的快照隔离**（Sequence Number-based Snapshot Isolation）实现 MVCC（多版本并发控制）。与 InnoDB 基于 Undo Log 回滚段的机制不同，MyRocks 利用 LSM-Tree 的天然多版本存储特性、递增序列号和快照功能来实现高效的并发控制，**避免了传统的回滚段开销**。

### 核心特点
- **序列号版本控制**：每个写操作分配全局递增的 SequenceNumber
- **LSM-Tree 多版本存储**：同一键的不同版本并存于不同层级
- **快照时点一致性**：基于序列号实现精确的时点读取
- **Compaction 垃圾回收**：自动清理无快照引用的旧版本数据

## 2. 架构概览

```mermaid
graph TD
    A["应用程序 SQL 请求"] --> B["MySQL Server Layer"]
    B --> C["MyRocks Handler (ha_rocksdb.cc)"]
    C --> D{事务类型}
    
    D --> E["悲观事务 (Rdb_transaction_impl)"]
    D --> F["乐观事务 (OptimisticTransaction)"]
    D --> G["WriteBatch (Rdb_writebatch_impl)"]
    
    E --> H["RocksDB TransactionDB"]
    F --> I["RocksDB OptimisticTransactionDB"]
    G --> J["RocksDB WriteBatchWithIndex"]
    
    H --> K["快照管理<br/>acquire_snapshot()<br/>release_snapshot()"]
    I --> K
    J --> L["直接写入，无锁"]
    
    K --> M["RocksDB Snapshot"]
    M --> N["LSM Tree 数据读取"]
    
    O["隔离级别"] --> P["READ COMMITTED<br/>每次读取获取新快照"]
    O --> Q["REPEATABLE READ<br/>事务开始获取快照"]
    
    P --> K
    Q --> K
    
    N --> R["合并 WriteBatch 和<br/>已提交数据返回结果"]
```

## 2.1. 基于序列号的多版本实现机制 ⭐

MyRocks 的多版本控制**不依赖 Undo Log**，而是基于 LSM-Tree 的天然多版本存储能力和全局递增的序列号（SequenceNumber）系统。

```mermaid
graph TD
    subgraph "用户操作和序列号生成"
        A["应用程序 SQL 写操作"] --> B["MyRocks Handler"]
        B --> C["RocksDB Transaction"]
        C --> D["分配 SequenceNumber<br/>递增唯一标识"]
    end
    
    subgraph "InternalKey 格式"
        E["用户键: user_key"] --> F["内部键结构"]
        D --> F
        G["类型: ValueType<br/>(PUT/DELETE/MERGE)"] --> F
        F --> H["InternalKey:<br/>[user_key][seq(56bit)+type(8bit)]"]
    end
    
    subgraph "LSM-Tree 多版本存储"
        H --> I["写入 LSM-Tree"]
        I --> J["Level 0 (MemTable -> SST)"]
        J --> K["Level 1"]
        K --> L["Level N..."]
        M["同一 user_key<br/>多个版本并存"] --> J
        M --> K
        M --> L
    end
    
    subgraph "快照和可见性"
        N["创建快照"] --> O["记录当前<br/>SequenceNumber"]
        O --> P["ReadOptions.snapshot"]
        P --> Q["读操作时<br/>可见性判断"]
        Q --> R["seq <= snapshot_seq<br/>? 可见 : 不可见"]
    end
    
    subgraph "垃圾回收机制"
        S["Compaction 触发"] --> T["合并多层级数据"]
        T --> U["保留最新版本"]
        U --> V["清理无快照引用<br/>的旧版本"]
        V --> W["释放存储空间"]
    end
```

### 2.1.1 InternalKey 格式和序列号编码

**源码位置**: `storage/rocksdb/rocksdb/db/dbformat.h:109-140`

```cpp
// 解析后的内部键结构
struct ParsedInternalKey {
  Slice user_key;                    // 用户键
  SequenceNumber sequence;           // 序列号 (56 bits)
  ValueType type;                    // 操作类型 (8 bits)
  
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) {}
};

// 序列号和类型打包为 64 位
inline uint64_t PackSequenceAndType(uint64_t seq, ValueType t) {
  assert(seq <= kMaxSequenceNumber);  // 最大序列号: (1ull << 56) - 1
  return (seq << 8) | t;              // 序列号占高 56 位，类型占低 8 位
}
```

```mermaid
graph TB
    subgraph "InternalKey 格式详解"
        A["InternalKey 总长度 = user_key_len + 8 bytes"]
        
        B["user_key<br/>(变长)"] --> C["sequence_number<br/>(56 bits)"]
        C --> D["value_type<br/>(8 bits)"]
        
        E["示例: key='user1', seq=100, type=PUT"]
        E --> F["InternalKey = 'user1' + 0x640001<br/>(100 << 8 | 1)"]
    end
    
    subgraph "ValueType 枚举"
        G["kTypeDeletion = 0x0"]
        H["kTypeValue = 0x1"]
        I["kTypeMerge = 0x2"]
        J["kTypeBlobIndex = 0x8"]
        K["kTypeSingleDeletion = 0x7"]
        L["kTypeRangeDeletion = 0xF"]
    end
    
    subgraph "InternalKey 比较规则"
        M["1. 首先按 user_key 排序<br/>(字典序)"]
        N["2. user_key 相同时<br/>按 sequence_number 降序<br/>(新版本在前)"]
        O["3. sequence_number 相同时<br/>按 value_type 升序"]
        
        M --> N --> O
    end
    
    subgraph "LSM-Tree 中的排序示例"
        P["user1@seq=102@DEL"]
        Q["user1@seq=101@PUT -> value2"]
        R["user1@seq=100@PUT -> value1"]
        S["user2@seq=103@PUT -> value3"]
        
        P --> Q --> R --> S
        
        T["相同 user_key 的多版本<br/>按序列号降序排列<br/>读取时优先返回高序列号版本"]
    end
```

### 2.1.2 快照可见性判断机制

**源码位置**: `storage/rocksdb/rocksdb/db/read_callback.h:26-39`

```cpp
class ReadCallback {
protected:
  SequenceNumber max_visible_seq_ = kMaxSequenceNumber;  // 快照序列号
  const SequenceNumber min_uncommitted_ = kMinUnCommittedSeq;

public:
  // 核心可见性判断逻辑
  inline bool IsVisible(SequenceNumber seq) {
    if (seq < min_uncommitted_) {
      // 已提交数据，检查是否在快照范围内
      return seq <= max_visible_seq_;
    } else if (max_visible_seq_ < seq) {
      // 序列号大于快照，不可见
      return false;
    } else {
      // 需要进一步检查（事务相关）
      return IsVisibleFullCheck(seq);
    }
  }
};
```

### 2.1.3 多版本数据存储和读取流程

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant MyRocks as MyRocks Handler
    participant TxDB as TransactionDB
    participant LSM as LSM Tree
    participant Compaction as Compaction进程

    Note over App,Compaction: 基于序列号的多版本示例
    
    App->>MyRocks: PUT key1=value1
    MyRocks->>TxDB: 分配 SeqNum=100
    TxDB->>LSM: 写入 [key1][100|PUT] -> value1
    LSM-->>App: 写入完成
    
    App->>MyRocks: 创建快照 Snapshot1
    MyRocks->>TxDB: GetSnapshot() -> SeqNum=100
    TxDB-->>MyRocks: Snapshot1(seq=100)
    
    App->>MyRocks: PUT key1=value2
    MyRocks->>TxDB: 分配 SeqNum=101
    TxDB->>LSM: 写入 [key1][101|PUT] -> value2
    Note over LSM: 现在有两个版本：<br/>[key1][100|PUT] -> value1<br/>[key1][101|PUT] -> value2
    
    App->>MyRocks: 使用 Snapshot1 读取 key1
    MyRocks->>TxDB: Get(key1, snapshot=100)
    TxDB->>LSM: 查找 seq <= 100 的版本
    LSM-->>TxDB: 返回 [key1][100|PUT] -> value1
    TxDB-->>App: value1 (旧版本)
    
    App->>MyRocks: 不使用快照读取 key1
    MyRocks->>TxDB: Get(key1, current)
    TxDB->>LSM: 查找最新版本
    LSM-->>TxDB: 返回 [key1][101|PUT] -> value2
    TxDB-->>App: value2 (最新版本)
    
    App->>MyRocks: DELETE key1
    MyRocks->>TxDB: 分配 SeqNum=102
    TxDB->>LSM: 写入 [key1][102|DEL]
    Note over LSM: 现在有三个版本：<br/>[key1][100|PUT] -> value1<br/>[key1][101|PUT] -> value2<br/>[key1][102|DEL]
    
    Note over Compaction: 触发 Compaction
    Compaction->>LSM: 检查版本和快照引用
    Compaction->>Compaction: Snapshot1 仍引用 seq=100
    Compaction->>LSM: 保留 [key1][100|PUT] 和 [key1][102|DEL]<br/>清理 [key1][101|PUT] (无引用)
    
    App->>MyRocks: 释放 Snapshot1
    MyRocks->>TxDB: ReleaseSnapshot(100)
    
    Note over Compaction: 再次 Compaction
    Compaction->>LSM: 无快照引用，清理所有旧版本<br/>只保留 [key1][102|DEL]
```

## 3. 核心组件分析

### 3.1 事务类层次结构

MyRocks 实现了三种事务类型，都继承自 `Rdb_transaction` 基类：

#### 3.1.1 Rdb_transaction 基类
**源码位置**: `storage/rocksdb/ha_rocksdb.cc:3092-4305`

```cpp
class Rdb_transaction {
protected:
  ulonglong m_write_count = 0;           // 写操作计数
  ulonglong m_row_lock_count = 0;        // 行锁计数
  bool m_is_delayed_snapshot = false;    // 延迟快照标志
  THD *m_thd = nullptr;                  // MySQL 线程句柄
  bool m_tx_read_only = false;           // 只读事务标志
  int m_timeout_sec = 0;                 // 锁等待超时
  
public:
  rocksdb::ReadOptions m_read_opts;      // RocksDB 读选项
  virtual void acquire_snapshot(bool acquire_now) = 0;
  virtual void release_snapshot() = 0;
};
```

#### 3.1.2 Rdb_transaction_impl（悲观事务）
**源码位置**: `storage/rocksdb/ha_rocksdb.cc:4355-4773`

```cpp
class Rdb_transaction_impl : public Rdb_transaction {
private:
  rocksdb::Transaction *m_rocksdb_tx = nullptr;    // RocksDB 事务对象
  rocksdb::Transaction *m_rocksdb_reuse_tx = nullptr;  // 事务复用对象
  
public:
  // 快照获取实现
  void acquire_snapshot(bool acquire_now) override {
    if (m_read_opts.snapshot == nullptr) {
      if (is_tx_read_only()) {
        // 只读事务直接从 DB 获取快照
        snapshot_created(rdb->GetSnapshot());
      } else if (acquire_now) {
        // 立即获取事务快照
        m_rocksdb_tx->SetSnapshot();
        snapshot_created(m_rocksdb_tx->GetSnapshot());
      } else if (!m_is_delayed_snapshot) {
        // 延迟快照：在下次操作时获取
        m_rocksdb_tx->SetSnapshotOnNextOperation(m_notifier);
        m_is_delayed_snapshot = true;
      }
    }
  }
};
```

#### 3.1.3 Rdb_writebatch_impl（批写事务）
**源码位置**: `storage/rocksdb/ha_rocksdb.cc:4783-4940`

```cpp
class Rdb_writebatch_impl : public Rdb_transaction {
private:
  rocksdb::WriteBatchWithIndex *m_batch;  // 带索引的写批次
  
public:
  // 简化的快照获取，直接从 DB 获取
  void acquire_snapshot(bool acquire_now) override {
    if (m_read_opts.snapshot == nullptr) 
      snapshot_created(rdb->GetSnapshot());
  }
};
```

### 3.2 隔离级别实现

#### 3.2.1 START TRANSACTION WITH CONSISTENT SNAPSHOT
**源码位置**: `storage/rocksdb/ha_rocksdb.cc:6075-6100`

```cpp
static int rocksdb_start_tx_and_assign_read_view(
    handlerton *const hton, THD *const thd) {
  ulong const tx_isolation = my_core::thd_tx_isolation(thd);
  
  Rdb_transaction *tx = get_or_create_tx(thd);
  assert(!tx->has_snapshot());
  tx->set_tx_read_only(true);
  rocksdb_register_tx(hton, thd, tx);

  if (tx_isolation == ISO_REPEATABLE_READ) {
    tx->acquire_snapshot(true);  // 立即获取一致性快照
  } else {
    push_warning_printf(thd, Sql_condition::SL_WARNING, HA_ERR_UNSUPPORTED,
                        "RocksDB: Only REPEATABLE READ isolation level is "
                        "supported for START TRANSACTION WITH CONSISTENT "
                        "SNAPSHOT in RocksDB Storage Engine.");
  }
  return HA_EXIT_SUCCESS;
}
```

#### 3.2.2 隔离级别对比

```mermaid
graph LR
    subgraph "READ COMMITTED"
        A1["事务开始"] --> B1["读操作1"]
        B1 --> C1["获取快照S1"]
        C1 --> D1["返回数据"]
        D1 --> E1["读操作2"]
        E1 --> F1["获取新快照S2"]
        F1 --> G1["返回最新数据"]
        G1 --> H1["提交"]
    end
    
    subgraph "REPEATABLE READ"
        A2["事务开始"] --> B2["acquire_snapshot()"]
        B2 --> C2["获取快照S"]
        C2 --> D2["读操作1"]
        D2 --> E2["使用快照S"]
        E2 --> F2["读操作2"]
        F2 --> G2["使用相同快照S"]
        G2 --> H2["返回一致数据"]
        H2 --> I2["提交"]
    end
    
    subgraph "WriteBatch模式"
        A3["批量操作"] --> B3["WriteBatchWithIndex"]
        B3 --> C3["累积写操作"]
        C3 --> D3["支持读取自己的写入"]
        D3 --> E3["原子提交"]
        E3 --> F3["高性能，无锁"]
    end
    
    subgraph "悲观 vs 乐观事务"
        A4["悲观事务<br/>TransactionDB"] --> B4["立即获取锁"]
        B4 --> C4["串行化冲突操作"]
        A5["乐观事务<br/>OptimisticTransactionDB"] --> B5["无锁执行"]
        B5 --> C5["提交时冲突检测"]
    end
```

## 4. MVCC 事务执行流程

### 4.1 REPEATABLE READ 事务完整流程

```mermaid
sequenceDiagram
    participant App as 应用程序
    participant MySQL as MySQL Server
    participant MyRocks as MyRocks Handler
    participant TxDB as RocksDB TransactionDB
    participant Snap as 快照管理器
    participant LSM as LSM Tree

    Note over App,LSM: REPEATABLE READ 事务示例
    
    App->>MySQL: BEGIN TRANSACTION
    MySQL->>MyRocks: start_transaction()
    MyRocks->>TxDB: BeginTransaction(tx_options)
    TxDB-->>MyRocks: Transaction对象
    
    App->>MySQL: SELECT * FROM table WHERE id=1
    MySQL->>MyRocks: 首次读取请求
    MyRocks->>TxDB: acquire_snapshot(true)
    TxDB->>Snap: SetSnapshot()
    Snap-->>TxDB: Snapshot对象
    TxDB->>LSM: Get(ReadOptions with snapshot)
    LSM-->>TxDB: 基于快照的数据
    TxDB-->>MyRocks: 返回数据
    MyRocks-->>App: 查询结果
    
    Note over App,LSM: 其他事务修改数据
    
    App->>MySQL: SELECT * FROM table WHERE id=1
    MySQL->>MyRocks: 再次读取同一数据
    MyRocks->>TxDB: Get(使用相同snapshot)
    TxDB->>LSM: 从相同快照读取
    LSM-->>TxDB: 相同的历史数据
    TxDB-->>MyRocks: 一致的结果
    MyRocks-->>App: 相同的查询结果
    
    App->>MySQL: UPDATE table SET val=? WHERE id=1
    MySQL->>MyRocks: 写入操作
    MyRocks->>TxDB: Put(key, value)
    TxDB->>TxDB: 写入WriteBatchWithIndex
    Note over TxDB: 支持读取自己的写入
    
    App->>MySQL: COMMIT
    MySQL->>MyRocks: commit()
    MyRocks->>TxDB: Commit()
    TxDB->>LSM: 原子写入LSM Tree
    TxDB->>Snap: ReleaseSnapshot()
    TxDB-->>MyRocks: 提交成功
    MyRocks-->>App: 事务完成
```

### 4.2 快照生命周期管理

**快照获取机制**:
- **立即获取**: `acquire_snapshot(true)` - 用于 REPEATABLE READ
- **延迟获取**: `SetSnapshotOnNextOperation()` - 用于 READ COMMITTED  
- **只读事务**: 直接从 DB 获取全局快照

**快照释放机制**:
```cpp
void release_snapshot() override {
  bool need_clear = m_is_delayed_snapshot;
  
  if (m_read_opts.snapshot != nullptr) {
    m_snapshot_timestamp = 0;
    if (is_tx_read_only()) {
      rdb->ReleaseSnapshot(m_read_opts.snapshot);  // 释放 DB 快照
      need_clear = false;
    } else {
      need_clear = true;  // 标记需要清理事务快照
    }
    m_read_opts.snapshot = nullptr;
  }
  
  if (need_clear && m_rocksdb_tx != nullptr) 
    m_rocksdb_tx->ClearSnapshot();  // 清理事务快照
  m_is_delayed_snapshot = false;
}
```

## 5. WriteBatchWithIndex 实现 Read-Your-Own-Writes

### 5.1 核心原理

**源码位置**: `storage/rocksdb/rocksdb/utilities/write_batch_with_index/write_batch_with_index.cc`

WriteBatchWithIndex 是 RocksDB 的核心组件，用于解决事务内 "读取自己写入的数据" 问题：

```cpp
struct WriteBatchWithIndex::Rep {
  ReadableWriteBatch write_batch;           // 实际的写批次
  WriteBatchEntryComparator comparator;     // 条目比较器
  Arena arena;                              // 内存分配器
  WriteBatchEntrySkipList skip_list;        // 跳跃表索引
  bool overwrite_key;                       // 是否覆盖键
  size_t last_entry_offset;                // 最后条目偏移
  size_t sub_batch_cnt;                     // 子批次数量
};
```

### 5.2 查询合并逻辑

当事务内进行读操作时，MyRocks 会：

1. **首先查询 WriteBatchWithIndex**: 检查当前事务的未提交写入
2. **然后查询 RocksDB**: 获取已提交的数据
3. **合并结果**: 优先返回事务内的修改，否则返回 DB 中的数据

```cpp
// 伪代码示例
auto result = wbwii.GetFromBatch(this, keys[i], &merge_context, &batch_value, s);

if (result == WBWIIteratorImpl::kFound) {
  // 在写批次中找到了数据，直接返回
  *pinnable_val->GetSelf() = std::move(batch_value);
  pinnable_val->PinSelf();
  continue;
}
if (result == WBWIIteratorImpl::kDeleted) {
  // 在写批次中被删除
  *s = Status::NotFound();
  continue;
}
// 否则从 DB 中查询
```

## 6. 悲观事务 vs 乐观事务

### 6.1 悲观事务（TransactionDB）

**特点**:
- 读写时立即获取锁
- 避免冲突，但可能导致死锁
- 适用于冲突较多的场景

**源码示例**:
```cpp
// storage/rocksdb/rocksdb/examples/transaction_example.cc
Transaction* txn = txn_db->BeginTransaction(write_options);
s = txn->GetForUpdate(read_options, "abc", &value);  // 获取排他锁
s = txn->Put("abc", "def");
s = txn->Commit();
```

### 6.2 乐观事务（OptimisticTransactionDB）

**特点**:
- 执行过程中不加锁
- 提交时检测冲突
- 适用于冲突较少的场景

**源码示例**:
```cpp
// storage/rocksdb/rocksdb/examples/optimistic_transaction_example.cc
Transaction* txn = txn_db->BeginTransaction(write_options);
s = txn->Get(read_options, "abc", &value);
s = txn->Put("abc", "xyz");
s = txn->Commit();  // 如果有冲突会返回 Busy 状态
```

## 7. 与 InnoDB MVCC 的对比

| 特性 | MyRocks (基于序列号) | InnoDB (基于回滚段) |
|------|---------|--------|
| **多版本实现** | **序列号 + LSM-Tree 天然多版本** | **Undo Log 回滚段重构历史** |
| **版本标识** | **全局递增 SequenceNumber (56bit)** | **事务ID + 回滚指针** |
| **版本存储** | **InternalKey 包含序列号直接存储** | **最新版本 + Undo Log 链** |
| **读取机制** | **按序列号过滤，直接读取对应版本** | **从最新版本回溯 Undo Log** |
| **版本清理** | **Compaction 时检查快照引用** | **Purge 线程清理无引用 Undo** |
| **存储开销** | **每版本完整存储，压缩补偿** | **增量存储，回滚开销** |
| **并发性能** | **无锁读取，Compaction 异步** | **Undo Log 竞争，Purge 延迟** |
| **快照成本** | **O(1) 记录序列号** | **O(N) 活跃事务列表** |
| **垃圾回收** | **后台 Compaction 自动** | **专门 Purge 线程** |
| **空间放大** | **写放大，读优化** | **相对紧凑** |

### 7.1 多版本实现机制的根本差异

#### MyRocks: 基于时间戳的正向版本链
```cpp
// MyRocks 中同一个键的多个版本
LSM-Tree 存储：
key1@seq=102@DEL              // 最新：删除操作
key1@seq=101@PUT -> value2    // 中间版本
key1@seq=100@PUT -> value1    // 历史版本

// 快照读取 (snapshot_seq=100)
读取逻辑：找到 seq <= 100 且类型不为 DEL 的最新版本
结果：直接返回 key1@seq=100 -> value1
```

#### InnoDB: 基于回滚段的逆向重构
```cpp
// InnoDB 中同一行的版本链
聚集索引：key1 -> value2 (最新版本，trx_id=101)
Undo Log： value2 -> value1 (回滚指针指向历史)

// 快照读取 (ReadView 看不到 trx_id=101)
读取逻辑：从最新版本开始，沿 Undo Log 回溯
步骤：value2 -> 检查 ReadView -> 不可见 -> 沿 Undo 回溯 -> value1
结果：重构后返回 value1
```

### 7.2 性能特征对比

| 场景 | MyRocks 优势 | InnoDB 优势 |
|------|-------------|------------|
| **历史版本读取** | 直接定位，无需重构 | 只存储增量，空间省 |
| **大量并发读** | 无锁读取，线性扩展 | ReadView 复制开销 |
| **长时间快照** | Compaction 推迟清理 | Undo Log 积累过多 |
| **频繁更新** | 版本累积，空间膨胀 | 增量存储，相对紧凑 |
| **批量写入** | LSM-Tree 顺序写优秀 | B+树随机写，页分裂 |

### 7.3 适用场景建议

**MyRocks 更适合**：
- 写多读少的 OLTP 场景
- 需要长时间一致性快照（如备份）
- 批量数据导入和 ETL
- 存储成本敏感（高压缩比）

**InnoDB 更适合**：
- 读多写少的传统 OLTP
- 需要复杂事务和外键约束
- 随机读取密集的场景
- 存储空间相对充裕

## 8. 性能优化策略

### 8.1 快照延迟获取

READ COMMITTED 隔离级别下使用 `SetSnapshotOnNextOperation()`，避免不必要的快照创建：

```cpp
if (!m_is_delayed_snapshot) {
  m_rocksdb_tx->SetSnapshotOnNextOperation(m_notifier);
  m_is_delayed_snapshot = true;
}
```

### 8.2 事务对象重用

MyRocks 通过 `m_rocksdb_reuse_tx` 重用事务对象，避免频繁的对象创建销毁：

```cpp
void release_tx(void) {
  assert(m_rocksdb_reuse_tx == nullptr);
  m_rocksdb_reuse_tx = m_rocksdb_tx;  // 保存以供重用
  m_rocksdb_tx = nullptr;
}
```

### 8.3 批量操作优化

对于复制线程等无冲突场景，使用 WriteBatch 模式避免事务锁开销：

```cpp
class Rdb_writebatch_impl : public Rdb_transaction {
  rocksdb::WriteBatchWithIndex *m_batch;
  // 跳过事务 API，直接批量写入
  rocksdb::Status s = rdb->Write(write_opts, optimize, m_batch->GetWriteBatch());
};
```

## 9. 总结

MyRocks 的 MVCC 实现**摒弃了传统的 Undo Log 回滚段机制**，转而采用**基于全局序列号和 LSM-Tree 天然多版本存储**的创新设计，通过以下关键技术实现高效的并发控制：

### 9.1 核心技术特点

1. **序列号版本控制**: 
   - 每个写操作分配全局递增的 SequenceNumber (56-bit)
   - InternalKey 包含 `user_key + sequence + type` 实现版本标识
   - 避免了回滚段的复杂管理和竞争问题

2. **LSM-Tree 多版本存储**: 
   - 同一键的多个版本自然并存于不同层级
   - 新版本不覆盖旧版本，而是追加写入
   - 支持高效的范围查询和批量操作

3. **快照隔离机制**: 
   - 基于 RocksDB Snapshot 的 O(1) 快照创建
   - ReadCallback 通过序列号比较实现可见性判断
   - 无需维护复杂的活跃事务列表

4. **WriteBatchWithIndex**: 
   - 解决事务内读写一致性问题
   - 支持 read-your-own-writes 语义
   - 高效的索引结构加速事务内查询

5. **异步垃圾回收**: 
   - Compaction 过程自动清理无快照引用的旧版本
   - 避免了专门的 Purge 线程开销
   - 与正常的 LSM-Tree 压缩过程集成

### 9.2 适用场景优势

**MyRocks 的序列号 MVCC 特别适合**：
- **写密集型工作负载**: LSM-Tree 的顺序写特性
- **长时间一致性读**: 基于序列号的快照成本低
- **批量数据处理**: 无锁读取支持高并发
- **存储成本敏感**: 高压缩比补偿多版本开销
- **时序数据场景**: 天然支持按时间版本查询

### 9.3 与传统 MVCC 的根本区别

MyRocks 的创新在于**将版本信息编码到键本身**，使得多版本控制从"事后重构"转变为"事前存储"，这种设计哲学的转变带来了：

- **读取性能**: 直接访问目标版本，无需回溯重构
- **并发扩展**: 无锁读取，线性扩展能力
- **存储效率**: 利用 LSM-Tree 的压缩能力
- **实现简洁**: 避免了复杂的回滚段管理

这使得 MyRocks 在现代云原生、高并发的数据库应用场景中具有独特的优势，特别是在需要处理大量写入和长时间一致性读取的场景下。

## 10. MySQL行数据在LSM中的存储格式深度解析 ⭐

### 10.1 数据存储整体架构

MyRocks将MySQL的行数据映射到RocksDB的Key-Value对中，通过精巧的编码方案实现高效的存储和查询。

```mermaid
graph TB
    subgraph "**MySQL表结构**"
        TABLE["**users表**<br/>id INT PRIMARY KEY<br/>name VARCHAR(100)<br/>age INT<br/>email VARCHAR(200)<br/>INDEX idx_name(name)"]
    end
    
    subgraph "**主键索引存储 (Primary Key)**"
        PK_KEY["**RocksDB Key**<br/>index_number(4B) + packed_pk"]
        PK_VALUE["**RocksDB Value**<br/>non-pk columns + unpack_info"]
        
        PK_EXAMPLE["**示例**<br/>Key: [idx#256][id=1001]<br/>Value: [name='Alice'][age=25][email='alice@example.com']"]
    end
    
    subgraph "**二级索引存储 (Secondary Index)**"
        SK_KEY["**RocksDB Key**<br/>index_number(4B) + packed_sk + packed_pk"]
        SK_VALUE["**RocksDB Value**<br/>unpack_info (覆盖索引时)"]
        
        SK_EXAMPLE["**示例**<br/>Key: [idx#257][name='Alice'][id=1001]<br/>Value: [unpack_info] 或 空"]
    end
    
    subgraph "**RocksDB Internal Key**"
        INTERNAL["**Internal Key格式**<br/>user_key + seq(56bit) + type(8bit)"]
        LSM_STORE["**LSM-Tree存储**<br/>Level 0: MemTable<br/>Level 1-N: SST Files"]
    end
    
    TABLE --> PK_KEY
    TABLE --> SK_KEY
    PK_KEY --> PK_VALUE
    SK_KEY --> SK_VALUE
    
    PK_KEY --> PK_EXAMPLE
    SK_KEY --> SK_EXAMPLE
    
    PK_EXAMPLE --> INTERNAL
    SK_EXAMPLE --> INTERNAL
    INTERNAL --> LSM_STORE
    
    style TABLE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style PK_KEY fill:#fff3e0,stroke:#333,stroke-width:2px
    style SK_KEY fill:#f3e5f5,stroke:#333,stroke-width:2px
    style INTERNAL fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 10.2 主键索引存储格式详解

#### 10.2.1 Primary Key存储结构

**源码位置**: `storage/rocksdb/rdb_datadic.h:238-243`

```mermaid
graph LR
    subgraph "**Primary Key = User Key**"
        INDEX_NUM["**Index Number**<br/>4 bytes<br/>索引ID"]
        PK_COL1["**PK Column 1**<br/>变长<br/>mem-comparable"]
        PK_COL2["**PK Column 2**<br/>变长<br/>mem-comparable"]
        PK_COLN["**...**"]
    end
    
    subgraph "**Primary Value = Stored Record**"
        TTL_FIELD["**TTL (可选)**<br/>8 bytes<br/>过期时间"]
        NULL_BITMAP["**NULL Bitmap**<br/>变长<br/>NULL标记"]
        UNPACK_INFO["**Unpack Info**<br/>变长<br/>解码信息"]
        NON_PK1["**Non-PK Column 1**<br/>变长<br/>实际数据"]
        NON_PK2["**Non-PK Column 2**<br/>变长<br/>实际数据"]
        NON_PKN["**...**"]
        CHECKSUM["**Checksum (可选)**<br/>固定<br/>校验和"]
    end
    
    INDEX_NUM --> PK_COL1
    PK_COL1 --> PK_COL2
    PK_COL2 --> PK_COLN
    
    TTL_FIELD --> NULL_BITMAP
    NULL_BITMAP --> UNPACK_INFO
    UNPACK_INFO --> NON_PK1
    NON_PK1 --> NON_PK2
    NON_PK2 --> NON_PKN
    NON_PKN --> CHECKSUM
    
    style INDEX_NUM fill:#ffebee,stroke:#333,stroke-width:2px
    style TTL_FIELD fill:#e8f5e8,stroke:#333,stroke-width:2px
    style NULL_BITMAP fill:#fff3e0,stroke:#333,stroke-width:2px
```

**格式说明**:

**Key部分**:
```cpp
// storage/rocksdb/rdb_datadic.cc:1356-1357
rdb_netbuf_store_index(tuple, get_index_number());  // 存储4字节索引号
tuple += INDEX_NUMBER_SIZE;                          // INDEX_NUMBER_SIZE = 4
// 然后是mem-comparable格式的主键列数据
```

**Value部分**:
```cpp
// storage/rocksdb/rdb_converter.cc:902-983
// 1. TTL字段(如果表支持TTL)
uint64 ts = static_cast<uint64>(std::time(nullptr));
rdb_netbuf_store_uint64(reinterpret_cast<uchar *>(data), ts);

// 2. NULL bitmap
m_storage_record.fill(m_null_bytes_length_in_record, 0);

// 3. Unpack Info (如果需要)
if (m_maybe_unpack_info) {
  m_storage_record.append(reinterpret_cast<char *>(pk_unpack_info->ptr()),
                          pk_unpack_info->get_current_pos());
}

// 4. 非主键列数据
for (uint i = 0; i < m_table->s->fields; i++) {
  // 跳过主键列(已在Key中)
  if (encoder.m_storage_type != Rdb_field_encoder::STORE_ALL) continue;
  
  // 存储实际列数据
  m_storage_record.append(...);
}
```

#### 10.2.2 实际存储示例

**示例表结构**:
```sql
CREATE TABLE users (
  id INT PRIMARY KEY,
  name VARCHAR(100),
  age INT,
  email VARCHAR(200)
) ENGINE=ROCKSDB;

INSERT INTO users VALUES (1001, 'Alice', 25, 'alice@example.com');
```

**在LSM中的存储**:

```mermaid
graph TB
    subgraph "**RocksDB Key-Value存储**"
        MEMCMP["**Key (mem-comparable格式)**<br/>[index#256][0x000003E9]<br/>index#256 = users主键索引<br/>0x000003E9 = 1001 (大端序)"]
        
        STORED_REC["**Value (StoredRecord格式)**<br/>[NULL bitmap: 0x00]<br/>[name length: 5]['Alice']<br/>[age: 0x00000019]<br/>[email length: 17]['alice@example.com']"]
    end
    
    subgraph "**RocksDB Internal Key (实际存储)**"
        INTERNAL_KEY["**Internal Key**<br/>user_key: [index#256][0x000003E9]<br/>sequence: 12345<br/>type: kTypeValue (0x01)"]
        
        BINARY_FORMAT["**二进制格式**<br/>[index#256][0x000003E9][seq=12345<<8 | 0x01]<br/>完整Key: 16 bytes<br/>Value: ~30 bytes (变长)"]
    end
    
    MEMCMP --> INTERNAL_KEY
    STORED_REC --> INTERNAL_KEY
    INTERNAL_KEY --> BINARY_FORMAT
    
    style MEMCMP fill:#e3f2fd,stroke:#333,stroke-width:2px
    style STORED_REC fill:#fff3e0,stroke:#333,stroke-width:2px
    style INTERNAL_KEY fill:#f3e5f5,stroke:#333,stroke-width:2px
```

**二进制数据布局**:

| 偏移 | 长度 | 字段 | 值 | 说明 |
|-----|-----|------|-----|------|
| **Key部分** |||||
| 0 | 4 | index_number | `0x00000100` | 索引ID=256 (大端序) |
| 4 | 4 | id (INT) | `0x000003E9` | id=1001 (mem-comparable) |
| **Internal Key元数据** |||||
| 8 | 8 | seq + type | `0x0000003039000001` | seq=12345, type=0x01 |
| **Value部分** |||||
| 0 | 1 | NULL bitmap | `0x00` | 无NULL列 |
| 1 | 1 | name_length | `0x05` | VARCHAR长度前缀 |
| 2 | 5 | name | `Alice` | 实际字符串 |
| 7 | 4 | age | `0x00000019` | age=25 |
| 11 | 1 | email_length | `0x11` | 长度=17 |
| 12 | 17 | email | `alice@example.com` | 实际字符串 |

### 10.3 二级索引存储格式详解

#### 10.3.1 Secondary Key存储结构

**源码位置**: `storage/rocksdb/rdb_datadic.h:246-250`

```mermaid
graph LR
    subgraph "**Secondary Key = User Key**"
        SK_INDEX["**Index Number**<br/>4 bytes<br/>二级索引ID"]
        SK_COL1["**SK Column 1**<br/>变长<br/>索引列1"]
        SK_COL2["**SK Column 2**<br/>变长<br/>索引列2"]
        SK_DOTS["**...**"]
        PK_REF["**PK Reference**<br/>变长<br/>主键引用"]
    end
    
    subgraph "**Secondary Value**"
        UNPACK["**Unpack Info**<br/>变长<br/>覆盖索引数据"]
        EMPTY["**或 空字符串**<br/>非覆盖索引"]
    end
    
    SK_INDEX --> SK_COL1
    SK_COL1 --> SK_COL2
    SK_COL2 --> SK_DOTS
    SK_DOTS --> PK_REF
    
    SK_INDEX -.-> UNPACK
    SK_INDEX -.-> EMPTY
    
    style SK_INDEX fill:#ffebee,stroke:#333,stroke-width:2px
    style PK_REF fill:#e8f5e8,stroke:#333,stroke-width:2px
    style UNPACK fill:#fff3e0,stroke:#333,stroke-width:2px
```

**格式说明**:

**关键特点**:
1. **Key包含主键引用**: 确保唯一性，支持回表查询
2. **Value可选**: 覆盖索引时存储unpack_info，否则为空
3. **mem-comparable**: 所有列都转换为可直接memcmp比较的格式

#### 10.3.2 实际存储示例

**示例索引**:
```sql
CREATE INDEX idx_name ON users(name);
```

**在LSM中的存储**:

```mermaid
graph TB
    subgraph "**二级索引Key-Value**"
        SK_KEY["**Key**<br/>[index#257]['Alice'][id=1001]<br/>index#257 = idx_name索引<br/>name = 'Alice' (mem-comparable)<br/>id = 1001 (主键引用)"]
        
        SK_VALUE["**Value**<br/>空字符串<br/>(非覆盖索引，无需存储额外数据)"]
    end
    
    subgraph "**覆盖索引示例**"
        COV_IDX["**CREATE INDEX idx_cov ON users(name, age)**"]
        COV_KEY["**Key**<br/>[index#258]['Alice'][age=25][id=1001]"]
        COV_VALUE["**Value**<br/>[unpack_info]<br/>用于还原name和age的原始格式"]
    end
    
    SK_KEY --> SK_VALUE
    COV_IDX --> COV_KEY
    COV_KEY --> COV_VALUE
    
    style SK_KEY fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SK_VALUE fill:#ffebee,stroke:#333,stroke-width:2px
    style COV_KEY fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 10.4 mem-comparable编码格式

**核心原理**: 将任意类型的数据转换为可以直接用memcmp()比较的字节序列，保持原始数据的排序顺序。

```mermaid
graph TB
    subgraph "**INT类型编码**"
        INT_ORIG["**原始值**<br/>-100, 0, 100, 1000"]
        INT_ENC["**编码后**<br/>0x7FFFFF9C<br/>0x80000000<br/>0x80000064<br/>0x800003E8"]
        INT_RULE["**规则**<br/>• 大端序存储<br/>• 有符号数 XOR 0x80000000<br/>• 保证字典序 = 数值序"]
    end
    
    subgraph "**VARCHAR类型编码**"
        VAR_ORIG["**原始值**<br/>'Alice', 'Bob'"]
        VAR_ENC["**编码后**<br/>'Alice\0'<br/>'Bob\0'"]
        VAR_RULE["**规则**<br/>• UTF-8字节序<br/>• 尾部补0终止<br/>• 字典序即排序序"]
    end
    
    subgraph "**FLOAT/DOUBLE编码**"
        FLOAT_ORIG["**原始值**<br/>-1.5, 0.0, 2.5"]
        FLOAT_ENC["**编码后**<br/>特殊浮点编码"]
        FLOAT_RULE["**规则**<br/>• 符号位翻转<br/>• 负数全部位翻转<br/>• 保证字典序 = 数值序"]
    end
    
    INT_ORIG --> INT_ENC
    INT_ENC --> INT_RULE
    VAR_ORIG --> VAR_ENC
    VAR_ENC --> VAR_RULE
    FLOAT_ORIG --> FLOAT_ENC
    FLOAT_ENC --> FLOAT_RULE
    
    style INT_ENC fill:#e3f2fd,stroke:#333,stroke-width:2px
    style VAR_ENC fill:#fff3e0,stroke:#333,stroke-width:2px
    style FLOAT_ENC fill:#f3e5f5,stroke:#333,stroke-width:2px
```

**编码示例**:

| 原始值 | 类型 | mem-comparable编码 | 说明 |
|-------|------|-------------------|------|
| 0 | INT | `0x80000000` | 0 XOR 0x80000000 |
| 100 | INT | `0x80000064` | 100 XOR 0x80000000 |
| -100 | INT | `0x7FFFFF9C` | -100 XOR 0x80000000 |
| 'Alice' | VARCHAR | `0x416C696365` | UTF-8字节 + 终止符 |
| NULL | Any | `0x00` | NULL标记 |

### 10.5 查询数据抽取流程

#### 10.5.1 主键查询时序

```mermaid
sequenceDiagram
    participant App as **应用程序**
    participant Handler as **ha_rocksdb**
    participant KeyDef as **Rdb_key_def**
    participant RocksDB as **RocksDB**
    participant LSM as **LSM-Tree**

    Note over App,LSM: **主键查询: SELECT * FROM users WHERE id=1001**

    App->>Handler: index_read_map(id=1001)
    Note over App,Handler: **开始主键查询**

    Handler->>KeyDef: pack_record(id=1001)
    Note over Handler,KeyDef: **将主键打包为mem-comparable格式**
    
    KeyDef->>KeyDef: 构建Key
    Note over KeyDef: **Key = [index#256][0x000003E9]**
    KeyDef-->>Handler: packed_key
    
    Handler->>RocksDB: Get(packed_key, ReadOptions)
    Note over Handler,RocksDB: **使用当前快照进行查询**
    
    RocksDB->>LSM: 在LSM-Tree中查找
    Note over LSM: **1. 检查MemTable**<br/>**2. 检查Immutable MemTable**<br/>**3. 查找SST文件(Level 0-N)**
    
    LSM-->>RocksDB: Internal Key + Value
    Note over LSM,RocksDB: **找到匹配的Key**<br/>**seq <= snapshot_seq**
    
    RocksDB-->>Handler: rocksdb::Slice (Value)
    
    Handler->>Handler: convert_record_from_storage_format()
    Note over Handler: **解析StoredRecord**
    
    Handler->>KeyDef: decode(key, value, buf)
    Note over Handler,KeyDef: **将Value解码为MySQL记录格式**
    
    KeyDef->>KeyDef: 解析NULL bitmap
    KeyDef->>KeyDef: 解析unpack_info
    KeyDef->>KeyDef: 解码各列数据
    Note over KeyDef: **name='Alice'**<br/>**age=25**<br/>**email='alice@example.com'**
    
    KeyDef-->>Handler: 记录填充到table->record[0]
    Handler-->>App: 查询结果
```

#### 10.5.2 二级索引查询时序

```mermaid
sequenceDiagram
    participant App as **应用程序**
    participant Handler as **ha_rocksdb**
    participant KeyDef as **Rdb_key_def**
    participant RocksDB as **RocksDB**
    participant LSM as **LSM-Tree**

    Note over App,LSM: **二级索引查询: SELECT * FROM users WHERE name='Alice'**

    App->>Handler: index_read_map(name='Alice')
    Note over App,Handler: **使用idx_name索引查询**

    Handler->>KeyDef: pack_index_tuple(name='Alice')
    Note over Handler,KeyDef: **打包二级索引Key**
    KeyDef-->>Handler: SK Key = [index#257]['Alice'][0x00...]
    
    Handler->>RocksDB: Get(SK Key)
    RocksDB->>LSM: 查找二级索引
    LSM-->>RocksDB: SK Key + Empty Value
    RocksDB-->>Handler: 找到二级索引记录
    
    Handler->>Handler: 从SK Key提取主键
    Note over Handler: **extract PK from SK Key**<br/>**id = 1001**
    
    rect rgb(255, 243, 224)
        Note over Handler,LSM: **回表查询 (二次查找)**
        
        Handler->>KeyDef: pack_record(id=1001)
        KeyDef-->>Handler: PK Key = [index#256][0x000003E9]
        
        Handler->>RocksDB: Get(PK Key)
        RocksDB->>LSM: 查找主键索引
        LSM-->>RocksDB: PK Value (完整行数据)
        RocksDB-->>Handler: StoredRecord
        
        Handler->>Handler: convert_record_from_storage_format()
        Handler->>KeyDef: decode(key, value, buf)
        KeyDef-->>Handler: 完整记录
    end
    
    Handler-->>App: 查询结果 (所有列)
```

#### 10.5.3 覆盖索引查询优化

```mermaid
sequenceDiagram
    participant App as **应用程序**
    participant Handler as **ha_rocksdb**
    participant KeyDef as **Rdb_key_def**
    participant RocksDB as **RocksDB**
    participant LSM as **LSM-Tree**

    Note over App,LSM: **覆盖索引查询: SELECT name, age FROM users WHERE name='Alice'**

    App->>Handler: index_read_map(name='Alice')
    Note over App,Handler: **使用idx_cov(name, age)索引**

    Handler->>KeyDef: pack_index_tuple(name='Alice')
    KeyDef-->>Handler: SK Key包含name和age
    
    Handler->>RocksDB: Get(SK Key)
    RocksDB->>LSM: 查找覆盖索引
    LSM-->>RocksDB: SK Key + unpack_info
    RocksDB-->>Handler: 找到索引记录
    
    rect rgb(232, 245, 232)
        Note over Handler: **覆盖索引优化：无需回表**
        
        Handler->>KeyDef: unpack_record(SK Key, unpack_info)
        Note over Handler,KeyDef: **从索引Key和unpack_info**<br/>**直接解码所需列**
        
        KeyDef->>KeyDef: 解析SK Key中的列
        Note over KeyDef: **name = 'Alice' (from Key)**<br/>**age = 25 (from Key)**
        
        KeyDef->>KeyDef: 使用unpack_info还原
        Note over KeyDef: **将mem-comparable格式**<br/>**还原为原始格式**
        
        KeyDef-->>Handler: 填充name和age列
    end
    
    Handler-->>App: 查询结果 (name, age)
    Note over App,Handler: **性能提升：避免一次LSM查找**
```

### 10.6 性能优化策略

#### 10.6.1 Key编码优化

```mermaid
graph LR
    subgraph "**优化策略**"
        PREFIX["**前缀压缩**<br/>• SST文件内Key前缀共享<br/>• 减少存储空间<br/>• 提升缓存效率"]
        
        BLOOM["**Bloom Filter**<br/>• 每个SST文件的Bloom Filter<br/>• 快速判断Key是否存在<br/>• 减少无效IO"]
        
        INDEX_BLOCK["**Index Block**<br/>• SST文件内部索引<br/>• 二分查找定位Data Block<br/>• O(log N)复杂度"]
    end
    
    subgraph "**存储优化**"
        COMPRESS["**Value压缩**<br/>• LZ4/Snappy/ZSTD<br/>• 减少磁盘占用<br/>• 降低IO带宽"]
        
        BLOCK_CACHE["**Block Cache**<br/>• 缓存热点Data Block<br/>• 减少磁盘读取<br/>• 配置rocksdb_block_cache_size"]
    end
    
    PREFIX --> COMPRESS
    BLOOM --> COMPRESS
    INDEX_BLOCK --> COMPRESS
    
    COMPRESS --> BLOCK_CACHE
    
    style PREFIX fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BLOOM fill:#fff3e0,stroke:#333,stroke-width:2px
    style COMPRESS fill:#f3e5f5,stroke:#333,stroke-width:2px
```

#### 10.6.2 查询性能对比

| 查询类型 | 索引 | LSM查找次数 | IO量 | 性能评级 |
|---------|-----|-----------|------|---------|
| **主键点查** | 主键索引 | 1次 | 1个Value | ⭐⭐⭐⭐⭐ 极快 |
| **二级索引点查** | 二级索引 | 2次 (索引+回表) | 1个Key + 1个Value | ⭐⭐⭐⭐ 快 |
| **覆盖索引查询** | 覆盖索引 | 1次 | 1个Key + 小Value | ⭐⭐⭐⭐⭐ 极快 |
| **范围扫描** | 任意索引 | N次 | N个KV对 | ⭐⭐⭐ 中等 |
| **全表扫描** | 主键索引 | 顺序读 | 全部Value | ⭐⭐ 慢 |

### 10.7 调试和监控

**查看存储格式的工具**:

```bash
# 1. 使用RocksDB sst_dump工具查看SST文件内容
./sst_dump --file=/path/to/sst/file --command=scan --output_hex

# 2. 使用MyRocks工具查看索引统计
mysql> SELECT * FROM information_schema.ROCKSDB_INDEX_FILE_MAP;
mysql> SELECT * FROM information_schema.ROCKSDB_DDL;

# 3. 查看Key格式和编码
mysql> SET SESSION rocksdb_debug_ttl_rec_ts = 0;
mysql> EXPLAIN FORMAT=TREE SELECT * FROM users WHERE id=1001\G
```

**监控Key-Value存储效率**:

```sql
-- 查看表的存储统计
SELECT 
    TABLE_SCHEMA,
    TABLE_NAME,
    DATA_LENGTH / 1024 / 1024 AS data_mb,
    INDEX_LENGTH / 1024 / 1024 AS index_mb,
    (DATA_LENGTH + INDEX_LENGTH) / TABLE_ROWS AS bytes_per_row
FROM information_schema.TABLES 
WHERE ENGINE = 'ROCKSDB'
ORDER BY (DATA_LENGTH + INDEX_LENGTH) DESC;

-- 查看索引效率
SELECT 
    INDEX_NUMBER,
    INDEX_NAME,
    KV_FORMAT_VERSION,
    KEY_COLS,
    INDEX_FLAGS
FROM information_schema.ROCKSDB_DDL
WHERE TABLE_SCHEMA = 'your_database';
```

### 10.8 核心要点总结

**存储格式设计哲学**:

1. **Key包含排序信息**: mem-comparable格式确保字典序 = 排序序
2. **Value存储实际数据**: 主键索引存完整行，二级索引可选
3. **二级索引包含主键**: 天然支持回表，无需额外映射
4. **覆盖索引优化**: 通过unpack_info避免回表查询

**性能权衡**:

- **写入性能**: LSM顺序写 > B+树随机写
- **点查性能**: 需要多次LSM查找（但有Cache优化）
- **范围查询**: LSM层级导致多次归并，但顺序读友好
- **空间放大**: 多版本存储 + Compaction开销

**最佳实践**:

1. **合理设计主键**: 紧凑的主键减少存储开销
2. **使用覆盖索引**: 避免回表提升查询性能
3. **避免过长的Key**: Key长度影响memcmp性能和存储空间
4. **监控Compaction**: 确保垃圾回收及时，控制空间放大

这种精巧的Key-Value映射设计，使得MyRocks能够在LSM-Tree架构上高效实现MySQL的关系型数据模型。

## 11. 参考源码文件

- `storage/rocksdb/ha_rocksdb.cc` - MyRocks 存储引擎主实现
- `storage/rocksdb/rdb_converter.cc` - 记录格式转换实现
- `storage/rocksdb/rdb_datadic.cc` - Key编码和索引定义
- `storage/rocksdb/rdb_datadic.h` - 数据字典和Key格式定义
- `storage/rocksdb/rocksdb/utilities/write_batch_with_index/` - WriteBatchWithIndex 实现
- `storage/rocksdb/rocksdb/utilities/transactions/` - 事务实现
- `storage/rocksdb/rocksdb/db/dbformat.h` - RocksDB Internal Key格式
- `storage/rocksdb/rocksdb/examples/transaction_example.cc` - 事务示例
- `storage/rocksdb/rocksdb/examples/optimistic_transaction_example.cc` - 乐观事务示例
