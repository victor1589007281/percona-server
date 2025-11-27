package types

// FIL Header 偏移量常量 (38 bytes)
const (
// FIL_PAGE_SPACE_OR_CHKSUM Checksum or space id (4 bytes)
FIL_PAGE_SPACE_OR_CHKSUM uint32 = 0

// FIL_PAGE_OFFSET Page number (4 bytes)
FIL_PAGE_OFFSET uint32 = 4

// FIL_PAGE_PREV Previous page number (4 bytes)
FIL_PAGE_PREV uint32 = 8

// FIL_PAGE_NEXT Next page number (4 bytes)
FIL_PAGE_NEXT uint32 = 12

// FIL_PAGE_LSN Log sequence number (8 bytes)
FIL_PAGE_LSN uint32 = 16

// FIL_PAGE_TYPE Page type (2 bytes)
FIL_PAGE_TYPE uint32 = 24

// FIL_PAGE_FILE_FLUSH_LSN File flush LSN (8 bytes)
FIL_PAGE_FILE_FLUSH_LSN uint32 = 26

// FIL_PAGE_SPACE_ID Space ID (4 bytes)
FIL_PAGE_SPACE_ID uint32 = 34

// FIL_PAGE_DATA Start of data (38 bytes from page start)
FIL_PAGE_DATA uint32 = 38
)

// FIL Trailer 常量 (8 bytes from page end)
const (
// FIL_PAGE_DATA_END Trailer size
FIL_PAGE_DATA_END uint32 = 8

// FIL_PAGE_END_LSN_OLD_CHKSUM Old checksum value (4 bytes)
FIL_PAGE_END_LSN_OLD_CHKSUM uint32 = 0
)

// FSP Header 偏移量 (从FIL_PAGE_DATA开始)
const (
// FSP_SPACE_ID Space ID (4 bytes)
FSP_SPACE_ID uint32 = 0

// FSP_NOT_USED Unused (4 bytes)
FSP_NOT_USED uint32 = 4

// FSP_SIZE Tablespace size in pages (4 bytes)
FSP_SIZE uint32 = 8

// FSP_FREE_LIMIT Free limit (4 bytes)
FSP_FREE_LIMIT uint32 = 12

// FSP_SPACE_FLAGS Space flags (4 bytes)
FSP_SPACE_FLAGS uint32 = 16

// FSP_FRAG_N_USED Number of used fragment pages (4 bytes)
FSP_FRAG_N_USED uint32 = 20

// FSP_SEG_ID Next segment ID (8 bytes)
FSP_SEG_ID uint32 = 72
)

// Page size 常量
const (
// UNIV_PAGE_SIZE_MIN Minimum page size (4 KB)
UNIV_PAGE_SIZE_MIN uint32 = 4096
// UNIV_PAGE_SIZE_DEF Default page size (16 KB)
UNIV_PAGE_SIZE_DEF uint32 = 16384
// UNIV_PAGE_SIZE_MAX Maximum page size (64 KB)
UNIV_PAGE_SIZE_MAX uint32 = 65536
// UNIV_ZIP_SIZE_MIN Minimum compressed page size (1 KB)
UNIV_ZIP_SIZE_MIN uint32 = 1024
)

// Page 类型常量
const (
// FIL_PAGE_INDEX B-tree node (0x45BF)
FIL_PAGE_INDEX uint16 = 0x45BF

// FIL_PAGE_UNDO_LOG Undo log page
FIL_PAGE_UNDO_LOG uint16 = 0x0002

// FIL_PAGE_INODE Index node
FIL_PAGE_INODE uint16 = 0x0003

// FIL_PAGE_IBUF_FREE_LIST Insert buffer free list
FIL_PAGE_IBUF_FREE_LIST uint16 = 0x0004

// FIL_PAGE_TYPE_ALLOCATED Freshly allocated page
FIL_PAGE_TYPE_ALLOCATED uint16 = 0x0000

// FIL_PAGE_IBUF_BITMAP Insert buffer bitmap
FIL_PAGE_IBUF_BITMAP uint16 = 0x0005

// FIL_PAGE_TYPE_SYS System page
FIL_PAGE_TYPE_SYS uint16 = 0x0006

// FIL_PAGE_TYPE_TRX_SYS Transaction system data
FIL_PAGE_TYPE_TRX_SYS uint16 = 0x0007

// FIL_PAGE_TYPE_FSP_HDR File space header
FIL_PAGE_TYPE_FSP_HDR uint16 = 0x0008

// FIL_PAGE_TYPE_XDES Extent descriptor page
FIL_PAGE_TYPE_XDES uint16 = 0x0009

// FIL_PAGE_TYPE_BLOB Uncompressed BLOB page
FIL_PAGE_TYPE_BLOB uint16 = 0x000A

// FIL_PAGE_SDI Serialized Dictionary Information
FIL_PAGE_SDI uint16 = 0x0011

// FIL_PAGE_TYPE_RSEG_ARRAY Rollback segment array page
FIL_PAGE_TYPE_RSEG_ARRAY uint16 = 0x0012

// FIL_PAGE_TYPE_LOB_INDEX LOB index page
FIL_PAGE_TYPE_LOB_INDEX uint16 = 0x0013

// FIL_PAGE_TYPE_LOB_DATA LOB data page
FIL_PAGE_TYPE_LOB_DATA uint16 = 0x0014

// FIL_PAGE_TYPE_LOB_FIRST First page of an uncompressed LOB
FIL_PAGE_TYPE_LOB_FIRST uint16 = 0x0015

// FIL_PAGE_TYPE_ZLOB_FIRST First page of a compressed LOB
FIL_PAGE_TYPE_ZLOB_FIRST uint16 = 0x0016

// FIL_PAGE_TYPE_ZLOB_DATA Data page of a compressed LOB
FIL_PAGE_TYPE_ZLOB_DATA uint16 = 0x0017

// FIL_PAGE_TYPE_ZLOB_INDEX Index page of a compressed LOB
FIL_PAGE_TYPE_ZLOB_INDEX uint16 = 0x0018

// FIL_PAGE_TYPE_ZLOB_FRAG Fragment page of a compressed LOB
FIL_PAGE_TYPE_ZLOB_FRAG uint16 = 0x0019
)
