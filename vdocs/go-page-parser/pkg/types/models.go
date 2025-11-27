package types

import "fmt"

// ChecksumAlgorithm Checksum算法类型
type ChecksumAlgorithm int

const (
// ChecksumNone 无checksum
ChecksumNone ChecksumAlgorithm = 0
// ChecksumCRC32 CRC32算法
ChecksumCRC32 ChecksumAlgorithm = 1
// ChecksumInnoDB InnoDB自定义算法
ChecksumInnoDB ChecksumAlgorithm = 2
)

// Page 表示InnoDB Page
type Page struct {
// Raw data
Data     []byte
PageSize uint32

// Header fields
Checksum    uint32
PageNumber  uint32
PrevPage    uint32
NextPage    uint32
LSN         uint64
PageType    uint16
FlushLSN    uint64
SpaceID     uint32

// Trailer fields
OldChecksum uint32
LSNLow      uint32
}

// FSPHeader File Space Header
type FSPHeader struct {
SpaceID    uint32
Size       uint32
FreeLimit  uint32
SpaceFlags uint32
FragNUsed  uint32
SegID      uint64
}

// Error InnoDB错误
type Error struct {
Code    int
Message string
}

func (e *Error) Error() string {
return fmt.Sprintf("InnoDB Error %d: %s", e.Code, e.Message)
}

// NewError 创建新错误
func NewError(code int, message string) *Error {
return &Error{
Code:    code,
Message: message,
}
}
