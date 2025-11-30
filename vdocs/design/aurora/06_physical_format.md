# 物理文件格式设计文档

## 1. 文件类型

| 文件类型 | 扩展名 | 说明 | 位置 |
|----------|--------|------|------|
| WAL 文件 | `.wal` | Redo Log 持久化 | 存储层 |
| Segment 文件 | `.seg` | Page 数据存储 | 存储层 |
| Index 文件 | `.idx` | LSN 索引 | 存储层 |
| Manifest | `.json` | 元数据清单 | 存储层 |

---

## 2. WAL 文件格式

### 2.1 文件结构

```
+----------------------------------------------------------+
|                   WAL File Header (64 bytes)              |
+----------------------------------------------------------+
|                   Redo Record 1                           |
+----------------------------------------------------------+
|                   Redo Record 2                           |
+----------------------------------------------------------+
|                          ...                              |
+----------------------------------------------------------+
|                   Redo Record N                           |
+----------------------------------------------------------+
|                   File Footer (32 bytes)                  |
+----------------------------------------------------------+
```

### 2.2 WAL Header（64 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 4 | magic | uint32 | 魔数 `0x57414C31` ("WAL1") |
| 4 | 4 | version | uint32 | 版本号 (1) |
| 8 | 8 | file_id | uint64 | 文件 ID |
| 16 | 16 | volume_id | [16]byte | Volume UUID |
| 32 | 8 | start_lsn | uint64 | 起始 LSN |
| 40 | 8 | end_lsn | uint64 | 结束 LSN（初始为 0） |
| 48 | 4 | record_count | uint32 | 记录数量 |
| 52 | 4 | flags | uint32 | 标志位 |
| 56 | 4 | checksum | uint32 | Header 校验和 (CRC32) |
| 60 | 4 | reserved | uint32 | 保留 |

```go
type WALHeader struct {
    Magic       uint32    // 0x57414C31
    Version     uint32    // 1
    FileID      uint64
    VolumeID    [16]byte
    StartLSN    uint64
    EndLSN      uint64
    RecordCount uint32
    Flags       uint32
    Checksum    uint32
    Reserved    uint32
}
```

### 2.3 Redo Record（变长）

```
+----------------------------------------------------------+
|                 Record Header (48 bytes)                  |
+----------------------------------------------------------+
|                 Record Data (变长)                        |
+----------------------------------------------------------+
|                 Record Checksum (4 bytes)                 |
+----------------------------------------------------------+
```

#### Record Header（48 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 8 | lsn | uint64 | 日志序列号 |
| 8 | 8 | space_id | uint64 | 表空间 ID |
| 16 | 8 | page_id | uint64 | 页面 ID |
| 24 | 8 | trx_id | uint64 | 事务 ID |
| 32 | 8 | mtr_id | uint64 | MTR ID |
| 40 | 2 | type | uint16 | Redo 类型 |
| 42 | 2 | flags | uint16 | 标志位 |
| 44 | 4 | data_len | uint32 | 数据长度 |

```go
type RedoRecordHeader struct {
    LSN      uint64
    SpaceID  uint64
    PageID   uint64
    TrxID    uint64
    MtrID    uint64
    Type     uint16
    Flags    uint16
    DataLen  uint32
}

const (
    RedoTypeInsert      uint16 = 1
    RedoTypeUpdate      uint16 = 2
    RedoTypeDelete      uint16 = 3
    RedoTypePageCreate  uint16 = 4
    RedoTypePageInit    uint16 = 5
    RedoTypeTrxCommit   uint16 = 6
    RedoTypeTrxRollback uint16 = 7
    RedoTypeDDL         uint16 = 8
    RedoTypeCheckpoint  uint16 = 9
    RedoTypeMtrCommit   uint16 = 10
)

const (
    FlagMtrStart uint16 = 0x01
    FlagMtrEnd   uint16 = 0x02
    FlagSync     uint16 = 0x04
)
```

#### Record Data 格式（按类型）

**INSERT 类型：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | slot_no | 槽位号 |
| 2 | 2 | rec_len | 记录长度 |
| 4 | N | rec_data | 记录数据 |

**UPDATE 类型：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | slot_no | 槽位号 |
| 2 | 2 | offset | 修改偏移 |
| 4 | 2 | old_len | 原数据长度 |
| 6 | 2 | new_len | 新数据长度 |
| 8 | M | old_data | 原数据 |
| 8+M | N | new_data | 新数据 |

**DELETE 类型：**

| 偏移 | 大小 | 字段 | 说明 |
|------|------|------|------|
| 0 | 2 | slot_no | 槽位号 |
| 2 | 2 | rec_len | 记录长度 |
| 4 | N | rec_data | 被删除记录 |

### 2.4 WAL Footer（32 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 4 | magic | uint32 | 结束魔数 `0x454E4431` |
| 4 | 8 | end_lsn | uint64 | 结束 LSN |
| 12 | 4 | record_count | uint32 | 记录数量 |
| 16 | 8 | file_size | uint64 | 文件大小 |
| 24 | 4 | checksum | uint32 | 文件校验和 |
| 28 | 4 | reserved | uint32 | 保留 |

---

## 3. Page Segment 文件格式

### 3.1 文件结构

```
+----------------------------------------------------------+
|                 Segment Header (128 bytes)                |
+----------------------------------------------------------+
|                 Page 0 (16KB)                             |
+----------------------------------------------------------+
|                 Page 1 (16KB)                             |
+----------------------------------------------------------+
|                          ...                              |
+----------------------------------------------------------+
|                 Page N (16KB)                             |
+----------------------------------------------------------+
|                 Segment Footer (64 bytes)                 |
+----------------------------------------------------------+
```

### 3.2 Segment Header（128 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 4 | magic | uint32 | 魔数 `0x53454731` |
| 4 | 4 | version | uint32 | 版本号 |
| 8 | 8 | segment_id | uint64 | Segment ID |
| 16 | 16 | volume_id | [16]byte | Volume UUID |
| 32 | 8 | creation_time | uint64 | 创建时间 |
| 40 | 8 | page_count | uint64 | 页面数量 |
| 48 | 8 | page_size | uint64 | 页面大小 (16384) |
| 56 | 8 | min_lsn | uint64 | 最小 LSN |
| 64 | 8 | max_lsn | uint64 | 最大 LSN |
| 72 | 4 | flags | uint32 | 标志位 |
| 76 | 4 | checksum | uint32 | Header 校验和 |
| 80 | 48 | reserved | [48]byte | 保留 |

### 3.3 Page 格式（16KB = 16384 字节）

#### Page Header（56 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 8 | space_id | uint64 | 表空间 ID |
| 8 | 8 | page_id | uint64 | 页面 ID |
| 16 | 8 | page_lsn | uint64 | 页面 LSN |
| 24 | 4 | page_type | uint32 | 页面类型 |
| 28 | 4 | checksum | uint32 | 页面校验和 |
| 32 | 2 | record_count | uint16 | 记录数量 |
| 34 | 2 | free_space | uint16 | 可用空间 |
| 36 | 2 | heap_top | uint16 | 堆顶偏移 |
| 38 | 2 | slot_count | uint16 | 槽位数量 |
| 40 | 8 | trx_id | uint64 | 最后修改事务 |
| 48 | 8 | prev_page | uint64 | 前一页 |

```go
const (
    PageTypeData     uint32 = 1   // 数据页
    PageTypeIndex    uint32 = 2   // 索引页
    PageTypeFSP      uint32 = 3   // 文件空间页
    PageTypeXDES     uint32 = 4   // 扩展描述页
    PageTypeBlob     uint32 = 5   // BLOB 页
    PageTypeUndo     uint32 = 6   // Undo 页
    PageTypeSysTable uint32 = 7   // 系统表页
)
```

#### Page Body（16272 字节）

```
+----------------------------------------------------------+
| Page Header (56 bytes)                                    |
+----------------------------------------------------------+
| Record Area (从低地址向高地址增长)                          |
|   Record 1                                                |
|   Record 2                                                |
|   ...                                                     |
+----------------------------------------------------------+
| Free Space                                                |
+----------------------------------------------------------+
| Slot Array (从高地址向低地址增长)                          |
|   Slot N  (2 bytes each)                                  |
|   ...                                                     |
|   Slot 1                                                  |
|   Slot 0                                                  |
+----------------------------------------------------------+
| Page Trailer (8 bytes)                                    |
+----------------------------------------------------------+
```

#### Page Trailer（8 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 4 | old_checksum | uint32 | 旧校验和 |
| 4 | 4 | lsn_low | uint32 | LSN 低 32 位 |

---

## 4. LSN Index 文件格式

### 4.1 文件结构

```
+----------------------------------------------------------+
|                 Index Header (64 bytes)                   |
+----------------------------------------------------------+
|                 Index Entry 1 (24 bytes)                  |
+----------------------------------------------------------+
|                 Index Entry 2 (24 bytes)                  |
+----------------------------------------------------------+
|                          ...                              |
+----------------------------------------------------------+
```

### 4.2 Index Header（64 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 4 | magic | uint32 | 魔数 `0x49445831` |
| 4 | 4 | version | uint32 | 版本号 |
| 8 | 8 | entry_count | uint64 | 条目数量 |
| 16 | 8 | min_lsn | uint64 | 最小 LSN |
| 24 | 8 | max_lsn | uint64 | 最大 LSN |
| 32 | 8 | granularity | uint64 | 索引粒度 |
| 40 | 4 | checksum | uint32 | 校验和 |
| 44 | 20 | reserved | [20]byte | 保留 |

### 4.3 Index Entry（24 字节）

| 偏移 | 大小 | 字段 | 类型 | 说明 |
|------|------|------|------|------|
| 0 | 8 | lsn | uint64 | LSN |
| 8 | 8 | file_id | uint64 | WAL 文件 ID |
| 16 | 8 | offset | uint64 | 文件内偏移 |

```go
type LSNIndexEntry struct {
    LSN     uint64
    FileID  uint64
    Offset  uint64
}
```

---

## 5. Manifest 文件格式

### 5.1 JSON 结构

```json
{
    "magic": "AURORA_MANIFEST_V1",
    "version": 1,
    "volume_id": "550e8400-e29b-41d4-a716-446655440000",
    "node_id": "storage-node-1",
    "created_at": "2025-01-01T00:00:00Z",
    "updated_at": "2025-01-01T12:00:00Z",
    
    "wal_files": [
        {
            "file_id": 1,
            "file_name": "wal_000001.wal",
            "start_lsn": 0,
            "end_lsn": 100000,
            "size_bytes": 134217728,
            "record_count": 50000,
            "status": "sealed"
        },
        {
            "file_id": 2,
            "file_name": "wal_000002.wal",
            "start_lsn": 100001,
            "end_lsn": 0,
            "size_bytes": 52428800,
            "record_count": 25000,
            "status": "active"
        }
    ],
    
    "segment_files": [
        {
            "segment_id": 1,
            "file_name": "segment_000001.seg",
            "page_count": 65536,
            "size_bytes": 1073741824,
            "min_lsn": 0,
            "max_lsn": 50000
        }
    ],
    
    "index_files": [
        {
            "file_name": "lsn_index.idx",
            "entry_count": 100000,
            "min_lsn": 0,
            "max_lsn": 100000
        }
    ],
    
    "stats": {
        "current_lsn": 125000,
        "total_redo_bytes": 186646528,
        "total_page_bytes": 1073741824,
        "total_pages": 65536
    }
}
```

---

## 6. 目录结构

```
/data/aurora/storage/{volume_id}/
├── manifest.json           # 元数据清单
├── wal/
│   ├── wal_000001.wal      # WAL 文件
│   ├── wal_000002.wal
│   └── ...
├── segment/
│   ├── segment_000001.seg  # Page Segment 文件
│   └── ...
└── index/
    └── lsn_index.idx       # LSN 索引文件
```

---

## 7. 文件命名规范

| 文件类型 | 命名格式 | 示例 |
|----------|----------|------|
| WAL 文件 | `wal_{file_id:06d}.wal` | `wal_000001.wal` |
| Segment 文件 | `segment_{seg_id:06d}.seg` | `segment_000001.seg` |
| Index 文件 | `lsn_index.idx` | `lsn_index.idx` |
| Manifest | `manifest.json` | `manifest.json` |

---

## 8. 字节序和校验

### 8.1 规范

- **字节序**：小端序（Little-Endian）
- **对齐**：8 字节对齐
- **校验和**：CRC32（IEEE 多项式）
- **UUID**：RFC 4122 格式

### 8.2 CRC32 计算

```go
import "hash/crc32"

func calculateChecksum(data []byte) uint32 {
    return crc32.ChecksumIEEE(data)
}

func verifyChecksum(data []byte, expected uint32) bool {
    return calculateChecksum(data) == expected
}
```
