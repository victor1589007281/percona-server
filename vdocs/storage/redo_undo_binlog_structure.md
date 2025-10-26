# MySQL Redo/Undo/Binlog 物理存储与内存结构深度解析

## 概述

本文档详细解析MySQL三大日志系统的物理存储结构和内存组织：**Redo Log（重做日志）**、**Undo Log（回滚日志）**、**Binlog（二进制日志）**。从**字节级别**深入分析它们的文件格式、内存结构、以及读写流程。

**基于源码**:

- Redo Log: `storage/innobase/include/log0*.h`
- Undo Log: `storage/innobase/include/trx0*.h`
- Binlog: `sql/log_event.h`, `sql/binlog*.h`

---

## 第一部分：Redo Log 深度解析

### 1.1 Redo Log 整体架构

```mermaid
graph TB
    subgraph "<b>Redo Log 文件组织</b>"
        REDO_DIR["<b>#innodb_redo目录</b><br/>MySQL 8.0.30+<br/>datadir/#innodb_redo/"]
        
        FILE_0["<b>#ib_redo0</b><br/>第一个redo文件"]
        FILE_1["<b>#ib_redo1</b><br/>第二个redo文件"]
        FILE_N["<b>#ib_redoN</b><br/>循环使用"]
    end
    
    subgraph "<b>单个Redo文件结构</b>"
        FILE_HDR["<b>文件头</b><br/>2048字节<br/>元数据"]
        
        CHECKPOINT_1["<b>Checkpoint#1</b><br/>512字节<br/>检查点信息"]
        
        CHECKPOINT_2["<b>Checkpoint#2</b><br/>512字节<br/>备份检查点"]
        
        LOG_BLOCKS["<b>Log Blocks</b><br/>512字节/块<br/>实际日志数据"]
    end
    
    subgraph "<b>内存结构</b>"
        LOG_BUF["<b>Log Buffer</b><br/>innodb_log_buffer_size<br/>16MB默认"]
        
        LOG_WRITER["<b>Log Writer</b><br/>后台写入线程"]
        
        LOG_FLUSHER["<b>Log Flusher</b><br/>fsync线程"]
    end
    
    REDO_DIR --> FILE_0
    FILE_0 --> FILE_1
    FILE_1 --> FILE_N
    
    FILE_0 --> FILE_HDR
    FILE_HDR --> CHECKPOINT_1
    CHECKPOINT_1 --> CHECKPOINT_2
    CHECKPOINT_2 --> LOG_BLOCKS
    
    LOG_BUF --> LOG_WRITER
    LOG_WRITER --> LOG_FLUSHER
    LOG_FLUSHER --> FILE_0
    
    style FILE_HDR fill:#e3f2fd,stroke:#333,stroke-width:2px
    style LOG_BLOCKS fill:#e8f5e8,stroke:#333,stroke-width:2px
    style LOG_BUF fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 1.2 Redo Log 文件头部字节级结构

**源码位置**: `storage/innobase/include/log0constants.h:183-239`

```mermaid
graph LR
    subgraph "<b>Redo Log文件头（2048字节）</b>"
        subgraph "<b>核心字段</b>"
            FORMAT["<b>0-3</b><br/>LOG_HEADER_FORMAT<br/>格式版本<br/>uint32"]
            UUID["<b>4-11</b><br/>LOG_HEADER_LOG_UUID<br/>日志UUID<br/>8字节"]
            START_LSN["<b>12-19</b><br/>LOG_HEADER_START_LSN<br/>起始LSN<br/>uint64"]
            CREATOR["<b>20-51</b><br/>LOG_HEADER_CREATOR<br/>创建者<br/>32字节字符串"]
            FLAGS["<b>52-55</b><br/>LOG_HEADER_FLAGS<br/>标志位<br/>uint32"]
        end
        
        ENCRYPTION["<b>52+</b><br/>加密信息<br/>可选"]
        PADDING["<b>剩余空间</b><br/>填充至2048字节"]
    end
    
    FORMAT --> UUID
    UUID --> START_LSN
    START_LSN --> CREATOR
    CREATOR --> FLAGS
    FLAGS --> ENCRYPTION
    ENCRYPTION --> PADDING
    
    style FORMAT fill:#ffebee,stroke:#333,stroke-width:2px
    style START_LSN fill:#e3f2fd,stroke:#333,stroke-width:2px
    style FLAGS fill:#fff3e0,stroke:#333,stroke-width:2px
```

**Redo Log文件头字节级映射表**:

| 偏移量 | 长度 | 字段名 | 数据类型 | 说明 |
|-------|-----|--------|---------|------|
| **0** | 4 | `LOG_HEADER_FORMAT` | uint32 BE | 格式版本 (6=8.0.30+) |
| **4** | 8 | `LOG_HEADER_LOG_UUID` | uint64 | 日志组UUID，用于检测文件混用 |
| **8** | 8 | `LOG_HEADER_START_LSN` | uint64 | 该文件第一个block的起始LSN |
| **16** | 32 | `LOG_HEADER_CREATOR` | char[32] | 创建者字符串，如"MySQL 8.0.35" |
| **48** | 4 | `LOG_HEADER_FLAGS` | uint32 | 标志位（见下表） |
| **52** | 变长 | 加密信息 | bytes | 如果启用加密 |
| **...** | ... | 填充 | zeros | 填充至2048字节 |

**LOG_HEADER_FLAGS 标志位**:

| 位 | 常量名 | 说明 |
|----|-------|------|
| **BIT-1** | `LOG_HEADER_FLAG_NO_LOGGING` | Redo logging已禁用 |
| **BIT-2** | `LOG_HEADER_FLAG_CRASH_UNSAFE` | 崩溃不安全（禁用日志时） |
| **BIT-3** | `LOG_HEADER_FLAG_NOT_INITIALIZED` | 数据目录未完全初始化 |
| **BIT-4** | `LOG_HEADER_FLAG_FILE_FULL` | 文件已满，关闭写入 |

### 1.3 Redo Log Block 详细结构

```mermaid
graph TB
    subgraph "<b>Log Block结构（512字节）</b>"
        subgraph "<b>Block Header（12字节）</b>"
            HDR_NO["<b>0-3</b><br/>LOG_BLOCK_HDR_NO<br/>块编号<br/>uint32"]
            DATA_LEN["<b>4-5</b><br/>LOG_BLOCK_HDR_DATA_LEN<br/>数据长度<br/>uint16"]
            FIRST_REC["<b>6-7</b><br/>LOG_BLOCK_FIRST_REC_GROUP<br/>第一个mtr偏移<br/>uint16"]
            EPOCH_NO["<b>8-11</b><br/>LOG_BLOCK_EPOCH_NO<br/>epoch编号<br/>uint32"]
        end
        
        BLOCK_DATA["<b>数据区域（496字节）</b><br/>12-507<br/>实际redo log记录"]
        
        BLOCK_TRAILER["<b>Block Trailer（4字节）</b><br/>508-511<br/>LOG_BLOCK_CHECKSUM<br/>校验和"]
    end
    
    HDR_NO --> DATA_LEN
    DATA_LEN --> FIRST_REC
    FIRST_REC --> EPOCH_NO
    EPOCH_NO --> BLOCK_DATA
    BLOCK_DATA --> BLOCK_TRAILER
    
    style HDR_NO fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BLOCK_DATA fill:#e8f5e8,stroke:#333,stroke-width:2px
    style BLOCK_TRAILER fill:#fff3e0,stroke:#333,stroke-width:2px
```

**Log Block字节级映射表**:

| 偏移量 | 长度 | 字段名 | 说明 |
|-------|-----|--------|------|
| **0** | 4 | `LOG_BLOCK_HDR_NO` | 块编号（>0），最高位可能用于flush标记 |
| **4** | 2 | `LOG_BLOCK_HDR_DATA_LEN` | 数据长度（含header），最高位标识加密 |
| **6** | 2 | `LOG_BLOCK_FIRST_REC_GROUP` | 第一个完整mtr记录组的偏移 |
| **8** | 4 | `LOG_BLOCK_EPOCH_NO` | Epoch编号，配合hdr_no形成全局块号 |
| **12** | 496 | 数据区域 | 实际redo log记录 |
| **508** | 4 | `LOG_BLOCK_CHECKSUM` | CRC32校验和 |

**块编号计算**:

- **全局块号** = `epoch_no * LOG_BLOCK_MAX_NO + hdr_no`
- **LOG_BLOCK_MAX_NO** = 1073741824 (2^30)
- **Epoch** = 每10.7亿个块为一个周期

### 1.4 Checkpoint 结构

```mermaid
graph LR
    subgraph "<b>Checkpoint页（512字节）</b>"
        CKPT_NO["<b>0-7</b><br/>Checkpoint No<br/>uint64<br/>检查点编号"]
        
        CKPT_LSN["<b>8-15</b><br/>LOG_CHECKPOINT_LSN<br/>uint64<br/>检查点LSN"]
        
        CKPT_OFFSET["<b>16-23</b><br/>Checkpoint Offset<br/>uint64<br/>文件偏移"]
        
        CKPT_BUF_SIZE["<b>24-31</b><br/>Buffer Size<br/>uint64<br/>日志缓冲区大小"]
        
        PADDING["<b>32-511</b><br/>填充和预留字段"]
    end
    
    CKPT_NO --> CKPT_LSN
    CKPT_LSN --> CKPT_OFFSET
    CKPT_OFFSET --> CKPT_BUF_SIZE
    CKPT_BUF_SIZE --> PADDING
    
    style CKPT_LSN fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CKPT_OFFSET fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**Checkpoint字段说明**:

- **Checkpoint LSN**: 恢复的起点LSN，之前的修改已安全刷盘
- **两个Checkpoint页**: 交替写入，保证至少有一个完整有效
- **位置**: 文件偏移2048和2560字节处

### 1.5 Redo Log 内存结构

```mermaid
graph TB
    subgraph "<b>Redo Log内存架构</b>"
        subgraph "<b>Log Buffer (环形缓冲区)</b>"
            BUF_START["<b>缓冲区起点</b><br/>buf_start"]
            BUF_RECENT_WRITTEN["<b>最近写入位置</b><br/>recent_written"]
            BUF_RECENT_CLOSED["<b>最近关闭位置</b><br/>recent_closed"]
            BUF_SIZE["<b>缓冲区大小</b><br/>innodb_log_buffer_size<br/>默认16MB"]
        end
        
        subgraph "<b>LSN跟踪</b>"
            WRITE_LSN["<b>write_lsn</b><br/>已写入文件的LSN"]
            FLUSHED_LSN["<b>flushed_to_disk_lsn</b><br/>已fsync的LSN"]
            CURRENT_LSN["<b>current_lsn</b><br/>当前分配的LSN"]
        end
        
        subgraph "<b>后台线程</b>"
            LOG_WRITER_T["<b>log_writer线程</b><br/>write()到文件"]
            LOG_FLUSHER_T["<b>log_flusher线程</b><br/>fsync()持久化"]
            LOG_WRITE_NOTIFIER["<b>log_write_notifier</b><br/>通知已写入"]
            LOG_FLUSH_NOTIFIER["<b>log_flush_notifier</b><br/>通知已刷盘"]
        end
    end
    
    BUF_START --> BUF_RECENT_WRITTEN
    BUF_RECENT_WRITTEN --> BUF_RECENT_CLOSED
    BUF_RECENT_CLOSED --> BUF_SIZE
    
    CURRENT_LSN --> WRITE_LSN
    WRITE_LSN --> FLUSHED_LSN
    
    BUF_SIZE --> LOG_WRITER_T
    LOG_WRITER_T --> LOG_FLUSHER_T
    LOG_FLUSHER_T --> LOG_WRITE_NOTIFIER
    LOG_WRITE_NOTIFIER --> LOG_FLUSH_NOTIFIER
    
    style BUF_START fill:#e3f2fd,stroke:#333,stroke-width:2px
    style WRITE_LSN fill:#fff3e0,stroke:#333,stroke-width:2px
    style LOG_WRITER_T fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 1.6 Redo Log 写入流程

```mermaid
sequenceDiagram
    participant TRX as **事务线程**
    participant MTR as **Mini-Transaction**
    participant LOGBUF as **Log Buffer**
    participant WRITER as **log_writer线程**
    participant FLUSHER as **log_flusher线程**
    participant FILE as **Redo文件**

    Note over TRX,FILE: **写入阶段**

    TRX->>MTR: 开始修改页面
    MTR->>MTR: 记录redo log<br/>生成log records
    MTR->>LOGBUF: mtr_commit()<br/>写入Log Buffer<br/>分配LSN
    
    Note over LOGBUF: **Log Buffer填充**<br/>recent_closed LSN前进
    
    alt innodb_flush_log_at_trx_commit=1
        TRX->>WRITER: 等待write完成
        TRX->>FLUSHER: 等待fsync完成
    else innodb_flush_log_at_trx_commit=2
        TRX->>WRITER: 等待write完成
    else innodb_flush_log_at_trx_commit=0
        TRX->>TRX: 不等待，继续
    end
    
    Note over WRITER,FILE: **后台写入**
    
    loop 每1秒或Buffer>50%
        WRITER->>WRITER: 检查recent_closed
        WRITER->>FILE: write()写入OS缓存<br/>更新write_lsn
        WRITER->>TRX: 通知已写入
    end
    
    loop 每1秒或需要时
        FLUSHER->>FLUSHER: 检查write_lsn
        FLUSHER->>FILE: fsync()刷盘<br/>更新flushed_to_disk_lsn
        FLUSHER->>TRX: 通知已持久化
    end
    
    TRX-->>TRX: 事务提交返回
```

**关键参数**:

| 参数 | 默认值 | 说明 |
|-----|-------|------|
| `innodb_log_buffer_size` | 16MB | Log Buffer大小 |
| `innodb_flush_log_at_trx_commit` | 1 | 0=不等待, 1=等待fsync, 2=等待write |
| `innodb_log_write_ahead_size` | 8KB | 写前对齐大小 |
| `innodb_redo_log_capacity` | 100MB | Redo log总容量（8.0.30+） |

---

## 第二部分：Undo Log 深度解析

### 2.1 Undo Log 整体架构

```mermaid
graph TB
    subgraph "<b>Undo Tablespace组织</b>"
        SYS_UNDO["<b>系统Undo表空间</b><br/>ibdata1中的<br/>undo segments"]
        
        UNDO_001["<b>undo_001</b><br/>独立undo表空间"]
        UNDO_002["<b>undo_002</b><br/>独立undo表空间"]
        UNDO_N["<b>undo_00N</b><br/>可配置数量<br/>innodb_undo_tablespaces"]
    end
    
    subgraph "<b>Rollback Segment结构</b>"
        RSEG_HEADER["<b>Rollback Segment Header</b><br/>Page 3<br/>段头页面"]
        
        RSEG_ARRAY["<b>Undo Slot Array</b><br/>1024个slot<br/>指向undo segment"]
        
        HISTORY_LIST["<b>History List</b><br/>已提交事务的<br/>undo log链表"]
    end
    
    subgraph "<b>Undo Segment结构</b>"
        UNDO_SEG_HDR["<b>Undo Segment Header</b><br/>段头信息"]
        
        UNDO_LOG_HDR["<b>Undo Log Header</b><br/>事务undo log头"]
        
        UNDO_RECORDS["<b>Undo Records</b><br/>实际undo记录"]
    end
    
    SYS_UNDO --> RSEG_HEADER
    UNDO_001 --> RSEG_HEADER
    UNDO_002 --> RSEG_HEADER
    UNDO_N --> RSEG_HEADER
    
    RSEG_HEADER --> RSEG_ARRAY
    RSEG_ARRAY --> HISTORY_LIST
    
    RSEG_ARRAY --> UNDO_SEG_HDR
    UNDO_SEG_HDR --> UNDO_LOG_HDR
    UNDO_LOG_HDR --> UNDO_RECORDS
    
    style RSEG_HEADER fill:#e3f2fd,stroke:#333,stroke-width:2px
    style UNDO_SEG_HDR fill:#fff3e0,stroke:#333,stroke-width:2px
    style UNDO_RECORDS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 2.2 Undo Segment Header 字节级结构

**源码位置**: `storage/innobase/include/trx0undo.h:502-520`

```mermaid
graph LR
    subgraph "<b>Undo Segment Header</b>"
        STATE["<b>0-1</b><br/>TRX_UNDO_STATE<br/>状态<br/>uint16"]
        
        LAST_LOG["<b>2-3</b><br/>TRX_UNDO_LAST_LOG<br/>最后undo log偏移<br/>uint16"]
        
        FSEG_HDR["<b>4-13</b><br/>TRX_UNDO_FSEG_HEADER<br/>文件段头<br/>10字节"]
        
        PAGE_LIST["<b>14-29</b><br/>TRX_UNDO_PAGE_LIST<br/>页面链表<br/>16字节"]
    end
    
    STATE --> LAST_LOG
    LAST_LOG --> FSEG_HDR
    FSEG_HDR --> PAGE_LIST
    
    style STATE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style FSEG_HDR fill:#fff3e0,stroke:#333,stroke-width:2px
```

**Undo Segment Header字节级映射表**:

| 偏移量 | 长度 | 字段名 | 说明 |
|-------|-----|--------|------|
| **0** | 2 | `TRX_UNDO_STATE` | 状态：ACTIVE(1), CACHED(2), TO_FREE(3), TO_PURGE(4), PREPARED(6) |
| **2** | 2 | `TRX_UNDO_LAST_LOG` | 最后一个undo log header的偏移，0表示无 |
| **4** | 10 | `TRX_UNDO_FSEG_HEADER` | 文件段头（space_id + page_no + offset） |
| **14** | 16 | `TRX_UNDO_PAGE_LIST` | Undo页面链表基节点（FLST_BASE_NODE） |

**Undo状态值**:

| 值 | 常量名 | 说明 |
|----|-------|------|
| **1** | `TRX_UNDO_ACTIVE` | 活动事务的undo log |
| **2** | `TRX_UNDO_CACHED` | 缓存以便重用 |
| **3** | `TRX_UNDO_TO_FREE` | Insert undo，可释放 |
| **4** | `TRX_UNDO_TO_PURGE` | Update undo，待purge |
| **6** | `TRX_UNDO_PREPARED` | XA prepared事务 |

### 2.3 Undo Log Header 详细结构

**源码位置**: `storage/innobase/include/trx0undo.h:522-608`

```mermaid
graph TB
    subgraph "<b>Undo Log Header（变长）</b>"
        subgraph "<b>基础头部（34字节）</b>"
            TRX_ID["<b>0-7</b><br/>TRX_UNDO_TRX_ID<br/>事务ID<br/>uint64"]
            TRX_NO["<b>8-15</b><br/>TRX_UNDO_TRX_NO<br/>事务号<br/>uint64"]
            DEL_MARKS["<b>16-17</b><br/>TRX_UNDO_DEL_MARKS<br/>删除标记<br/>uint16"]
            LOG_START["<b>18-19</b><br/>TRX_UNDO_LOG_START<br/>undo log起始<br/>uint16"]
            FLAGS["<b>20</b><br/>TRX_UNDO_FLAGS<br/>标志位<br/>uint8"]
            DICT_TRANS["<b>21</b><br/>TRX_UNDO_DICT_TRANS<br/>DDL事务<br/>uint8"]
            TABLE_ID["<b>22-29</b><br/>TRX_UNDO_TABLE_ID<br/>表ID(deprecated)<br/>uint64"]
            NEXT_LOG["<b>30-31</b><br/>TRX_UNDO_NEXT_LOG<br/>下一个undo log<br/>uint16"]
            PREV_LOG["<b>32-33</b><br/>TRX_UNDO_PREV_LOG<br/>前一个undo log<br/>uint16"]
        end
        
        HISTORY_NODE["<b>34-45</b><br/>TRX_UNDO_HISTORY_NODE<br/>历史链表节点<br/>12字节"]
        
        subgraph "<b>扩展部分（可选）</b>"
            XA_INFO["<b>46-203</b><br/>XA事务信息<br/>158字节<br/>格式ID+GTRID+BQUAL"]
            GTID_INFO["<b>204-268</b><br/>GTID信息<br/>65字节<br/>版本+GTID"]
        end
    end
    
    TRX_ID --> TRX_NO
    TRX_NO --> DEL_MARKS
    DEL_MARKS --> LOG_START
    LOG_START --> FLAGS
    FLAGS --> DICT_TRANS
    DICT_TRANS --> TABLE_ID
    TABLE_ID --> NEXT_LOG
    NEXT_LOG --> PREV_LOG
    PREV_LOG --> HISTORY_NODE
    HISTORY_NODE --> XA_INFO
    XA_INFO --> GTID_INFO
    
    style TRX_ID fill:#e3f2fd,stroke:#333,stroke-width:2px
    style FLAGS fill:#fff3e0,stroke:#333,stroke-width:2px
    style XA_INFO fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**TRX_UNDO_FLAGS标志位**:

| 位 | 常量名 | 说明 |
|----|-------|------|
| **0x01** | `TRX_UNDO_FLAG_XID` | 包含XA事务标识 |
| **0x02** | `TRX_UNDO_FLAG_GTID` | 包含GTID信息 |
| **0x04** | `TRX_UNDO_FLAG_XA_PREPARE_GTID` | XA PREPARE的GTID |

### 2.4 Undo Record 格式

```mermaid
graph LR
    subgraph "<b>Undo Record结构</b>"
        subgraph "<b>记录头</b>"
            REC_TYPE["<b>Byte 0</b><br/>type_cmpl<br/>类型+编译信息"]
            UNDO_NO["<b>1-N</b><br/>undo_no<br/>压缩uint<br/>事务内序号"]
            TABLE_ID_R["<b>N-M</b><br/>table_id<br/>压缩uint<br/>表ID"]
        end
        
        subgraph "<b>记录内容（取决于类型）</b>"
            INFO_BITS["<b>info_bits</b><br/>记录信息位"]
            TRX_ID_R["<b>trx_id</b><br/>事务ID"]
            ROLL_PTR["<b>roll_ptr</b><br/>回滚指针"]
            FIELDS["<b>字段数据</b><br/>修改的列值"]
        end
    end
    
    REC_TYPE --> UNDO_NO
    UNDO_NO --> TABLE_ID_R
    TABLE_ID_R --> INFO_BITS
    INFO_BITS --> TRX_ID_R
    TRX_ID_R --> ROLL_PTR
    ROLL_PTR --> FIELDS
    
    style REC_TYPE fill:#ffebee,stroke:#333,stroke-width:2px
    style TRX_ID_R fill:#e3f2fd,stroke:#333,stroke-width:2px
    style FIELDS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**源码位置**: `storage/innobase/include/trx0rec.h:298-316`

**Undo Record类型**:

| 类型值 | 常量名 | 说明 |
|-------|-------|------|
| **11** | `TRX_UNDO_INSERT_REC` | INSERT操作的undo |
| **12** | `TRX_UNDO_UPD_EXIST_REC` | UPDATE非删除标记记录 |
| **13** | `TRX_UNDO_UPD_DEL_REC` | UPDATE删除标记转为非删除 |
| **14** | `TRX_UNDO_DEL_MARK_REC` | DELETE标记（不改字段） |

**type_cmpl字节位域**:

- **Bit 0-3**: 记录类型（11, 12, 13, 14）
- **Bit 4-5**: 编译信息（CMPL_INFO）
- **Bit 6**: 保留
- **Bit 7**: `TRX_UNDO_MODIFY_BLOB` (64) - 修改了BLOB
- **Bit 7**: `TRX_UNDO_UPD_EXTERN` (128) - 更新外部存储

### 2.5 Undo Log 内存结构

```mermaid
graph TB
    subgraph "<b>Undo内存结构</b>"
        subgraph "<b>trx_sys (事务系统)</b>"
            RSEG_ARRAY_MEM["<b>rseg_array</b><br/>128个rollback segment"]
            RSEG_HISTORY["<b>rseg_history_len</b><br/>history list长度<br/>待purge的undo数量"]
        end
        
        subgraph "<b>trx_rseg_t (Rollback Segment)</b>"
            RSEG_ID["<b>id</b><br/>Rollback segment ID"]
            RSEG_SPACE["<b>space_id</b><br/>表空间ID"]
            RSEG_PAGE["<b>page_no</b><br/>段头页号"]
            RSEG_MUTEX["<b>mutex</b><br/>并发保护"]
            RSEG_INSERT_LIST["<b>insert_undo_list</b><br/>INSERT undo链表"]
            RSEG_UPDATE_LIST["<b>update_undo_list</b><br/>UPDATE undo链表"]
        end
        
        subgraph "<b>trx_undo_t (Undo Segment)</b>"
            UNDO_ID["<b>id</b><br/>undo segment slot ID"]
            UNDO_TYPE["<b>type</b><br/>INSERT/UPDATE"]
            UNDO_STATE["<b>state</b><br/>ACTIVE/CACHED/..."]
            UNDO_SIZE["<b>size</b><br/>页面数量"]
            UNDO_TRX_ID_M["<b>trx_id</b><br/>所属事务ID"]
            UNDO_HDR_PAGE["<b>hdr_page_no</b><br/>头部页号"]
        end
    end
    
    RSEG_ARRAY_MEM --> RSEG_ID
    RSEG_HISTORY --> RSEG_ID
    
    RSEG_ID --> RSEG_SPACE
    RSEG_SPACE --> RSEG_PAGE
    RSEG_PAGE --> RSEG_MUTEX
    RSEG_MUTEX --> RSEG_INSERT_LIST
    RSEG_INSERT_LIST --> RSEG_UPDATE_LIST
    
    RSEG_UPDATE_LIST --> UNDO_ID
    UNDO_ID --> UNDO_TYPE
    UNDO_TYPE --> UNDO_STATE
    UNDO_STATE --> UNDO_SIZE
    UNDO_SIZE --> UNDO_TRX_ID_M
    UNDO_TRX_ID_M --> UNDO_HDR_PAGE
    
    style RSEG_ARRAY_MEM fill:#e3f2fd,stroke:#333,stroke-width:2px
    style RSEG_MUTEX fill:#fff3e0,stroke:#333,stroke-width:2px
    style UNDO_STATE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 2.6 Undo Log 写入与回滚流程

```mermaid
sequenceDiagram
    participant TRX as **事务**
    participant RSEG as **Rollback Segment**
    participant UNDO_SEG as **Undo Segment**
    participant UNDO_PAGE as **Undo页面**
    participant REDO as **Redo Log**

    Note over TRX,REDO: **Undo写入流程**

    TRX->>RSEG: 开始事务<br/>分配undo segment
    RSEG->>UNDO_SEG: 从cached中复用<br/>或分配新的
    
    TRX->>TRX: UPDATE table SET col=new<br/>WHERE id=1
    
    TRX->>UNDO_PAGE: 构造undo record<br/>记录旧值(col=old)
    UNDO_PAGE->>UNDO_PAGE: 写入：<br/>• type=TRX_UNDO_UPD_EXIST_REC<br/>• undo_no<br/>• table_id<br/>• 旧列值
    
    TRX->>REDO: 记录undo写入的redo<br/>MLOG_UNDO_INSERT
    
    TRX->>TRX: 更新聚集索引记录<br/>DB_TRX_ID=trx_id<br/>DB_ROLL_PTR=undo_ptr
    
    Note over TRX,REDO: **回滚流程**
    
    alt 事务回滚
        TRX->>UNDO_SEG: 读取undo log header
        loop 倒序遍历undo records
            TRX->>UNDO_PAGE: 读取undo record
            UNDO_PAGE-->>TRX: 返回旧值
            TRX->>TRX: 恢复记录：<br/>col=old
            TRX->>REDO: 记录恢复操作的redo
        end
        TRX->>UNDO_SEG: 标记状态=TO_FREE/TO_PURGE
    else 事务提交
        TRX->>UNDO_SEG: INSERT undo: 标记TO_FREE<br/>UPDATE undo: 加入history list
        TRX->>RSEG: update_undo缓存或<br/>等待purge清理
    end
```

---

## 第三部分：Binlog 深度解析

### 3.1 Binlog 整体架构

```mermaid
graph TB
    subgraph "<b>Binlog文件组织</b>"
        BINLOG_DIR["<b>Binlog目录</b><br/>--log-bin=datadir/binlog"]
        
        BINLOG_000001["<b>binlog.000001</b><br/>第一个文件"]
        BINLOG_000002["<b>binlog.000002</b><br/>第二个文件"]
        BINLOG_N["<b>binlog.N</b><br/>轮转文件"]
        
        BINLOG_INDEX["<b>binlog.index</b><br/>索引文件<br/>记录所有binlog文件名"]
    end
    
    subgraph "<b>单个Binlog文件结构</b>"
        MAGIC["<b>文件魔数</b><br/>4字节<br/>0xfe626976"]
        
        FDE["<b>FORMAT_DESCRIPTION_EVENT</b><br/>格式描述<br/>binlog版本信息"]
        
        PREV_GTIDS["<b>PREVIOUS_GTIDS_EVENT</b><br/>前文件GTID集合"]
        
        EVENTS["<b>各种Event</b><br/>Query/Table_map/Write_rows等"]
        
        ROTATE["<b>ROTATE_EVENT</b><br/>指向下一个文件"]
    end
    
    subgraph "<b>内存结构</b>"
        BINLOG_CACHE["<b>Binlog Cache</b><br/>每个事务一个<br/>binlog_cache_size"]
        
        BINLOG_IO_CACHE["<b>IO_CACHE</b><br/>写缓冲"]
        
        SYNC_BINLOG["<b>Sync策略</b><br/>sync_binlog参数"]
    end
    
    BINLOG_DIR --> BINLOG_000001
    BINLOG_000001 --> BINLOG_000002
    BINLOG_000002 --> BINLOG_N
    BINLOG_DIR --> BINLOG_INDEX
    
    BINLOG_000001 --> MAGIC
    MAGIC --> FDE
    FDE --> PREV_GTIDS
    PREV_GTIDS --> EVENTS
    EVENTS --> ROTATE
    
    BINLOG_CACHE --> BINLOG_IO_CACHE
    BINLOG_IO_CACHE --> SYNC_BINLOG
    SYNC_BINLOG --> BINLOG_000001
    
    style MAGIC fill:#ffebee,stroke:#333,stroke-width:2px
    style FDE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style EVENTS fill:#e8f5e8,stroke:#333,stroke-width:2px
    style BINLOG_CACHE fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 3.2 Binlog文件头部

**源码位置**: `sql/log_event.h:213-215`

```mermaid
graph LR
    subgraph "<b>Binlog文件头（4字节）</b>"
        MAGIC_BYTES["<b>BINLOG_MAGIC</b><br/>0xfe 0x62 0x69 0x6e<br/>4字节固定"]
    end
    
    subgraph "<b>第一个Event: FORMAT_DESCRIPTION</b>"
        FD_HEADER["<b>Event Header</b><br/>19字节"]
        FD_BODY["<b>Event Body</b><br/>格式描述信息"]
    end
    
    MAGIC_BYTES --> FD_HEADER
    FD_HEADER --> FD_BODY
    
    style MAGIC_BYTES fill:#ffebee,stroke:#333,stroke-width:2px
    style FD_HEADER fill:#e3f2fd,stroke:#333,stroke-width:2px
```

**BINLOG_MAGIC**:

- **十六进制**: `0xfe 0x62 0x69 0x6e`
- **ASCII**: `þbin`
- **作用**: 标识这是一个MySQL binlog文件

### 3.3 Binlog Event Header 字节级结构

**源码位置**: `sql/rpl_source.cc:812-840`, `sql/log_event.cc:1227-1264`

```mermaid
graph LR
    subgraph "<b>Event Header（19字节，binlog v4）</b>"
        TIMESTAMP["<b>0-3</b><br/>timestamp<br/>uint32<br/>事件时间戳"]
        
        EVENT_TYPE["<b>4</b><br/>event_type<br/>uint8<br/>事件类型"]
        
        SERVER_ID["<b>5-8</b><br/>server_id<br/>uint32<br/>源服务器ID"]
        
        EVENT_SIZE["<b>9-12</b><br/>event_size<br/>uint32<br/>事件总大小"]
        
        LOG_POS["<b>13-16</b><br/>log_pos<br/>uint32<br/>下一事件位置"]
        
        FLAGS["<b>17-18</b><br/>flags<br/>uint16<br/>事件标志"]
    end
    
    TIMESTAMP --> EVENT_TYPE
    EVENT_TYPE --> SERVER_ID
    SERVER_ID --> EVENT_SIZE
    EVENT_SIZE --> LOG_POS
    LOG_POS --> FLAGS
    
    style TIMESTAMP fill:#e3f2fd,stroke:#333,stroke-width:2px
    style EVENT_TYPE fill:#fff3e0,stroke:#333,stroke-width:2px
    style LOG_POS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**Event Header字节级映射表**:

| 偏移量 | 长度 | 字段名 | 数据类型 | 说明 |
|-------|-----|--------|---------|------|
| **0** | 4 | `timestamp` | uint32 LE | Unix时间戳（秒） |
| **4** | 1 | `event_type` | uint8 | 事件类型（见下表） |
| **5** | 4 | `server_id` | uint32 LE | 源服务器ID，用于循环复制过滤 |
| **9** | 4 | `event_size` | uint32 LE | 事件总大小（header+body+checksum） |
| **13** | 4 | `log_pos` | uint32 LE | 下一个事件在文件中的位置 |
| **17** | 2 | `flags` | uint16 LE | 事件标志位 |

**常见Event类型**:

| 类型值 | 事件名称 | 说明 |
|-------|---------|------|
| **2** | QUERY_EVENT | SQL语句（DDL/BEGIN等） |
| **4** | ROTATE_EVENT | binlog文件轮转 |
| **15** | FORMAT_DESCRIPTION_EVENT | binlog格式描述 |
| **16** | XID_EVENT | 事务提交（XA） |
| **19** | TABLE_MAP_EVENT | 表映射（ROW格式） |
| **30** | WRITE_ROWS_EVENT | INSERT（ROW格式） |
| **31** | UPDATE_ROWS_EVENT | UPDATE（ROW格式） |
| **32** | DELETE_ROWS_EVENT | DELETE（ROW格式） |
| **33** | GTID_LOG_EVENT | GTID信息 |

### 3.4 FORMAT_DESCRIPTION_EVENT 详细结构

```mermaid
graph TB
    subgraph "<b>FORMAT_DESCRIPTION_EVENT</b>"
        FD_HEADER_E["<b>Event Header</b><br/>19字节"]
        
        subgraph "<b>Event Body</b>"
            BINLOG_VER["<b>0-1</b><br/>binlog_version<br/>uint16<br/>版本号(4)"]
            
            SERVER_VER["<b>2-51</b><br/>server_version<br/>char[50]<br/>MySQL版本字符串"]
            
            CREATE_TS["<b>52-55</b><br/>create_timestamp<br/>uint32<br/>创建时间戳"]
            
            HEADER_LEN["<b>56</b><br/>common_header_len<br/>uint8<br/>Event header长度(19)"]
            
            POST_HEADER_LEN["<b>57-N</b><br/>post_header_len<br/>uint8数组<br/>每种event的post-header长度"]
        end
        
        CHECKSUM["<b>最后4字节</b><br/>CRC32校验和"]
    end
    
    FD_HEADER_E --> BINLOG_VER
    BINLOG_VER --> SERVER_VER
    SERVER_VER --> CREATE_TS
    CREATE_TS --> HEADER_LEN
    HEADER_LEN --> POST_HEADER_LEN
    POST_HEADER_LEN --> CHECKSUM
    
    style FD_HEADER_E fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SERVER_VER fill:#fff3e0,stroke:#333,stroke-width:2px
    style POST_HEADER_LEN fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 3.5 QUERY_EVENT 结构

```mermaid
graph LR
    subgraph "<b>QUERY_EVENT（SQL语句）</b>"
        QE_HEADER["<b>Event Header</b><br/>19字节"]
        
        subgraph "<b>Post-Header（13字节）</b>"
            THREAD_ID["<b>0-3</b><br/>thread_id<br/>uint32<br/>线程ID"]
            EXEC_TIME["<b>4-7</b><br/>exec_time<br/>uint32<br/>执行时长(秒)"]
            DB_LEN["<b>8</b><br/>db_len<br/>uint8<br/>数据库名长度"]
            ERROR_CODE["<b>9-10</b><br/>error_code<br/>uint16<br/>错误码"]
            STATUS_LEN["<b>11-12</b><br/>status_vars_len<br/>uint16<br/>状态变量长度"]
        end
        
        subgraph "<b>Event Body</b>"
            STATUS_VARS["<b>status_vars</b><br/>状态变量"]
            DATABASE["<b>database</b><br/>数据库名"]
            QUERY["<b>query</b><br/>SQL语句"]
        end
        
        QE_CHECKSUM["<b>Checksum</b><br/>4字节CRC32"]
    end
    
    QE_HEADER --> THREAD_ID
    THREAD_ID --> EXEC_TIME
    EXEC_TIME --> DB_LEN
    DB_LEN --> ERROR_CODE
    ERROR_CODE --> STATUS_LEN
    STATUS_LEN --> STATUS_VARS
    STATUS_VARS --> DATABASE
    DATABASE --> QUERY
    QUERY --> QE_CHECKSUM
    
    style QE_HEADER fill:#e3f2fd,stroke:#333,stroke-width:2px
    style QUERY fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 3.6 Binlog 内存结构与缓存

```mermaid
graph TB
    subgraph "<b>Binlog内存架构</b>"
        subgraph "<b>每事务Binlog Cache</b>"
            TRX_CACHE["<b>binlog_cache_data</b><br/>事务级缓存"]
            CACHE_SIZE["<b>binlog_cache_size</b><br/>初始大小32KB"]
            MAX_CACHE["<b>max_binlog_cache_size</b><br/>最大可扩展大小"]
            CACHE_FILE["<b>临时文件</b><br/>超过内存大小时"]
        end
        
        subgraph "<b>Statement Cache（非事务表）</b>"
            STMT_CACHE["<b>binlog_stmt_cache</b><br/>语句级缓存"]
            STMT_SIZE["<b>binlog_stmt_cache_size</b>"]
        end
        
        subgraph "<b>Binlog文件缓冲</b>"
            IO_CACHE["<b>IO_CACHE</b><br/>文件写缓冲<br/>binlog_cache_disk_use"]
            GROUP_COMMIT["<b>Group Commit</b><br/>批量提交优化"]
        end
        
        subgraph "<b>GTID管理</b>"
            GTID_SET["<b>gtid_executed</b><br/>已执行的GTID集合"]
            GTID_OWNED["<b>gtid_owned</b><br/>事务持有的GTID"]
        end
    end
    
    TRX_CACHE --> CACHE_SIZE
    CACHE_SIZE --> MAX_CACHE
    MAX_CACHE --> CACHE_FILE
    
    STMT_CACHE --> STMT_SIZE
    
    TRX_CACHE --> IO_CACHE
    STMT_CACHE --> IO_CACHE
    IO_CACHE --> GROUP_COMMIT
    
    GROUP_COMMIT --> GTID_SET
    GTID_SET --> GTID_OWNED
    
    style TRX_CACHE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style IO_CACHE fill:#fff3e0,stroke:#333,stroke-width:2px
    style GROUP_COMMIT fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 3.7 Binlog 写入流程（Group Commit）

```mermaid
sequenceDiagram
    participant TRX1 as **事务1**
    participant TRX2 as **事务2**
    participant CACHE as **Binlog Cache**
    participant PREPARE_Q as **Prepare队列**
    participant FLUSH_Q as **Flush队列**
    participant SYNC_Q as **Sync队列**
    participant COMMIT_Q as **Commit队列**
    participant FILE as **Binlog文件**

    Note over TRX1,FILE: **Group Commit三阶段**

    TRX1->>CACHE: SQL执行<br/>写入binlog cache
    TRX2->>CACHE: SQL执行<br/>写入binlog cache
    
    Note over TRX1,TRX2: **Prepare阶段**
    TRX1->>PREPARE_Q: 准备提交<br/>ha_prepare_low()
    TRX2->>PREPARE_Q: 准备提交
    
    Note over PREPARE_Q: **Leader线程处理**
    PREPARE_Q->>PREPARE_Q: 选择Leader(TRX1)<br/>收集Follower(TRX2)
    
    Note over FLUSH_Q,FILE: **Flush阶段**
    PREPARE_Q->>FLUSH_Q: Leader带领所有事务
    FLUSH_Q->>FILE: write()写入OS缓存<br/>合并所有事务的cache
    FLUSH_Q->>FLUSH_Q: 更新binlog position
    
    Note over SYNC_Q,FILE: **Sync阶段**
    alt sync_binlog > 0
        FLUSH_Q->>SYNC_Q: 进入Sync队列
        SYNC_Q->>FILE: fsync()持久化<br/>每sync_binlog个事务一次
    else sync_binlog = 0
        FLUSH_Q->>COMMIT_Q: 跳过fsync
    end
    
    Note over COMMIT_Q: **Commit阶段**
    SYNC_Q->>COMMIT_Q: 所有事务进入
    COMMIT_Q->>COMMIT_Q: InnoDB commit<br/>标记事务完成
    
    COMMIT_Q-->>TRX1: 返回成功
    COMMIT_Q-->>TRX2: 返回成功
```

**Group Commit关键参数**:

| 参数 | 默认值 | 说明 |
|-----|-------|------|
| `binlog_cache_size` | 32KB | 每个事务的binlog缓存初始大小 |
| `max_binlog_cache_size` | 18EB | binlog缓存最大大小 |
| `sync_binlog` | 1 | N>0: 每N个事务fsync一次, 0: 由OS控制 |
| `binlog_group_commit_sync_delay` | 0 | 延迟N微秒收集更多事务（提高吞吐） |
| `binlog_group_commit_sync_no_delay_count` | 0 | 收集N个事务后立即提交 |

---

## 第四部分：三大日志对比与协作

### 4.1 三大日志对比矩阵

```mermaid
graph TB
    subgraph "<b>三大日志特性对比</b>"
        subgraph "<b>Redo Log</b>"
            REDO_PURPOSE["<b>目的</b><br/>崩溃恢复<br/>持久性保证"]
            REDO_LEVEL["<b>层次</b><br/>InnoDB存储引擎层"]
            REDO_FORMAT["<b>格式</b><br/>物理日志<br/>页面修改"]
            REDO_SIZE["<b>大小</b><br/>循环使用<br/>默认100MB"]
        end
        
        subgraph "<b>Undo Log</b>"
            UNDO_PURPOSE["<b>目的</b><br/>事务回滚<br/>MVCC"]
            UNDO_LEVEL["<b>层次</b><br/>InnoDB存储引擎层"]
            UNDO_FORMAT["<b>格式</b><br/>逻辑日志<br/>反向操作"]
            UNDO_SIZE["<b>大小</b><br/>按需分配<br/>purge清理"]
        end
        
        subgraph "<b>Binlog</b>"
            BINLOG_PURPOSE["<b>目的</b><br/>复制<br/>PITR"]
            BINLOG_LEVEL["<b>层次</b><br/>MySQL Server层"]
            BINLOG_FORMAT["<b>格式</b><br/>逻辑日志<br/>SQL/ROW/MIXED"]
            BINLOG_SIZE["<b>大小</b><br/>持续增长<br/>手动清理"]
        end
    end
    
    REDO_PURPOSE --> REDO_LEVEL
    REDO_LEVEL --> REDO_FORMAT
    REDO_FORMAT --> REDO_SIZE
    
    UNDO_PURPOSE --> UNDO_LEVEL
    UNDO_LEVEL --> UNDO_FORMAT
    UNDO_FORMAT --> UNDO_SIZE
    
    BINLOG_PURPOSE --> BINLOG_LEVEL
    BINLOG_LEVEL --> BINLOG_FORMAT
    BINLOG_FORMAT --> BINLOG_SIZE
    
    style REDO_PURPOSE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style UNDO_PURPOSE fill:#fff3e0,stroke:#333,stroke-width:2px
    style BINLOG_PURPOSE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

| 特性 | Redo Log | Undo Log | Binlog |
|-----|---------|---------|--------|
| **层次** | InnoDB引擎 | InnoDB引擎 | MySQL Server |
| **目的** | 崩溃恢复、持久性 | 事务回滚、MVCC | 复制、PITR |
| **格式** | 物理（页面修改） | 逻辑（反向操作） | 逻辑（SQL/ROW） |
| **写入时机** | 事务执行中 | 事务执行中 | 事务提交时 |
| **大小管理** | 循环覆盖 | 按需分配+purge | 持续增长 |
| **块/页大小** | 512字节block | 16KB页面 | Event可变 |
| **校验和** | CRC32（block级） | CRC32（页级） | CRC32（event级） |

### 4.2 事务提交的两阶段提交（2PC）

```mermaid
sequenceDiagram
    participant APP as **应用**
    participant MYSQL as **MySQL Server**
    participant INNODB as **InnoDB引擎**
    participant REDO as **Redo Log**
    participant BINLOG as **Binlog**

    Note over APP,BINLOG: **事务执行阶段**
    
    APP->>MYSQL: BEGIN
    APP->>MYSQL: UPDATE table SET col=new
    MYSQL->>INNODB: 修改Buffer Pool中的页
    INNODB->>REDO: 写入redo log buffer<br/>记录物理修改
    INNODB->>INNODB: 写入undo log<br/>记录旧值
    MYSQL->>MYSQL: 写入binlog cache<br/>记录逻辑修改
    
    Note over APP,BINLOG: **两阶段提交（2PC）**
    
    APP->>MYSQL: COMMIT
    
    Note over INNODB,REDO: **Prepare阶段**
    MYSQL->>INNODB: prepare()
    INNODB->>REDO: 写入Prepare标记的redo<br/>XID_EVENT标记
    INNODB->>REDO: fsync redo log<br/>根据innodb_flush_log_at_trx_commit
    INNODB-->>MYSQL: Prepare完成
    
    Note over MYSQL,BINLOG: **Commit阶段**
    MYSQL->>BINLOG: 写入binlog（Group Commit）
    BINLOG->>BINLOG: fsync binlog<br/>根据sync_binlog
    
    MYSQL->>INNODB: commit()
    INNODB->>INNODB: 写入Commit标记<br/>释放锁
    INNODB->>INNODB: undo log标记提交<br/>加入history list
    
    MYSQL-->>APP: 返回成功
    
    Note over APP,BINLOG: **崩溃恢复**
    alt 崩溃在Prepare后、Binlog前
        INNODB->>INNODB: 回滚该事务
    else 崩溃在Binlog后、Commit前
        INNODB->>BINLOG: 检查XID在binlog中
        BINLOG-->>INNODB: XID存在
        INNODB->>INNODB: 重新提交事务
    end
```

**两阶段提交保证**:

- **原子性**: Redo+Undo共同保证单事务原子性
- **一致性**: Binlog与Redo保持一致，保证主从一致
- **持久性**: 双重保证（Redo Log + Binlog）
- **隔离性**: MVCC（Undo）+ 锁机制

---

## 第五部分：性能优化与监控

### 5.1 关键性能参数

**Redo Log优化**:

```sql
-- 查看redo log使用情况
SHOW ENGINE INNODB STATUS\G
-- 关注：Log sequence number, Log flushed up to

-- 关键参数
SET GLOBAL innodb_log_buffer_size = 67108864;  -- 64MB
SET GLOBAL innodb_flush_log_at_trx_commit = 2; -- 性能优先
SET GLOBAL innodb_log_write_ahead_size = 16384; -- 16KB
```

**Undo Log优化**:

```sql
-- 查看undo使用情况
SELECT 
    tablespace_name,
    file_name,
    file_size/1024/1024 AS size_mb
FROM information_schema.FILES
WHERE file_type LIKE '%UNDO%';

-- 查看history list长度
SHOW ENGINE INNODB STATUS\G
-- 关注：History list length

-- 关键参数
SET GLOBAL innodb_undo_tablespaces = 4;        -- undo表空间数量
SET GLOBAL innodb_max_undo_log_size = 1073741824; -- 1GB
SET GLOBAL innodb_purge_threads = 4;           -- purge线程数
SET GLOBAL innodb_purge_batch_size = 300;      -- 批量purge大小
```

**Binlog优化**:

```sql
-- 查看binlog使用情况
SHOW BINARY LOGS;
SHOW MASTER STATUS;

-- 关键参数
SET GLOBAL binlog_cache_size = 131072;         -- 128KB
SET GLOBAL sync_binlog = 100;                  -- 性能优先
SET GLOBAL binlog_group_commit_sync_delay = 1000; -- 1ms
SET GLOBAL binlog_group_commit_sync_no_delay_count = 100;
SET GLOBAL max_binlog_size = 1073741824;       -- 1GB
```

### 5.2 监控指标

```sql
-- Redo Log监控
SELECT 
    'Redo Log' AS component,
    variable_name,
    variable_value
FROM performance_schema.global_status
WHERE variable_name IN (
    'Innodb_redo_log_enabled',
    'Innodb_redo_log_resize_status'
);

-- Undo Log监控
SELECT 
    NAME,
    SUBSYSTEM,
    COUNT
FROM information_schema.INNODB_METRICS
WHERE NAME LIKE '%undo%' OR NAME LIKE '%purge%';

-- Binlog监控
SHOW GLOBAL STATUS LIKE 'Binlog%';
-- 关注：
-- Binlog_cache_disk_use: 使用临时文件次数
-- Binlog_cache_use: 使用cache次数
```

---

## 总结

### 核心要点回顾

**Redo Log**:

- 固定大小，循环使用
- 512字节Block结构
- 物理日志，记录"在某页某偏移写入什么"
- WAL机制，先写日志后写数据
- LSN全局递增，追踪写入进度

**Undo Log**:

- 存储在undo表空间中
- 按需分配，purge线程清理
- 逻辑日志，记录反向操作
- 支持MVCC和事务回滚
- History list维护已提交但未purge的undo

**Binlog**:

- Server层日志，与存储引擎无关
- Event-based结构
- 支持STATEMENT/ROW/MIXED格式
- Group Commit优化提交性能
- 用于主从复制和Point-in-Time恢复

**三者协作**:

- **两阶段提交**保证Redo和Binlog一致
- **XID**作为桥梁关联InnoDB事务和Binlog事务
- **崩溃恢复**时根据Prepare/Commit状态和Binlog决定提交或回滚

这种精巧的日志系统设计，是MySQL保证ACID特性和高可用的基石。
