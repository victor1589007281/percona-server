package checksum

import (
"testing"
"github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func TestCRC32(t *testing.T) {
// 创建测试Page
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 写入一些测试数据
for i := 0; i < len(data); i++ {
data[i] = byte(i % 256)
}

// 计算CRC32
crc := CalculateCRC32(data)
if crc == 0 {
t.Errorf("CRC32 should not be 0")
}

// 更新Checksum
Update(data, types.ChecksumCRC32)

// 验证Checksum
if !Verify(data, types.ChecksumCRC32) {
t.Errorf("Checksum verification failed")
}
}

func TestInnoDBChecksum(t *testing.T) {
// 创建测试Page
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 写入一些测试数据
for i := 0; i < len(data); i++ {
data[i] = byte(i % 256)
}

// 计算InnoDB Checksum
checksum := CalculateInnoDBChecksum(data)
if checksum == 0 {
t.Errorf("InnoDB checksum should not be 0")
}
}

func TestChecksumNone(t *testing.T) {
// 创建测试Page
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

// 使用NONE算法
Update(data, types.ChecksumNone)

// 验证应该总是成功
if !Verify(data, types.ChecksumNone) {
t.Errorf("Checksum NONE verification should always pass")
}
}

func BenchmarkCRC32(b *testing.B) {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

b.ResetTimer()
for i := 0; i < b.N; i++ {
CalculateCRC32(data)
}
}

func BenchmarkInnoDBChecksum(b *testing.B) {
data := make([]byte, types.UNIV_PAGE_SIZE_DEF)

b.ResetTimer()
for i := 0; i < b.N; i++ {
CalculateInnoDBChecksum(data)
}
}
