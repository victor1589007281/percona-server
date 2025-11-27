package protocol

import (
"bytes"
"errors"
"io"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

var (
// ErrPacketTooLarge 包太大
ErrPacketTooLarge = errors.New("packet too large")

// ErrInvalidSequenceID 无效的序列号
ErrInvalidSequenceID = errors.New("invalid sequence ID")
)

// PacketReader 包读取器
type PacketReader struct {
r              io.Reader
expectedSeqID  byte
sequenceID     byte
resetOnCommand bool
}

// NewPacketReader 创建包读取器
func NewPacketReader(r io.Reader) *PacketReader {
return &PacketReader{
r:              r,
expectedSeqID:  0,
resetOnCommand: true,
}
}

// ReadPacket 读取一个完整的MySQL包
// 自动处理16MB+的分包情况
func (pr *PacketReader) ReadPacket() (*types.Packet, error) {
var payload []byte

for {
// 读取包头
header, err := pr.readPacketHeader()
if err != nil {
return nil, err
}

// 验证序列号
if header.SequenceID != pr.expectedSeqID {
return nil, ErrInvalidSequenceID
}
pr.expectedSeqID++

// 读取payload
if header.PayloadLength > 0 {
chunkPayload := make([]byte, header.PayloadLength)
if _, err := io.ReadFull(pr.r, chunkPayload); err != nil {
return nil, err
}
payload = append(payload, chunkPayload...)
}

// 如果payload长度 < MaxPacketSize，说明这是最后一个包
if header.PayloadLength < types.MaxPacketSize {
break
}
}

return &types.Packet{
SequenceID: pr.sequenceID,
Payload:    payload,
}, nil
}

// readPacketHeader 读取包头
func (pr *PacketReader) readPacketHeader() (*types.PacketHeader, error) {
// 读取4字节包头
headerBuf := make([]byte, types.PacketHeaderSize)
if _, err := io.ReadFull(pr.r, headerBuf); err != nil {
return nil, err
}

// 解析包头
payloadLength := uint32(headerBuf[0]) | 
uint32(headerBuf[1])<<8 | 
uint32(headerBuf[2])<<16
sequenceID := headerBuf[3]

pr.sequenceID = sequenceID

return &types.PacketHeader{
PayloadLength: payloadLength,
SequenceID:    sequenceID,
}, nil
}

// ResetSequence 重置序列号
func (pr *PacketReader) ResetSequence() {
pr.expectedSeqID = 0
}

// PacketWriter 包写入器
type PacketWriter struct {
w          io.Writer
sequenceID byte
}

// NewPacketWriter 创建包写入器
func NewPacketWriter(w io.Writer) *PacketWriter {
return &PacketWriter{
w:          w,
sequenceID: 0,
}
}

// WritePacket 写入一个MySQL包
// 自动处理16MB+的分包情况
func (pw *PacketWriter) WritePacket(payload []byte) error {
payloadLen := len(payload)
sentLastMaxPacket := false

// 处理空payload的情况
if payloadLen == 0 {
header := make([]byte, types.PacketHeaderSize)
header[3] = pw.sequenceID
if _, err := pw.w.Write(header); err != nil {
return err
}
pw.sequenceID++
return nil
}

for payloadLen > 0 {
// 当前包的payload大小
chunkSize := payloadLen
if chunkSize > types.MaxPacketSize {
chunkSize = types.MaxPacketSize
}

// 写入包头
header := make([]byte, types.PacketHeaderSize)
header[0] = byte(chunkSize)
header[1] = byte(chunkSize >> 8)
header[2] = byte(chunkSize >> 16)
header[3] = pw.sequenceID

if _, err := pw.w.Write(header); err != nil {
return err
}

// 写入payload
if _, err := pw.w.Write(payload[:chunkSize]); err != nil {
return err
}

// 记录是否刚发送了一个MaxPacketSize的包
sentLastMaxPacket = (chunkSize == types.MaxPacketSize)

payload = payload[chunkSize:]
payloadLen -= chunkSize
pw.sequenceID++
}

// 如果最后一个包正好是MaxPacketSize，需要发送一个空包来标识结束
if sentLastMaxPacket {
header := make([]byte, types.PacketHeaderSize)
header[3] = pw.sequenceID
if _, err := pw.w.Write(header); err != nil {
return err
}
pw.sequenceID++
}

return nil
}

// WritePacketWithSequence 写入包并指定序列号
func (pw *PacketWriter) WritePacketWithSequence(payload []byte, seqID byte) error {
pw.sequenceID = seqID
return pw.WritePacket(payload)
}

// ResetSequence 重置序列号
func (pw *PacketWriter) ResetSequence() {
pw.sequenceID = 0
}

// GetSequenceID 获取当前序列号
func (pw *PacketWriter) GetSequenceID() byte {
return pw.sequenceID
}

// ParsePacketType 判断包类型
func ParsePacketType(payload []byte) byte {
if len(payload) == 0 {
return 0
}
return payload[0]
}

// IsOKPacket 判断是否是OK包
func IsOKPacket(payload []byte) bool {
if len(payload) < 7 {
return false
}
return payload[0] == types.PacketOK
}

// IsEOFPacket 判断是否是EOF包
func IsEOFPacket(payload []byte) bool {
if len(payload) >= 9 {
return false
}
return payload[0] == types.PacketEOF
}

// IsERRPacket 判断是否是ERR包
func IsERRPacket(payload []byte) bool {
if len(payload) < 3 {
return false
}
return payload[0] == types.PacketERR
}

// IsResultSet 判断是否是结果集包（列计数包）
func IsResultSet(payload []byte) bool {
if len(payload) == 0 {
return false
}

// 结果集以length-encoded integer开头（列数量）
firstByte := payload[0]

// 0x00, 0xFE, 0xFF是特殊包标识
if firstByte == types.PacketOK || 
   firstByte == types.PacketEOF || 
   firstByte == types.PacketERR {
return false
}

return true
}

// BuildPacket 构建包（辅助函数）
func BuildPacket(data ...interface{}) ([]byte, error) {
var buf bytes.Buffer

for _, d := range data {
switch v := d.(type) {
case byte:
if err := utils.WriteUint8(&buf, v); err != nil {
return nil, err
}
case uint16:
if err := utils.WriteUint16(&buf, v); err != nil {
return nil, err
}
case uint32:
if err := utils.WriteUint32(&buf, v); err != nil {
return nil, err
}
case uint64:
if err := utils.WriteUint64(&buf, v); err != nil {
return nil, err
}
case string:
if _, err := buf.Write([]byte(v)); err != nil {
return nil, err
}
case []byte:
if _, err := buf.Write(v); err != nil {
return nil, err
}
default:
return nil, errors.New("unsupported type")
}
}

return buf.Bytes(), nil
}
