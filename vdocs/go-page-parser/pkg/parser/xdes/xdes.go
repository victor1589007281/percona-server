package xdes

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseXDESPage 解析Extent Descriptor Page
func ParseXDESPage(page *types.Page) (*types.XDESPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_XDES {
return nil, types.NewError(1, "Not an XDES page")
}

xdesPage := &types.XDESPage{
Page:    page,
Entries: make([]*types.XDESEntry, 0),
}

// XDES从FIL_PAGE_DATA开始
offset := types.FIL_PAGE_DATA

// 计算可以容纳多少个extent descriptor
maxEntries := (page.PageSize - offset - types.FIL_PAGE_DATA_END) / types.XDES_SIZE

for i := uint32(0); i < maxEntries; i++ {
entryOffset := offset + i*types.XDES_SIZE
if entryOffset+types.XDES_SIZE > page.PageSize-types.FIL_PAGE_DATA_END {
break
}

data := page.Data[entryOffset:]

entry := &types.XDESEntry{
SegmentID: binary.BigEndian.Uint64(data[types.XDES_ID:]),
State:     binary.BigEndian.Uint32(data[types.XDES_STATE:]),
Bitmap:    make([]byte, 16),
}

// 复制bitmap
copy(entry.Bitmap, data[types.XDES_BITMAP:types.XDES_BITMAP+16])

xdesPage.Entries = append(xdesPage.Entries, entry)
}

return xdesPage, nil
}

// WriteXDESEntry 写入Extent Descriptor Entry
func WriteXDESEntry(page *types.Page, index uint32, entry *types.XDESEntry) error {
offset := types.FIL_PAGE_DATA + index*types.XDES_SIZE

if offset+types.XDES_SIZE > page.PageSize-types.FIL_PAGE_DATA_END {
return types.NewError(1, "XDES index out of range")
}

data := page.Data[offset:]

binary.BigEndian.PutUint64(data[types.XDES_ID:], entry.SegmentID)
binary.BigEndian.PutUint32(data[types.XDES_STATE:], entry.State)
copy(data[types.XDES_BITMAP:], entry.Bitmap)

return nil
}
