package modifier

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/checksum"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// Modifier Page修改器
type Modifier struct {
page *types.Page
}

// NewModifier 创建修改器
func NewModifier(page *types.Page) *Modifier {
return &Modifier{page: page}
}

// SetPageNumber 修改Page号
func (m *Modifier) SetPageNumber(pageNum uint32) {
binary.BigEndian.PutUint32(m.page.Data[types.FIL_PAGE_OFFSET:], pageNum)
m.page.PageNumber = pageNum
}

// SetSpaceID 修改Space ID
func (m *Modifier) SetSpaceID(spaceID uint32) {
binary.BigEndian.PutUint32(m.page.Data[types.FIL_PAGE_SPACE_ID:], spaceID)
m.page.SpaceID = spaceID
}

// SetPrevPage 修改前一个Page
func (m *Modifier) SetPrevPage(prevPage uint32) {
binary.BigEndian.PutUint32(m.page.Data[types.FIL_PAGE_PREV:], prevPage)
m.page.PrevPage = prevPage
}

// SetNextPage 修改下一个Page
func (m *Modifier) SetNextPage(nextPage uint32) {
binary.BigEndian.PutUint32(m.page.Data[types.FIL_PAGE_NEXT:], nextPage)
m.page.NextPage = nextPage
}

// WriteData 写入数据到指定偏移
func (m *Modifier) WriteData(offset uint32, data []byte) error {
if int(offset)+len(data) > len(m.page.Data) {
return types.NewError(1, "Write beyond page boundary")
}
copy(m.page.Data[offset:], data)
return nil
}

// UpdateChecksum 重新计算并更新Checksum
func (m *Modifier) UpdateChecksum(algo types.ChecksumAlgorithm) {
checksum.Update(m.page.Data, algo)
m.page.Checksum = binary.BigEndian.Uint32(m.page.Data[types.FIL_PAGE_SPACE_OR_CHKSUM:])
}

// GetPage 获取修改后的Page
func (m *Modifier) GetPage() *types.Page {
return m.page
}
