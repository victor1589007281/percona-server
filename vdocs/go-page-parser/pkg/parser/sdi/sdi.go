package sdi

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseSDIPage 解析SDI (Serialized Dictionary Information) Page
func ParseSDIPage(page *types.Page) (*types.SDIPage, error) {
if page.PageType != types.FIL_PAGE_SDI {
return nil, types.NewError(1, "Not an SDI page")
}

data := page.Data[types.FIL_PAGE_DATA:]

header := &types.SDIHeader{
Version:    binary.BigEndian.Uint32(data[types.SDI_VERSION:]),
Type:       binary.BigEndian.Uint32(data[types.SDI_TYPE:]),
Compressed: binary.BigEndian.Uint32(data[types.SDI_COMPRESSED:]),
DataLen:    binary.BigEndian.Uint32(data[types.SDI_DATA_LEN:]),
}

// SDI数据从header之后开始
dataStart := types.FIL_PAGE_DATA + 16 // 4个uint32
dataEnd := dataStart + header.DataLen

if dataEnd > page.PageSize-types.FIL_PAGE_DATA_END {
dataEnd = page.PageSize - types.FIL_PAGE_DATA_END
}

sdiPage := &types.SDIPage{
Page:   page,
Header: header,
Data:   page.Data[dataStart:dataEnd],
}

return sdiPage, nil
}

// WriteSDIPageHeader 写入SDI Page Header
func WriteSDIPageHeader(page *types.Page, header *types.SDIHeader) {
data := page.Data[types.FIL_PAGE_DATA:]

binary.BigEndian.PutUint32(data[types.SDI_VERSION:], header.Version)
binary.BigEndian.PutUint32(data[types.SDI_TYPE:], header.Type)
binary.BigEndian.PutUint32(data[types.SDI_COMPRESSED:], header.Compressed)
binary.BigEndian.PutUint32(data[types.SDI_DATA_LEN:], header.DataLen)
}

// WriteSDIData 写入SDI数据
func WriteSDIData(page *types.Page, data []byte) error {
dataStart := types.FIL_PAGE_DATA + 16
dataEnd := dataStart + uint32(len(data))

if dataEnd > page.PageSize-types.FIL_PAGE_DATA_END {
return types.NewError(1, "SDI data too large")
}

copy(page.Data[dataStart:], data)

return nil
}
