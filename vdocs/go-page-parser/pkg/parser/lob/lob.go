package lob

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseLOBFirstPage 解析LOB First Page
func ParseLOBFirstPage(page *types.Page) (*types.LOBFirstPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_LOB_FIRST && 
   page.PageType != types.FIL_PAGE_TYPE_ZLOB_FIRST {
return nil, types.NewError(1, "Not a LOB first page")
}

data := page.Data[types.FIL_PAGE_DATA:]

header := &types.LOBPageHeader{
Version: binary.BigEndian.Uint32(data[0:]),
DataLen: binary.BigEndian.Uint32(data[4:]),
TrxID:   binary.BigEndian.Uint64(data[8:]),
}

lobFirst := &types.LOBFirstPage{
Page:      page,
Header:    header,
LOBLen:    binary.BigEndian.Uint64(data[16:]),
IndexPage: binary.BigEndian.Uint32(data[24:]),
}

return lobFirst, nil
}

// ParseLOBDataPage 解析LOB Data Page
func ParseLOBDataPage(page *types.Page) (*types.LOBDataPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_LOB_DATA && 
   page.PageType != types.FIL_PAGE_TYPE_ZLOB_DATA {
return nil, types.NewError(1, "Not a LOB data page")
}

data := page.Data[types.FIL_PAGE_DATA:]

header := &types.LOBPageHeader{
Version: binary.BigEndian.Uint32(data[0:]),
DataLen: binary.BigEndian.Uint32(data[4:]),
TrxID:   binary.BigEndian.Uint64(data[8:]),
}

// 实际数据从header之后开始
dataStart := types.FIL_PAGE_DATA + types.LOB_HDR_SIZE
dataEnd := page.PageSize - types.FIL_PAGE_DATA_END

lobData := &types.LOBDataPage{
Page:   page,
Header: header,
Data:   page.Data[dataStart:dataEnd],
}

return lobData, nil
}

// ParseLOBIndexPage 解析LOB Index Page
func ParseLOBIndexPage(page *types.Page) (*types.LOBIndexPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_LOB_INDEX && 
   page.PageType != types.FIL_PAGE_TYPE_ZLOB_INDEX {
return nil, types.NewError(1, "Not a LOB index page")
}

data := page.Data[types.FIL_PAGE_DATA:]

header := &types.LOBPageHeader{
Version: binary.BigEndian.Uint32(data[0:]),
DataLen: binary.BigEndian.Uint32(data[4:]),
TrxID:   binary.BigEndian.Uint64(data[8:]),
}

lobIndex := &types.LOBIndexPage{
Page:    page,
Header:  header,
Entries: make([]*types.LOBIndexEntry, 0),
}

// 解析index entries
entryStart := types.FIL_PAGE_DATA + types.LOB_HDR_SIZE
entrySize := uint32(12) // PageNo(4) + Offset(4) + Length(4)

for offset := entryStart; offset+entrySize <= page.PageSize-types.FIL_PAGE_DATA_END; offset += entrySize {
entryData := page.Data[offset:]
entry := &types.LOBIndexEntry{
PageNo: binary.BigEndian.Uint32(entryData[0:]),
Offset: binary.BigEndian.Uint32(entryData[4:]),
Length: binary.BigEndian.Uint32(entryData[8:]),
}

// 如果PageNo为0，表示没有更多entry
if entry.PageNo == 0 {
break
}

lobIndex.Entries = append(lobIndex.Entries, entry)
}

return lobIndex, nil
}

// WriteLOBPageHeader 写入LOB Page Header
func WriteLOBPageHeader(page *types.Page, header *types.LOBPageHeader) {
data := page.Data[types.FIL_PAGE_DATA:]

binary.BigEndian.PutUint32(data[0:], header.Version)
binary.BigEndian.PutUint32(data[4:], header.DataLen)
binary.BigEndian.PutUint64(data[8:], header.TrxID)
}
