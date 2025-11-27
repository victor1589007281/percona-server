package types

// IndexPageHeader Index Page头部信息
type IndexPageHeader struct {
NDirSlots    uint16 // 目录槽数量
HeapTop      uint16 // 堆顶指针
NHeap        uint16 // 堆中记录数
Free         uint16 // 空闲记录链表
Garbage      uint16 // 已删除记录字节数
LastInsert   uint16 // 最后插入记录
Direction    uint8  // 插入方向
NDirection   uint16 // 连续插入方向次数
NRecs        uint16 // 用户记录数
MaxTrxID     uint64 // 最大事务ID
Level        uint16 // B-tree层级
IndexID      uint64 // 索引ID
IsCompact    bool   // 是否为新式紧凑格式
}

// IndexPage Index Page (B-tree页)
type IndexPage struct {
Page   *Page
Header *IndexPageHeader
}

// UndoPageHeader Undo Log Page头部
type UndoPageHeader struct {
PageType  uint16 // TRX_UNDO_INSERT或TRX_UNDO_UPDATE
PageStart uint16 // 最新事务undo记录开始位置
PageFree  uint16 // 第一个空闲字节偏移
}

// UndoSegHeader Undo Segment头部
type UndoSegHeader struct {
State   uint16 // Segment状态
LastLog uint16 // 最后一个undo log header偏移
}

// UndoLogHeader Undo Log头部
type UndoLogHeader struct {
TrxID      uint64 // 事务ID
TrxNo      uint64 // 事务编号
DelMarks   uint16 // 是否有删除标记
LogStart   uint16 // 第一个undo记录偏移
Flags      uint8  // Undo标志
DictTrans  uint8  // 是否为DDL事务
TableID    uint64 // 表ID
NextLog    uint16 // 下一个undo log header偏移
PrevLog    uint16 // 前一个undo log header偏移
}

// UndoPage Undo Log Page
type UndoPage struct {
Page      *Page
Header    *UndoPageHeader
SegHeader *UndoSegHeader // 只在第一页存在
LogHeader *UndoLogHeader // 可能有多个
}

// FSEGInode File Segment Inode
type FSEGInode struct {
SegmentID     uint64     // Segment ID
NotFullNUsed  uint32     // 非满extent中已使用page数
MagicN        uint32     // Magic number
FragmentPages []uint32   // 碎片page数组
}

// InodePage Inode Page
type InodePage struct {
Page   *Page
Inodes []*FSEGInode // Inode数组
}

// XDESEntry Extent Descriptor Entry
type XDESEntry struct {
SegmentID uint64 // Segment ID
State     uint32 // Extent状态
Bitmap    []byte // Page状态位图
}

// XDESPage Extent Descriptor Page
type XDESPage struct {
Page    *Page
Entries []*XDESEntry // Extent descriptor数组
}

// TRXSysHeader Transaction System头部
type TRXSysHeader struct {
TrxIDStore uint64   // 事务ID存储
RSegs      []uint32 // Rollback segment page号数组
}

// TRXSysPage Transaction System Page
type TRXSysPage struct {
Page   *Page
Header *TRXSysHeader
}

// RSEGArrayPage Rollback Segment Array Page
type RSEGArrayPage struct {
Page          *Page
RSegPageNos   []uint32 // Rollback segment page号数组
}

// LOBPageHeader LOB Page头部
type LOBPageHeader struct {
Version   uint32 // 版本
DataLen   uint32 // 数据长度
TrxID     uint64 // 事务ID
}

// LOBIndexEntry LOB Index Entry
type LOBIndexEntry struct {
PageNo   uint32 // Page号
Offset   uint32 // 偏移
Length   uint32 // 长度
}

// LOBIndexPage LOB Index Page
type LOBIndexPage struct {
Page    *Page
Header  *LOBPageHeader
Entries []*LOBIndexEntry
}

// LOBDataPage LOB Data Page
type LOBDataPage struct {
Page   *Page
Header *LOBPageHeader
Data   []byte // 实际数据
}

// LOBFirstPage LOB First Page
type LOBFirstPage struct {
Page      *Page
Header    *LOBPageHeader
LOBLen    uint64 // LOB总长度
IndexPage uint32 // Index page号
}

// SDIHeader SDI头部
type SDIHeader struct {
Version    uint32 // SDI版本
Type       uint32 // SDI类型
Compressed uint32 // 是否压缩
DataLen    uint32 // 数据长度
}

// SDIPage SDI Page
type SDIPage struct {
Page   *Page
Header *SDIHeader
Data   []byte // SDI数据(JSON格式)
}

// FilAddr File Address
type FilAddr struct {
PageNo uint32 // Page号
Offset uint32 // Byte偏移
}

// FLSTNode File List Node
type FLSTNode struct {
Prev *FilAddr // 前一个节点
Next *FilAddr // 下一个节点
}

// FLSTBase File List Base Node
type FLSTBase struct {
Len   uint32   // 链表长度
First *FilAddr // 第一个节点
Last  *FilAddr // 最后一个节点
}
