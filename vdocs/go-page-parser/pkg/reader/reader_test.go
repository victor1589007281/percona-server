package reader

import (
"os"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createTestIBDFile(t *testing.T, filename string, pageCount int) {
f, err := os.Create(filename)
if err != nil {
t.Fatal(err)
}
defer f.Close()

// 创建测试Page
pageSize := types.UNIV_PAGE_SIZE_DEF
for i := 0; i < pageCount; i++ {
data := make([]byte, pageSize)
// 写入Page号
data[types.FIL_PAGE_OFFSET] = byte(i >> 24)
data[types.FIL_PAGE_OFFSET+1] = byte(i >> 16)
data[types.FIL_PAGE_OFFSET+2] = byte(i >> 8)
data[types.FIL_PAGE_OFFSET+3] = byte(i)

// 写入Page类型
if i == 0 {
data[types.FIL_PAGE_TYPE] = 0x00
data[types.FIL_PAGE_TYPE+1] = 0x08 // FSP_HDR
} else {
data[types.FIL_PAGE_TYPE] = 0x45
data[types.FIL_PAGE_TYPE+1] = 0xBF // INDEX
}

if _, err := f.Write(data); err != nil {
t.Fatal(err)
}
}
}

func TestOpen(t *testing.T) {
testFile := "/tmp/test_reader.ibd"
defer os.Remove(testFile)

createTestIBDFile(t, testFile, 10)

r, err := Open(testFile)
if err != nil {
t.Fatalf("Failed to open file: %v", err)
}
defer r.Close()

if r.GetPageSize() != types.UNIV_PAGE_SIZE_DEF {
t.Errorf("Page size = %d; want %d", r.GetPageSize(), types.UNIV_PAGE_SIZE_DEF)
}

if r.GetPageCount() != 10 {
t.Errorf("Page count = %d; want 10", r.GetPageCount())
}
}

func TestReadPage(t *testing.T) {
testFile := "/tmp/test_reader.ibd"
defer os.Remove(testFile)

createTestIBDFile(t, testFile, 10)

r, err := Open(testFile)
if err != nil {
t.Fatalf("Failed to open file: %v", err)
}
defer r.Close()

// 读取Page 0
page, err := r.ReadPage(0)
if err != nil {
t.Fatalf("Failed to read page: %v", err)
}

if page.PageNumber != 0 {
t.Errorf("Page number = %d; want 0", page.PageNumber)
}

if page.PageType != types.FIL_PAGE_TYPE_FSP_HDR {
t.Errorf("Page type = 0x%04X; want 0x%04X", page.PageType, types.FIL_PAGE_TYPE_FSP_HDR)
}
}

func TestReadPages(t *testing.T) {
testFile := "/tmp/test_reader.ibd"
defer os.Remove(testFile)

createTestIBDFile(t, testFile, 10)

r, err := Open(testFile)
if err != nil {
t.Fatalf("Failed to open file: %v", err)
}
defer r.Close()

// 批量读取Page
pages, err := r.ReadPages(0, 5)
if err != nil {
t.Fatalf("Failed to read pages: %v", err)
}

if len(pages) != 5 {
t.Errorf("Pages count = %d; want 5", len(pages))
}

for i, page := range pages {
if page.PageNumber != uint32(i) {
t.Errorf("Page %d number = %d; want %d", i, page.PageNumber, i)
}
}
}
