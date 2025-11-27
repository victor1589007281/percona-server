package checksum

import (
"encoding/binary"
"hash/crc32"

"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

// CalculateCRC32 计算CRC32 checksum
func CalculateCRC32(data []byte) uint32 {
pageSize := uint32(len(data))

// 跳过checksum字段本身 (FIL_PAGE_SPACE_OR_CHKSUM)
// 和trailer中的checksum字段
c1 := crc32.ChecksumIEEE(data[types.FIL_PAGE_OFFSET:types.FIL_PAGE_FILE_FLUSH_LSN])
c2 := crc32.ChecksumIEEE(data[types.FIL_PAGE_DATA:pageSize-types.FIL_PAGE_DATA_END])

return c1 ^ c2
}

// calculateInnoDBChecksum 计算InnoDB checksum (简化版)
func calculateInnoDBChecksum(data []byte) uint32 {
pageSize := uint32(len(data))

// 使用类似Adler32的算法
var sum1, sum2 uint32

// 跳过checksum字段
for i := types.FIL_PAGE_OFFSET; i < types.FIL_PAGE_FILE_FLUSH_LSN; i++ {
sum1 = (sum1 + uint32(data[i])) % 65521
sum2 = (sum2 + sum1) % 65521
}

for i := types.FIL_PAGE_DATA; i < pageSize-types.FIL_PAGE_DATA_END; i++ {
sum1 = (sum1 + uint32(data[i])) % 65521
sum2 = (sum2 + sum1) % 65521
}

return (sum2 << 16) | sum1
}

// CalculateInnoDBChecksum 计算InnoDB checksum (导出版本)
func CalculateInnoDBChecksum(data []byte) uint32 {
return calculateInnoDBChecksum(data)
}

// Update 更新Page的Checksum
func Update(data []byte, algo types.ChecksumAlgorithm) {
var checksum uint32

// 根据算法计算checksum
if algo == types.ChecksumCRC32 {
checksum = CalculateCRC32(data)
} else if algo == types.ChecksumInnoDB {
checksum = calculateInnoDBChecksum(data)
} else {
// NONE
checksum = 0
}

// 更新Page头部的checksum
binary.BigEndian.PutUint32(data[types.FIL_PAGE_SPACE_OR_CHKSUM:], checksum)

// 更新Trailer的checksum
trailerOffset := uint32(len(data)) - types.FIL_PAGE_DATA_END
binary.BigEndian.PutUint32(data[trailerOffset:], checksum)
}

// Verify 验证Page的Checksum
func Verify(data []byte, algo types.ChecksumAlgorithm) bool {
storedChecksum := binary.BigEndian.Uint32(data[types.FIL_PAGE_SPACE_OR_CHKSUM:])

var calculatedChecksum uint32
if algo == types.ChecksumCRC32 {
calculatedChecksum = CalculateCRC32(data)
} else if algo == types.ChecksumInnoDB {
calculatedChecksum = calculateInnoDBChecksum(data)
} else {
// NONE: always valid
return true
}

return storedChecksum == calculatedChecksum
}
