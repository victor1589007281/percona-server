package types

// Log format versions
const (
LogFormatLegacy   uint32 = 0
LogFormat_5_7_9   uint32 = 1
LogFormat_8_0_1   uint32 = 2
LogFormat_8_0_3   uint32 = 3
LogFormat_8_0_19  uint32 = 4
LogFormat_8_0_28  uint32 = 5
LogFormat_8_0_30  uint32 = 6 // Current version
LogFormatCurrent         = LogFormat_8_0_30
)

// Block constants
const (
OSFileLogBlockSize  = 512 // Standard log block size
LogBlockHdrSize     = 12  // Block header size
LogBlockTrlSize     = 4   // Block trailer size
LogBlockDataSize    = OSFileLogBlockSize - LogBlockHdrSize - LogBlockTrlSize
LogBlockFlushBitMask = uint32(0x80000000)
LogBlockMaxNo       = 0x3FFFFFFF + 1
)

// Block header offsets
const (
LogBlockHdrNo          = 0  // Block number offset
LogBlockHdrDataLen     = 4  // Data length offset
LogBlockFirstRecGroup  = 6  // First record group offset
LogBlockEpochNo        = 8  // Epoch number offset
)

// Block trailer offset
const (
LogBlockChecksum = 4 // Checksum offset from end
)

// File header constants
const (
LogFileHdrSize        = 4 * OSFileLogBlockSize // 2048 bytes
LogStartLsn           = 16 * OSFileLogBlockSize // 8192
LogCheckpoint1        = OSFileLogBlockSize
LogEncryption         = 2 * OSFileLogBlockSize
LogCheckpoint2        = 3 * OSFileLogBlockSize
)

// File header field offsets
const (
LogHeaderFormat              = 0
LogHeaderLogUuid             = 4
LogHeaderStartLsn            = 8
LogHeaderCreator             = 16
LogHeaderCreatorMaxLength    = 31
LogHeaderCreatorEnd          = LogHeaderCreator + LogHeaderCreatorMaxLength + 1
LogHeaderFlags               = LogHeaderCreatorEnd
)

// Log header flags
const (
LogHeaderFlagNoLogging      = 1 << 0 // Redo logging disabled
LogHeaderFlagCrashUnsafe    = 1 << 1 // Not recoverable on crash
LogHeaderFlagNotInitialized = 1 << 2 // Data directory not initialized
LogHeaderFlagFileFull       = 1 << 3 // File completely full
)

// Checkpoint header offset
const (
LogCheckpointLsn = 8 // Checkpoint LSN offset
)

// Special values
const (
LogNoChecksumMagic = 0xDEADBEEF // Magic value when checksums disabled
LsnMax             = (1 << 63) - 1
)

// MLOG record types
const (
MlogSingleRecFlag = 128 // Single record flag

// Simple byte write types
Mlog1Byte  = 1
Mlog2Bytes = 2
Mlog4Bytes = 4
Mlog8Bytes = 8

// Record operations (8.0.27 compatibility)
MlogRecInsert8027          = 9
MlogRecClustDeleteMark8027 = 10
MlogRecSecDeleteMark       = 11
MlogRecUpdateInPlace8027   = 13
MlogRecDelete8027          = 14
MlogListEndDelete8027      = 15
MlogListStartDelete8027    = 16
MlogListEndCopyCreated8027 = 17
MlogPageReorganize8027     = 18

// Page operations
MlogPageCreate = 19

// Undo log operations
MlogUndoInsert    = 20
MlogUndoEraseEnd  = 21
MlogUndoInit      = 22
MlogUndoHdrReuse  = 24
MlogUndoHdrCreate = 25

// Other operations
MlogRecMinMark       = 26
MlogIbufBitmapInit   = 27
MlogInitFilePage     = 29 // Deprecated
MlogWriteString      = 30
MlogMultiRecEnd      = 31
MlogDummyRecord      = 32
MlogFileCreate       = 33
MlogFileRename       = 34
MlogFileDelete       = 35

// Compact format operations
MlogCompRecMinMark        = 36
MlogCompPageCreate        = 37
MlogCompRecInsert8027     = 38
MlogCompRecClustDeleteMark8027 = 39
MlogCompRecSecDeleteMark  = 40
MlogCompRecUpdateInPlace8027 = 41
MlogCompRecDelete8027     = 42
MlogCompListEndDelete8027 = 43
MlogCompListStartDelete8027 = 44
MlogCompListEndCopyCreated8027 = 45
MlogCompPageReorganize8027 = 46

// Compressed page operations
MlogZipWriteNodePtr        = 48
MlogZipWriteBlobPtr        = 49
MlogZipWriteHeader         = 50
MlogZipPageCompress        = 51
MlogZipPageCompressNoData8027 = 52
MlogZipPageReorganize8027  = 53

// R-Tree operations
MlogPageCreateRtree     = 57
MlogCompPageCreateRtree = 58

// New operations
MlogInitFilePage2       = 59
MlogIndexLoad           = 61
MlogTableDynamicMeta    = 62
MlogPageCreateSdi       = 63
MlogCompPageCreateSdi   = 64
MlogFileExtend          = 65
MlogTest                = 66

// Current version record types (8.0.28+)
MlogRecInsert               = 67
MlogRecClustDeleteMark      = 68
MlogRecDelete               = 69
MlogRecUpdateInPlace        = 70
MlogListEndCopyCreated      = 71
MlogPageReorganize          = 72
MlogZipPageReorganize       = 73
MlogZipPageCompressNoData   = 74
MlogListEndDelete           = 75
MlogListStartDelete         = 76

MlogBiggestType = MlogListStartDelete
)

// Initial log record info size
const RedoLogInitialInfoSize = 11 // Type(1) + SpaceID(max5) + PageNo(max5)
