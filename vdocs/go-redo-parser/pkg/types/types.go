package types

import "time"

// LSN represents Log Sequence Number
type LSN uint64

// SpaceID represents tablespace ID
type SpaceID uint32

// PageNo represents page number within a tablespace
type PageNo uint32

// MlogType represents the type of a redo log record
type MlogType byte

// LogFormat represents the redo log format version
type LogFormat uint32

// LogFlags represents log header flags
type LogFlags uint32

// LogFileHeader represents the main header of a redo log file
type LogFileHeader struct {
Format      LogFormat // Format version
LogUuid     uint32    // UUID for this log file set
StartLsn    LSN       // LSN of first block in file
CreatorName string    // Creator name (e.g., "MySQL 8.0.30")
Flags       LogFlags  // Header flags
}

// LogCheckpointHeader represents a checkpoint header
type LogCheckpointHeader struct {
CheckpointLsn LSN // Checkpoint LSN for recovery start
}

// LogDataBlockHeader represents the header of a log data block
type LogDataBlockHeader struct {
EpochNo       uint32 // Epoch number
HdrNo         uint32 // Block number
DataLen       uint16 // Bytes written to this block
FirstRecGroup uint16 // Offset to first mtr group start
}

// LogBlock represents a complete 512-byte log block
type LogBlock struct {
Header   LogDataBlockHeader
Data     []byte // 496 bytes of actual log records
Checksum uint32 // Block checksum
}

// LogRecord represents a parsed redo log record
type LogRecord struct {
Type    MlogType // Record type (MLOG_*)
SpaceID SpaceID  // Tablespace ID (if applicable)
PageNo  PageNo   // Page number (if applicable)
Offset  uint16   // Offset within page (if applicable)
Data    []byte   // Record-specific data
LSN     LSN      // LSN where this record starts
}

// MTR represents a Mini-Transaction (group of log records)
type MTR struct {
Records  []*LogRecord // List of records in this MTR
StartLsn LSN          // Starting LSN of this MTR
EndLsn   LSN          // Ending LSN of this MTR
}

// LogFileInfo holds metadata about a redo log file
type LogFileInfo struct {
FileID       uint64    // File ID (from filename)
FilePath     string    // Full file path
FileSize     int64     // File size in bytes
StartLsn     LSN       // First LSN in file
EndLsn       LSN       // Last LSN in file
Header       *LogFileHeader // File header
ModifiedTime time.Time // Last modified time
}

// RecoveryStats holds statistics about recovery process
type RecoveryStats struct {
BlocksRead      uint64 // Total blocks read
RecordsParsed   uint64 // Total records parsed
MtrsProcessed   uint64 // Total MTRs processed
BytesProcessed  uint64 // Total bytes processed
CorruptBlocks   uint64 // Corrupt blocks found
StartTime       time.Time
EndTime         time.Time
}

// String returns a human-readable name for MlogType
func (t MlogType) String() string {
names := map[MlogType]string{
Mlog1Byte:              "MLOG_1BYTE",
Mlog2Bytes:             "MLOG_2BYTES",
Mlog4Bytes:             "MLOG_4BYTES",
Mlog8Bytes:             "MLOG_8BYTES",
MlogRecInsert:          "MLOG_REC_INSERT",
MlogRecClustDeleteMark: "MLOG_REC_CLUST_DELETE_MARK",
MlogRecSecDeleteMark:   "MLOG_REC_SEC_DELETE_MARK",
MlogRecUpdateInPlace:   "MLOG_REC_UPDATE_IN_PLACE",
MlogRecDelete:          "MLOG_REC_DELETE",
MlogPageCreate:         "MLOG_PAGE_CREATE",
MlogUndoInsert:         "MLOG_UNDO_INSERT",
MlogUndoEraseEnd:       "MLOG_UNDO_ERASE_END",
MlogUndoInit:           "MLOG_UNDO_INIT",
MlogWriteString:        "MLOG_WRITE_STRING",
MlogMultiRecEnd:        "MLOG_MULTI_REC_END",
MlogDummyRecord:        "MLOG_DUMMY_RECORD",
MlogFileCreate:         "MLOG_FILE_CREATE",
MlogFileRename:         "MLOG_FILE_RENAME",
MlogFileDelete:         "MLOG_FILE_DELETE",
MlogPageReorganize:     "MLOG_PAGE_REORGANIZE",
MlogCompPageCreate:     "MLOG_COMP_PAGE_CREATE",
MlogInitFilePage2:      "MLOG_INIT_FILE_PAGE2",
MlogIndexLoad:          "MLOG_INDEX_LOAD",
MlogFileExtend:         "MLOG_FILE_EXTEND",
}

if name, ok := names[t]; ok {
return name
}
return "MLOG_UNKNOWN"
}

// IsSingleRec checks if this type has the single record flag set
func (t MlogType) IsSingleRec() bool {
return (byte(t) & MlogSingleRecFlag) != 0
}

// BaseType returns the type without the single record flag
func (t MlogType) BaseType() MlogType {
return MlogType(byte(t) & ^byte(MlogSingleRecFlag))
}
