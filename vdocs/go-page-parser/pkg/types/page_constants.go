package types

// Index Page Header 常量 (从 PAGE_HEADER = 38 开始)
const (
// PAGE_HEADER Index page header起始偏移
PAGE_HEADER uint32 = FIL_PAGE_DATA // 38

// PAGE_N_DIR_SLOTS 目录槽数量 (2 bytes)
PAGE_N_DIR_SLOTS uint32 = 0

// PAGE_HEAP_TOP 堆顶指针 (2 bytes)
PAGE_HEAP_TOP uint32 = 2

// PAGE_N_HEAP 堆中记录数，bit 15=新式紧凑格式标志 (2 bytes)
PAGE_N_HEAP uint32 = 4

// PAGE_FREE 空闲记录链表开始 (2 bytes)
PAGE_FREE uint32 = 6

// PAGE_GARBAGE 已删除记录字节数 (2 bytes)
PAGE_GARBAGE uint32 = 8

// PAGE_LAST_INSERT 最后插入记录指针 (2 bytes)
PAGE_LAST_INSERT uint32 = 10

// PAGE_DIRECTION 最后插入方向 (1 byte)
PAGE_DIRECTION uint32 = 12

// PAGE_N_DIRECTION 同方向连续插入次数 (2 bytes)
PAGE_N_DIRECTION uint32 = 14

// PAGE_N_RECS 用户记录数 (2 bytes)
PAGE_N_RECS uint32 = 16

// PAGE_MAX_TRX_ID 最大事务ID (8 bytes)
PAGE_MAX_TRX_ID uint32 = 18

// PAGE_LEVEL B-tree层级，0为叶子节点 (2 bytes)
PAGE_LEVEL uint32 = 26

// PAGE_INDEX_ID 索引ID (8 bytes)
PAGE_INDEX_ID uint32 = 28

// PAGE_BTR_SEG_LEAF 叶子节点段头 (10 bytes)
PAGE_BTR_SEG_LEAF uint32 = 36

// PAGE_BTR_SEG_TOP 非叶子节点段头 (10 bytes)
PAGE_BTR_SEG_TOP uint32 = 36 + 10
)

// Page Directory 常量
const (
// PAGE_DIR 目录从page末尾开始的偏移
PAGE_DIR uint32 = FIL_PAGE_DATA_END // 8

// PAGE_DIR_SLOT_SIZE 目录槽大小 (2 bytes)
PAGE_DIR_SLOT_SIZE uint32 = 2

// PAGE_DIR_SLOT_MAX_N_OWNED 目录槽最大拥有记录数
PAGE_DIR_SLOT_MAX_N_OWNED uint32 = 8

// PAGE_DIR_SLOT_MIN_N_OWNED 目录槽最小拥有记录数
PAGE_DIR_SLOT_MIN_N_OWNED uint32 = 4
)

// Undo Log Page 常量
const (
// TRX_UNDO_PAGE_HDR Undo log page header起始 = 38
TRX_UNDO_PAGE_HDR uint32 = FIL_PAGE_DATA

// TRX_UNDO_PAGE_TYPE TRX_UNDO_INSERT 或 TRX_UNDO_UPDATE (2 bytes)
TRX_UNDO_PAGE_TYPE uint32 = 0

// TRX_UNDO_PAGE_START 最新事务undo记录开始位置 (2 bytes)
TRX_UNDO_PAGE_START uint32 = 2

// TRX_UNDO_PAGE_FREE 第一个空闲字节偏移 (2 bytes)
TRX_UNDO_PAGE_FREE uint32 = 4

// TRX_UNDO_PAGE_NODE 文件链表节点 (12 bytes)
TRX_UNDO_PAGE_NODE uint32 = 6

// TRX_UNDO_PAGE_HDR_SIZE Undo page header大小
TRX_UNDO_PAGE_HDR_SIZE uint32 = 6 + 12 // FLST_NODE_SIZE = 12
)

// Undo Segment Header 常量 (在第一页)
const (
// TRX_UNDO_SEG_HDR Segment header偏移
TRX_UNDO_SEG_HDR uint32 = TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE

// TRX_UNDO_STATE TRX_UNDO_ACTIVE, TRX_UNDO_CACHED等 (2 bytes)
TRX_UNDO_STATE uint32 = 0

// TRX_UNDO_LAST_LOG 最后一个undo log header偏移 (2 bytes)
TRX_UNDO_LAST_LOG uint32 = 2

// TRX_UNDO_FSEG_HEADER 文件段头 (10 bytes)
TRX_UNDO_FSEG_HEADER uint32 = 4

// TRX_UNDO_PAGE_LIST Page链表基节点 (16 bytes)
TRX_UNDO_PAGE_LIST uint32 = 4 + 10
)

// Undo Log Header 常量
const (
// TRX_UNDO_TRX_ID 事务ID (8 bytes)
TRX_UNDO_TRX_ID uint32 = 0

// TRX_UNDO_TRX_NO 事务编号 (8 bytes)
TRX_UNDO_TRX_NO uint32 = 8

// TRX_UNDO_DEL_MARKS 是否有删除标记 (2 bytes)
TRX_UNDO_DEL_MARKS uint32 = 16

// TRX_UNDO_LOG_START 第一个undo记录偏移 (2 bytes)
TRX_UNDO_LOG_START uint32 = 18

// TRX_UNDO_FLAGS Undo标志 (1 byte)
TRX_UNDO_FLAGS uint32 = 20

// TRX_UNDO_DICT_TRANS 是否为DDL事务 (1 byte)
TRX_UNDO_DICT_TRANS uint32 = 21

// TRX_UNDO_TABLE_ID 表ID (8 bytes)
TRX_UNDO_TABLE_ID uint32 = 22

// TRX_UNDO_NEXT_LOG 下一个undo log header偏移 (2 bytes)
TRX_UNDO_NEXT_LOG uint32 = 30

// TRX_UNDO_PREV_LOG 前一个undo log header偏移 (2 bytes)
TRX_UNDO_PREV_LOG uint32 = 32

// TRX_UNDO_HISTORY_NODE 历史链表节点 (12 bytes)
TRX_UNDO_HISTORY_NODE uint32 = 34
)

// Inode Page 常量
const (
// FSEG_INODE_PAGE_NODE Inode page链表节点 (12 bytes)
FSEG_INODE_PAGE_NODE uint32 = FIL_PAGE_DATA

// FSEG_ARR_OFFSET Inode数组起始偏移
FSEG_ARR_OFFSET uint32 = FIL_PAGE_DATA + 12

// FSEG_ID Segment ID (8 bytes)
FSEG_ID uint32 = 0

// FSEG_NOT_FULL_N_USED 非满extent中已使用page数 (4 bytes)
FSEG_NOT_FULL_N_USED uint32 = 8

// FSEG_FREE 空闲extent链表 (16 bytes)
FSEG_FREE uint32 = 12

// FSEG_NOT_FULL 部分使用extent链表 (16 bytes)
FSEG_NOT_FULL uint32 = 28

// FSEG_FULL 满extent链表 (16 bytes)
FSEG_FULL uint32 = 44

// FSEG_MAGIC_N Magic number (4 bytes)
FSEG_MAGIC_N uint32 = 60

// FSEG_FRAG_ARR 碎片page数组 (32 * 4 = 128 bytes)
FSEG_FRAG_ARR uint32 = 64

// FSEG_FRAG_ARR_N_SLOTS 碎片数组槽位数
FSEG_FRAG_ARR_N_SLOTS uint32 = 32

// FSEG_INODE_SIZE 单个inode大小
FSEG_INODE_SIZE uint32 = 192
)

// XDES (Extent Descriptor) 常量
const (
// XDES_ID Segment ID (8 bytes)
XDES_ID uint32 = 0

// XDES_FLST_NODE 链表节点 (12 bytes)
XDES_FLST_NODE uint32 = 8

// XDES_STATE Extent状态 (4 bytes)
XDES_STATE uint32 = 20

// XDES_BITMAP Page状态位图 (16 bytes for 64 pages)
XDES_BITMAP uint32 = 24

// XDES_SIZE Descriptor大小
XDES_SIZE uint32 = 40

// Extent状态常量
XDES_FREE      uint32 = 1 // 空闲extent
XDES_FREE_FRAG uint32 = 2 // 部分使用的extent
XDES_FULL_FRAG uint32 = 3 // 完全使用的extent
XDES_FSEG      uint32 = 4 // 属于某个segment的extent
)

// TRX_SYS (Transaction System) Page 常量
const (
// TRX_SYS Transaction system header偏移
TRX_SYS uint32 = FIL_PAGE_DATA

// TRX_SYS_TRX_ID_STORE 事务ID存储 (8 bytes)
TRX_SYS_TRX_ID_STORE uint32 = 0

// TRX_SYS_FSEG_HEADER 文件段头 (10 bytes)
TRX_SYS_FSEG_HEADER uint32 = 8

// TRX_SYS_RSEGS Rollback segment数组 (128 * 8 = 1024 bytes)
TRX_SYS_RSEGS uint32 = 18

// TRX_SYS_MYSQL_LOG_INFO MySQL binlog信息 (4096 bytes)
TRX_SYS_MYSQL_LOG_INFO uint32 = 18 + 1024
)

// RSEG_ARRAY Page 常量
const (
// RSEG_ARRAY_PAGES_OFFSET Rollback segment page数组偏移
RSEG_ARRAY_PAGES_OFFSET uint32 = FIL_PAGE_DATA

// RSEG_ARRAY_SIZE 数组大小
RSEG_ARRAY_SIZE uint32 = 128

// RSEG_ARRAY_PAGE_NO_SIZE 单个page号大小 (4 bytes)
RSEG_ARRAY_PAGE_NO_SIZE uint32 = 4
)

// LOB (Large Object) 相关常量
const (
// LOB_HDR_SIZE LOB header大小
LOB_HDR_SIZE uint32 = 64

// LOB_INDEX_SIZE LOB index大小
LOB_INDEX_SIZE uint32 = 60
)

// SDI (Serialized Dictionary Information) 常量
const (
// SDI_VERSION SDI版本 (4 bytes)
SDI_VERSION uint32 = 0

// SDI_TYPE SDI类型 (4 bytes)
SDI_TYPE uint32 = 4

// SDI_COMPRESSED 是否压缩 (4 bytes)
SDI_COMPRESSED uint32 = 8

// SDI_DATA_LEN 数据长度 (4 bytes)
SDI_DATA_LEN uint32 = 12
)

// 插入方向常量
const (
PAGE_LEFT  uint8 = 1
PAGE_RIGHT uint8 = 2
PAGE_SAME  uint8 = 3
PAGE_NO_DIRECTION uint8 = 5
)

// Undo类型常量
const (
TRX_UNDO_INSERT uint32 = 1 // Insert undo
TRX_UNDO_UPDATE uint32 = 2 // Update undo
)

// Undo状态常量
const (
TRX_UNDO_ACTIVE    uint32 = 1 // 活跃状态
TRX_UNDO_CACHED    uint32 = 2 // 缓存状态
TRX_UNDO_TO_FREE   uint32 = 3 // 待释放
TRX_UNDO_TO_PURGE  uint32 = 4 // 待清理
TRX_UNDO_PREPARED  uint32 = 6 // 准备状态
)

// Undo标志常量
const (
TRX_UNDO_FLAG_XID             uint8 = 0x01 // XID存在
TRX_UNDO_FLAG_GTID            uint8 = 0x02 // GTID存在
TRX_UNDO_FLAG_XA_PREPARE_GTID uint8 = 0x04 // XA PREPARE GTID
)

// FLST (File List) 常量
const (
// FLST_BASE_NODE_SIZE 基节点大小
FLST_BASE_NODE_SIZE uint32 = 16

// FLST_NODE_SIZE 链表节点大小
FLST_NODE_SIZE uint32 = 12

// FLST_LEN 链表长度 (4 bytes)
FLST_LEN uint32 = 0

// FLST_FIRST 第一个节点指针 (6 bytes)
FLST_FIRST uint32 = 4

// FLST_LAST 最后一个节点指针 (6 bytes)
FLST_LAST uint32 = 10

// FLST_PREV 前一个节点指针 (6 bytes)
FLST_PREV uint32 = 0

// FLST_NEXT 下一个节点指针 (6 bytes)
FLST_NEXT uint32 = 6
)
