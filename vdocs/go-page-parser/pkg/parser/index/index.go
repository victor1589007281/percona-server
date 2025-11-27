package index

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseIndexPage 解析Index Page
func ParseIndexPage(page *types.Page) (*types.IndexPage, error) {
if page.PageType != types.FIL_PAGE_INDEX {
return nil, types.NewError(1, "Not an index page")
}

// 解析Page Header
data := page.Data[types.PAGE_HEADER:]

header := &types.IndexPageHeader{
NDirSlots:  binary.BigEndian.Uint16(data[types.PAGE_N_DIR_SLOTS:]),
HeapTop:    binary.BigEndian.Uint16(data[types.PAGE_HEAP_TOP:]),
NHeap:      binary.BigEndian.Uint16(data[types.PAGE_N_HEAP:]),
Free:       binary.BigEndian.Uint16(data[types.PAGE_FREE:]),
Garbage:    binary.BigEndian.Uint16(data[types.PAGE_GARBAGE:]),
LastInsert: binary.BigEndian.Uint16(data[types.PAGE_LAST_INSERT:]),
Direction:  data[types.PAGE_DIRECTION],
NDirection: binary.BigEndian.Uint16(data[types.PAGE_N_DIRECTION:]),
NRecs:      binary.BigEndian.Uint16(data[types.PAGE_N_RECS:]),
MaxTrxID:   binary.BigEndian.Uint64(data[types.PAGE_MAX_TRX_ID:]),
Level:      binary.BigEndian.Uint16(data[types.PAGE_LEVEL:]),
IndexID:    binary.BigEndian.Uint64(data[types.PAGE_INDEX_ID:]),
}

// 检查是否为紧凑格式 (bit 15 of PAGE_N_HEAP)
header.IsCompact = (header.NHeap & 0x8000) != 0
header.NHeap = header.NHeap & 0x7FFF // 清除标志位

return &types.IndexPage{
Page:   page,
Header: header,
}, nil
}

// GetDirectorySlots 获取Page Directory槽
func GetDirectorySlots(indexPage *types.IndexPage) []uint16 {
nSlots := indexPage.Header.NDirSlots
slots := make([]uint16, nSlots)

data := indexPage.Page.Data
pageSize := indexPage.Page.PageSize

// Directory从page末尾向前存储
for i := uint16(0); i < nSlots; i++ {
offset := pageSize - types.PAGE_DIR - (uint32(i)+1)*types.PAGE_DIR_SLOT_SIZE
slots[i] = binary.BigEndian.Uint16(data[offset:])
}

return slots
}

// WriteIndexPageHeader 写入Index Page Header
func WriteIndexPageHeader(page *types.Page, header *types.IndexPageHeader) {
data := page.Data[types.PAGE_HEADER:]

binary.BigEndian.PutUint16(data[types.PAGE_N_DIR_SLOTS:], header.NDirSlots)
binary.BigEndian.PutUint16(data[types.PAGE_HEAP_TOP:], header.HeapTop)

// 设置NHeap并包含紧凑格式标志
nHeap := header.NHeap
if header.IsCompact {
nHeap |= 0x8000
}
binary.BigEndian.PutUint16(data[types.PAGE_N_HEAP:], nHeap)

binary.BigEndian.PutUint16(data[types.PAGE_FREE:], header.Free)
binary.BigEndian.PutUint16(data[types.PAGE_GARBAGE:], header.Garbage)
binary.BigEndian.PutUint16(data[types.PAGE_LAST_INSERT:], header.LastInsert)
data[types.PAGE_DIRECTION] = header.Direction
binary.BigEndian.PutUint16(data[types.PAGE_N_DIRECTION:], header.NDirection)
binary.BigEndian.PutUint16(data[types.PAGE_N_RECS:], header.NRecs)
binary.BigEndian.PutUint64(data[types.PAGE_MAX_TRX_ID:], header.MaxTrxID)
binary.BigEndian.PutUint16(data[types.PAGE_LEVEL:], header.Level)
binary.BigEndian.PutUint64(data[types.PAGE_INDEX_ID:], header.IndexID)
}
