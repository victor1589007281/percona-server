package inode

import (
"encoding/binary"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// ParseInodePage 解析Inode Page
func ParseInodePage(page *types.Page) (*types.InodePage, error) {
if page.PageType != types.FIL_PAGE_INODE {
return nil, types.NewError(1, "Not an inode page")
}

inodePage := &types.InodePage{
Page:   page,
Inodes: make([]*types.FSEGInode, 0),
}

// 从FSEG_ARR_OFFSET开始解析inode数组
offset := types.FSEG_ARR_OFFSET

// 计算可以容纳多少个inode
maxInodes := (page.PageSize - offset - types.FIL_PAGE_DATA_END) / types.FSEG_INODE_SIZE

for i := uint32(0); i < maxInodes; i++ {
inodeOffset := offset + i*types.FSEG_INODE_SIZE
if inodeOffset+types.FSEG_INODE_SIZE > page.PageSize-types.FIL_PAGE_DATA_END {
break
}

data := page.Data[inodeOffset:]

// 检查Segment ID是否为0（表示未使用的inode）
segmentID := binary.BigEndian.Uint64(data[types.FSEG_ID:])
if segmentID == 0 {
continue
}

inode := &types.FSEGInode{
SegmentID:    segmentID,
NotFullNUsed: binary.BigEndian.Uint32(data[types.FSEG_NOT_FULL_N_USED:]),
MagicN:       binary.BigEndian.Uint32(data[types.FSEG_MAGIC_N:]),
}

// 解析碎片page数组
inode.FragmentPages = make([]uint32, types.FSEG_FRAG_ARR_N_SLOTS)
fragOffset := types.FSEG_FRAG_ARR
for j := uint32(0); j < types.FSEG_FRAG_ARR_N_SLOTS; j++ {
pageNo := binary.BigEndian.Uint32(data[fragOffset + j*4:])
inode.FragmentPages[j] = pageNo
}

inodePage.Inodes = append(inodePage.Inodes, inode)
}

return inodePage, nil
}

// WriteInodeEntry 写入Inode Entry
func WriteInodeEntry(page *types.Page, index uint32, inode *types.FSEGInode) error {
offset := types.FSEG_ARR_OFFSET + index*types.FSEG_INODE_SIZE

if offset+types.FSEG_INODE_SIZE > page.PageSize-types.FIL_PAGE_DATA_END {
return types.NewError(1, "Inode index out of range")
}

data := page.Data[offset:]

binary.BigEndian.PutUint64(data[types.FSEG_ID:], inode.SegmentID)
binary.BigEndian.PutUint32(data[types.FSEG_NOT_FULL_N_USED:], inode.NotFullNUsed)
binary.BigEndian.PutUint32(data[types.FSEG_MAGIC_N:], inode.MagicN)

// 写入碎片page数组
fragOffset := types.FSEG_FRAG_ARR
for j := uint32(0); j < types.FSEG_FRAG_ARR_N_SLOTS && j < uint32(len(inode.FragmentPages)); j++ {
binary.BigEndian.PutUint32(data[fragOffset+j*4:], inode.FragmentPages[j])
}

return nil
}
