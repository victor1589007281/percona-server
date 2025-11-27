# MySQL Binlog Parser - Go Implementation Guide

## Overview
This document provides a comprehensive guide for implementing a MySQL binlog parser in Go. It covers all event types supported by Percona Server based on the current codebase.

## Binlog Event Structure

Every binlog event consists of four parts:
1. **Common Header** (19 bytes)
2. **Post-Header** (variable length, event-specific)
3. **Body** (variable length, event-specific)
4. **Footer** (4 bytes checksum, optional)

### Common Header Format (19 bytes)

| Field | Size | Description |
|-------|------|-------------|
| timestamp | 4 bytes | Unix timestamp when event was created |
| type_code | 1 byte | Event type (see Log_event_type enum) |
| server_id | 4 bytes | Server ID of the originating server |
| event_length | 4 bytes | Total size of event (header + post-header + body + footer) |
| next_position | 4 bytes | Position of next event |
| flags | 2 bytes | Event flags (16 bits) |

### Common Footer Format

| Field | Size | Description |
|-------|------|-------------|
| checksum | 4 bytes | CRC32 checksum (if enabled) |

## Complete Event Type Reference

### Event Type Enum (Log_event_type)

```go
const (
    UNKNOWN_EVENT                  = 0
    START_EVENT_V3                 = 1  // Deprecated since MySQL 8.0.2
    QUERY_EVENT                    = 2
    STOP_EVENT                     = 3
    ROTATE_EVENT                   = 4
    INTVAR_EVENT                   = 5
    // 6 - not used
    SLAVE_EVENT                    = 7
    // 8 - not used
    APPEND_BLOCK_EVENT            = 9
    // 10 - not used
    DELETE_FILE_EVENT             = 11
    // 12 - not used
    RAND_EVENT                    = 13
    USER_VAR_EVENT                = 14
    FORMAT_DESCRIPTION_EVENT      = 15
    XID_EVENT                     = 16
    BEGIN_LOAD_QUERY_EVENT        = 17
    EXECUTE_LOAD_QUERY_EVENT      = 18
    TABLE_MAP_EVENT               = 19
    // 20-22 - not used
    OBSOLETE_WRITE_ROWS_EVENT_V1  = 23  // Obsolete since 8.2.0
    OBSOLETE_UPDATE_ROWS_EVENT_V1 = 24  // Obsolete since 8.2.0
    OBSOLETE_DELETE_ROWS_EVENT_V1 = 25  // Obsolete since 8.2.0
    INCIDENT_EVENT                = 26
    HEARTBEAT_LOG_EVENT           = 27
    IGNORABLE_LOG_EVENT           = 28
    ROWS_QUERY_LOG_EVENT          = 29
    WRITE_ROWS_EVENT              = 30  // V2
    UPDATE_ROWS_EVENT             = 31  // V2
    DELETE_ROWS_EVENT             = 32  // V2
    GTID_LOG_EVENT                = 33
    ANONYMOUS_GTID_LOG_EVENT      = 34
    PREVIOUS_GTIDS_LOG_EVENT      = 35
    TRANSACTION_CONTEXT_EVENT     = 36
    VIEW_CHANGE_EVENT             = 37
    XA_PREPARE_LOG_EVENT          = 38
    PARTIAL_UPDATE_ROWS_EVENT     = 39
    TRANSACTION_PAYLOAD_EVENT     = 40
    HEARTBEAT_LOG_EVENT_V2        = 41
    GTID_TAGGED_LOG_EVENT         = 42
    // Future events to be added above
    MYSQL_END_EVENT               // Marker for MySQL events boundary
    ENUM_END_EVENT                // End marker
)
```

## Event Detailed Specifications

### 1. FORMAT_DESCRIPTION_EVENT (15)
**Description**: First event in every binlog file, describes the format of the binlog.

**Post-Header Length**: START_V3_HEADER_LEN + 1 + LOG_EVENT_TYPES bytes

**Structure**:
```go
type FormatDescriptionEvent struct {
    BinlogVersion      uint16  // 2 bytes - always 4
    ServerVersion      [50]byte // 50 bytes - MySQL server version string
    CreateTimestamp    uint32  // 4 bytes - binlog file creation time
    CommonHeaderLength uint8   // 1 byte - usually 19
    PostHeaderLengths  []uint8 // Variable - array of post-header lengths for each event type
}
```

### 2. QUERY_EVENT (2)
**Description**: Contains SQL statement that was executed.

**Post-Header Length**: 13 bytes (QUERY_HEADER_LEN)

**Structure**:
```go
type QueryEvent struct {
    ThreadID       uint32 // 4 bytes - thread that executed
    ExecTime       uint32 // 4 bytes - execution time in seconds
    DbNameLength   uint8  // 1 byte - database name length
    ErrorCode      uint16 // 2 bytes - error code
    StatusVarsLen  uint16 // 2 bytes - status variables length
    StatusVars     []byte // Variable - status variables
    DbName         string // Variable - database name (null-terminated)
    Query          string // Variable - SQL query
}
```

### 3. STOP_EVENT (3)
**Description**: Written when mysqld stops normally.

**Post-Header Length**: 0 bytes

**Structure**: No additional data beyond common header.

### 4. ROTATE_EVENT (4)
**Description**: Points to the next binlog file in the sequence.

**Post-Header Length**: 8 bytes (ROTATE_HEADER_LEN)

**Structure**:
```go
type RotateEvent struct {
    Position      uint64 // 8 bytes - position in next binlog
    NextBinlogName string // Variable - name of next binlog file
}
```

### 5. INTVAR_EVENT (5)
**Description**: Contains the value of an auto_increment or last_insert_id variable.

**Post-Header Length**: 0 bytes

**Structure**:
```go
type IntvarEvent struct {
    Type  uint8  // 1 byte - LAST_INSERT_ID_EVENT=1 or INSERT_ID_EVENT=2
    Value uint64 // 8 bytes - the value
}
```

### 6. SLAVE_EVENT (7)
**Description**: Deprecated event type, ignored by modern servers.

**Post-Header Length**: 0 bytes

### 7. APPEND_BLOCK_EVENT (9)
**Description**: Used with LOAD DATA INFILE to append data to temporary file.

**Post-Header Length**: 4 bytes (APPEND_BLOCK_HEADER_LEN)

**Structure**:
```go
type AppendBlockEvent struct {
    FileID uint32 // 4 bytes - file identifier
    Data   []byte // Variable - block data
}
```

### 8. DELETE_FILE_EVENT (11)
**Description**: Used with LOAD DATA INFILE to delete temporary file.

**Post-Header Length**: 4 bytes (DELETE_FILE_HEADER_LEN)

**Structure**:
```go
type DeleteFileEvent struct {
    FileID uint32 // 4 bytes - file identifier to delete
}
```

### 9. RAND_EVENT (13)
**Description**: Contains RAND() seed values.

**Post-Header Length**: 0 bytes

**Structure**:
```go
type RandEvent struct {
    Seed1 uint64 // 8 bytes - first seed value
    Seed2 uint64 // 8 bytes - second seed value
}
```

### 10. USER_VAR_EVENT (14)
**Description**: Contains user variable assignment.

**Post-Header Length**: 0 bytes

**Structure**:
```go
type UserVarEvent struct {
    NameLength uint32 // 4 bytes - variable name length
    Name       string // Variable - variable name
    IsNull     uint8  // 1 byte - 0=not null, 1=null
    Type       uint8  // 1 byte - variable type
    Charset    uint32 // 4 bytes - character set
    ValueLen   uint32 // 4 bytes - value length
    Value      []byte // Variable - value data
    Flags      uint8  // 1 byte - flags
}
```

### 11. XID_EVENT (16)
**Description**: Marks commit of a transaction.

**Post-Header Length**: 0 bytes

**Structure**:
```go
type XidEvent struct {
    Xid uint64 // 8 bytes - transaction ID
}
```

### 12. BEGIN_LOAD_QUERY_EVENT (17)
**Description**: Begin a LOAD DATA INFILE operation.

**Post-Header Length**: 4 bytes (BEGIN_LOAD_QUERY_HEADER_LEN)

**Structure**:
```go
type BeginLoadQueryEvent struct {
    FileID uint32 // 4 bytes - file identifier
    Data   []byte // Variable - block data
}
```

### 13. EXECUTE_LOAD_QUERY_EVENT (18)
**Description**: Execute a LOAD DATA INFILE operation.

**Post-Header Length**: 26 bytes (EXECUTE_LOAD_QUERY_HEADER_LEN)

**Structure**:
```go
type ExecuteLoadQueryEvent struct {
    ThreadID       uint32 // 4 bytes
    ExecTime       uint32 // 4 bytes
    DbNameLength   uint8  // 1 byte
    ErrorCode      uint16 // 2 bytes
    StatusVarsLen  uint16 // 2 bytes
    FileID         uint32 // 4 bytes
    StartPos       uint32 // 4 bytes
    EndPos         uint32 // 4 bytes
    DupHandling    uint8  // 1 byte
    StatusVars     []byte // Variable
    DbName         string // Variable
    Query          string // Variable
}
```

### 14. TABLE_MAP_EVENT (19)
**Description**: Maps table ID to database and table name (used before row events).

**Post-Header Length**: 8 bytes (TABLE_MAP_HEADER_LEN)

**Structure**:
```go
type TableMapEvent struct {
    TableID       uint64 // 6 bytes - table ID
    Flags         uint16 // 2 bytes - flags
    DbNameLength  uint8  // 1 byte
    DbName        string // Variable (null-terminated)
    TableNameLen  uint8  // 1 byte
    TableName     string // Variable (null-terminated)
    ColumnCount   uint64 // Packed integer
    ColumnTypes   []byte // Variable - one byte per column
    ColumnMeta    []byte // Variable - metadata for each column
    NullBits      []byte // Variable - null bitmap
    // Optional metadata fields (if present)
}
```

### 15-17. OBSOLETE Row Events V1 (23-25)
**Note**: These events (WRITE_ROWS_V1, UPDATE_ROWS_V1, DELETE_ROWS_V1) are obsolete since MySQL 8.2.0 and rejected by applier since 8.4.0. Use V2 events (30-32) instead.

### 18. INCIDENT_EVENT (26)
**Description**: Indicates that something went wrong on the master.

**Post-Header Length**: 2 bytes (INCIDENT_HEADER_LEN)

**Structure**:
```go
type IncidentEvent struct {
    IncidentType uint16 // 2 bytes - type of incident
    MessageLen   uint8  // 1 byte - message length
    Message      string // Variable - incident message
}
```

### 19. HEARTBEAT_LOG_EVENT (27)
**Description**: Sent by master to slave to ensure connection is alive.

**Post-Header Length**: 0 bytes (HEARTBEAT_HEADER_LEN)

**Structure**:
```go
type HeartbeatEvent struct {
    LogIdent string // Variable - binlog filename
}
```

### 20. IGNORABLE_LOG_EVENT (28)
**Description**: Event that can be safely ignored if not recognized.

**Post-Header Length**: 0 bytes (IGNORABLE_HEADER_LEN)

### 21. ROWS_QUERY_LOG_EVENT (29)
**Description**: Contains the original query that caused row events (when binlog_rows_query_log_events=ON).

**Structure**:
```go
type RowsQueryEvent struct {
    QueryLength uint8  // 1 byte - length indicator
    Query       string // Variable - the SQL query
}
```

### 22. WRITE_ROWS_EVENT (30)
**Description**: Row insert event (version 2).

**Post-Header Length**: 10 bytes (ROWS_HEADER_LEN_V2)

**Structure**:
```go
type WriteRowsEvent struct {
    TableID     uint64 // 6 bytes - table ID (from TABLE_MAP_EVENT)
    Flags       uint16 // 2 bytes - flags
    ExtraData   []byte // Variable - extra row data (length encoded)
    ColumnCount uint64 // Packed integer
    Columns     []byte // Variable - bitmap of columns present
    Rows        [][]interface{} // Variable - row data
}
```

### 23. UPDATE_ROWS_EVENT (31)
**Description**: Row update event (version 2).

**Post-Header Length**: 10 bytes (ROWS_HEADER_LEN_V2)

**Structure**:
```go
type UpdateRowsEvent struct {
    TableID     uint64 // 6 bytes - table ID
    Flags       uint16 // 2 bytes - flags
    ExtraData   []byte // Variable - extra row data
    ColumnCount uint64 // Packed integer
    ColumnsBefore []byte // Variable - bitmap of columns in before image
    ColumnsAfter  []byte // Variable - bitmap of columns in after image
    Rows        [][2][]interface{} // Variable - pairs of before/after row data
}
```

### 24. DELETE_ROWS_EVENT (32)
**Description**: Row delete event (version 2).

**Post-Header Length**: 10 bytes (ROWS_HEADER_LEN_V2)

**Structure**:
```go
type DeleteRowsEvent struct {
    TableID     uint64 // 6 bytes - table ID
    Flags       uint16 // 2 bytes - flags
    ExtraData   []byte // Variable - extra row data
    ColumnCount uint64 // Packed integer
    Columns     []byte // Variable - bitmap of columns present
    Rows        [][]interface{} // Variable - row data
}
```

### 25. GTID_LOG_EVENT (33)
**Description**: Contains GTID (Global Transaction Identifier) for a transaction.

**Structure**:
```go
type GtidEvent struct {
    CommitFlag          uint8    // 1 byte - commit flag
    SID                 [16]byte // 16 bytes - source UUID
    GNO                 uint64   // 8 bytes - group number
    LastCommitted       uint64   // 8 bytes - logical timestamp
    SequenceNumber      uint64   // 8 bytes - sequence number
    ImmediateCommitTS   uint64   // 7 bytes - immediate commit timestamp (optional)
    OriginalCommitTS    uint64   // 7 bytes - original commit timestamp (optional)
}
```

### 26. ANONYMOUS_GTID_LOG_EVENT (34)
**Description**: Similar to GTID event but for transactions without GTID.

**Structure**:
```go
type AnonymousGtidEvent struct {
    CommitFlag        uint8  // 1 byte - commit flag
    LastCommitted     uint64 // 8 bytes - logical timestamp
    SequenceNumber    uint64 // 8 bytes - sequence number
}
```

### 27. PREVIOUS_GTIDS_LOG_EVENT (35)
**Description**: Contains set of all GTIDs in all previous binlog files.

**Structure**:
```go
type PreviousGtidsEvent struct {
    GtidSet []byte // Variable - encoded GTID set
}
```

### 28. TRANSACTION_CONTEXT_EVENT (36)
**Description**: Contains transaction context information for Group Replication.

**Post-Header Length**: 18 bytes (TRANSACTION_CONTEXT_HEADER_LEN)

**Structure**:
```go
type TransactionContextEvent struct {
    ServerUUID         [16]byte // 16 bytes - server UUID
    ThreadID           uint32   // 4 bytes - thread ID
    GtidSpecified      bool     // 1 byte - GTID specified flag
    EncodedSnapshotVersion []byte // Variable
    WriteSetSize       uint32   // 4 bytes
    WriteSets          []string // Variable
    ReadSetSize        uint32   // 4 bytes
    ReadSets           []string // Variable
}
```

### 29. VIEW_CHANGE_EVENT (37)
**Description**: Group Replication view change event.

**Post-Header Length**: 52 bytes (VIEW_CHANGE_HEADER_LEN)

**Structure**:
```go
type ViewChangeEvent struct {
    ViewID             [40]byte // 40 bytes - view ID
    SEQ_NUMBER         uint64   // 8 bytes - sequence number
    CertificationInfo  []byte   // Variable
}
```

### 30. XA_PREPARE_LOG_EVENT (38)
**Description**: Prepared XA transaction event.

**Post-Header Length**: 0 bytes (XA_PREPARE_HEADER_LEN)

**Structure**:
```go
type XaPrepareEvent struct {
    OnePhaseTrx  bool   // 1 byte - one phase transaction flag
    FormatID     uint32 // 4 bytes - XA format ID
    GtridLength  uint32 // 4 bytes - global transaction ID length
    BqualLength  uint32 // 4 bytes - branch qualifier length
    Xid          []byte // Variable - XID data (gtrid + bqual)
}
```

### 31. PARTIAL_UPDATE_ROWS_EVENT (39)
**Description**: Extension of UPDATE_ROWS_EVENT with partial column updates (JSON partial updates).

**Post-Header Length**: 10 bytes (ROWS_HEADER_LEN_V2)

**Structure**:
```go
type PartialUpdateRowsEvent struct {
    TableID     uint64 // 6 bytes - table ID
    Flags       uint16 // 2 bytes - flags
    ExtraData   []byte // Variable - extra row data
    ColumnCount uint64 // Packed integer
    ColumnsBefore []byte // Variable - bitmap
    ColumnsAfter  []byte // Variable - bitmap  
    Rows        []PartialUpdateRow // Variable - with JSON diff info
}
```

### 32. TRANSACTION_PAYLOAD_EVENT (40)
**Description**: Compressed transaction payload containing multiple events.

**Post-Header Length**: 0 bytes (TRANSACTION_PAYLOAD_HEADER_LEN)

**Structure**:
```go
type TransactionPayloadEvent struct {
    PayloadSize       uint64 // Packed integer - compressed size
    CompressionType   uint8  // 1 byte - compression algorithm (0=NONE, 1=ZSTD)
    UncompressedSize  uint64 // Packed integer - original size
    Payload           []byte // Variable - compressed event data
}
```

### 33. HEARTBEAT_LOG_EVENT_V2 (41)
**Description**: Enhanced heartbeat event (version 2).

**Post-Header Length**: 0 bytes

**Structure**:
```go
type HeartbeatEventV2 struct {
    LogIdent string // Variable - binlog filename
}
```

### 34. GTID_TAGGED_LOG_EVENT (42)
**Description**: GTID event with additional tags/metadata.

**Structure**:
```go
type GtidTaggedEvent struct {
    CommitFlag          uint8    // 1 byte - commit flag
    SID                 [16]byte // 16 bytes - source UUID
    GNO                 uint64   // 8 bytes - group number
    LastCommitted       uint64   // 8 bytes
    SequenceNumber      uint64   // 8 bytes
    ImmediateCommitTS   uint64   // 7 bytes (optional)
    OriginalCommitTS    uint64   // 7 bytes (optional)
    Tag                 string   // Variable - GTID tag
}
```

## Helper Functions

### Packed Integer Decoding

```go
func ReadPackedInteger(data []byte) (value uint64, bytesRead int, err error) {
    if len(data) == 0 {
        return 0, 0, fmt.Errorf("empty data")
    }
    
    first := data[0]
    
    if first < 251 {
        return uint64(first), 1, nil
    } else if first == 252 {
        if len(data) < 3 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        return uint64(binary.LittleEndian.Uint16(data[1:3])), 3, nil
    } else if first == 253 {
        if len(data) < 4 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        v := uint64(data[1]) | uint64(data[2])<<8 | uint64(data[3])<<16
        return v, 4, nil
    } else if first == 254 {
        if len(data) < 9 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        return binary.LittleEndian.Uint64(data[1:9]), 9, nil
    }
    
    return 0, 0, fmt.Errorf("invalid packed integer")
}
```

### Checksum Calculation (CRC32)

```go
import "hash/crc32"

func CalculateBinlogChecksum(data []byte) uint32 {
    return crc32.ChecksumIEEE(data)
}

func VerifyBinlogChecksum(eventData []byte) bool {
    if len(eventData) < 4 {
        return false
    }
    
    dataLen := len(eventData) - 4
    data := eventData[:dataLen]
    storedChecksum := binary.LittleEndian.Uint32(eventData[dataLen:])
    
    calculatedChecksum := CalculateBinlogChecksum(data)
    return calculatedChecksum == storedChecksum
}
```

## MySQL Column Types

```go
const (
    MYSQL_TYPE_DECIMAL     = 0
    MYSQL_TYPE_TINY        = 1
    MYSQL_TYPE_SHORT       = 2
    MYSQL_TYPE_LONG        = 3
    MYSQL_TYPE_FLOAT       = 4
    MYSQL_TYPE_DOUBLE      = 5
    MYSQL_TYPE_NULL        = 6
    MYSQL_TYPE_TIMESTAMP   = 7
    MYSQL_TYPE_LONGLONG    = 8
    MYSQL_TYPE_INT24       = 9
    MYSQL_TYPE_DATE        = 10
    MYSQL_TYPE_TIME        = 11
    MYSQL_TYPE_DATETIME    = 12
    MYSQL_TYPE_YEAR        = 13
    MYSQL_TYPE_NEWDATE     = 14
    MYSQL_TYPE_VARCHAR     = 15
    MYSQL_TYPE_BIT         = 16
    MYSQL_TYPE_TIMESTAMP2  = 17
    MYSQL_TYPE_DATETIME2   = 18
    MYSQL_TYPE_TIME2       = 19
    MYSQL_TYPE_JSON        = 245
    MYSQL_TYPE_NEWDECIMAL  = 246
    MYSQL_TYPE_ENUM        = 247
    MYSQL_TYPE_SET         = 248
    MYSQL_TYPE_TINY_BLOB   = 249
    MYSQL_TYPE_MEDIUM_BLOB = 250
    MYSQL_TYPE_LONG_BLOB   = 251
    MYSQL_TYPE_BLOB        = 252
    MYSQL_TYPE_VAR_STRING  = 253
    MYSQL_TYPE_STRING      = 254
    MYSQL_TYPE_GEOMETRY    = 255
)
```

## Event Flags

```go
const (
    LOG_EVENT_BINLOG_IN_USE_F           = 0x0001
    LOG_EVENT_FORCED_ROTATE_F           = 0x0002
    LOG_EVENT_THREAD_SPECIFIC_F         = 0x0004
    LOG_EVENT_SUPPRESS_USE_F            = 0x0008
    LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F = 0x0010
    LOG_EVENT_ARTIFICIAL_F              = 0x0020
    LOG_EVENT_RELAY_LOG_F               = 0x0040
    LOG_EVENT_IGNORABLE_F               = 0x0080
    LOG_EVENT_NO_FILTER_F               = 0x0100
    LOG_EVENT_MTS_ISOLATE_F             = 0x0200
)
```

## References

- Source: libs/mysql/binlog/event/binlog_event.h
- Source: libs/mysql/binlog/event/binlog_event.cpp
- Source: libs/mysql/binlog/event/statement_events.h
- Source: libs/mysql/binlog/event/control_events.h
- Source: libs/mysql/binlog/event/rows_event.h
