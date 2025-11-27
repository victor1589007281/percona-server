package index

import (
"encoding/binary"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createTestIndexPage() *types.Page {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 设置FIL Header
binary.BigEndian.PutUint16(data[types.FIL_PAGE_TYPE:], types.FIL_PAGE_INDEX)
binary.BigEndian.PutUint32(data[types.FIL_PAGE_OFFSET:], 1)

// 设置Index Page Header
pageHeader := data[types.PAGE_HEADER:]
binary.BigEndian.PutUint16(pageHeader[types.PAGE_N_DIR_SLOTS:], 2)
binary.BigEndian.PutUint16(pageHeader[types.PAGE_HEAP_TOP:], 200)
binary.BigEndian.PutUint16(pageHeader[types.PAGE_N_HEAP:], 0x8005) // 5 records, compact format
binary.BigEndian.PutUint16(pageHeader[types.PAGE_N_RECS:], 3)
binary.BigEndian.PutUint64(pageHeader[types.PAGE_MAX_TRX_ID:], 12345)
binary.BigEndian.PutUint16(pageHeader[types.PAGE_LEVEL:], 0)
binary.BigEndian.PutUint64(pageHeader[types.PAGE_INDEX_ID:], 100)

return &types.Page{
Data:       data,
PageSize:   types.UNIV_PAGE_SIZE_DEF,
PageNumber: 1,
PageType:   types.FIL_PAGE_INDEX,
}
}

func TestParseIndexPage(t *testing.T) {
page := createTestIndexPage()

indexPage, err := ParseIndexPage(page)
if err != nil {
t.Fatalf("ParseIndexPage failed: %v", err)
}

if indexPage.Header.NDirSlots != 2 {
t.Errorf("NDirSlots = %d; want 2", indexPage.Header.NDirSlots)
}

if indexPage.Header.HeapTop != 200 {
t.Errorf("HeapTop = %d; want 200", indexPage.Header.HeapTop)
}

if indexPage.Header.NHeap != 5 {
t.Errorf("NHeap = %d; want 5", indexPage.Header.NHeap)
}

if !indexPage.Header.IsCompact {
t.Error("IsCompact should be true")
}

if indexPage.Header.NRecs != 3 {
t.Errorf("NRecs = %d; want 3", indexPage.Header.NRecs)
}

if indexPage.Header.MaxTrxID != 12345 {
t.Errorf("MaxTrxID = %d; want 12345", indexPage.Header.MaxTrxID)
}

if indexPage.Header.Level != 0 {
t.Errorf("Level = %d; want 0", indexPage.Header.Level)
}

if indexPage.Header.IndexID != 100 {
t.Errorf("IndexID = %d; want 100", indexPage.Header.IndexID)
}
}

func TestWriteIndexPageHeader(t *testing.T) {
page := createTestIndexPage()

header := &types.IndexPageHeader{
NDirSlots:  5,
HeapTop:    300,
NHeap:      10,
NRecs:      8,
MaxTrxID:   99999,
Level:      1,
IndexID:    200,
IsCompact:  true,
Direction:  types.PAGE_RIGHT,
NDirection: 5,
}

WriteIndexPageHeader(page, header)

// 重新解析验证
indexPage, err := ParseIndexPage(page)
if err != nil {
t.Fatalf("ParseIndexPage failed: %v", err)
}

if indexPage.Header.NDirSlots != 5 {
t.Errorf("NDirSlots = %d; want 5", indexPage.Header.NDirSlots)
}

if indexPage.Header.Level != 1 {
t.Errorf("Level = %d; want 1", indexPage.Header.Level)
}

if indexPage.Header.IndexID != 200 {
t.Errorf("IndexID = %d; want 200", indexPage.Header.IndexID)
}
}
