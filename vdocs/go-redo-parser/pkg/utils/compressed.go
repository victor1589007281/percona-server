package utils

import (
"encoding/binary"
"fmt"
)

// ParseCompressed parses a compressed integer from the buffer
// Returns the value and number of bytes consumed
// Format:
//   1 byte:  0xxxxxxx (value < 128)
//   2 bytes: 10xxxxxx xxxxxxxx (value < 16K)
//   3 bytes: 110xxxxx xxxxxxxx xxxxxxxx (value < 2M)
//   4 bytes: 1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx (value < 256M)
//   5 bytes: 11110000 xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
func ParseCompressed(data []byte) (uint64, int, error) {
if len(data) == 0 {
return 0, 0, fmt.Errorf("empty buffer")
}

firstByte := data[0]

// 1 byte: 0xxxxxxx
if (firstByte & 0x80) == 0 {
return uint64(firstByte), 1, nil
}

// 2 bytes: 10xxxxxx xxxxxxxx
if (firstByte & 0xC0) == 0x80 {
if len(data) < 2 {
return 0, 0, fmt.Errorf("incomplete 2-byte compressed int")
}
val := uint64(firstByte&0x3F)<<8 | uint64(data[1])
return val, 2, nil
}

// 3 bytes: 110xxxxx xxxxxxxx xxxxxxxx
if (firstByte & 0xE0) == 0xC0 {
if len(data) < 3 {
return 0, 0, fmt.Errorf("incomplete 3-byte compressed int")
}
val := uint64(firstByte&0x1F)<<16 | uint64(data[1])<<8 | uint64(data[2])
return val, 3, nil
}

// 4 bytes: 1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx
if (firstByte & 0xF0) == 0xE0 {
if len(data) < 4 {
return 0, 0, fmt.Errorf("incomplete 4-byte compressed int")
}
val := uint64(firstByte&0x0F)<<24 | uint64(data[1])<<16 | 
       uint64(data[2])<<8 | uint64(data[3])
return val, 4, nil
}

// 5 bytes: 11110000 xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
if firstByte == 0xF0 {
if len(data) < 5 {
return 0, 0, fmt.Errorf("incomplete 5-byte compressed int")
}
val := uint64(data[1])<<24 | uint64(data[2])<<16 | 
       uint64(data[3])<<8 | uint64(data[4])
return val, 5, nil
}

return 0, 0, fmt.Errorf("invalid compressed int format: 0x%02x", firstByte)
}

// WriteCompressed writes a compressed integer to the buffer
// Returns number of bytes written
func WriteCompressed(val uint64, buf []byte) (int, error) {
// 1 byte: 0xxxxxxx (< 128)
if val < 0x80 {
if len(buf) < 1 {
return 0, fmt.Errorf("buffer too small for 1-byte int")
}
buf[0] = byte(val)
return 1, nil
}

// 2 bytes: 10xxxxxx xxxxxxxx (< 16K)
if val < 0x4000 {
if len(buf) < 2 {
return 0, fmt.Errorf("buffer too small for 2-byte int")
}
buf[0] = byte(0x80 | (val >> 8))
buf[1] = byte(val)
return 2, nil
}

// 3 bytes: 110xxxxx xxxxxxxx xxxxxxxx (< 2M)
if val < 0x200000 {
if len(buf) < 3 {
return 0, fmt.Errorf("buffer too small for 3-byte int")
}
buf[0] = byte(0xC0 | (val >> 16))
buf[1] = byte(val >> 8)
buf[2] = byte(val)
return 3, nil
}

// 4 bytes: 1110xxxx xxxxxxxx xxxxxxxx xxxxxxxx (< 256M)
if val < 0x10000000 {
if len(buf) < 4 {
return 0, fmt.Errorf("buffer too small for 4-byte int")
}
buf[0] = byte(0xE0 | (val >> 24))
buf[1] = byte(val >> 16)
buf[2] = byte(val >> 8)
buf[3] = byte(val)
return 4, nil
}

// 5 bytes: 11110000 xxxxxxxx xxxxxxxx xxxxxxxx xxxxxxxx
if len(buf) < 5 {
return 0, fmt.Errorf("buffer too small for 5-byte int")
}
buf[0] = 0xF0
buf[1] = byte(val >> 24)
buf[2] = byte(val >> 16)
buf[3] = byte(val >> 8)
buf[4] = byte(val)
return 5, nil
}

// CompressedSize returns the number of bytes needed to store val in compressed format
func CompressedSize(val uint64) int {
if val < 0x80 {
return 1
} else if val < 0x4000 {
return 2
} else if val < 0x200000 {
return 3
} else if val < 0x10000000 {
return 4
}
return 5
}

// ReadUint16BE reads a big-endian uint16
func ReadUint16BE(data []byte) (uint16, error) {
if len(data) < 2 {
return 0, fmt.Errorf("buffer too small for uint16")
}
return binary.BigEndian.Uint16(data), nil
}

// ReadUint32BE reads a big-endian uint32
func ReadUint32BE(data []byte) (uint32, error) {
if len(data) < 4 {
return 0, fmt.Errorf("buffer too small for uint32")
}
return binary.BigEndian.Uint32(data), nil
}

// ReadUint64BE reads a big-endian uint64
func ReadUint64BE(data []byte) (uint64, error) {
if len(data) < 8 {
return 0, fmt.Errorf("buffer too small for uint64")
}
return binary.BigEndian.Uint64(data), nil
}

// WriteUint16BE writes a big-endian uint16
func WriteUint16BE(val uint16, buf []byte) error {
if len(buf) < 2 {
return fmt.Errorf("buffer too small for uint16")
}
binary.BigEndian.PutUint16(buf, val)
return nil
}

// WriteUint32BE writes a big-endian uint32
func WriteUint32BE(val uint32, buf []byte) error {
if len(buf) < 4 {
return fmt.Errorf("buffer too small for uint32")
}
binary.BigEndian.PutUint32(buf, val)
return nil
}

// WriteUint64BE writes a big-endian uint64
func WriteUint64BE(val uint64, buf []byte) error {
if len(buf) < 8 {
return fmt.Errorf("buffer too small for uint64")
}
binary.BigEndian.PutUint64(buf, val)
return nil
}
