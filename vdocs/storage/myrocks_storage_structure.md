# MyRocks (RocksDB) 物理存储与内存结构深度解析

## 概述

本文档详细解析MySQL MyRocks存储引擎的物理存储结构和内存组织。MyRocks基于Facebook的**RocksDB**（LSM-Tree架构），提供高效的写入性能和压缩比。从**字节级别**深入分析文件格式、内存结构、以及读写流程。

**基于源码**: `storage/rocksdb/`

---

## 第一部分：MyRocks 文件组织架构

### 1.1 整体文件结构

```mermaid
graph TB
    subgraph "<b>MyRocks数据目录结构</b>"
        ROCKSDB_DIR["<b>.rocksdb目录</b><br/>主数据目录"]
        
        subgraph "<b>WAL文件</b>"
            WAL_FILES["<b>*.log文件</b><br/>Write-Ahead Log<br/>32KB block size"]
        end
        
        subgraph "<b>SST文件（分层）</b>"
            L0_SST["<b>Level 0 SST</b><br/>*.sst文件<br/>Memtable直接刷盘<br/>可能有重叠"]
            L1_SST["<b>Level 1 SST</b><br/>合并后的文件<br/>无重叠"]
            L6_SST["<b>Level 6 SST</b><br/>最底层<br/>数据最多"]
        end
        
        subgraph "<b>元数据文件</b>"
            MANIFEST["<b>MANIFEST文件</b><br/>版本信息和文件元数据"]
            CURRENT["<b>CURRENT文件</b><br/>指向当前MANIFEST"]
            OPTIONS["<b>OPTIONS文件</b><br/>配置参数"]
        end
    end
    
    ROCKSDB_DIR --> WAL_FILES
    ROCKSDB_DIR --> L0_SST
    L0_SST --> L1_SST
    L1_SST --> L6_SST
    ROCKSDB_DIR --> MANIFEST
    MANIFEST --> CURRENT
    MANIFEST --> OPTIONS
    
    style WAL_FILES fill:#e3f2fd,stroke:#333,stroke-width:2px
    style L0_SST fill:#fff3e0,stroke:#333,stroke-width:2px
    style L6_SST fill:#e8f5e8,stroke:#333,stroke-width:2px
    style MANIFEST fill:#f3e5f5,stroke:#333,stroke-width:2px
```

---

## 第二部分：WAL (Write-Ahead Log) 详细结构

### 2.1 WAL文件物理格式

**源码位置**: `storage/rocksdb/rocksdb/db/log_format.h:45-52`

```mermaid
graph LR
    subgraph "<b>WAL文件结构（32KB Block）</b>"
        BLOCK1["<b>Block 0</b><br/>32768字节"]
        BLOCK2["<b>Block 1</b><br/>32768字节"]
        BLOCKN["<b>Block N</b><br/>32768字节"]
    end
    
    subgraph "<b>单个Block内部结构</b>"
        REC1["<b>Record 1</b><br/>Header + Data"]
        REC2["<b>Record 2</b><br/>Header + Data"]
        PADDING["<b>Padding</b><br/>0x00填充"]
    end
    
    BLOCK1 --> BLOCK2
    BLOCK2 --> BLOCKN
    
    BLOCK1 --> REC1
    REC1 --> REC2
    REC2 --> PADDING
    
    style BLOCK1 fill:#e3f2fd,stroke:#333,stroke-width:2px
    style REC1 fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 2.2 WAL Record 字节级结构

**源码位置**: `storage/rocksdb/rocksdb/db/log_writer.h:50-72`

```mermaid
graph LR
    subgraph "<b>Legacy WAL Record（7字节Header）</b>"
        CRC["<b>0-3</b><br/>CRC32<br/>校验和<br/>4字节"]
        SIZE["<b>4-5</b><br/>Length<br/>数据长度<br/>2字节"]
        TYPE["<b>6</b><br/>Type<br/>记录类型<br/>1字节"]
        PAYLOAD["<b>7+</b><br/>Payload<br/>实际数据"]
    end
    
    subgraph "<b>Recyclable WAL Record（11字节Header）</b>"
        CRC_R["<b>0-3</b><br/>CRC32<br/>4字节"]
        SIZE_R["<b>4-5</b><br/>Length<br/>2字节"]
        TYPE_R["<b>6</b><br/>Type<br/>1字节"]
        LOG_NUM["<b>7-10</b><br/>Log Number<br/>日志编号<br/>4字节"]
        PAYLOAD_R["<b>11+</b><br/>Payload"]
    end
    
    CRC --> SIZE
    SIZE --> TYPE
    TYPE --> PAYLOAD
    
    CRC_R --> SIZE_R
    SIZE_R --> TYPE_R
    TYPE_R --> LOG_NUM
    LOG_NUM --> PAYLOAD_R
    
    style CRC fill:#e3f2fd,stroke:#333,stroke-width:2px
    style TYPE fill:#fff3e0,stroke:#333,stroke-width:2px
    style LOG_NUM fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**WAL Record字节级映射表**:

| 偏移量 | 长度 | 字段名 | 数据类型 | 说明 |
|-------|-----|--------|---------|------|
| **0** | 4 | `CRC` | uint32 LE | CRC32校验和（type + payload） |
| **4** | 2 | `Length` | uint16 LE | Payload长度 |
| **6** | 1 | `Type` | uint8 | 记录类型（见下表） |
| **7** | 4 | `Log Number` | uint32 LE | **仅Recyclable格式**，日志文件编号 |
| **7/11** | N | `Payload` | bytes | 实际的WriteBatch数据 |

**WAL Record类型**:

| 类型值 | 常量名 | 说明 |
|-------|-------|------|
| **0** | `kZeroType` | 预分配文件的填充 |
| **1** | `kFullType` | 完整记录，单个block内 |
| **2** | `kFirstType` | 分片记录的第一部分 |
| **3** | `kMiddleType` | 分片记录的中间部分 |
| **4** | `kLastType` | 分片记录的最后部分 |
| **5-8** | `kRecyclable*` | 可回收格式（含Log Number） |

---

## 第三部分：SST文件详细结构

### 3.1 SST文件整体布局

```mermaid
graph TB
    subgraph "<b>SST文件完整结构</b>"
        subgraph "<b>Data Blocks</b>"
            DATA_BLOCK1["<b>Data Block 1</b><br/>KV pairs<br/>默认4KB"]
            DATA_BLOCK2["<b>Data Block 2</b><br/>KV pairs"]
            DATA_BLOCKN["<b>Data Block N</b><br/>KV pairs"]
        end
        
        COMPRESSION["<b>Compression Dict Block</b><br/>压缩字典（可选）"]
        
        META_INDEX["<b>MetaIndex Block</b><br/>指向Filter Block等"]
        
        FILTER["<b>Filter Block</b><br/>Bloom Filter<br/>加速查找"]
        
        INDEX["<b>Index Block</b><br/>Data Block索引<br/>Key -> Block Handle"]
        
        PROPERTIES["<b>Properties Block</b><br/>表属性和统计"]
        
        subgraph "<b>Footer（固定大小）</b>"
            FOOTER_MI["<b>MetaIndex Handle</b><br/>10字节"]
            FOOTER_IDX["<b>Index Handle</b><br/>10字节"]
            FOOTER_PADDING["<b>Padding</b><br/>填充"]
            FOOTER_MAGIC["<b>Magic Number</b><br/>8字节<br/>文件类型标识"]
        end
    end
    
    DATA_BLOCK1 --> DATA_BLOCK2
    DATA_BLOCK2 --> DATA_BLOCKN
    DATA_BLOCKN --> COMPRESSION
    COMPRESSION --> META_INDEX
    META_INDEX --> FILTER
    FILTER --> INDEX
    INDEX --> PROPERTIES
    PROPERTIES --> FOOTER_MI
    FOOTER_MI --> FOOTER_IDX
    FOOTER_IDX --> FOOTER_PADDING
    FOOTER_PADDING --> FOOTER_MAGIC
    
    style DATA_BLOCK1 fill:#e3f2fd,stroke:#333,stroke-width:2px
    style INDEX fill:#fff3e0,stroke:#333,stroke-width:2px
    style FOOTER_MAGIC fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 3.2 SST Footer 字节级结构

**源码位置**: `storage/rocksdb/rocksdb/table/format.h:132-180`

```mermaid
graph LR
    subgraph "<b>Footer结构（固定尾部）</b>"
        MI_HANDLE["<b>MetaIndex Handle</b><br/>Block Handle<br/>10字节<br/>offset(varint) + size(varint)"]
        
        IDX_HANDLE["<b>Index Handle</b><br/>Block Handle<br/>10字节"]
        
        PADDING_F["<b>Padding</b><br/>变长<br/>对齐到固定大小"]
        
        MAGIC["<b>Table Magic Number</b><br/>8字节<br/>标识文件格式"]
    end
    
    MI_HANDLE --> IDX_HANDLE
    IDX_HANDLE --> PADDING_F
    PADDING_F --> MAGIC
    
    style MI_HANDLE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style MAGIC fill:#fff3e0,stroke:#333,stroke-width:2px
```

**Footer字段说明**:

| 字段 | 长度 | 说明 |
|-----|-----|------|
| `MetaIndex Block Handle` | ~10B | 指向MetaIndex Block的位置和大小 |
| `Index Block Handle` | ~10B | 指向Index Block的位置和大小 |
| Padding | 变长 | 填充至固定Footer大小 |
| `Magic Number` | 8B | 标识SST格式（Block-Based: 0x88e241b785f4cff7） |

**BlockHandle格式** (varint编码):

- **Offset**: Block在文件中的偏移量（varint64）
- **Size**: Block的大小（varint64）

### 3.3 Data Block 内部结构

```mermaid
graph TB
    subgraph "<b>Data Block结构</b>"
        subgraph "<b>Block Body</b>"
            KV1["<b>Entry 1</b><br/>shared_key_len<br/>unshared_key_len<br/>value_len<br/>key_delta<br/>value"]
            KV2["<b>Entry 2</b><br/>前缀压缩"]
            KVN["<b>Entry N</b>"]
        end
        
        RESTART_ARRAY["<b>Restart Array</b><br/>每16个entry一个restart point<br/>uint32数组"]
        
        RESTART_NUM["<b>Restart Array Size</b><br/>4字节<br/>restart数量"]
        
        BLOCK_TRAILER["<b>Block Trailer</b><br/>5字节<br/>Compression Type (1B)<br/>+ CRC32 (4B)"]
    end
    
    KV1 --> KV2
    KV2 --> KVN
    KVN --> RESTART_ARRAY
    RESTART_ARRAY --> RESTART_NUM
    RESTART_NUM --> BLOCK_TRAILER
    
    style KV1 fill:#e3f2fd,stroke:#333,stroke-width:2px
    style RESTART_ARRAY fill:#fff3e0,stroke:#333,stroke-width:2px
    style BLOCK_TRAILER fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**Entry格式** (前缀压缩):

| 字段 | 编码 | 说明 |
|-----|-----|------|
| `shared_key_length` | varint32 | 与前一个key共享的前缀长度 |
| `unshared_key_length` | varint32 | 当前key的非共享部分长度 |
| `value_length` | varint32 | Value的长度 |
| `key_delta` | bytes | Key的非共享部分 |
| `value` | bytes | 实际的value数据 |

**示例**:

```text
Entry 1: key="apple", value="red"
  shared=0, unshared=5, value_len=3
  key_delta="apple", value="red"

Entry 2: key="application", value="software"
  shared=3, unshared=8, value_len=8
  key_delta="lication", value="software"
```

---

## 第四部分：MyRocks 内存结构

### 4.1 内存组件架构

```mermaid
graph TB
    subgraph "<b>MyRocks内存架构</b>"
        subgraph "<b>写入路径</b>"
            WRITE_BUFFER["<b>Write Buffer Manager</b><br/>控制总内存使用<br/>rocksdb_db_write_buffer_size"]
            
            MEMTABLE_ACTIVE["<b>Active MemTable</b><br/>当前写入的内存表<br/>SkipList结构"]
            
            MEMTABLE_IMMUTABLE["<b>Immutable MemTable</b><br/>只读memtable链表<br/>等待刷盘"]
        end
        
        subgraph "<b>读取路径</b>"
            BLOCK_CACHE["<b>Block Cache</b><br/>rocksdb_block_cache_size<br/>默认512MB<br/>LRU缓存"]
            
            TABLE_CACHE["<b>Table Cache</b><br/>缓存打开的SST文件<br/>File Descriptor + Index"]
            
            ROW_CACHE["<b>Row Cache</b><br/>行级缓存（可选）<br/>rocksdb_row_cache_size"]
        end
        
        subgraph "<b>压缩相关</b>"
            COMPRESSION_BUFFER["<b>Compression Buffer</b><br/>Zlib/LZ4/Snappy等"]
        end
    end
    
    WRITE_BUFFER --> MEMTABLE_ACTIVE
    MEMTABLE_ACTIVE --> MEMTABLE_IMMUTABLE
    
    MEMTABLE_IMMUTABLE -.刷盘.-> BLOCK_CACHE
    
    BLOCK_CACHE --> TABLE_CACHE
    TABLE_CACHE --> ROW_CACHE
    
    COMPRESSION_BUFFER --> MEMTABLE_IMMUTABLE
    
    style MEMTABLE_ACTIVE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BLOCK_CACHE fill:#fff3e0,stroke:#333,stroke-width:2px
    style ROW_CACHE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 4.2 MemTable 结构详解

```mermaid
graph TB
    subgraph "<b>MemTable内存结构</b>"
        subgraph "<b>MemTable对象</b>"
            MEM_REF["<b>引用计数</b><br/>ref_count"]
            MEM_SIZE["<b>内存使用</b><br/>approximate_memory_usage"]
            MEM_SKIPLIST["<b>Skip List</b><br/>核心数据结构"]
            MEM_CF["<b>Column Family</b><br/>所属列族"]
        end
        
        subgraph "<b>Skip List结构</b>"
            SKIPLIST_HEAD["<b>Head Node</b><br/>层数由概率决定"]
            SKIPLIST_NODE1["<b>Node 1</b><br/>InternalKey + Value<br/>多层指针"]
            SKIPLIST_NODE2["<b>Node 2</b>"]
            SKIPLIST_TAIL["<b>Tail</b>"]
        end
        
        subgraph "<b>InternalKey格式</b>"
            USER_KEY["<b>User Key</b><br/>用户指定的key"]
            SEQUENCE["<b>Sequence Number</b><br/>7字节<br/>全局递增"]
            TYPE["<b>Type</b><br/>1字节<br/>kTypeValue/kTypeDeletion等"]
        end
    end
    
    MEM_REF --> MEM_SIZE
    MEM_SIZE --> MEM_SKIPLIST
    MEM_SKIPLIST --> MEM_CF
    
    MEM_SKIPLIST --> SKIPLIST_HEAD
    SKIPLIST_HEAD --> SKIPLIST_NODE1
    SKIPLIST_NODE1 --> SKIPLIST_NODE2
    SKIPLIST_NODE2 --> SKIPLIST_TAIL
    
    SKIPLIST_NODE1 --> USER_KEY
    USER_KEY --> SEQUENCE
    SEQUENCE --> TYPE
    
    style MEM_SKIPLIST fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SKIPLIST_NODE1 fill:#fff3e0,stroke:#333,stroke-width:2px
    style SEQUENCE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**InternalKey编码**:

```text
InternalKey = UserKey + SequenceNumber(7B) + ValueType(1B)
Total = UserKey.size() + 8 bytes
```

**ValueType枚举** (`storage/rocksdb/rocksdb/db/dbformat.h:39-74`):

| 类型值 | 常量名 | 说明 |
|-------|-------|------|
| **0x1** | `kTypeValue` | 普通value |
| **0x0** | `kTypeDeletion` | 删除标记 |
| **0x2** | `kTypeMerge` | Merge操作 |
| **0x7** | `kTypeSingleDeletion` | 单点删除 |
| **0xF** | `kTypeRangeDeletion` | 范围删除 |

### 4.3 Block Cache 详细结构

```mermaid
graph LR
    subgraph "<b>Block Cache（LRU Cache）</b>"
        subgraph "<b>Shard 0</b>"
            SHARD0_HANDLE["<b>Cache Handle</b><br/>key: Cache Key<br/>value: Block*"]
            SHARD0_LRU["<b>LRU List</b><br/>最近最少使用"]
        end
        
        subgraph "<b>Shard 1</b>"
            SHARD1_HANDLE["<b>Cache Handle</b>"]
            SHARD1_LRU["<b>LRU List</b>"]
        end
        
        subgraph "<b>Shard N</b>"
            SHARDN_HANDLE["<b>Cache Handle</b>"]
            SHARDN_LRU["<b>LRU List</b>"]
        end
    end
    
    subgraph "<b>Cache Key格式</b>"
        FILE_NUM["<b>File Number</b><br/>SST文件编号"]
        OFFSET["<b>Block Offset</b><br/>Block在文件中的偏移"]
    end
    
    subgraph "<b>Cached Block类型</b>"
        DATA_CACHED["<b>Data Block</b><br/>KV数据"]
        INDEX_CACHED["<b>Index Block</b><br/>索引数据"]
        FILTER_CACHED["<b>Filter Block</b><br/>Bloom Filter"]
    end
    
    SHARD0_HANDLE --> SHARD0_LRU
    SHARD1_HANDLE --> SHARD1_LRU
    SHARDN_HANDLE --> SHARDN_LRU
    
    SHARD0_HANDLE --> FILE_NUM
    FILE_NUM --> OFFSET
    
    SHARD0_LRU --> DATA_CACHED
    DATA_CACHED --> INDEX_CACHED
    INDEX_CACHED --> FILTER_CACHED
    
    style SHARD0_HANDLE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SHARD0_LRU fill:#fff3e0,stroke:#333,stroke-width:2px
    style DATA_CACHED fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**Block Cache配置参数**:

| 参数 | 默认值 | 说明 |
|-----|-------|------|
| `rocksdb_block_cache_size` | 512MB | Block Cache总大小 |
| `rocksdb_cache_index_and_filter_blocks` | ON | 是否缓存Index和Filter |
| `rocksdb_pin_l0_filter_and_index_blocks_in_cache` | ON | L0层的Index/Filter常驻 |
| `rocksdb_cache_high_pri_pool_ratio` | 0.5 | 高优先级池比例 |

---

## 第五部分：MyRocks 读写流程

### 5.1 写入流程

```mermaid
sequenceDiagram
    participant APP as **应用**
    participant MYSQL as **MySQL Server**
    participant MYROCKS as **MyRocks Handler**
    participant WAL as **WAL**
    participant MEMTABLE as **MemTable**
    participant FLUSH as **Flush线程**
    participant SST as **SST文件**

    Note over APP,SST: **写入流程**

    APP->>MYSQL: INSERT/UPDATE
    MYSQL->>MYROCKS: ha_rocksdb::write_row()
    
    MYROCKS->>MYROCKS: 构造InternalKey<br/>UserKey + SeqNum + Type
    
    MYROCKS->>WAL: 写入WAL（同步或异步）<br/>rocksdb_flush_log_at_trx_commit
    WAL->>WAL: 追加到当前Block<br/>CRC + Length + Type + Payload
    
    alt sync_wal=true
        WAL->>WAL: fsync()持久化
    end
    
    MYROCKS->>MEMTABLE: 写入Active MemTable<br/>SkipList.Insert()
    MEMTABLE->>MEMTABLE: 更新memory_usage
    
    alt MemTable已满
        MEMTABLE->>MEMTABLE: 转为Immutable MemTable
        MEMTABLE->>FLUSH: 触发后台刷盘
        
        Note over FLUSH,SST: **后台Flush流程**
        
        FLUSH->>MEMTABLE: 遍历Immutable MemTable
        FLUSH->>FLUSH: 构造SST文件<br/>Data Blocks + Index + Filter
        FLUSH->>SST: 写入Level 0
        FLUSH->>FLUSH: 删除对应WAL
        FLUSH->>MEMTABLE: 释放Immutable MemTable
    end
    
    MYROCKS-->>MYSQL: 返回成功
    MYSQL-->>APP: 返回成功
```

### 5.2 读取流程

```mermaid
sequenceDiagram
    participant APP as **应用**
    participant MYSQL as **MySQL Server**
    participant MYROCKS as **MyRocks Handler**
    participant MEMTABLE as **MemTable**
    participant BLOCK_CACHE as **Block Cache**
    participant SST as **SST文件**

    Note over APP,SST: **点查流程 (Get)**

    APP->>MYSQL: SELECT * WHERE pk=1
    MYSQL->>MYROCKS: ha_rocksdb::index_read()
    
    MYROCKS->>MYROCKS: 构造Lookup Key<br/>UserKey + MaxSeqNum
    
    Note over MYROCKS,MEMTABLE: **1. 查找MemTable**
    MYROCKS->>MEMTABLE: 查找Active MemTable
    
    alt 在MemTable中找到
        MEMTABLE-->>MYROCKS: 返回最新值
        MYROCKS-->>MYSQL: 返回结果
        MYSQL-->>APP: 返回行数据
    else 未找到
        MYROCKS->>MEMTABLE: 查找Immutable MemTables
        
        alt 在Immutable中找到
            MEMTABLE-->>MYROCKS: 返回值
        else 未找到
            Note over MYROCKS,SST: **2. 查找SST文件**
            
            loop Level 0 to Level 6
                MYROCKS->>BLOCK_CACHE: 查找Index Block
                
                alt Index在Cache中
                    BLOCK_CACHE-->>MYROCKS: 返回Index
                else Index不在Cache
                    MYROCKS->>SST: 读取Index Block
                    SST-->>MYROCKS: Index数据
                    MYROCKS->>BLOCK_CACHE: 缓存Index
                end
                
                MYROCKS->>MYROCKS: 二分查找Index<br/>定位Data Block
                
                MYROCKS->>BLOCK_CACHE: 查找Data Block
                
                alt Data在Cache中
                    BLOCK_CACHE-->>MYROCKS: 返回Block
                else Data不在Cache
                    MYROCKS->>SST: 读取Data Block
                    SST-->>MYROCKS: 解压缩Data
                    MYROCKS->>BLOCK_CACHE: 缓存Data Block
                end
                
                MYROCKS->>MYROCKS: Block内二分查找
                
                alt 找到key
                    MYROCKS-->>MYSQL: 返回值
                    break
                end
            end
        end
    end
```

### 5.3 Compaction 流程

```mermaid
graph TB
    subgraph "<b>Compaction流程</b>"
        L0_FILES["<b>Level 0</b><br/>4-8个SST文件<br/>可能有重叠"]
        
        TRIGGER["<b>触发Compaction</b><br/>• L0文件数>=4<br/>• Level大小超限<br/>• Manual Compaction"]
        
        PICK_FILES["<b>选择文件</b><br/>• L0: 选择重叠文件<br/>• L1+: 选择一个文件<br/>+ L(n+1)层重叠文件"]
        
        MERGE_SORT["<b>归并排序</b><br/>多路归并<br/>保留最新version"]
        
        subgraph "<b>写入新文件</b>"
            NEW_SST["<b>新SST文件</b><br/>写入L(n+1)层<br/>无重叠<br/>有序"]
        end
        
        DELETE_OLD["<b>删除旧文件</b><br/>更新MANIFEST<br/>原子替换"]
        
        STATS["<b>更新统计</b><br/>write amplification<br/>space amplification"]
    end
    
    L0_FILES --> TRIGGER
    TRIGGER --> PICK_FILES
    PICK_FILES --> MERGE_SORT
    MERGE_SORT --> NEW_SST
    NEW_SST --> DELETE_OLD
    DELETE_OLD --> STATS
    
    style TRIGGER fill:#e3f2fd,stroke:#333,stroke-width:2px
    style MERGE_SORT fill:#fff3e0,stroke:#333,stroke-width:2px
    style NEW_SST fill:#e8f5e8,stroke:#333,stroke-width:2px
```

---

## 第六部分：性能优化与监控

### 6.1 关键性能参数

**写入优化**:

```sql
-- MemTable和WAL设置
SET GLOBAL rocksdb_db_write_buffer_size = 4294967296;  -- 4GB
SET GLOBAL rocksdb_write_buffer_size = 134217728;      -- 128MB per CF
SET GLOBAL rocksdb_max_write_buffer_number = 4;        -- Immutable数量
SET GLOBAL rocksdb_flush_log_at_trx_commit = 0;        -- 性能优先

-- WAL设置
SET GLOBAL rocksdb_wal_size_limit_mb = 0;  -- 不限制WAL大小
SET GLOBAL rocksdb_wal_ttl_seconds = 0;    -- WAL不过期
```

**读取优化**:

```sql
-- Block Cache设置
SET GLOBAL rocksdb_block_cache_size = 2147483648;  -- 2GB
SET GLOBAL rocksdb_cache_index_and_filter_blocks = 1;
SET GLOBAL rocksdb_pin_l0_filter_and_index_blocks_in_cache = 1;

-- Row Cache（可选）
SET GLOBAL rocksdb_row_cache_size = 1073741824;  -- 1GB

-- Bloom Filter
SET GLOBAL rocksdb_bloom_bits_per_key = 10;  -- 每key 10 bits
```

**Compaction优化**:

```sql
-- Compaction线程数
SET GLOBAL rocksdb_max_background_compactions = 4;
SET GLOBAL rocksdb_max_background_flushes = 2;

-- Level配置
SET GLOBAL rocksdb_level0_file_num_compaction_trigger = 4;
SET GLOBAL rocksdb_level0_slowdown_writes_trigger = 20;
SET GLOBAL rocksdb_level0_stop_writes_trigger = 36;
```

### 6.2 监控指标

```sql
-- 查看RocksDB统计信息
SELECT * FROM information_schema.ROCKSDB_DBSTATS;

-- 查看SST文件分布
SELECT * FROM information_schema.ROCKSDB_SST_PROPS;

-- 查看Compaction统计
SELECT * FROM information_schema.ROCKSDB_COMPACTION_STATS;

-- 关键指标监控
SHOW STATUS LIKE 'rocksdb_block_cache%';      -- Block Cache命中率
SHOW STATUS LIKE 'rocksdb_memtable%';         -- MemTable使用情况
SHOW STATUS LIKE 'rocksdb_num_keys_%';        -- Key统计
SHOW STATUS LIKE 'rocksdb_compaction_%';      -- Compaction统计
SHOW STATUS LIKE 'rocksdb_wal_%';             -- WAL统计
```

**关键指标**:

| 指标 | 说明 | 优化目标 |
|-----|------|---------|
| `rocksdb_block_cache_hit` | Block Cache命中次数 | 命中率>95% |
| `rocksdb_memtable_hit` | MemTable命中次数 | 越高越好 |
| `rocksdb_l0_num_files` | Level 0文件数 | <10个 |
| `rocksdb_compaction_pending` | 待Compaction文件数 | <50 |
| `rocksdb_write_stall_us` | 写入停顿时间 | 越少越好 |
| `rocksdb_wal_bytes` | WAL写入字节数 | 监控增长 |

### 6.3 LSM-Tree特性分析

```mermaid
graph LR
    subgraph "<b>LSM-Tree优势</b>"
        WRITE_OPT["<b>写入优化</b><br/>顺序写WAL+MemTable<br/>写放大低<br/>无随机写磁盘"]
        
        COMPRESSION["<b>高压缩比</b><br/>LZ4/Snappy<br/>节省50-80%空间"]
        
        RANGE_SCAN["<b>范围扫描</b><br/>SST文件有序<br/>扫描效率高"]
    end
    
    subgraph "<b>LSM-Tree劣势</b>"
        READ_AMP["<b>读放大</b><br/>需查找多层SST<br/>Bloom Filter缓解"]
        
        SPACE_AMP["<b>空间放大</b><br/>Compaction前数据重复<br/>后台合并解决"]
        
        WRITE_AMP_COMP["<b>写放大（Compaction）</b><br/>多次重写数据<br/>Level策略优化"]
    end
    
    WRITE_OPT --> COMPRESSION
    COMPRESSION --> RANGE_SCAN
    
    READ_AMP --> SPACE_AMP
    SPACE_AMP --> WRITE_AMP_COMP
    
    style WRITE_OPT fill:#e8f5e8,stroke:#333,stroke-width:2px
    style READ_AMP fill:#ffebee,stroke:#333,stroke-width:2px
```

---

## 总结

### 核心要点回顾

**文件结构**:

- **WAL文件**: 32KB block，7/11字节header，顺序写入
- **SST文件**: 分层组织（L0-L6），Footer + Index + Data Blocks + Filter
- **MANIFEST**: 版本控制和文件元数据管理

**内存结构**:

- **MemTable**: SkipList实现，InternalKey = UserKey + SeqNum + Type
- **Block Cache**: LRU缓存，分Shard减少锁竞争
- **Write Buffer Manager**: 统一管理内存使用

**读写流程**:

- **写入**: WAL → MemTable → Immutable → Flush to L0
- **读取**: MemTable → Block Cache → SST (L0→L6)
- **Compaction**: 归并排序，消除重复，维护LSM-Tree

**性能特点**:

- **写入优化**: 顺序写，无随机写磁盘
- **读放大**: 多层查找，Bloom Filter和Cache优化
- **空间效率**: 高压缩比，Compaction消除冗余

**适用场景**:

- **写多读少**: 日志、时序数据、监控数据
- **范围查询**: 有序数据扫描
- **大数据量**: TB级别数据存储

MyRocks的LSM-Tree架构，为MySQL提供了高吞吐量的写入能力和优秀的压缩率，是OLTP和时序数据的理想选择。
