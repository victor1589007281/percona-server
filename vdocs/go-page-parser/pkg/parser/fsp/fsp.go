package fsp

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseFSPHeader 解析FSP Header (Page 0)
func ParseFSPHeader(page *types.Page) (*types.FSPHeader, error) {
if page.PageType != types.FIL_PAGE_TYPE_FSP_HDR {
return nil, types.NewError(1, "Not a FSP header page")
}

data := page.Data[types.FIL_PAGE_DATA:]

header := &types.FSPHeader{
SpaceID:    binary.BigEndian.Uint32(data[types.FSP_SPACE_ID:]),
Size:       binary.BigEndian.Uint32(data[types.FSP_SIZE:]),
FreeLimit:  binary.BigEndian.Uint32(data[types.FSP_FREE_LIMIT:]),
SpaceFlags: binary.BigEndian.Uint32(data[types.FSP_SPACE_FLAGS:]),
FragNUsed:  binary.BigEndian.Uint32(data[types.FSP_FRAG_N_USED:]),
SegID:      binary.BigEndian.Uint64(data[types.FSP_SEG_ID:]),
}

return header, nil
}
