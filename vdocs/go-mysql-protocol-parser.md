# MySQL Protocol Parser - Go Implementation Guide

## Overview
This document provides a comprehensive guide for implementing a MySQL client/server protocol parser in Go for Percona Server. It covers all command types and packet structures.

## MySQL Protocol Architecture

### Connection Phases

The MySQL protocol has several phases:

1. **Handshake Phase**: Server sends handshake, client responds with credentials
2. **Command Phase**: Client sends commands, server responds with results
3. **Replication Phase**: Special protocol for replication (BINLOG_DUMP)
4. **Prepared Statement Phase**: Binary protocol for prepared statements

## Packet Structure

### Basic Packet Format

Every MySQL packet has a 4-byte header followed by payload:

```
+-------------------+
| Payload Length (3)|  3 bytes (little-endian)
+-------------------+
| Sequence ID    (1)|  1 byte
+-------------------+
| Payload (variable)|  Variable length
+-------------------+
```

```go
type Packet struct {
    Length     uint32  // 3 bytes (actually uint24)
    SequenceID uint8   // 1 byte
    Payload    []byte  // Variable length
}

const (
    MaxPacketSize = 0xFFFFFF  // 16MB - 1 byte
)

func ParsePacket(data []byte) (*Packet, error) {
    if len(data) < 4 {
        return nil, fmt.Errorf("packet too short")
    }
    
    packet := &Packet{
        Length:     uint32(data[0]) | uint32(data[1])<<8 | uint32(data[2])<<16,
        SequenceID: data[3],
        Payload:    data[4:],
    }
    
    if len(packet.Payload) != int(packet.Length) {
        return nil, fmt.Errorf("payload length mismatch")
    }
    
    return packet, nil
}
```

### Multi-Packet Messages

If payload is ≥ 16MB, it's split into multiple packets:
- First packet: length = 0xFFFFFF, sequence_id = 0
- Next packet: length = 0xFFFFFF, sequence_id = 1
- Last packet: length < 0xFFFFFF, sequence_id = n

## Handshake Phase

### 1. Initial Handshake Packet (Server → Client)

```go
type HandshakeV10 struct {
    ProtocolVersion    uint8     // Always 10
    ServerVersion      string    // Null-terminated string
    ConnectionID       uint32    // Thread ID
    AuthPluginData1    [8]byte   // First 8 bytes of auth data
    Filler1            uint8     // Always 0x00
    CapabilityFlagsLow uint16    // Lower 2 bytes of capabilities
    CharacterSet       uint8     // Default server charset
    StatusFlags        uint16    // Server status flags
    CapabilityFlagsHigh uint16   // Upper 2 bytes of capabilities
    AuthPluginDataLen  uint8     // Length of auth data
    Reserved           [10]byte  // Reserved (all 0x00)
    AuthPluginData2    []byte    // Rest of auth data (if CLIENT_SECURE_CONNECTION)
    AuthPluginName     string    // Name of auth plugin (if CLIENT_PLUGIN_AUTH)
}

// Capability flags
const (
    CLIENT_LONG_PASSWORD                  = 0x00000001
    CLIENT_FOUND_ROWS                     = 0x00000002
    CLIENT_LONG_FLAG                      = 0x00000004
    CLIENT_CONNECT_WITH_DB                = 0x00000008
    CLIENT_NO_SCHEMA                      = 0x00000010
    CLIENT_COMPRESS                       = 0x00000020
    CLIENT_ODBC                           = 0x00000040
    CLIENT_LOCAL_FILES                    = 0x00000080
    CLIENT_IGNORE_SPACE                   = 0x00000100
    CLIENT_PROTOCOL_41                    = 0x00000200
    CLIENT_INTERACTIVE                    = 0x00000400
    CLIENT_SSL                            = 0x00000800
    CLIENT_IGNORE_SIGPIPE                 = 0x00001000
    CLIENT_TRANSACTIONS                   = 0x00002000
    CLIENT_RESERVED                       = 0x00004000
    CLIENT_SECURE_CONNECTION              = 0x00008000
    CLIENT_MULTI_STATEMENTS               = 0x00010000
    CLIENT_MULTI_RESULTS                  = 0x00020000
    CLIENT_PS_MULTI_RESULTS               = 0x00040000
    CLIENT_PLUGIN_AUTH                    = 0x00080000
    CLIENT_CONNECT_ATTRS                  = 0x00100000
    CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 0x00200000
    CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   = 0x00400000
    CLIENT_SESSION_TRACK                  = 0x00800000
    CLIENT_DEPRECATE_EOF                  = 0x01000000
    CLIENT_OPTIONAL_RESULTSET_METADATA    = 0x02000000
    CLIENT_ZSTD_COMPRESSION_ALGORITHM     = 0x04000000
    CLIENT_QUERY_ATTRIBUTES               = 0x08000000
    MULTI_FACTOR_AUTHENTICATION           = 0x10000000
    CLIENT_CAPABILITY_EXTENSION           = 0x20000000
    CLIENT_SSL_VERIFY_SERVER_CERT         = 0x40000000
    CLIENT_REMEMBER_OPTIONS               = 0x80000000
)
```

### 2. Handshake Response Packet (Client → Server)

```go
type HandshakeResponse41 struct {
    CapabilityFlags   uint32   // Client capabilities
    MaxPacketSize     uint32   // Max packet size
    CharacterSet      uint8    // Client charset
    Reserved          [23]byte // Reserved (all 0x00)
    Username          string   // Null-terminated
    AuthResponse      []byte   // Length-encoded if CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA
    Database          string   // Null-terminated (if CLIENT_CONNECT_WITH_DB)
    AuthPluginName    string   // Null-terminated (if CLIENT_PLUGIN_AUTH)
    ConnectAttrs      map[string]string // Key-value pairs (if CLIENT_CONNECT_ATTRS)
    ZstdCompressionLevel uint8 // (if CLIENT_ZSTD_COMPRESSION_ALGORITHM)
}
```

### 3. Auth Switch Request (Server → Client)

```go
type AuthSwitchRequest struct {
    StatusTag      uint8    // 0xFE
    PluginName     string   // Null-terminated
    PluginAuthData []byte   // Rest of packet
}
```

### 4. Auth More Data (Server ↔ Client)

```go
type AuthMoreData struct {
    StatusTag uint8    // 0x01
    Data      []byte   // Authentication data
}
```

## Command Phase

### Command Packet Format (Client → Server)

```
+----------------+
| Command Byte   |  1 byte
+----------------+
| Arguments      |  Variable length
+----------------+
```

### Complete Command List

```go
const (
    COM_SLEEP                              = 0x00  // Internal, not a real command
    COM_QUIT                               = 0x01  // Close connection
    COM_INIT_DB                            = 0x02  // Change database
    COM_QUERY                              = 0x03  // Execute SQL query
    COM_FIELD_LIST                         = 0x04  // Deprecated: Get column definitions
    COM_CREATE_DB                          = 0x05  // Deprecated: Create database
    COM_DROP_DB                            = 0x06  // Deprecated: Drop database
    COM_UNUSED_2                           = 0x07  // Was COM_REFRESH
    COM_UNUSED_1                           = 0x08  // Was COM_SHUTDOWN
    COM_STATISTICS                         = 0x09  // Get server statistics
    COM_UNUSED_4                           = 0x0A  // Was COM_PROCESS_INFO
    COM_CONNECT                            = 0x0B  // Internal
    COM_UNUSED_5                           = 0x0C  // Was COM_PROCESS_KILL
    COM_DEBUG                              = 0x0D  // Dump debug info
    COM_PING                               = 0x0E  // Ping server
    COM_TIME                               = 0x0F  // Internal
    COM_DELAYED_INSERT                     = 0x10  // Deprecated
    COM_CHANGE_USER                        = 0x11  // Change user
    COM_BINLOG_DUMP                        = 0x12  // Request binlog stream
    COM_TABLE_DUMP                         = 0x13  // Internal
    COM_CONNECT_OUT                        = 0x14  // Internal
    COM_REGISTER_SLAVE                     = 0x15  // Register as replica
    COM_STMT_PREPARE                       = 0x16  // Prepare statement
    COM_STMT_EXECUTE                       = 0x17  // Execute prepared statement
    COM_STMT_SEND_LONG_DATA                = 0x18  // Send long data for prepared stmt
    COM_STMT_CLOSE                         = 0x19  // Close prepared statement
    COM_STMT_RESET                         = 0x1A  // Reset prepared statement
    COM_SET_OPTION                         = 0x1B  // Set connection options
    COM_STMT_FETCH                         = 0x1C  // Fetch rows from cursor
    COM_DAEMON                             = 0x1D  // Internal: mark as daemon thread
    COM_BINLOG_DUMP_GTID                   = 0x1E  // Request binlog with GTID
    COM_RESET_CONNECTION                   = 0x1F  // Reset connection state
    COM_CLONE                              = 0x20  // Clone plugin command
    COM_SUBSCRIBE_GROUP_REPLICATION_STREAM = 0x21  // Subscribe to GR stream
    COM_END                                = 0x22  // Not a real command (marker)
)
```

## Command Details

### 1. COM_QUIT (0x01)

```go
type ComQuit struct {
    Command uint8  // 0x01
}

// No arguments, server closes connection without response
```

### 2. COM_INIT_DB (0x02)

```go
type ComInitDB struct {
    Command    uint8   // 0x02
    SchemaName string  // Database name
}

// Response: OK_Packet or ERR_Packet
```

### 3. COM_QUERY (0x03)

```go
type ComQuery struct {
    Command uint8   // 0x03
    Query   string  // SQL query
}

// Response: Resultset or OK_Packet or ERR_Packet
```

### 4. COM_FIELD_LIST (0x04) - Deprecated

```go
type ComFieldList struct {
    Command   uint8   // 0x04
    Table     string  // Null-terminated table name
    FieldWildcard string  // Field wildcard
}

// Response: Column Definition packets + EOF
```

### 5. COM_STATISTICS (0x09)

```go
type ComStatistics struct {
    Command uint8  // 0x09
}

// Response: Statistics string (not a packet, just string)
```

### 6. COM_DEBUG (0x0D)

```go
type ComDebug struct {
    Command uint8  // 0x0D
}

// Response: EOF_Packet (requires SUPER privilege)
```

### 7. COM_PING (0x0E)

```go
type ComPing struct {
    Command uint8  // 0x0E
}

// Response: OK_Packet
```

### 8. COM_CHANGE_USER (0x11)

```go
type ComChangeUser struct {
    Command        uint8    // 0x11
    Username       string   // Null-terminated
    AuthResponse   []byte   // Length-encoded
    SchemaName     string   // Null-terminated
    CharacterSet   uint16   // 2 bytes
    AuthPluginName string   // Null-terminated (if CLIENT_PLUGIN_AUTH)
    ConnectAttrs   map[string]string  // (if CLIENT_CONNECT_ATTRS)
}

// Response: OK_Packet or ERR_Packet or Auth packets
```

### 9. COM_BINLOG_DUMP (0x12)

```go
type ComBinlogDump struct {
    Command      uint8    // 0x12
    BinlogPos    uint32   // Position in binlog
    Flags        uint16   // Flags
    ServerID     uint32   // Replica server ID
    BinlogFilename string // Binlog filename
}

// Response: Binlog event stream
```

### 10. COM_REGISTER_SLAVE (0x15)

```go
type ComRegisterSlave struct {
    Command    uint8    // 0x15
    ServerID   uint32   // Replica server ID
    HostLen    uint8    // Hostname length
    Host       string   // Hostname
    UserLen    uint8    // Username length
    User       string   // Username
    PasswordLen uint8   // Password length
    Password   string   // Password
    Port       uint16   // Replica port
    ReplicationRank uint32  // Replication rank (ignored)
    MasterID   uint32   // Master ID (usually 0)
}

// Response: OK_Packet or ERR_Packet
```

### 11. COM_STMT_PREPARE (0x16)

```go
type ComStmtPrepare struct {
    Command uint8   // 0x16
    Query   string  // SQL query with ? placeholders
}

// Response: COM_STMT_PREPARE_OK or ERR_Packet
```

### 12. COM_STMT_EXECUTE (0x17)

```go
type ComStmtExecute struct {
    Command         uint8    // 0x17
    StatementID     uint32   // Statement ID from PREPARE
    Flags           uint8    // Cursor flags
    IterationCount  uint32   // Always 1
    NullBitmap      []byte   // Null bitmap
    NewParamsBound  uint8    // 1 if parameters provided
    ParameterTypes  []uint16 // Parameter types (if NewParamsBound=1)
    ParameterValues []interface{} // Parameter values
}

// Cursor flags
const (
    CURSOR_TYPE_NO_CURSOR  = 0x00
    CURSOR_TYPE_READ_ONLY  = 0x01
    CURSOR_TYPE_FOR_UPDATE = 0x02
    CURSOR_TYPE_SCROLLABLE = 0x04
)

// Response: Resultset or OK_Packet or ERR_Packet
```

### 13. COM_STMT_SEND_LONG_DATA (0x18)

```go
type ComStmtSendLongData struct {
    Command      uint8    // 0x18
    StatementID  uint32   // Statement ID
    ParameterID  uint16   // Parameter number
    Data         []byte   // Data chunk
}

// No response (can be sent multiple times)
```

### 14. COM_STMT_CLOSE (0x19)

```go
type ComStmtClose struct {
    Command     uint8    // 0x19
    StatementID uint32   // Statement ID
}

// No response
```

### 15. COM_STMT_RESET (0x1A)

```go
type ComStmtReset struct {
    Command     uint8    // 0x1A
    StatementID uint32   // Statement ID
}

// Response: OK_Packet
```

### 16. COM_SET_OPTION (0x1B)

```go
type ComSetOption struct {
    Command uint8    // 0x1B
    Option  uint16   // Option code
}

// Options
const (
    MYSQL_OPTION_MULTI_STATEMENTS_ON  = 0
    MYSQL_OPTION_MULTI_STATEMENTS_OFF = 1
)

// Response: EOF_Packet
```

### 17. COM_STMT_FETCH (0x1C)

```go
type ComStmtFetch struct {
    Command      uint8    // 0x1C
    StatementID  uint32   // Statement ID
    NumRows      uint32   // Number of rows to fetch
}

// Response: Row packets + EOF_Packet
```

### 18. COM_BINLOG_DUMP_GTID (0x1E)

```go
type ComBinlogDumpGTID struct {
    Command      uint8    // 0x1E
    Flags        uint16   // Flags
    ServerID     uint32   // Replica server ID
    FilenameLen  uint32   // Binlog filename length
    Filename     string   // Binlog filename
    Position     uint64   // Position in binlog
    DataSize     uint32   // Size of GTID data
    GTIDData     []byte   // Encoded GTID set
}

// Response: Binlog event stream
```

### 19. COM_RESET_CONNECTION (0x1F)

```go
type ComResetConnection struct {
    Command uint8  // 0x1F
}

// Response: OK_Packet
// Resets: session variables, user variables, temp tables, prepared statements
```

### 20. COM_CLONE (0x20)

```go
type ComClone struct {
    Command uint8   // 0x20
    // Clone-specific data
}

// Used by MySQL Clone plugin
```

### 21. COM_SUBSCRIBE_GROUP_REPLICATION_STREAM (0x21)

```go
type ComSubscribeGroupReplicationStream struct {
    Command uint8  // 0x21
    // GR-specific data
}

// Used by Group Replication
```

## Response Packet Types

### OK_Packet

```go
type OKPacket struct {
    Header           uint8    // 0x00 or 0xFE
    AffectedRows     uint64   // Length-encoded integer
    LastInsertID     uint64   // Length-encoded integer
    StatusFlags      uint16   // Server status flags
    Warnings         uint16   // Warning count
    Info             string   // Human-readable info
    SessionStateInfo string   // Session state changes (if CLIENT_SESSION_TRACK)
}

// Status flags
const (
    SERVER_STATUS_IN_TRANS             = 0x0001
    SERVER_STATUS_AUTOCOMMIT           = 0x0002
    SERVER_MORE_RESULTS_EXISTS         = 0x0008
    SERVER_STATUS_NO_GOOD_INDEX_USED   = 0x0010
    SERVER_STATUS_NO_INDEX_USED        = 0x0020
    SERVER_STATUS_CURSOR_EXISTS        = 0x0040
    SERVER_STATUS_LAST_ROW_SENT        = 0x0080
    SERVER_STATUS_DB_DROPPED           = 0x0100
    SERVER_STATUS_NO_BACKSLASH_ESCAPES = 0x0200
    SERVER_STATUS_METADATA_CHANGED     = 0x0400
    SERVER_QUERY_WAS_SLOW              = 0x0800
    SERVER_PS_OUT_PARAMS               = 0x1000
    SERVER_STATUS_IN_TRANS_READONLY    = 0x2000
    SERVER_SESSION_STATE_CHANGED       = 0x4000
)
```

### ERR_Packet

```go
type ERRPacket struct {
    Header       uint8    // 0xFF
    ErrorCode    uint16   // Error code
    SQLStateMarker string // '#' (if CLIENT_PROTOCOL_41)
    SQLState     string   // 5-byte SQL state (if CLIENT_PROTOCOL_41)
    ErrorMessage string   // Human-readable error message
}
```

### EOF_Packet (Deprecated in MySQL 5.7+)

```go
type EOFPacket struct {
    Header       uint8    // 0xFE
    Warnings     uint16   // Warning count
    StatusFlags  uint16   // Server status flags
}

// Note: With CLIENT_DEPRECATE_EOF, OK_Packet is used instead
```

### Resultset Packets

#### 1. Column Count Packet

```go
type ColumnCountPacket struct {
    ColumnCount uint64  // Length-encoded integer
}
```

#### 2. Column Definition Packet (Protocol 4.1)

```go
type ColumnDefinition41 struct {
    Catalog      string  // Length-encoded string (always "def")
    Schema       string  // Length-encoded string
    Table        string  // Length-encoded string (virtual table)
    OrgTable     string  // Length-encoded string (physical table)
    Name         string  // Length-encoded string (virtual column name)
    OrgName      string  // Length-encoded string (physical column name)
    FixedLength  uint8   // Length of fixed-length fields (always 0x0C)
    CharacterSet uint16  // Column character set
    ColumnLength uint32  // Maximum length of field
    ColumnType   uint8   // Column type (see FIELD_TYPE_*)
    Flags        uint16  // Column flags
    Decimals     uint8   // Decimal precision
    Filler       uint16  // Always 0x00 0x00
    DefaultValue []byte  // Default value (if COM_FIELD_LIST)
}

// Column types
const (
    MYSQL_TYPE_DECIMAL     = 0x00
    MYSQL_TYPE_TINY        = 0x01
    MYSQL_TYPE_SHORT       = 0x02
    MYSQL_TYPE_LONG        = 0x03
    MYSQL_TYPE_FLOAT       = 0x04
    MYSQL_TYPE_DOUBLE      = 0x05
    MYSQL_TYPE_NULL        = 0x06
    MYSQL_TYPE_TIMESTAMP   = 0x07
    MYSQL_TYPE_LONGLONG    = 0x08
    MYSQL_TYPE_INT24       = 0x09
    MYSQL_TYPE_DATE        = 0x0A
    MYSQL_TYPE_TIME        = 0x0B
    MYSQL_TYPE_DATETIME    = 0x0C
    MYSQL_TYPE_YEAR        = 0x0D
    MYSQL_TYPE_NEWDATE     = 0x0E
    MYSQL_TYPE_VARCHAR     = 0x0F
    MYSQL_TYPE_BIT         = 0x10
    MYSQL_TYPE_TIMESTAMP2  = 0x11
    MYSQL_TYPE_DATETIME2   = 0x12
    MYSQL_TYPE_TIME2       = 0x13
    MYSQL_TYPE_TYPED_ARRAY = 0x14
    MYSQL_TYPE_JSON        = 0xF5
    MYSQL_TYPE_NEWDECIMAL  = 0xF6
    MYSQL_TYPE_ENUM        = 0xF7
    MYSQL_TYPE_SET         = 0xF8
    MYSQL_TYPE_TINY_BLOB   = 0xF9
    MYSQL_TYPE_MEDIUM_BLOB = 0xFA
    MYSQL_TYPE_LONG_BLOB   = 0xFB
    MYSQL_TYPE_BLOB        = 0xFC
    MYSQL_TYPE_VAR_STRING  = 0xFD
    MYSQL_TYPE_STRING      = 0xFE
    MYSQL_TYPE_GEOMETRY    = 0xFF
)

// Column flags
const (
    NOT_NULL_FLAG         = 0x0001
    PRI_KEY_FLAG          = 0x0002
    UNIQUE_KEY_FLAG       = 0x0004
    MULTIPLE_KEY_FLAG     = 0x0008
    BLOB_FLAG             = 0x0010
    UNSIGNED_FLAG         = 0x0020
    ZEROFILL_FLAG         = 0x0040
    BINARY_FLAG           = 0x0080
    ENUM_FLAG             = 0x0100
    AUTO_INCREMENT_FLAG   = 0x0200
    TIMESTAMP_FLAG        = 0x0400
    SET_FLAG              = 0x0800
    NO_DEFAULT_VALUE_FLAG = 0x1000
    ON_UPDATE_NOW_FLAG    = 0x2000
    NUM_FLAG              = 0x4000
    PART_KEY_FLAG         = 0x8000
)
```

#### 3. Row Data Packet (Text Protocol)

```go
type TextResultsetRow struct {
    Values [][]byte  // Each value is length-encoded string (0xFB for NULL)
}

// Length-encoded string: length byte(s) + string
// NULL is represented by 0xFB
```

#### 4. Row Data Packet (Binary Protocol)

```go
type BinaryResultsetRow struct {
    PacketHeader uint8    // 0x00
    NullBitmap   []byte   // (column_count + 7 + 2) / 8 bytes
    Values       []interface{}  // Binary-encoded values
}
```

## Length-Encoded Integer

```go
func ReadLengthEncodedInteger(data []byte) (value uint64, bytesRead int, err error) {
    if len(data) == 0 {
        return 0, 0, fmt.Errorf("empty data")
    }
    
    first := data[0]
    
    if first < 0xFB {
        return uint64(first), 1, nil
    } else if first == 0xFC {
        if len(data) < 3 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := uint64(data[1]) | uint64(data[2])<<8
        return val, 3, nil
    } else if first == 0xFD {
        if len(data) < 4 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := uint64(data[1]) | uint64(data[2])<<8 | uint64(data[3])<<16
        return val, 4, nil
    } else if first == 0xFE {
        if len(data) < 9 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := uint64(data[1]) | uint64(data[2])<<8 | uint64(data[3])<<16 |
               uint64(data[4])<<24 | uint64(data[5])<<32 | uint64(data[6])<<40 |
               uint64(data[7])<<48 | uint64(data[8])<<56
        return val, 9, nil
    }
    
    return 0, 0, fmt.Errorf("invalid length-encoded integer")
}
```

## Length-Encoded String

```go
func ReadLengthEncodedString(data []byte) (value string, bytesRead int, err error) {
    length, n, err := ReadLengthEncodedInteger(data)
    if err != nil {
        return "", 0, err
    }
    
    if len(data) < n+int(length) {
        return "", 0, fmt.Errorf("insufficient data for string")
    }
    
    return string(data[n : n+int(length)]), n+int(length), nil
}
```

## Compression (if CLIENT_COMPRESS)

```go
type CompressedPacket struct {
    CompressedLength   uint32  // 3 bytes
    SequenceID         uint8   // 1 byte
    UncompressedLength uint32  // 3 bytes (0 if not compressed)
    CompressedPayload  []byte  // zlib compressed (if UncompressedLength > 0)
}
```

## SSL/TLS Support

If CLIENT_SSL capability is set, SSL handshake occurs after initial handshake:
1. Server sends Initial Handshake
2. Client sends SSL Request (same structure as HandshakeResponse but shorter)
3. SSL handshake
4. Client sends HandshakeResponse (encrypted)

## References

- Source: `include/my_command.h`
- Source: `sql/protocol_classic.cc`
- Source: `sql-common/client.cc`
- MySQL Client/Server Protocol Documentation

## Implementation Checklist

- [ ] Parse packet header (3-byte length + 1-byte sequence)
- [ ] Handle multi-packet messages (16MB+)
- [ ] Parse Initial Handshake (server)
- [ ] Parse Handshake Response (client)
- [ ] Parse all command packets (COM_*)
- [ ] Parse OK_Packet
- [ ] Parse ERR_Packet
- [ ] Parse EOF_Packet
- [ ] Parse Resultset (column definitions + rows)
- [ ] Handle Text Protocol resultset
- [ ] Handle Binary Protocol resultset (prepared statements)
- [ ] Implement length-encoded integers
- [ ] Implement length-encoded strings
- [ ] Handle NULL values (0xFB)
- [ ] Support compression (if enabled)
- [ ] Support SSL/TLS
- [ ] Handle authentication plugins
- [ ] Parse connection attributes
- [ ] Handle session tracking

## Notes

1. **Sequence ID**: Increments with each packet, wraps around at 255.

2. **Protocol Versions**: Protocol 4.1 is standard since MySQL 4.1.

3. **Deprecations**: COM_FIELD_LIST, COM_CREATE_DB, COM_DROP_DB are deprecated.

4. **EOF vs OK**: With CLIENT_DEPRECATE_EOF, OK_Packet (0xFE header) replaces EOF_Packet.

5. **Text vs Binary**: Text protocol sends all values as strings, binary protocol uses native types.

6. **Compression**: Can use zlib or zstd (CLIENT_ZSTD_COMPRESSION_ALGORITHM).

7. **Multi-Statements**: Requires CLIENT_MULTI_STATEMENTS capability.

8. **Prepared Statements**: Use binary protocol for better performance.

9. **Authentication**: Multiple authentication plugins supported (mysql_native_password, caching_sha2_password, etc.).

10. **Byte Order**: All multi-byte integers are little-endian.
