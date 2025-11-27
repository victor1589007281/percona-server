package utils

import (
"testing"
)

func TestParseCompressed(t *testing.T) {
tests := []struct {
name     string
data     []byte
expected uint64
consumed int
wantErr  bool
}{
{
name:     "1-byte value (0)",
data:     []byte{0x00},
expected: 0,
consumed: 1,
},
{
name:     "1-byte value (127)",
data:     []byte{0x7F},
expected: 127,
consumed: 1,
},
{
name:     "2-byte value (128)",
data:     []byte{0x80, 0x80},
expected: 128,
consumed: 2,
},
{
name:     "2-byte value (16383)",
data:     []byte{0xBF, 0xFF},
expected: 16383,
consumed: 2,
},
{
name:     "3-byte value (16384)",
data:     []byte{0xC0, 0x40, 0x00},
expected: 16384,
consumed: 3,
},
{
name:     "4-byte value (2097152)",
data:     []byte{0xE0, 0x20, 0x00, 0x00},
expected: 2097152,
consumed: 4,
},
{
name:     "5-byte value (268435456)",
data:     []byte{0xF0, 0x10, 0x00, 0x00, 0x00},
expected: 268435456,
consumed: 5,
},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
got, consumed, err := ParseCompressed(tt.data)
if (err != nil) != tt.wantErr {
t.Errorf("ParseCompressed() error = %v, wantErr %v", err, tt.wantErr)
return
}
if got != tt.expected {
t.Errorf("ParseCompressed() got = %v, want %v", got, tt.expected)
}
if consumed != tt.consumed {
t.Errorf("ParseCompressed() consumed = %v, want %v", consumed, tt.consumed)
}
})
}
}

func TestWriteCompressed(t *testing.T) {
tests := []struct {
name     string
value    uint64
expected []byte
wantErr  bool
}{
{
name:     "1-byte value (0)",
value:    0,
expected: []byte{0x00},
},
{
name:     "1-byte value (127)",
value:    127,
expected: []byte{0x7F},
},
{
name:     "2-byte value (128)",
value:    128,
expected: []byte{0x80, 0x80},
},
{
name:     "3-byte value (16384)",
value:    16384,
expected: []byte{0xC0, 0x40, 0x00},
},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
buf := make([]byte, 5)
n, err := WriteCompressed(tt.value, buf)
if (err != nil) != tt.wantErr {
t.Errorf("WriteCompressed() error = %v, wantErr %v", err, tt.wantErr)
return
}

got := buf[:n]
if len(got) != len(tt.expected) {
t.Errorf("WriteCompressed() length = %v, want %v", len(got), len(tt.expected))
}

for i := range got {
if got[i] != tt.expected[i] {
t.Errorf("WriteCompressed() byte[%d] = 0x%02x, want 0x%02x", i, got[i], tt.expected[i])
}
}
})
}
}

func TestRoundTrip(t *testing.T) {
values := []uint64{0, 1, 127, 128, 16383, 16384, 2097151, 2097152, 268435455, 268435456}

for _, val := range values {
t.Run("", func(t *testing.T) {
// Write
buf := make([]byte, 5)
n, err := WriteCompressed(val, buf)
if err != nil {
t.Fatalf("WriteCompressed(%d) error: %v", val, err)
}

// Read back
got, consumed, err := ParseCompressed(buf)
if err != nil {
t.Fatalf("ParseCompressed() error: %v", err)
}

if got != val {
t.Errorf("Round trip failed: wrote %d, read %d", val, got)
}

if consumed != n {
t.Errorf("Consumed bytes mismatch: wrote %d, consumed %d", n, consumed)
}
})
}
}
