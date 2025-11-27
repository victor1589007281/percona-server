package undo

import (
"encoding/binary"
"testing"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func createTestUndoPage() *types.Page {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 设置FIL Header
binary.BigEndian.PutUint16(data[types.FIL_PAGE_TYPE:], types.FIL_PAGE_UNDO_LOG)
binary.BigEndian.PutUint32(data[types.FIL_PAGE_OFFSET:], 10)

// 设置Undo Page Header
undoHeader := data[types.TRX_UNDO_PAGE_HDR:]
binary.BigEndian.PutUint16(undoHeader[types.TRX_UNDO_PAGE_TYPE:], uint16(types.TRX_UNDO_UPDATE))
binary.BigEndian.PutUint16(undoHeader[types.TRX_UNDO_PAGE_START:], 100)
binary.BigEndian.PutUint16(undoHeader[types.TRX_UNDO_PAGE_FREE:], 500)

return &types.Page{
Data:       data,
PageSize:   types.UNIV_PAGE_SIZE_DEF,
PageNumber: 10,
PageType:   types.FIL_PAGE_UNDO_LOG,
}
}

func TestParseUndoPage(t *testing.T) {
page := createTestUndoPage()

undoPage, err := ParseUndoPage(page)
if err != nil {
t.Fatalf("ParseUndoPage failed: %v", err)
}

if undoPage.Header.PageType != uint16(types.TRX_UNDO_UPDATE) {
t.Errorf("PageType = %d; want %d", undoPage.Header.PageType, types.TRX_UNDO_UPDATE)
}

if undoPage.Header.PageStart != 100 {
t.Errorf("PageStart = %d; want 100", undoPage.Header.PageStart)
}

if undoPage.Header.PageFree != 500 {
t.Errorf("PageFree = %d; want 500", undoPage.Header.PageFree)
}
}

func TestParseUndoLogHeader(t *testing.T) {
page := createTestUndoPage()

// 在特定偏移写入Undo Log Header
offset := uint32(100)
data := page.Data[offset:]
binary.BigEndian.PutUint64(data[types.TRX_UNDO_TRX_ID:], 123456)
binary.BigEndian.PutUint64(data[types.TRX_UNDO_TRX_NO:], 789)
binary.BigEndian.PutUint64(data[types.TRX_UNDO_TABLE_ID:], 999)

logHeader, err := ParseUndoLogHeader(page, offset)
if err != nil {
t.Fatalf("ParseUndoLogHeader failed: %v", err)
}

if logHeader.TrxID != 123456 {
t.Errorf("TrxID = %d; want 123456", logHeader.TrxID)
}

if logHeader.TrxNo != 789 {
t.Errorf("TrxNo = %d; want 789", logHeader.TrxNo)
}

if logHeader.TableID != 999 {
t.Errorf("TableID = %d; want 999", logHeader.TableID)
}
}

func TestWriteUndoPageHeader(t *testing.T) {
page := createTestUndoPage()

header := &types.UndoPageHeader{
PageType:  uint16(types.TRX_UNDO_INSERT),
PageStart: 200,
PageFree:  800,
}

WriteUndoPageHeader(page, header)

// 重新解析验证
undoPage, err := ParseUndoPage(page)
if err != nil {
t.Fatalf("ParseUndoPage failed: %v", err)
}

if undoPage.Header.PageType != uint16(types.TRX_UNDO_INSERT) {
t.Errorf("PageType = %d; want %d", undoPage.Header.PageType, types.TRX_UNDO_INSERT)
}

if undoPage.Header.PageStart != 200 {
t.Errorf("PageStart = %d; want 200", undoPage.Header.PageStart)
}
}
