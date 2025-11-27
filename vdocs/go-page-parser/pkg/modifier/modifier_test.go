package modifier

import (
"encoding/binary"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createTestPage() *types.Page {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 初始化Page Header
binary.BigEndian.PutUint32(data[types.FIL_PAGE_OFFSET:], 1)       // Page number
binary.BigEndian.PutUint32(data[types.FIL_PAGE_SPACE_ID:], 100)   // Space ID
binary.BigEndian.PutUint32(data[types.FIL_PAGE_PREV:], 0)         // Prev page
binary.BigEndian.PutUint32(data[types.FIL_PAGE_NEXT:], 2)         // Next page
binary.BigEndian.PutUint16(data[types.FIL_PAGE_TYPE:], types.FIL_PAGE_INDEX)

return &types.Page{
Data:       data,
PageSize:   types.UNIV_PAGE_SIZE_DEF,
PageNumber: 1,
SpaceID:    100,
PrevPage:   0,
NextPage:   2,
PageType:   types.FIL_PAGE_INDEX,
}
}

func TestSetPageNumber(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

newPageNum := uint32(999)
mod.SetPageNumber(newPageNum)

if page.PageNumber != newPageNum {
t.Errorf("PageNumber = %d; want %d", page.PageNumber, newPageNum)
}

// 验证数据已更新
actual := binary.BigEndian.Uint32(page.Data[types.FIL_PAGE_OFFSET:])
if actual != newPageNum {
t.Errorf("Data PageNumber = %d; want %d", actual, newPageNum)
}
}

func TestSetSpaceID(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

newSpaceID := uint32(888)
mod.SetSpaceID(newSpaceID)

if page.SpaceID != newSpaceID {
t.Errorf("SpaceID = %d; want %d", page.SpaceID, newSpaceID)
}

// 验证数据已更新
actual := binary.BigEndian.Uint32(page.Data[types.FIL_PAGE_SPACE_ID:])
if actual != newSpaceID {
t.Errorf("Data SpaceID = %d; want %d", actual, newSpaceID)
}
}

func TestSetPrevNextPage(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

newPrev := uint32(10)
newNext := uint32(20)

mod.SetPrevPage(newPrev)
mod.SetNextPage(newNext)

if page.PrevPage != newPrev {
t.Errorf("PrevPage = %d; want %d", page.PrevPage, newPrev)
}

if page.NextPage != newNext {
t.Errorf("NextPage = %d; want %d", page.NextPage, newNext)
}
}

func TestWriteData(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

testData := []byte{0x11, 0x22, 0x33, 0x44}
offset := uint32(100)

err := mod.WriteData(offset, testData)
if err != nil {
t.Fatalf("WriteData failed: %v", err)
}

// 验证数据已写入
for i, b := range testData {
if page.Data[offset+uint32(i)] != b {
t.Errorf("Data[%d] = 0x%02X; want 0x%02X", offset+uint32(i), page.Data[offset+uint32(i)], b)
}
}
}

func TestWriteDataBeyondBoundary(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

testData := make([]byte, 100)
offset := uint32(len(page.Data) - 50)

err := mod.WriteData(offset, testData)
if err == nil {
t.Error("Expected error when writing beyond page boundary")
}
}

func TestUpdateChecksum(t *testing.T) {
page := createTestPage()
mod := NewModifier(page)

oldChecksum := page.Checksum

// 修改一些数据
mod.SetPageNumber(999)

// 更新Checksum
mod.UpdateChecksum(types.ChecksumCRC32)

// Checksum应该已改变
if page.Checksum == oldChecksum {
t.Error("Checksum should change after updating")
}
}
