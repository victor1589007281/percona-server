package rseg

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseRSEGArrayPage 解析Rollback Segment Array Page
func ParseRSEGArrayPage(page *types.Page) (*types.RSEGArrayPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_RSEG_ARRAY {
return nil, types.NewError(1, "Not a RSEG_ARRAY page")
}

rsegPage := &types.RSEGArrayPage{
Page:        page,
RSegPageNos: make([]uint32, types.RSEG_ARRAY_SIZE),
}

offset := types.RSEG_ARRAY_PAGES_OFFSET

// 解析rollback segment page号数组
for i := uint32(0); i < types.RSEG_ARRAY_SIZE; i++ {
pageNoOffset := offset + i*types.RSEG_ARRAY_PAGE_NO_SIZE
if pageNoOffset+types.RSEG_ARRAY_PAGE_NO_SIZE <= page.PageSize-types.FIL_PAGE_DATA_END {
rsegPage.RSegPageNos[i] = binary.BigEndian.Uint32(page.Data[pageNoOffset:])
}
}

return rsegPage, nil
}

// WriteRSEGArrayPage 写入Rollback Segment Array
func WriteRSEGArrayPage(page *types.Page, rsegs []uint32) error {
if uint32(len(rsegs)) > types.RSEG_ARRAY_SIZE {
return types.NewError(1, "Too many rollback segments")
}

offset := types.RSEG_ARRAY_PAGES_OFFSET

for i := uint32(0); i < uint32(len(rsegs)); i++ {
pageNoOffset := offset + i*types.RSEG_ARRAY_PAGE_NO_SIZE
if pageNoOffset+types.RSEG_ARRAY_PAGE_NO_SIZE <= page.PageSize-types.FIL_PAGE_DATA_END {
binary.BigEndian.PutUint32(page.Data[pageNoOffset:], rsegs[i])
}
}

return nil
}
