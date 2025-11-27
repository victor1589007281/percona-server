package utils

import (
"encoding/binary"
"errors"
"io"
)

var (
// ErrInvalidLengthEncodedInteger 无效的变长整数格式
ErrInvalidLengthEncodedInteger = errors.New("invalid length-encoded integer")

// ErrBufferTooSmall 缓冲区太小
ErrBufferTooSmall = errors.New("buffer too small")

// ErrInvalidNullTerminatedString 无效的NULL结尾字符串
ErrInvalidNullTerminatedString = errors.New("invalid null-terminated string")
)

// ReadLengthEncodedInteger 读取变长整数 (length-encoded integer)
// 格式：
//   - < 0xFB (251): 1字节，值就是该字节
//   - 0xFC: 接下来2字节是值（小端序）
//   - 0xFD: 接下来3字节是值（小端序）
//   - 0xFE: 接下来8字节是值（小端序）
func ReadLengthEncodedInteger(r io.Reader) (uint64, error) {
var firstByte [1]byte
if _, err := io.ReadFull(r, firstByte[:]); err != nil {
return 0, err
}

switch firstByte[0] {
case 0xFB:
// NULL值（在某些上下文中使用）
return 0, nil
case 0xFC:
// 2字节整数
var buf [2]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return uint64(binary.LittleEndian.Uint16(buf[:])), nil
case 0xFD:
// 3字节整数
var buf [3]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return uint64(buf[0]) | uint64(buf[1])<<8 | uint64(buf[2])<<16, nil
case 0xFE:
// 8字节整数
var buf [8]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return binary.LittleEndian.Uint64(buf[:]), nil
default:
// 1字节整数
return uint64(firstByte[0]), nil
}
}

// WriteLengthEncodedInteger 写入变长整数
func WriteLengthEncodedInteger(w io.Writer, value uint64) error {
if value < 251 {
// 1字节
_, err := w.Write([]byte{byte(value)})
return err
} else if value < 0x10000 {
// 3字节: 0xFC + 2字节值
buf := make([]byte, 3)
buf[0] = 0xFC
binary.LittleEndian.PutUint16(buf[1:], uint16(value))
_, err := w.Write(buf)
return err
} else if value < 0x1000000 {
// 4字节: 0xFD + 3字节值
buf := make([]byte, 4)
buf[0] = 0xFD
buf[1] = byte(value)
buf[2] = byte(value >> 8)
buf[3] = byte(value >> 16)
_, err := w.Write(buf)
return err
} else {
// 9字节: 0xFE + 8字节值
buf := make([]byte, 9)
buf[0] = 0xFE
binary.LittleEndian.PutUint64(buf[1:], value)
_, err := w.Write(buf)
return err
}
}

// ReadLengthEncodedString 读取变长字符串 (length-encoded string)
// 格式：length-encoded integer + string[length]
func ReadLengthEncodedString(r io.Reader) (string, error) {
length, err := ReadLengthEncodedInteger(r)
if err != nil {
return "", err
}

if length == 0 {
return "", nil
}

buf := make([]byte, length)
if _, err := io.ReadFull(r, buf); err != nil {
return "", err
}

return string(buf), nil
}

// WriteLengthEncodedString 写入变长字符串
func WriteLengthEncodedString(w io.Writer, s string) error {
if err := WriteLengthEncodedInteger(w, uint64(len(s))); err != nil {
return err
}
if len(s) > 0 {
_, err := w.Write([]byte(s))
return err
}
return nil
}

// ReadNullTerminatedString 读取NULL结尾字符串
func ReadNullTerminatedString(r io.Reader) (string, error) {
var buf []byte
var b [1]byte

for {
if _, err := io.ReadFull(r, b[:]); err != nil {
return "", err
}
if b[0] == 0 {
break
}
buf = append(buf, b[0])
}

return string(buf), nil
}

// WriteNullTerminatedString 写入NULL结尾字符串
func WriteNullTerminatedString(w io.Writer, s string) error {
if _, err := w.Write([]byte(s)); err != nil {
return err
}
_, err := w.Write([]byte{0})
return err
}

// ReadFixedLengthString 读取固定长度字符串
func ReadFixedLengthString(r io.Reader, length int) (string, error) {
buf := make([]byte, length)
if _, err := io.ReadFull(r, buf); err != nil {
return "", err
}
return string(buf), nil
}

// WriteFixedLengthString 写入固定长度字符串
// 如果s长度不足，用0填充；如果超长，截断
func WriteFixedLengthString(w io.Writer, s string, length int) error {
buf := make([]byte, length)
copy(buf, []byte(s))
_, err := w.Write(buf)
return err
}

// ReadUint8 读取1字节无符号整数
func ReadUint8(r io.Reader) (uint8, error) {
var buf [1]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return buf[0], nil
}

// WriteUint8 写入1字节无符号整数
func WriteUint8(w io.Writer, value uint8) error {
_, err := w.Write([]byte{value})
return err
}

// ReadUint16 读取2字节无符号整数（小端序）
func ReadUint16(r io.Reader) (uint16, error) {
var buf [2]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return binary.LittleEndian.Uint16(buf[:]), nil
}

// WriteUint16 写入2字节无符号整数（小端序）
func WriteUint16(w io.Writer, value uint16) error {
buf := make([]byte, 2)
binary.LittleEndian.PutUint16(buf, value)
_, err := w.Write(buf)
return err
}

// ReadUint24 读取3字节无符号整数（小端序）
func ReadUint24(r io.Reader) (uint32, error) {
var buf [3]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return uint32(buf[0]) | uint32(buf[1])<<8 | uint32(buf[2])<<16, nil
}

// WriteUint24 写入3字节无符号整数（小端序）
func WriteUint24(w io.Writer, value uint32) error {
buf := []byte{
byte(value),
byte(value >> 8),
byte(value >> 16),
}
_, err := w.Write(buf)
return err
}

// ReadUint32 读取4字节无符号整数（小端序）
func ReadUint32(r io.Reader) (uint32, error) {
var buf [4]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return binary.LittleEndian.Uint32(buf[:]), nil
}

// WriteUint32 写入4字节无符号整数（小端序）
func WriteUint32(w io.Writer, value uint32) error {
buf := make([]byte, 4)
binary.LittleEndian.PutUint32(buf, value)
_, err := w.Write(buf)
return err
}

// ReadUint64 读取8字节无符号整数（小端序）
func ReadUint64(r io.Reader) (uint64, error) {
var buf [8]byte
if _, err := io.ReadFull(r, buf[:]); err != nil {
return 0, err
}
return binary.LittleEndian.Uint64(buf[:]), nil
}

// WriteUint64 写入8字节无符号整数（小端序）
func WriteUint64(w io.Writer, value uint64) error {
buf := make([]byte, 8)
binary.LittleEndian.PutUint64(buf, value)
_, err := w.Write(buf)
return err
}

// ReadBytes 读取指定长度的字节
func ReadBytes(r io.Reader, length int) ([]byte, error) {
buf := make([]byte, length)
if _, err := io.ReadFull(r, buf); err != nil {
return nil, err
}
return buf, nil
}

// WriteBytes 写入字节数组
func WriteBytes(w io.Writer, data []byte) error {
_, err := w.Write(data)
return err
}
