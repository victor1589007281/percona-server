# InnoDB Redo Log Parser - Go Implementation Guide

## Overview
This document provides a comprehensive guide for implementing an InnoDB redo log parser in Go for Percona Server. It covers all redo log record types (mlog_id_t) supported by the current codebase.

## Redo Log Structure

### General Architecture
The InnoDB redo log is a write-ahead log (WAL) that ensures durability of transactions. Changes are logged before they are written to data pages.

### Mini-Transaction (MTR)
- All changes are grouped into mini-transactions (mtr_t)
- A single MTR can span multiple pages
- On commit, all log records from an MTR are written atomically
- During recovery, either all changes from an MTR are applied or none

### Redo Log Format

#### Log Block Structure
Each log block is 512 bytes and contains:
- **Header** (12 bytes)
  - Checksum (4 bytes)
  - Block number (4 bytes)
  - Data length (2 bytes)
  - First record offset (2 bytes)
- **Data** (496 bytes)
  - Log records
- **Footer** (4 bytes)
  - Checksum (4 bytes)

#### Log Record Structure
Each redo log record has:
1. **Type** (1 byte) - mlog_id_t value
2. **Space ID** (variable) - tablespace ID (compressed integer)
3. **Page Number** (variable) - page number (compressed integer)
4. **Data** (variable) - record-specific data

### Compressed Integer Format
InnoDB uses a compressed integer format to save space:
- If value < 0x80: 1 byte
- If value < 0x4000: 2 bytes (0x80 flag)
- If value < 0x200000: 3 bytes (0xC0 flag)
- If value < 0x10000000: 4 bytes (0xE0 flag)
- Otherwise: 5 bytes (0xF0 flag + 4 bytes)

```go
func ReadCompressedUint(data []byte) (value uint64, bytesRead int, err error) {
    if len(data) == 0 {
        return 0, 0, fmt.Errorf("empty data")
    }
    
    flag := data[0]
    
    if flag < 0x80 {
        return uint64(flag), 1, nil
    } else if flag < 0xC0 {
        if len(data) < 2 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := ((uint64(flag) & 0x3F) << 8) | uint64(data[1])
        return val, 2, nil
    } else if flag < 0xE0 {
        if len(data) < 3 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := ((uint64(flag) & 0x1F) << 16) | (uint64(data[1]) << 8) | uint64(data[2])
        return val, 3, nil
    } else if flag < 0xF0 {
        if len(data) < 4 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := ((uint64(flag) & 0x0F) << 24) | (uint64(data[1]) << 16) | 
               (uint64(data[2]) << 8) | uint64(data[3])
        return val, 4, nil
    } else {
        if len(data) < 5 {
            return 0, 0, fmt.Errorf("insufficient data")
        }
        val := (uint64(data[1]) << 24) | (uint64(data[2]) << 16) | 
               (uint64(data[3]) << 8) | uint64(data[4])
        return val, 5, nil
    }
}
```

## Complete Redo Log Record Types (mlog_id_t)

```go
const (
    // Special flag
    MLOG_SINGLE_REC_FLAG = 128  // ORed with type if mtr has only one record
    
    // Basic write operations
    MLOG_1BYTE  = 1   // Write 1 byte
    MLOG_2BYTES = 2   // Write 2 bytes
    MLOG_4BYTES = 4   // Write 4 bytes
    MLOG_8BYTES = 8   // Write 8 bytes
    
    // Record operations (old format for MySQL 8.0.27 and earlier)
    MLOG_REC_INSERT_8027              = 9   // Insert record (old)
    MLOG_REC_CLUST_DELETE_MARK_8027   = 10  // Mark clustered index record deleted (old)
    MLOG_REC_SEC_DELETE_MARK          = 11  // Mark secondary index record deleted
    MLOG_REC_UPDATE_IN_PLACE_8027     = 13  // Update record in place (old)
    MLOG_REC_DELETE_8027              = 14  // Delete record (old)
    MLOG_LIST_END_DELETE_8027         = 15  // Delete record list end (old)
    MLOG_LIST_START_DELETE_8027       = 16  // Delete record list start (old)
    MLOG_LIST_END_COPY_CREATED_8027   = 17  // Copy record list end to new page (old)
    MLOG_PAGE_REORGANIZE_8027         = 18  // Reorganize page ROW_FORMAT=REDUNDANT (old)
    
    // Page operations
    MLOG_PAGE_CREATE         = 19  // Create index page
    
    // Undo log operations
    MLOG_UNDO_INSERT         = 20  // Insert entry in undo log
    MLOG_UNDO_ERASE_END      = 21  // Erase undo log page end
    MLOG_UNDO_INIT           = 22  // Initialize undo log page
    MLOG_UNDO_HDR_REUSE      = 24  // Reuse insert undo log header
    MLOG_UNDO_HDR_CREATE     = 25  // Create undo log header
    
    // Special markers
    MLOG_REC_MIN_MARK        = 26  // Mark index record as minimum
    
    // Insert buffer
    MLOG_IBUF_BITMAP_INIT    = 27  // Initialize insert buffer bitmap page
    
    // Debug (conditional)
    MLOG_LSN                 = 28  // Current LSN (only if UNIV_LOG_LSN_DEBUG)
    
    // File page operations
    MLOG_INIT_FILE_PAGE      = 29  // Take file page into use (deprecated)
    MLOG_WRITE_STRING        = 30  // Write string to page
    
    // Multi-record markers
    MLOG_MULTI_REC_END       = 31  // End of multi-record sequence
    MLOG_DUMMY_RECORD        = 32  // Dummy record to pad log block
    
    // File operations
    MLOG_FILE_CREATE         = 33  // Create .ibd file
    MLOG_FILE_RENAME         = 34  // Rename tablespace file
    MLOG_FILE_DELETE         = 35  // Delete tablespace file
    
    // Compact format operations
    MLOG_COMP_REC_MIN_MARK           = 36  // Mark compact record as minimum
    MLOG_COMP_PAGE_CREATE            = 37  // Create compact index page
    MLOG_COMP_REC_INSERT_8027        = 38  // Insert compact record (old)
    MLOG_COMP_REC_CLUST_DELETE_MARK_8027 = 39  // Mark compact clustered deleted (old)
    MLOG_COMP_REC_SEC_DELETE_MARK    = 40  // Mark compact secondary deleted
    MLOG_COMP_REC_UPDATE_IN_PLACE_8027 = 41  // Update compact record in place (old)
    MLOG_COMP_REC_DELETE_8027        = 42  // Delete compact record (old)
    MLOG_COMP_LIST_END_DELETE_8027   = 43  // Delete compact list end (old)
    MLOG_COMP_LIST_START_DELETE_8027 = 44  // Delete compact list start (old)
    MLOG_COMP_LIST_END_COPY_CREATED_8027 = 45  // Copy compact list end (old)
    MLOG_COMP_PAGE_REORGANIZE_8027   = 46  // Reorganize compact page (old)
    
    // Compressed page operations
    MLOG_ZIP_WRITE_NODE_PTR           = 48  // Write node pointer on compressed page
    MLOG_ZIP_WRITE_BLOB_PTR           = 49  // Write BLOB pointer on compressed page
    MLOG_ZIP_WRITE_HEADER             = 50  // Write to compressed page header
    MLOG_ZIP_PAGE_COMPRESS            = 51  // Compress index page
    MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027 = 52  // Compress without image (old)
    MLOG_ZIP_PAGE_REORGANIZE_8027     = 53  // Reorganize compressed page (old)
    
    // R-Tree operations
    MLOG_PAGE_CREATE_RTREE       = 57  // Create R-Tree index page
    MLOG_COMP_PAGE_CREATE_RTREE  = 58  // Create R-Tree compact page
    
    // Enhanced file page operation
    MLOG_INIT_FILE_PAGE2         = 59  // Take file page into use (new)
    
    // Index operations
    MLOG_INDEX_LOAD              = 61  // Index tree loading (no individual page logs)
    
    // Metadata operations
    MLOG_TABLE_DYNAMIC_META      = 62  // Persistent dynamic metadata change
    
    // SDI (Serialized Dictionary Information) operations
    MLOG_PAGE_CREATE_SDI         = 63  // Create SDI index page
    MLOG_COMP_PAGE_CREATE_SDI    = 64  // Create SDI compact page
    
    // File extension
    MLOG_FILE_EXTEND             = 65  // Extend tablespace
    
    // Testing
    MLOG_TEST                    = 66  // Test record (unit tests only)
    
    // New format operations (MySQL 8.0.28+)
    MLOG_REC_INSERT              = 67  // Insert record (new)
    MLOG_REC_CLUST_DELETE_MARK   = 68  // Mark clustered index record deleted (new)
    MLOG_REC_DELETE              = 69  // Delete record (new)
    MLOG_REC_UPDATE_IN_PLACE     = 70  // Update record in place (new)
    MLOG_LIST_END_COPY_CREATED   = 71  // Copy record list end to new page (new)
    MLOG_PAGE_REORGANIZE         = 72  // Reorganize page (new)
    MLOG_ZIP_PAGE_REORGANIZE     = 73  // Reorganize compressed page (new)
    MLOG_ZIP_PAGE_COMPRESS_NO_DATA = 74  // Compress without image (new)
    MLOG_LIST_END_DELETE         = 75  // Delete record list end (new)
    MLOG_LIST_START_DELETE       = 76  // Delete record list start (new)
    
    // Maximum value
    MLOG_BIGGEST_TYPE            = MLOG_LIST_START_DELETE
)
```

## Record Type Details

### 1. MLOG_1BYTE / MLOG_2BYTES / MLOG_4BYTES / MLOG_8BYTES (1, 2, 4, 8)
**Purpose**: Write 1/2/4/8 bytes at a specific offset on a page.

**Format**:
```go
type MlogWriteBytes struct {
    Type        uint8   // 1, 2, 4, or 8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Offset      uint16  // 2 bytes - offset within page
    Value       []byte  // 1, 2, 4, or 8 bytes
}
```

### 2. MLOG_REC_INSERT (9 old, 67 new)
**Purpose**: Insert a record into an index page.

**Format**:
```go
type MlogRecInsert struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    // Old format (8027):
    RecOffset   uint16  // Record offset
    RecSize     uint16  // Record size
    RecData     []byte  // Record data
    // New format (67):
    // Enhanced format with additional metadata
}
```

### 3. MLOG_REC_CLUST_DELETE_MARK (10 old, 68 new)
**Purpose**: Mark a clustered index record as deleted.

**Format**:
```go
type MlogRecClustDeleteMark struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    RecOffset   uint16  // Record offset
    DeleteFlag  uint8   // 1 = deleted, 0 = undeleted
}
```

### 4. MLOG_REC_SEC_DELETE_MARK (11)
**Purpose**: Mark a secondary index record as deleted.

**Format**: Similar to MLOG_REC_CLUST_DELETE_MARK.

### 5. MLOG_REC_UPDATE_IN_PLACE (13 old, 70 new)
**Purpose**: Update a record in place (preserves field sizes).

**Format**:
```go
type MlogRecUpdateInPlace struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    RecOffset   uint16  // Record offset
    UpdateVector []byte // Update information
}
```

### 6. MLOG_REC_DELETE (14 old, 69 new)
**Purpose**: Physically delete a record from a page.

**Format**:
```go
type MlogRecDelete struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    RecOffset   uint16  // Record offset
}
```

### 7. MLOG_PAGE_CREATE (19)
**Purpose**: Create a new index page.

**Format**:
```go
type MlogPageCreate struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    PageType    uint16  // FIL_PAGE_INDEX, etc.
    IndexID     uint64  // Index ID (compressed)
}
```

### 8. MLOG_COMP_PAGE_CREATE (37)
**Purpose**: Create a new compact format index page.

**Format**: Similar to MLOG_PAGE_CREATE but for compact row format.

### 9. MLOG_UNDO_INSERT (20)
**Purpose**: Insert an entry in the undo log.

**Format**:
```go
type MlogUndoInsert struct {
    Type        uint8
    SpaceID     uint32  // Compressed (undo tablespace)
    PageNo      uint32  // Compressed
    UndoType    uint8   // TRX_UNDO_INSERT_REC, etc.
    UndoData    []byte  // Undo record data
}
```

### 10. MLOG_UNDO_ERASE_END (21)
**Purpose**: Erase the end of an undo log page.

**Format**:
```go
type MlogUndoEraseEnd struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Offset      uint16  // New end offset
}
```

### 11. MLOG_UNDO_INIT (22)
**Purpose**: Initialize an undo log page.

**Format**:
```go
type MlogUndoInit struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
}
```

### 12. MLOG_UNDO_HDR_CREATE (25)
**Purpose**: Create an undo log header.

**Format**:
```go
type MlogUndoHdrCreate struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    TrxID       uint64  // Transaction ID
}
```

### 13. MLOG_UNDO_HDR_REUSE (24)
**Purpose**: Reuse an insert undo log header.

**Format**: Similar to MLOG_UNDO_HDR_CREATE.

### 14. MLOG_WRITE_STRING (30)
**Purpose**: Write a string of bytes to a page.

**Format**:
```go
type MlogWriteString struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Offset      uint16  // Offset within page
    Length      uint16  // String length (compressed)
    Data        []byte  // String data
}
```

### 15. MLOG_FILE_CREATE (33)
**Purpose**: Create a new .ibd file (tablespace).

**Format**:
```go
type MlogFileCreate struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    Flags       uint32  // Tablespace flags
    NameLen     uint16  // Filename length
    Name        string  // Filename
}
```

### 16. MLOG_FILE_RENAME (34)
**Purpose**: Rename a tablespace file.

**Format**:
```go
type MlogFileRename struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    OldNameLen  uint16
    OldName     string
    NewNameLen  uint16
    NewName     string
}
```

### 17. MLOG_FILE_DELETE (35)
**Purpose**: Delete a tablespace file.

**Format**:
```go
type MlogFileDelete struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    NameLen     uint16
    Name        string
}
```

### 18. MLOG_FILE_EXTEND (65)
**Purpose**: Extend a tablespace (add pages).

**Format**:
```go
type MlogFileExtend struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    OldSize     uint32  // Old size in pages
    NewSize     uint32  // New size in pages
}
```

### 19. MLOG_PAGE_REORGANIZE (18 old, 72 new)
**Purpose**: Reorganize a page to reclaim space.

**Format**:
```go
type MlogPageReorganize struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
}
```

### 20. MLOG_ZIP_WRITE_NODE_PTR (48)
**Purpose**: Write node pointer on a compressed non-leaf B-tree page.

**Format**:
```go
type MlogZipWriteNodePtr struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Offset      uint16
    Length      uint16
    Data        []byte
}
```

### 21. MLOG_ZIP_WRITE_BLOB_PTR (49)
**Purpose**: Write BLOB pointer on a compressed page.

**Format**: Similar to MLOG_ZIP_WRITE_NODE_PTR.

### 22. MLOG_ZIP_WRITE_HEADER (50)
**Purpose**: Write to compressed page header.

**Format**:
```go
type MlogZipWriteHeader struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Offset      uint16
    Length      uint16
    Data        []byte
}
```

### 23. MLOG_ZIP_PAGE_COMPRESS (51)
**Purpose**: Compress an index page.

**Format**:
```go
type MlogZipPageCompress struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
    Level       uint8   // Compression level
    // May include compressed page image
}
```

### 24. MLOG_PAGE_CREATE_RTREE (57) / MLOG_COMP_PAGE_CREATE_RTREE (58)
**Purpose**: Create R-Tree index pages (for spatial indexes).

**Format**: Similar to MLOG_PAGE_CREATE but for R-Tree indexes.

### 25. MLOG_PAGE_CREATE_SDI (63) / MLOG_COMP_PAGE_CREATE_SDI (64)
**Purpose**: Create SDI (Serialized Dictionary Information) pages.

**Format**: Similar to MLOG_PAGE_CREATE but for SDI pages.

### 26. MLOG_INDEX_LOAD (61)
**Purpose**: Notify that an index tree is being loaded without individual page logs.

**Format**:
```go
type MlogIndexLoad struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    IndexID     uint64  // Index ID (compressed)
}
```

### 27. MLOG_TABLE_DYNAMIC_META (62)
**Purpose**: Log changes to persistent dynamic metadata.

**Format**:
```go
type MlogTableDynamicMeta struct {
    Type        uint8
    TableID     uint64  // Table ID (compressed)
    MetaType    uint8   // Type of metadata
    MetaData    []byte  // Metadata content
}
```

### 28. MLOG_MULTI_REC_END (31)
**Purpose**: Marks the end of a multi-record MTR sequence.

**Format**:
```go
type MlogMultiRecEnd struct {
    Type        uint8
    // No additional data
}
```

### 29. MLOG_DUMMY_RECORD (32)
**Purpose**: Pad a log block to fill it.

**Format**: No data, just the type byte.

### 30. MLOG_INIT_FILE_PAGE2 (59)
**Purpose**: Initialize a file page (replaces deprecated MLOG_INIT_FILE_PAGE).

**Format**:
```go
type MlogInitFilePage2 struct {
    Type        uint8
    SpaceID     uint32  // Compressed
    PageNo      uint32  // Compressed
}
```

## MTR Logging Modes

```go
const (
    MTR_LOG_ALL           = 0  // Log all operations (default)
    MTR_LOG_NONE          = 1  // No logging, no dirty pages
    MTR_LOG_NO_REDO       = 2  // No REDO log but add dirty pages
    MTR_LOG_SHORT_INSERTS = 3  // Log inserts in shorter form
)
```

## LSN (Log Sequence Number)

- **Type**: uint64
- **Purpose**: Identifies a position in the redo log
- **Format**: Absolute byte offset from the beginning of the log

```go
type LSN uint64
```

## Checkpoint Information

```go
type Checkpoint struct {
    Number      uint64  // Checkpoint number
    LSN         LSN     // Checkpoint LSN
    Offset      uint64  // Offset in log file
    LogBufSize  uint32  // Log buffer size
}
```

## Implementation Guidelines

### 1. Log Block Reading

```go
type LogBlock struct {
    Header struct {
        Checksum         uint32
        BlockNumber      uint32
        DataLength       uint16
        FirstRecOffset   uint16
    }
    Data   [496]byte
    Footer struct {
        Checksum uint32
    }
}

func (lb *LogBlock) Verify() bool {
    // Verify header and footer checksums
    // Return true if valid
}
```

### 2. Log Record Parsing

```go
func ParseLogRecord(data []byte) (record interface{}, bytesRead int, err error) {
    if len(data) == 0 {
        return nil, 0, fmt.Errorf("empty data")
    }
    
    recordType := data[0]
    offset := 1
    
    // Check for MLOG_SINGLE_REC_FLAG
    singleRec := (recordType & MLOG_SINGLE_REC_FLAG) != 0
    if singleRec {
        recordType &= ^MLOG_SINGLE_REC_FLAG
    }
    
    // Parse space ID and page number (for most record types)
    spaceID, n, err := ReadCompressedUint(data[offset:])
    if err != nil {
        return nil, 0, err
    }
    offset += n
    
    pageNo, n, err := ReadCompressedUint(data[offset:])
    if err != nil {
        return nil, 0, err
    }
    offset += n
    
    // Parse record-specific data based on type
    switch recordType {
    case MLOG_1BYTE:
        return parseMlog1Byte(data[offset:], spaceID, pageNo)
    case MLOG_2BYTES:
        return parseMlog2Bytes(data[offset:], spaceID, pageNo)
    // ... handle all record types
    default:
        return nil, 0, fmt.Errorf("unknown record type: %d", recordType)
    }
}
```

### 3. Recovery Process

```go
type RecoveryState struct {
    CheckpointLSN   LSN
    CurrentLSN      LSN
    ParsedRecords   []interface{}
    ModifiedPages   map[uint32]map[uint32]bool  // space_id -> page_no -> true
}

func (rs *RecoveryState) Apply(record interface{}) error {
    // Apply the log record to the appropriate page
    // Update ModifiedPages
    return nil
}
```

## References

- Source: `storage/innobase/include/mtr0types.h`
- Source: `storage/innobase/include/log0recv.h`
- Source: `storage/innobase/log/log0log.cc`
- Source: `storage/innobase/log/log0recv.cc`

## Notes

1. **Version Differences**: Records with "_8027" suffix are from MySQL 8.0.27 and earlier. New versions use the non-suffixed variants.

2. **Compression**: Most multi-byte integers are compressed to save space.

3. **Single Record Flag**: When an MTR contains only one record for a page, the flag MLOG_SINGLE_REC_FLAG (128) is ORed with the record type.

4. **Crash Recovery**: During recovery, records are parsed from checkpoint LSN to end of log, then applied to pages.

5. **Checksums**: Both log blocks and individual pages have checksums for integrity.

6. **Page Types**: Different page types (INDEX, UNDO, INODE, etc.) have different record types.

7. **Compressed Pages**: Special record types (MLOG_ZIP_*) handle compressed pages.

8. **R-Tree**: Spatial indexes use special R-Tree record types.

9. **SDI**: Serialized Dictionary Information pages have dedicated record types.

10. **Testing**: MLOG_TEST (66) should never appear in production logs.
