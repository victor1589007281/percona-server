package factory

import (
"encoding/binary"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createGenericTestPage(pageType uint16) *types.Page {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

binary.BigEndian.PutUint16(data[types.FIL_PAGE_TYPE:], pageType)
binary.BigEndian.PutUint32(data[types.FIL_PAGE_OFFSET:], 1)

return &types.Page{
Data:       data,
PageSize:   types.UNIV_PAGE_SIZE_DEF,
PageNumber: 1,
PageType:   pageType,
}
}

func TestGetPageTypeName(t *testing.T) {
tests := []struct {
pageType uint16
expected string
}{
{types.FIL_PAGE_INDEX, "INDEX"},
{types.FIL_PAGE_UNDO_LOG, "UNDO_LOG"},
{types.FIL_PAGE_INODE, "INODE"},
{types.FIL_PAGE_TYPE_FSP_HDR, "FSP_HDR"},
{types.FIL_PAGE_TYPE_XDES, "XDES"},
{types.FIL_PAGE_TYPE_TRX_SYS, "TRX_SYS"},
{types.FIL_PAGE_SDI, "SDI"},
{types.FIL_PAGE_TYPE_RSEG_ARRAY, "RSEG_ARRAY"},
{types.FIL_PAGE_TYPE_LOB_FIRST, "LOB_FIRST"},
{0x9999, "UNKNOWN(0x9999)"},
}

for _, tt := range tests {
t.Run(tt.expected, func(t *testing.T) {
name := GetPageTypeName(tt.pageType)
if name != tt.expected {
t.Errorf("GetPageTypeName(0x%04X) = %s; want %s", tt.pageType, name, tt.expected)
}
})
}
}

func TestIsValidPageType(t *testing.T) {
validTypes := []uint16{
types.FIL_PAGE_TYPE_ALLOCATED,
types.FIL_PAGE_UNDO_LOG,
types.FIL_PAGE_INODE,
types.FIL_PAGE_INDEX,
types.FIL_PAGE_TYPE_FSP_HDR,
types.FIL_PAGE_TYPE_XDES,
types.FIL_PAGE_TYPE_TRX_SYS,
types.FIL_PAGE_SDI,
types.FIL_PAGE_TYPE_RSEG_ARRAY,
types.FIL_PAGE_TYPE_LOB_FIRST,
types.FIL_PAGE_TYPE_ZLOB_DATA,
}

for _, pageType := range validTypes {
if !IsValidPageType(pageType) {
t.Errorf("IsValidPageType(0x%04X) = false; want true", pageType)
}
}

// 测试无效类型
invalidTypes := []uint16{0x9999, 0xFFFF, 0x1234}
for _, pageType := range invalidTypes {
if IsValidPageType(pageType) {
t.Errorf("IsValidPageType(0x%04X) = true; want false", pageType)
}
}
}

func TestParsePageIndex(t *testing.T) {
// 创建一个Index Page
page := createGenericTestPage(types.FIL_PAGE_INDEX)

// 设置Index Page Header
pageHeader := page.Data[types.PAGE_HEADER:]
binary.BigEndian.PutUint16(pageHeader[types.PAGE_N_DIR_SLOTS:], 2)
binary.BigEndian.PutUint16(pageHeader[types.PAGE_N_RECS:], 5)
binary.BigEndian.PutUint64(pageHeader[types.PAGE_INDEX_ID:], 123)

parsed, err := ParsePage(page)
if err != nil {
t.Fatalf("ParsePage failed: %v", err)
}

if parsed.GetPageType() != types.FIL_PAGE_INDEX {
t.Errorf("GetPageType() = 0x%04X; want 0x%04X", parsed.GetPageType(), types.FIL_PAGE_INDEX)
}

if parsed.GetTypeName() != "INDEX" {
t.Errorf("GetTypeName() = %s; want INDEX", parsed.GetTypeName())
}

// 获取具体的IndexPage
var indexPage *types.IndexPage
err = GetSpecificPage(parsed, &indexPage)
if err != nil {
t.Fatalf("GetSpecificPage failed: %v", err)
}

if indexPage.Header.NDirSlots != 2 {
t.Errorf("NDirSlots = %d; want 2", indexPage.Header.NDirSlots)
}

if indexPage.Header.IndexID != 123 {
t.Errorf("IndexID = %d; want 123", indexPage.Header.IndexID)
}
}

func TestParsePageFSP(t *testing.T) {
page := createGenericTestPage(types.FIL_PAGE_TYPE_FSP_HDR)

// 设置FSP Header
fspData := page.Data[types.FIL_PAGE_DATA:]
binary.BigEndian.PutUint32(fspData[types.FSP_SPACE_ID:], 10)
binary.BigEndian.PutUint32(fspData[types.FSP_SIZE:], 1000)

parsed, err := ParsePage(page)
if err != nil {
t.Fatalf("ParsePage failed: %v", err)
}

if parsed.GetTypeName() != "FSP_HDR" {
t.Errorf("GetTypeName() = %s; want FSP_HDR", parsed.GetTypeName())
}

var fspHeader *types.FSPHeader
err = GetSpecificPage(parsed, &fspHeader)
if err != nil {
t.Fatalf("GetSpecificPage failed: %v", err)
}

if fspHeader.SpaceID != 10 {
t.Errorf("SpaceID = %d; want 10", fspHeader.SpaceID)
}

if fspHeader.Size != 1000 {
t.Errorf("Size = %d; want 1000", fspHeader.Size)
}
}
