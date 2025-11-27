package undo

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseUndoPage 解析Undo Log Page
func ParseUndoPage(page *types.Page) (*types.UndoPage, error) {
if page.PageType != types.FIL_PAGE_UNDO_LOG {
return nil, types.NewError(1, "Not an undo log page")
}

// 解析Undo Page Header
data := page.Data[types.TRX_UNDO_PAGE_HDR:]

header := &types.UndoPageHeader{
PageType:  binary.BigEndian.Uint16(data[types.TRX_UNDO_PAGE_TYPE:]),
PageStart: binary.BigEndian.Uint16(data[types.TRX_UNDO_PAGE_START:]),
PageFree:  binary.BigEndian.Uint16(data[types.TRX_UNDO_PAGE_FREE:]),
}

undoPage := &types.UndoPage{
Page:   page,
Header: header,
}

// 如果是第一页(page_num == 某个特定值，这里简化判断)
// 解析Segment Header
if page.PageNumber != 0 { // 简化判断
segData := data[types.TRX_UNDO_PAGE_HDR_SIZE:]
undoPage.SegHeader = &types.UndoSegHeader{
State:   binary.BigEndian.Uint16(segData[types.TRX_UNDO_STATE:]),
LastLog: binary.BigEndian.Uint16(segData[types.TRX_UNDO_LAST_LOG:]),
}
}

return undoPage, nil
}

// ParseUndoLogHeader 解析Undo Log Header
func ParseUndoLogHeader(page *types.Page, offset uint32) (*types.UndoLogHeader, error) {
if offset >= uint32(len(page.Data)) {
return nil, types.NewError(1, "Invalid undo log header offset")
}

data := page.Data[offset:]

header := &types.UndoLogHeader{
TrxID:     binary.BigEndian.Uint64(data[types.TRX_UNDO_TRX_ID:]),
TrxNo:     binary.BigEndian.Uint64(data[types.TRX_UNDO_TRX_NO:]),
DelMarks:  binary.BigEndian.Uint16(data[types.TRX_UNDO_DEL_MARKS:]),
LogStart:  binary.BigEndian.Uint16(data[types.TRX_UNDO_LOG_START:]),
Flags:     data[types.TRX_UNDO_FLAGS],
DictTrans: data[types.TRX_UNDO_DICT_TRANS],
TableID:   binary.BigEndian.Uint64(data[types.TRX_UNDO_TABLE_ID:]),
NextLog:   binary.BigEndian.Uint16(data[types.TRX_UNDO_NEXT_LOG:]),
PrevLog:   binary.BigEndian.Uint16(data[types.TRX_UNDO_PREV_LOG:]),
}

return header, nil
}

// WriteUndoPageHeader 写入Undo Page Header
func WriteUndoPageHeader(page *types.Page, header *types.UndoPageHeader) {
data := page.Data[types.TRX_UNDO_PAGE_HDR:]

binary.BigEndian.PutUint16(data[types.TRX_UNDO_PAGE_TYPE:], header.PageType)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_PAGE_START:], header.PageStart)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_PAGE_FREE:], header.PageFree)
}

// WriteUndoLogHeader 写入Undo Log Header
func WriteUndoLogHeader(page *types.Page, offset uint32, header *types.UndoLogHeader) error {
if offset >= uint32(len(page.Data)) {
return types.NewError(1, "Invalid undo log header offset")
}

data := page.Data[offset:]

binary.BigEndian.PutUint64(data[types.TRX_UNDO_TRX_ID:], header.TrxID)
binary.BigEndian.PutUint64(data[types.TRX_UNDO_TRX_NO:], header.TrxNo)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_DEL_MARKS:], header.DelMarks)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_LOG_START:], header.LogStart)
data[types.TRX_UNDO_FLAGS] = header.Flags
data[types.TRX_UNDO_DICT_TRANS] = header.DictTrans
binary.BigEndian.PutUint64(data[types.TRX_UNDO_TABLE_ID:], header.TableID)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_NEXT_LOG:], header.NextLog)
binary.BigEndian.PutUint16(data[types.TRX_UNDO_PREV_LOG:], header.PrevLog)

return nil
}
