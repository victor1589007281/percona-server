package fsp

import (
"encoding/binary"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createTestFSPPage() *types.Page {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 设置Page类型为FSP_HDR
binary.BigEndian.PutUint16(data[types.FIL_PAGE_TYPE:], types.FIL_PAGE_TYPE_FSP_HDR)

// 填充FSP Header
fspData := data[types.FIL_PAGE_DATA:]
binary.BigEndian.PutUint32(fspData[types.FSP_SPACE_ID:], 123)        // Space ID
binary.BigEndian.PutUint32(fspData[types.FSP_SIZE:], 1000)           // Size
binary.BigEndian.PutUint32(fspData[types.FSP_FREE_LIMIT:], 500)      // Free limit
binary.BigEndian.PutUint32(fspData[types.FSP_SPACE_FLAGS:], 0x0001)  // Flags
binary.BigEndian.PutUint32(fspData[types.FSP_FRAG_N_USED:], 10)      // Frag N Used
binary.BigEndian.PutUint64(fspData[types.FSP_SEG_ID:], 42)           // Seg ID

return &types.Page{
Data:     data,
PageSize: types.UNIV_PAGE_SIZE_DEF,
PageType: types.FIL_PAGE_TYPE_FSP_HDR,
}
}

func TestParseFSPHeader(t *testing.T) {
page := createTestFSPPage()

header, err := ParseFSPHeader(page)
if err != nil {
t.Fatalf("ParseFSPHeader failed: %v", err)
}

if header.SpaceID != 123 {
t.Errorf("SpaceID = %d; want 123", header.SpaceID)
}

if header.Size != 1000 {
t.Errorf("Size = %d; want 1000", header.Size)
}

if header.FreeLimit != 500 {
t.Errorf("FreeLimit = %d; want 500", header.FreeLimit)
}

if header.SpaceFlags != 0x0001 {
t.Errorf("SpaceFlags = 0x%04X; want 0x0001", header.SpaceFlags)
}

if header.FragNUsed != 10 {
t.Errorf("FragNUsed = %d; want 10", header.FragNUsed)
}

if header.SegID != 42 {
t.Errorf("SegID = %d; want 42", header.SegID)
}
}

func TestParseFSPHeaderWrongPageType(t *testing.T) {
page := createTestFSPPage()
page.PageType = types.FIL_PAGE_INDEX
binary.BigEndian.PutUint16(page.Data[types.FIL_PAGE_TYPE:], types.FIL_PAGE_INDEX)

_, err := ParseFSPHeader(page)
if err == nil {
t.Error("Expected error when parsing non-FSP page")
}
}
