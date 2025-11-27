package utils

import (
"bytes"
"testing"
)

func TestLengthEncodedInteger(t *testing.T) {
tests := []struct {
name  string
value uint64
}{
{"1 byte", 0},
{"1 byte max", 250},
{"2 bytes", 251},
{"2 bytes max", 65535},
{"3 bytes", 65536},
{"3 bytes max", 16777215},
{"8 bytes", 16777216},
{"8 bytes large", 0xFFFFFFFFFFFFFFFF},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
var buf bytes.Buffer

// 写入
if err := WriteLengthEncodedInteger(&buf, tt.value); err != nil {
t.Fatalf("WriteLengthEncodedInteger failed: %v", err)
}

// 读取
value, err := ReadLengthEncodedInteger(&buf)
if err != nil {
t.Fatalf("ReadLengthEncodedInteger failed: %v", err)
}

if value != tt.value {
t.Errorf("Expected %d, got %d", tt.value, value)
}
})
}
}

func TestLengthEncodedString(t *testing.T) {
tests := []struct {
name string
str  string
}{
{"empty", ""},
{"short", "hello"},
{"long", "Lorem ipsum dolor sit amet, consectetur adipiscing elit"},
{"unicode", "你好世界🌍"},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
var buf bytes.Buffer

// 写入
if err := WriteLengthEncodedString(&buf, tt.str); err != nil {
t.Fatalf("WriteLengthEncodedString failed: %v", err)
}

// 读取
str, err := ReadLengthEncodedString(&buf)
if err != nil {
t.Fatalf("ReadLengthEncodedString failed: %v", err)
}

if str != tt.str {
t.Errorf("Expected %q, got %q", tt.str, str)
}
})
}
}

func TestNullTerminatedString(t *testing.T) {
tests := []struct {
name string
str  string
}{
{"empty", ""},
{"short", "hello"},
{"with spaces", "hello world"},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
var buf bytes.Buffer

// 写入
if err := WriteNullTerminatedString(&buf, tt.str); err != nil {
t.Fatalf("WriteNullTerminatedString failed: %v", err)
}

// 读取
str, err := ReadNullTerminatedString(&buf)
if err != nil {
t.Fatalf("ReadNullTerminatedString failed: %v", err)
}

if str != tt.str {
t.Errorf("Expected %q, got %q", tt.str, str)
}
})
}
}

func TestFixedLengthString(t *testing.T) {
tests := []struct {
name   string
str    string
length int
}{
{"exact", "hello", 5},
{"padded", "hi", 5},
{"truncated", "hello world", 5},
}

for _, tt := range tests {
t.Run(tt.name, func(t *testing.T) {
var buf bytes.Buffer

// 写入
if err := WriteFixedLengthString(&buf, tt.str, tt.length); err != nil {
t.Fatalf("WriteFixedLengthString failed: %v", err)
}

// 读取
str, err := ReadFixedLengthString(&buf, tt.length)
if err != nil {
t.Fatalf("ReadFixedLengthString failed: %v", err)
}

// 对于被截断或填充的情况，验证长度
if len(str) != tt.length {
t.Errorf("Expected length %d, got %d", tt.length, len(str))
}
})
}
}

func TestUintReadWrite(t *testing.T) {
t.Run("Uint8", func(t *testing.T) {
var buf bytes.Buffer
value := uint8(123)

if err := WriteUint8(&buf, value); err != nil {
t.Fatalf("WriteUint8 failed: %v", err)
}

result, err := ReadUint8(&buf)
if err != nil {
t.Fatalf("ReadUint8 failed: %v", err)
}

if result != value {
t.Errorf("Expected %d, got %d", value, result)
}
})

t.Run("Uint16", func(t *testing.T) {
var buf bytes.Buffer
value := uint16(12345)

if err := WriteUint16(&buf, value); err != nil {
t.Fatalf("WriteUint16 failed: %v", err)
}

result, err := ReadUint16(&buf)
if err != nil {
t.Fatalf("ReadUint16 failed: %v", err)
}

if result != value {
t.Errorf("Expected %d, got %d", value, result)
}
})

t.Run("Uint24", func(t *testing.T) {
var buf bytes.Buffer
value := uint32(1234567)

if err := WriteUint24(&buf, value); err != nil {
t.Fatalf("WriteUint24 failed: %v", err)
}

result, err := ReadUint24(&buf)
if err != nil {
t.Fatalf("ReadUint24 failed: %v", err)
}

if result != value {
t.Errorf("Expected %d, got %d", value, result)
}
})

t.Run("Uint32", func(t *testing.T) {
var buf bytes.Buffer
value := uint32(123456789)

if err := WriteUint32(&buf, value); err != nil {
t.Fatalf("WriteUint32 failed: %v", err)
}

result, err := ReadUint32(&buf)
if err != nil {
t.Fatalf("ReadUint32 failed: %v", err)
}

if result != value {
t.Errorf("Expected %d, got %d", value, result)
}
})

t.Run("Uint64", func(t *testing.T) {
var buf bytes.Buffer
value := uint64(12345678901234)

if err := WriteUint64(&buf, value); err != nil {
t.Fatalf("WriteUint64 failed: %v", err)
}

result, err := ReadUint64(&buf)
if err != nil {
t.Fatalf("ReadUint64 failed: %v", err)
}

if result != value {
t.Errorf("Expected %d, got %d", value, result)
}
})
}
