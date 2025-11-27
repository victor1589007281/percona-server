package writer

import (
"os"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// IBDWriter IBD文件写入器
type IBDWriter struct {
file     *os.File
pageSize uint32
}

// Open 打开IBD文件用于写入
func Open(filePath string, pageSize uint32) (*IBDWriter, error) {
file, err := os.OpenFile(filePath, os.O_RDWR, 0644)
if err != nil {
return nil, err
}

return &IBDWriter{
file:     file,
pageSize: pageSize,
}, nil
}

// WritePage 写入Page到指定位置
func (w *IBDWriter) WritePage(page *types.Page) error {
offset := int64(page.PageNumber) * int64(w.pageSize)

_, err := w.file.WriteAt(page.Data, offset)
return err
}

// WritePageAt 写入Page到指定Page号
func (w *IBDWriter) WritePageAt(pageNum uint32, data []byte) error {
offset := int64(pageNum) * int64(w.pageSize)

if len(data) != int(w.pageSize) {
return types.NewError(1, "Data size mismatch")
}

_, err := w.file.WriteAt(data, offset)
return err
}

// Sync 同步到磁盘
func (w *IBDWriter) Sync() error {
return w.file.Sync()
}

// Close 关闭文件
func (w *IBDWriter) Close() error {
if w.file != nil {
return w.file.Close()
}
return nil
}
