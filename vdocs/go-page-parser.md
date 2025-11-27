# InnoDB Page Parser - Go Implementation Guide

## Overview
This document provides a comprehensive guide for implementing an InnoDB page parser in Go for Percona Server. It covers all page types and page format structures.

## InnoDB Page Structure

### Standard Page Size
- Default: 16 KB (16384 bytes)
- Configurable: 4 KB, 8 KB, 16 KB, 32 KB, 64 KB
- Page size is set at tablespace creation and cannot be changed

### Page Layout

Every InnoDB page consists of three main parts:

```
+------------------+
|   FIL Header     |  38 bytes
+------------------+
|   Page Body      |  Variable (depends on page type)
+------------------+
|   FIL Trailer    |  8 bytes
+------------------+
```

## FIL Header (38 bytes)

The File Header (FIL Header) is common to all page types.

| Offset | Size | Field Name | Description |
|--------|------|------------|-------------|
| 0 | 4 | FIL_PAGE_SPACE_OR_CHKSUM | Checksum (new) or 0 in old format |
| 4 | 4 | FIL_PAGE_OFFSET | Page number within tablespace |
| 8 | 4 | FIL_PAGE_PREV | Previous page number (for linked pages) |
| 12 | 4 | FIL_PAGE_NEXT | Next page number (for linked pages) |
| 16 | 8 | FIL_PAGE_LSN | Log Sequence Number of last page modification |
| 24 | 2 | FIL_PAGE_TYPE | Page type (see below) |
| 26 | 8 | FIL_PAGE_FILE_FLUSH_LSN | LSN at which file was flushed (page 0 only) |
| 34 | 4 | FIL_PAGE_SPACE_ID | Tablespace ID |

### Go Structure for FIL Header

```go
type FilHeader struct {
    Checksum    uint32  // FIL_PAGE_SPACE_OR_CHKSUM
    PageNo      uint32  // FIL_PAGE_OFFSET
    PrevPage    uint32  // FIL_PAGE_PREV
    NextPage    uint32  // FIL_PAGE_NEXT
    LSN         uint64  // FIL_PAGE_LSN
    PageType    uint16  // FIL_PAGE_TYPE
    FlushLSN    uint64  // FIL_PAGE_FILE_FLUSH_LSN
    SpaceID     uint32  // FIL_PAGE_SPACE_ID
}

const (
    FIL_PAGE_SPACE_OR_CHKSUM = 0
    FIL_PAGE_OFFSET          = 4
    FIL_PAGE_PREV            = 8
    FIL_PAGE_NEXT            = 12
    FIL_PAGE_LSN             = 16
    FIL_PAGE_TYPE            = 24
    FIL_PAGE_FILE_FLUSH_LSN  = 26
    FIL_PAGE_SPACE_ID        = 34
    FIL_PAGE_DATA            = 38  // Start of page body
)
```

## FIL Trailer (8 bytes)

The trailer is at the end of every page.

| Offset from end | Size | Field Name | Description |
|-----------------|------|------------|-------------|
| -8 | 4 | FIL_PAGE_END_LSN_OLD_CHKSUM | Old checksum (low 4 bytes) |
| -4 | 4 | FIL_PAGE_END_LSN | Low 4 bytes of FIL_PAGE_LSN |

```go
type FilTrailer struct {
    OldChecksum uint32  // FIL_PAGE_END_LSN_OLD_CHKSUM
    LSNLow      uint32  // FIL_PAGE_END_LSN (should match low 4 bytes of header LSN)
}

const (
    FIL_PAGE_DATA_END = 8  // Size of trailer
)
```

## Complete Page Type Reference

```go
const (
    // Basic page types
    FIL_PAGE_TYPE_ALLOCATED             = 0   // Freshly allocated page
    FIL_PAGE_INDEX                      = 17855  // B-tree node (0x45BF)
    FIL_PAGE_RTREE                      = 17854  // R-tree node
    FIL_PAGE_SDI                        = 17853  // Serialized Dictionary Information
    FIL_PAGE_UNDO_LOG                   = 2   // Undo log page
    FIL_PAGE_INODE                      = 3   // Index node (file segment inode)
    FIL_PAGE_IBUF_FREE_LIST             = 4   // Insert buffer free list
    FIL_PAGE_IBUF_BITMAP                = 5   // Insert buffer bitmap
    FIL_PAGE_TYPE_SYS                   = 6   // System page
    FIL_PAGE_TYPE_TRX_SYS               = 7   // Transaction system data
    FIL_PAGE_TYPE_FSP_HDR               = 8   // File space header
    FIL_PAGE_TYPE_XDES                  = 9   // Extent descriptor page
    
    // BLOB page types
    FIL_PAGE_TYPE_BLOB                  = 10  // Uncompressed BLOB page
    FIL_PAGE_TYPE_ZBLOB                 = 11  // First compressed BLOB page
    FIL_PAGE_TYPE_ZBLOB2                = 12  // Subsequent compressed BLOB page
    
    // Special page types
    FIL_PAGE_TYPE_UNKNOWN               = 13  // Unknown (garbage replaced)
    FIL_PAGE_COMPRESSED                 = 14  // Compressed page
    FIL_PAGE_ENCRYPTED                  = 15  // Encrypted page
    FIL_PAGE_COMPRESSED_AND_ENCRYPTED   = 16  // Compressed and encrypted
    FIL_PAGE_ENCRYPTED_RTREE            = 17  // Encrypted R-tree page
    
    // SDI BLOB types
    FIL_PAGE_SDI_BLOB                   = 18  // Uncompressed SDI BLOB page
    FIL_PAGE_SDI_ZBLOB                  = 19  // Compressed SDI BLOB page
    
    // Doublewrite buffer
    FIL_PAGE_TYPE_LEGACY_DBLWR          = 20  // Legacy doublewrite buffer page
    
    // Rollback segment
    FIL_PAGE_TYPE_RSEG_ARRAY            = 21  // Rollback Segment Array page
    
    // LOB (Large Object) types
    FIL_PAGE_TYPE_LOB_INDEX             = 22  // Index pages of uncompressed LOB
    FIL_PAGE_TYPE_LOB_DATA              = 23  // Data pages of uncompressed LOB
    FIL_PAGE_TYPE_LOB_FIRST             = 24  // First page of uncompressed LOB
    FIL_PAGE_TYPE_ZLOB_FIRST            = 25  // First page of compressed LOB
    FIL_PAGE_TYPE_ZLOB_DATA             = 26  // Data pages of compressed LOB
    FIL_PAGE_TYPE_ZLOB_INDEX            = 27  // Index pages of compressed LOB
    FIL_PAGE_TYPE_ZLOB_FRAG             = 28  // Fragment pages of compressed LOB
    FIL_PAGE_TYPE_ZLOB_FRAG_ENTRY       = 29  // Index of fragment pages (compressed LOB)
    
    FIL_PAGE_TYPE_LAST                  = FIL_PAGE_TYPE_ZLOB_FRAG_ENTRY
)
```

## Page Type Details

### 1. FIL_PAGE_INDEX (17855) - B-Tree Index Page

The most common page type, used for B-tree indexes (both clustered and secondary).

**Additional Header (in page body)**:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 38 | 2 | PAGE_N_DIR_SLOTS | Number of directory slots |
| 40 | 2 | PAGE_HEAP_TOP | Pointer to record heap top |
| 42 | 2 | PAGE_N_HEAP | Number of records in heap (bit 15: compact format flag) |
| 44 | 2 | PAGE_FREE | Pointer to start of free record list |
| 46 | 2 | PAGE_GARBAGE | Number of bytes in deleted records |
| 48 | 2 | PAGE_LAST_INSERT | Pointer to last inserted record |
| 50 | 2 | PAGE_DIRECTION | Last insert direction |
| 52 | 2 | PAGE_N_DIRECTION | Number of consecutive inserts in same direction |
| 54 | 2 | PAGE_N_RECS | Number of user records |
| 56 | 8 | PAGE_MAX_TRX_ID | Highest trx ID that may have modified a record |
| 64 | 2 | PAGE_LEVEL | Level in index tree (0 = leaf) |
| 66 | 8 | PAGE_INDEX_ID | Index ID |
| 74 | 20 | PAGE_BTR_SEG_LEAF | File segment header for leaf pages |
| 94 | 20 | PAGE_BTR_SEG_TOP | File segment header for non-leaf pages |

```go
type IndexPageHeader struct {
    NDirSlots    uint16  // Number of directory slots
    HeapTop      uint16  // Heap top pointer
    NHeap        uint16  // Number of records (bit 15 = format flag)
    Free         uint16  // Free list pointer
    Garbage      uint16  // Deleted bytes
    LastInsert   uint16  // Last insert pointer
    Direction    uint16  // Insert direction
    NDirection   uint16  // Consecutive inserts in direction
    NRecs        uint16  // Number of user records
    MaxTrxID     uint64  // Maximum transaction ID
    Level        uint16  // Page level (0 = leaf)
    IndexID      uint64  // Index ID
}

const (
    PAGE_HEADER       = 38  // Start of index page header
    PAGE_N_DIR_SLOTS  = 38
    PAGE_HEAP_TOP     = 40
    PAGE_N_HEAP       = 42
    PAGE_FREE         = 44
    PAGE_GARBAGE      = 46
    PAGE_LAST_INSERT  = 48
    PAGE_DIRECTION    = 50
    PAGE_N_DIRECTION  = 52
    PAGE_N_RECS       = 54
    PAGE_MAX_TRX_ID   = 56
    PAGE_LEVEL        = 64
    PAGE_INDEX_ID     = 66
)
```

**Record Format**:
- **Redundant Format**: Old format, deprecated
- **Compact Format**: Default since MySQL 5.0
- **Dynamic Format**: Default since MySQL 5.7, optimized for large fields
- **Compressed Format**: For ROW_FORMAT=COMPRESSED tables

### 2. FIL_PAGE_RTREE (17854) - R-Tree Index Page

Used for spatial indexes (GEOMETRY columns).

Structure similar to B-tree but with:
- Different split algorithms
- Minimum Bounding Rectangle (MBR) handling
- Split Sequence Number at FIL_RTREE_SPLIT_SEQ_NUM

### 3. FIL_PAGE_SDI (17853) - Serialized Dictionary Information

Contains serialized data dictionary information.

### 4. FIL_PAGE_UNDO_LOG (2) - Undo Log Page

Stores undo log records for transaction rollback and MVCC.

**Structure**:
```go
type UndoPageHeader struct {
    Type         uint16  // TRX_UNDO_INSERT or TRX_UNDO_UPDATE
    Latest       uint16  // Offset to latest undo log header
    FreeList     uint16  // Free space list
    ListNode     uint64  // List node for undo page list
}
```

### 5. FIL_PAGE_INODE (3) - Index Node Page

Contains file segment inodes (metadata for file segments).

### 6. FIL_PAGE_IBUF_FREE_LIST (4) - Insert Buffer Free List

Manages free pages in the insert buffer.

### 7. FIL_PAGE_IBUF_BITMAP (5) - Insert Buffer Bitmap

Bitmap tracking insert buffer availability for pages.

### 8. FIL_PAGE_TYPE_SYS (6) - System Page

System-level page (rarely used).

### 9. FIL_PAGE_TYPE_TRX_SYS (7) - Transaction System Page

Contains transaction system header (page 5 of system tablespace).

**Structure**:
- Transaction system header
- MySQL binlog information
- Doublewrite buffer information
- Rollback segment pointers

### 10. FIL_PAGE_TYPE_FSP_HDR (8) - File Space Header

First page (page 0) of every tablespace.

**Structure**:
```go
type FileSpaceHeader struct {
    SpaceID          uint32  // Tablespace ID
    NotUsed          uint32  // Not used
    Size             uint32  // Current size in pages
    FreeLimit        uint32  // Free page limit
    Flags            uint32  // Tablespace flags
    FragNUsed        uint32  // Number of used pages in fragment extent
    FreeList         uint64  // Free extent list
    FreeFragList     uint64  // Free fragment extent list
    FullFragList     uint64  // Full fragment extent list
    SegID            uint64  // Next unused segment ID
    SegInodeFullList uint64  // List of full segment inode pages
    SegInodeFreeList uint64  // List of free segment inode pages
}
```

### 11. FIL_PAGE_TYPE_XDES (9) - Extent Descriptor Page

Describes extent allocation information.

**Extent**: Group of 64 consecutive pages (1 MB for 16KB pages).

### 12. FIL_PAGE_TYPE_BLOB (10) - Uncompressed BLOB Page

Stores externally stored column data (TEXT, BLOB).

**Structure**:
- FIL header
- Blob header (contains length info)
- Blob data
- FIL trailer

### 13-14. FIL_PAGE_TYPE_ZBLOB (11-12) - Compressed BLOB Pages

Compressed versions of BLOB pages.
- Type 11: First compressed BLOB page
- Type 12: Subsequent compressed BLOB pages

### 15. FIL_PAGE_COMPRESSED (14) - Compressed Page

Page compressed using hole-punch compression (Transparent Page Compression).

**Compression Info** (in FIL_PAGE_FILE_FLUSH_LSN area):
```go
type CompressedPageInfo struct {
    Version       uint8   // Control information version
    Algorithm     uint8   // Compression algorithm
    OriginalType  uint16  // Original page type before compression
    OriginalSize  uint16  // Original data size
    CompressSize  uint16  // Size after compression
}

const (
    FIL_PAGE_VERSION         = 26  // Version
    FIL_PAGE_ALGORITHM_V1    = 27  // Algorithm
    FIL_PAGE_ORIGINAL_TYPE_V1 = 28  // Original type
    FIL_PAGE_ORIGINAL_SIZE_V1 = 30  // Original size
    FIL_PAGE_COMPRESS_SIZE_V1 = 32  // Compressed size
)
```

### 16. FIL_PAGE_ENCRYPTED (15) - Encrypted Page

Page encrypted using InnoDB tablespace encryption.

### 17. FIL_PAGE_COMPRESSED_AND_ENCRYPTED (16)

Page that is both compressed and encrypted.

### 18. FIL_PAGE_ENCRYPTED_RTREE (17)

Encrypted R-tree page.

### 19-20. FIL_PAGE_SDI_BLOB / FIL_PAGE_SDI_ZBLOB (18-19)

BLOB pages for Serialized Dictionary Information.

### 21. FIL_PAGE_TYPE_LEGACY_DBLWR (20)

Legacy doublewrite buffer page (MySQL 5.7 and earlier).

### 22. FIL_PAGE_TYPE_RSEG_ARRAY (21)

Rollback Segment Array page (maps undo tablespace slots).

### 23-29. LOB Page Types (22-29)

Large Object (LOB) pages for handling TEXT/BLOB columns in new format:

- **FIL_PAGE_TYPE_LOB_INDEX (22)**: LOB index page (uncompressed)
- **FIL_PAGE_TYPE_LOB_DATA (23)**: LOB data page (uncompressed)
- **FIL_PAGE_TYPE_LOB_FIRST (24)**: First LOB page (uncompressed)
- **FIL_PAGE_TYPE_ZLOB_FIRST (25)**: First LOB page (compressed)
- **FIL_PAGE_TYPE_ZLOB_DATA (26)**: LOB data page (compressed)
- **FIL_PAGE_TYPE_ZLOB_INDEX (27)**: LOB index page (compressed)
- **FIL_PAGE_TYPE_ZLOB_FRAG (28)**: LOB fragment page (compressed)
- **FIL_PAGE_TYPE_ZLOB_FRAG_ENTRY (29)**: LOB fragment index (compressed)

## Checksum Algorithms

InnoDB supports multiple checksum algorithms:

```go
const (
    SRV_CHECKSUM_ALGORITHM_CRC32     = 0  // CRC32 (default, fastest)
    SRV_CHECKSUM_ALGORITHM_STRICT_CRC32 = 1
    SRV_CHECKSUM_ALGORITHM_INNODB    = 2  // InnoDB checksum
    SRV_CHECKSUM_ALGORITHM_STRICT_INNODB = 3
    SRV_CHECKSUM_ALGORITHM_NONE      = 4  // No checksum
    SRV_CHECKSUM_ALGORITHM_STRICT_NONE = 5
)

// CRC32 checksum (MySQL 5.7+)
func CalculatePageChecksumCRC32(page []byte) uint32 {
    // Calculate CRC32 over page[4:FIL_PAGE_FILE_FLUSH_LSN] and 
    // page[FIL_PAGE_DATA:pageSize-FIL_PAGE_DATA_END]
    crc := crc32.ChecksumIEEE(page[4:FIL_PAGE_FILE_FLUSH_LSN])
    crc = crc32.Update(crc, crc32.IEEETable, page[FIL_PAGE_DATA:len(page)-FIL_PAGE_DATA_END])
    return crc
}

// InnoDB checksum (legacy)
func CalculatePageChecksumInnoDB(page []byte) uint32 {
    // Fold-based checksum (legacy algorithm)
    // Implementation details in storage/innobase/ut/ut0crc32.cc
}
```

## Page Parsing Example

```go
type Page struct {
    Header    FilHeader
    Body      []byte
    Trailer   FilTrailer
    PageSize  int
}

func ParsePage(data []byte) (*Page, error) {
    if len(data) < 64 {  // Minimum page size
        return nil, fmt.Errorf("data too short")
    }
    
    page := &Page{
        PageSize: len(data),
        Body: data[FIL_PAGE_DATA : len(data)-FIL_PAGE_DATA_END],
    }
    
    // Parse FIL header
    page.Header.Checksum = binary.BigEndian.Uint32(data[0:4])
    page.Header.PageNo = binary.BigEndian.Uint32(data[4:8])
    page.Header.PrevPage = binary.BigEndian.Uint32(data[8:12])
    page.Header.NextPage = binary.BigEndian.Uint32(data[12:16])
    page.Header.LSN = binary.BigEndian.Uint64(data[16:24])
    page.Header.PageType = binary.BigEndian.Uint16(data[24:26])
    page.Header.FlushLSN = binary.BigEndian.Uint64(data[26:34])
    page.Header.SpaceID = binary.BigEndian.Uint32(data[34:38])
    
    // Parse FIL trailer
    trailerOffset := len(data) - FIL_PAGE_DATA_END
    page.Trailer.OldChecksum = binary.BigEndian.Uint32(data[trailerOffset : trailerOffset+4])
    page.Trailer.LSNLow = binary.BigEndian.Uint32(data[trailerOffset+4 : trailerOffset+8])
    
    // Verify LSN consistency
    headerLSNLow := uint32(page.Header.LSN & 0xFFFFFFFF)
    if page.Trailer.LSNLow != headerLSNLow {
        return nil, fmt.Errorf("LSN mismatch: header=%08x, trailer=%08x", 
                               headerLSNLow, page.Trailer.LSNLow)
    }
    
    // Verify checksum
    if !VerifyPageChecksum(data, page.Header.Checksum) {
        return nil, fmt.Errorf("checksum verification failed")
    }
    
    return page, nil
}

func (p *Page) ParseIndexPage() (*IndexPageHeader, error) {
    if p.Header.PageType != FIL_PAGE_INDEX {
        return nil, fmt.Errorf("not an index page")
    }
    
    if len(p.Body) < 76 {  // Minimum index header size
        return nil, fmt.Errorf("invalid index page")
    }
    
    hdr := &IndexPageHeader{}
    data := p.Body
    
    hdr.NDirSlots = binary.BigEndian.Uint16(data[0:2])
    hdr.HeapTop = binary.BigEndian.Uint16(data[2:4])
    hdr.NHeap = binary.BigEndian.Uint16(data[4:6])
    hdr.Free = binary.BigEndian.Uint16(data[6:8])
    hdr.Garbage = binary.BigEndian.Uint16(data[8:10])
    hdr.LastInsert = binary.BigEndian.Uint16(data[10:12])
    hdr.Direction = binary.BigEndian.Uint16(data[12:14])
    hdr.NDirection = binary.BigEndian.Uint16(data[14:16])
    hdr.NRecs = binary.BigEndian.Uint16(data[16:18])
    hdr.MaxTrxID = binary.BigEndian.Uint64(data[18:26])
    hdr.Level = binary.BigEndian.Uint16(data[26:28])
    hdr.IndexID = binary.BigEndian.Uint64(data[28:36])
    
    return hdr, nil
}
```

## Record Format

### Compact Record Format

**Record Header** (5 or 6 bytes):
- Info flags (4 bits)
- Number of records owned (4 bits)
- Heap number (13 bits)
- Record type (3 bits)
- Next record offset (16 bits)

**Null Bitmap**:
- 1 bit per nullable column

**Variable Length Field Lengths**:
- 1 or 2 bytes per variable-length field

**Record Data**:
- Actual column data

## Tablespace Flags

```go
func ParseTablespaceFlags(flags uint32) map[string]interface{} {
    return map[string]interface{}{
        "post_antelope":   (flags & 0x1) != 0,
        "zip_ssize":       (flags >> 1) & 0xF,  // 0=uncompressed, 1-5=compressed
        "atomic_blobs":    (flags & 0x20) != 0,
        "page_ssize":      (flags >> 6) & 0xF,
        "data_dir":        (flags & 0x400) != 0,
        "shared":          (flags & 0x800) != 0,
        "temporary":       (flags & 0x1000) != 0,
        "encryption":      (flags & 0x2000) != 0,
        "sdi":             (flags & 0x4000) != 0,
    }
}
```

## References

- Source: `storage/innobase/include/fil0fil.h`
- Source: `storage/innobase/include/fil0types.h`
- Source: `storage/innobase/include/page0types.h`
- Source: `storage/innobase/include/page0page.h`

## Implementation Checklist

- [ ] Parse FIL header (all page types)
- [ ] Parse FIL trailer (all page types)
- [ ] Verify checksums (CRC32, InnoDB legacy)
- [ ] Verify LSN consistency
- [ ] Parse INDEX pages (B-tree)
- [ ] Parse RTREE pages (spatial indexes)
- [ ] Parse UNDO_LOG pages
- [ ] Parse FSP_HDR pages (tablespace header)
- [ ] Parse XDES pages (extent descriptors)
- [ ] Parse BLOB pages (external storage)
- [ ] Parse LOB pages (new LOB format)
- [ ] Handle compressed pages
- [ ] Handle encrypted pages
- [ ] Parse record format (compact/dynamic)
- [ ] Parse page directory
- [ ] Handle all page sizes (4K, 8K, 16K, 32K, 64K)

## Notes

1. **Page Numbers**: Start from 0. Page 0 is always FSP_HDR.

2. **Linked Pages**: B-tree pages at the same level are doubly-linked via PREV/NEXT.

3. **LSN**: Log Sequence Number must match between header and trailer.

4. **Checksums**: Can be CRC32, InnoDB legacy, or none. Algorithm is configurable.

5. **Compression**: Transparent page compression is different from table compression.

6. **Encryption**: Tablespace encryption encrypts pages at rest.

7. **SDI**: Serialized Dictionary Information stores data dictionary in tablespace.

8. **LOB**: New LOB format (MySQL 8.0+) uses multiple page types for large objects.

9. **Page Type Constants**: FIL_PAGE_INDEX=17855 (0x45BF) is a special constant, not sequential.

10. **Byte Order**: All multi-byte integers are stored in big-endian format.
