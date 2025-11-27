package types

import (
"testing"
)

func TestPageConstants(t *testing.T) {
// 测试Page大小常量
if UNIV_PAGE_SIZE_MIN != 4096 {
t.Errorf("UNIV_PAGE_SIZE_MIN = %d; want 4096", UNIV_PAGE_SIZE_MIN)
}

if UNIV_PAGE_SIZE_DEF != 16384 {
t.Errorf("UNIV_PAGE_SIZE_DEF = %d; want 16384", UNIV_PAGE_SIZE_DEF)
}

if UNIV_PAGE_SIZE_MAX != 65536 {
t.Errorf("UNIV_PAGE_SIZE_MAX = %d; want 65536", UNIV_PAGE_SIZE_MAX)
}
}

func TestPageHeaderOffsets(t *testing.T) {
// 测试FIL Header偏移量
tests := []struct {
name   string
offset uint32
want   uint32
}{
{"FIL_PAGE_SPACE_OR_CHKSUM", FIL_PAGE_SPACE_OR_CHKSUM, 0},
{"FIL_PAGE_OFFSET", FIL_PAGE_OFFSET, 4},
{"FIL_PAGE_PREV", FIL_PAGE_PREV, 8},
{"FIL_PAGE_NEXT", FIL_PAGE_NEXT, 12},
{"FIL_PAGE_LSN", FIL_PAGE_LSN, 16},
{"FIL_PAGE_TYPE", FIL_PAGE_TYPE, 24},
{"FIL_PAGE_DATA", FIL_PAGE_DATA, 38},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
if tt.offset != tt.want {
t.Errorf("%s = %d; want %d", tt.name, tt.offset, tt.want)
}
})
}
}

func TestPageTypeConstants(t *testing.T) {
// 测试Page类型常量
if FIL_PAGE_INDEX != 0x45BF {
t.Errorf("FIL_PAGE_INDEX = 0x%04X; want 0x45BF", FIL_PAGE_INDEX)
}

if FIL_PAGE_TYPE_FSP_HDR != 0x0008 {
t.Errorf("FIL_PAGE_TYPE_FSP_HDR = 0x%04X; want 0x0008", FIL_PAGE_TYPE_FSP_HDR)
}
}

func TestError(t *testing.T) {
err := NewError(1, "test error")
if err.Code != 1 {
t.Errorf("Error.Code = %d; want 1", err.Code)
}

if err.Message != "test error" {
t.Errorf("Error.Message = %s; want 'test error'", err.Message)
}

expected := "InnoDB Error 1: test error"
if err.Error() != expected {
t.Errorf("Error.Error() = %s; want %s", err.Error(), expected)
}
}

func TestChecksumAlgorithms(t *testing.T) {
if ChecksumNone != 0 {
t.Errorf("ChecksumNone = %d; want 0", ChecksumNone)
}

if ChecksumCRC32 != 1 {
t.Errorf("ChecksumCRC32 = %d; want 1", ChecksumCRC32)
}

if ChecksumInnoDB != 2 {
t.Errorf("ChecksumInnoDB = %d; want 2", ChecksumInnoDB)
}
}
