package protocol

import (
"bytes"
"testing"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
)

func TestPacketReadWrite(t *testing.T) {
tests := []struct {
name    string
payload []byte
}{
{"empty", []byte{}},
{"small", []byte("hello")},
{"medium", bytes.Repeat([]byte("a"), 1000)},
{"at boundary", bytes.Repeat([]byte("b"), types.MaxPacketSize)},
{"over boundary", bytes.Repeat([]byte("c"), types.MaxPacketSize+1000)},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
var buf bytes.Buffer

// 写入
writer := NewPacketWriter(&buf)
if err := writer.WritePacket(tt.payload); err != nil {
t.Fatalf("WritePacket failed: %v", err)
}

// 读取
reader := NewPacketReader(&buf)
packet, err := reader.ReadPacket()
if err != nil {
t.Fatalf("ReadPacket failed: %v", err)
}

// 验证
if !bytes.Equal(packet.Payload, tt.payload) {
t.Errorf("Payload mismatch: expected %d bytes, got %d bytes", 
len(tt.payload), len(packet.Payload))
}
})
}
}

func TestPacketSequenceID(t *testing.T) {
var buf bytes.Buffer
writer := NewPacketWriter(&buf)

// 写入3个包
for i := 0; i < 3; i++ {
payload := []byte{byte(i)}
if err := writer.WritePacket(payload); err != nil {
t.Fatalf("WritePacket %d failed: %v", i, err)
}
}

// 读取并验证序列号
reader := NewPacketReader(&buf)
for i := 0; i < 3; i++ {
packet, err := reader.ReadPacket()
if err != nil {
t.Fatalf("ReadPacket %d failed: %v", i, err)
}

if packet.SequenceID != byte(i) {
t.Errorf("Expected sequence ID %d, got %d", i, packet.SequenceID)
}
}
}

func TestPacketType(t *testing.T) {
tests := []struct {
name     string
payload  []byte
isOK     bool
isEOF    bool
isERR    bool
isResult bool
}{
{
name:     "OK packet",
payload:  append([]byte{types.PacketOK}, make([]byte, 10)...),
isOK:     true,
isEOF:    false,
isERR:    false,
isResult: false,
},
{
name:     "EOF packet",
payload:  []byte{types.PacketEOF, 0, 0, 0, 0},
isOK:     false,
isEOF:    true,
isERR:    false,
isResult: false,
},
{
name:     "ERR packet",
payload:  []byte{types.PacketERR, 0, 0},
isOK:     false,
isEOF:    false,
isERR:    true,
isResult: false,
},
{
name:     "ResultSet packet",
payload:  []byte{0x01}, // Column count = 1
isOK:     false,
isEOF:    false,
isERR:    false,
isResult: true,
},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
if got := IsOKPacket(tt.payload); got != tt.isOK {
t.Errorf("IsOKPacket() = %v, want %v", got, tt.isOK)
}
if got := IsEOFPacket(tt.payload); got != tt.isEOF {
t.Errorf("IsEOFPacket() = %v, want %v", got, tt.isEOF)
}
if got := IsERRPacket(tt.payload); got != tt.isERR {
t.Errorf("IsERRPacket() = %v, want %v", got, tt.isERR)
}
if got := IsResultSet(tt.payload); got != tt.isResult {
t.Errorf("IsResultSet() = %v, want %v", got, tt.isResult)
}
})
}
}

func TestBuildPacket(t *testing.T) {
payload, err := BuildPacket(
byte(1),
uint16(256),
uint32(65536),
"test",
[]byte{0x01, 0x02},
)

if err != nil {
t.Fatalf("BuildPacket failed: %v", err)
}

// 验证payload长度
// 1 + 2 + 4 + 4 + 2 = 13
expectedLen := 1 + 2 + 4 + 4 + 2
if len(payload) != expectedLen {
t.Errorf("Expected payload length %d, got %d", expectedLen, len(payload))
}
}
