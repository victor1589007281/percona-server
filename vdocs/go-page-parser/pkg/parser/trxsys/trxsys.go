package trxsys

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseTRXSysPage 解析Transaction System Page
func ParseTRXSysPage(page *types.Page) (*types.TRXSysPage, error) {
if page.PageType != types.FIL_PAGE_TYPE_TRX_SYS {
return nil, types.NewError(1, "Not a TRX_SYS page")
}

data := page.Data[types.TRX_SYS:]

header := &types.TRXSysHeader{
TrxIDStore: binary.BigEndian.Uint64(data[types.TRX_SYS_TRX_ID_STORE:]),
RSegs:      make([]uint32, types.RSEG_ARRAY_SIZE),
}

// 解析Rollback segment数组
rsegsOffset := types.TRX_SYS_RSEGS
for i := uint32(0); i < types.RSEG_ARRAY_SIZE; i++ {
offset := rsegsOffset + i*8 // 每个rseg entry 8字节
if offset+8 <= types.TRX_SYS_MYSQL_LOG_INFO {
pageNo := binary.BigEndian.Uint32(data[offset:])
header.RSegs[i] = pageNo
}
}

return &types.TRXSysPage{
Page:   page,
Header: header,
}, nil
}

// WriteTRXSysHeader 写入Transaction System Header
func WriteTRXSysHeader(page *types.Page, header *types.TRXSysHeader) {
data := page.Data[types.TRX_SYS:]

binary.BigEndian.PutUint64(data[types.TRX_SYS_TRX_ID_STORE:], header.TrxIDStore)

// 写入Rollback segment数组
rsegsOffset := types.TRX_SYS_RSEGS
for i := uint32(0); i < types.RSEG_ARRAY_SIZE && i < uint32(len(header.RSegs)); i++ {
offset := rsegsOffset + i*8
if offset+8 <= types.TRX_SYS_MYSQL_LOG_INFO {
binary.BigEndian.PutUint32(data[offset:], header.RSegs[i])
}
}
}
