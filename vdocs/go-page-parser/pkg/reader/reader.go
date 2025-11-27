package reader

import (
"encoding/binary"
"io"
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// IBDReader IBD文件读取器
type IBDReader struct {
file     *os.File
filePath string
pageSize uint32
fileSize int64
}

// Open 打开IBD文件
func Open(filePath string) (*IBDReader, error) {
file, err := os.Open(filePath)
if err != nil {
return nil, err
}

// 获取文件大小
stat, err := file.Stat()
if err != nil {
file.Close()
return nil, err
}

reader := &IBDReader{
file:     file,
filePath: filePath,
fileSize: stat.Size(),
pageSize: types.UNIV_PAGE_SIZE_DEF, // 默认16KB
}

// 读取第一个Page确定Page大小
if err := reader.detectPageSize(); err != nil {
file.Close()
return nil, err
}

return reader, nil
}

// detectPageSize 检测Page大小
func (r *IBDReader) detectPageSize() error {
// 尝试读取最小Page大小
tempBuf := make([]byte, types.UNIV_PAGE_SIZE_MIN)
if _, err := r.file.ReadAt(tempBuf, 0); err != nil {
return err
}

// 从FSP Header读取flags确定Page大小
// 简化处理：默认16KB
r.pageSize = types.UNIV_PAGE_SIZE_DEF

return nil
}

// ReadPage 读取指定Page
func (r *IBDReader) ReadPage(pageNum uint32) (*types.Page, error) {
offset := int64(pageNum) * int64(r.pageSize)

if offset+int64(r.pageSize) > r.fileSize {
return nil, types.NewError(1, "Page number out of range")
}

data := make([]byte, r.pageSize)
if _, err := r.file.ReadAt(data, offset); err != nil {
return nil, err
}

page := &types.Page{
Data:     data,
PageSize: r.pageSize,
}

// 解析Header
page.Checksum = binary.BigEndian.Uint32(data[types.FIL_PAGE_SPACE_OR_CHKSUM:])
page.PageNumber = binary.BigEndian.Uint32(data[types.FIL_PAGE_OFFSET:])
page.PrevPage = binary.BigEndian.Uint32(data[types.FIL_PAGE_PREV:])
page.NextPage = binary.BigEndian.Uint32(data[types.FIL_PAGE_NEXT:])
page.LSN = binary.BigEndian.Uint64(data[types.FIL_PAGE_LSN:])
page.PageType = binary.BigEndian.Uint16(data[types.FIL_PAGE_TYPE:])
page.FlushLSN = binary.BigEndian.Uint64(data[types.FIL_PAGE_FILE_FLUSH_LSN:])
page.SpaceID = binary.BigEndian.Uint32(data[types.FIL_PAGE_SPACE_ID:])

// 解析Trailer
trailerOffset := r.pageSize - types.FIL_PAGE_DATA_END
page.OldChecksum = binary.BigEndian.Uint32(data[trailerOffset:])
page.LSNLow = binary.BigEndian.Uint32(data[trailerOffset+4:])

return page, nil
}

// GetPageCount 获取Page总数
func (r *IBDReader) GetPageCount() uint32 {
return uint32(r.fileSize / int64(r.pageSize))
}

// GetPageSize 获取Page大小
func (r *IBDReader) GetPageSize() uint32 {
return r.pageSize
}

// Close 关闭文件
func (r *IBDReader) Close() error {
if r.file != nil {
return r.file.Close()
}
return nil
}

// ReadPages 批量读取Page
func (r *IBDReader) ReadPages(startPage, count uint32) ([]*types.Page, error) {
pages := make([]*types.Page, 0, count)
for i := uint32(0); i < count; i++ {
page, err := r.ReadPage(startPage + i)
if err != nil {
if err == io.EOF {
break
}
return nil, err
}
pages = append(pages, page)
}
return pages, nil
}
