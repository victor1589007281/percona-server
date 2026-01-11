// Package wal implements WAL (Write-Ahead Log) file format handling
package wal

import (
	"encoding/binary"
	"hash/crc32"
)

const (
	// WAL file magic number "WAL1"
	WALMagic uint32 = 0x57414C31
	// WAL end magic number "END1"
	WALEndMagic uint32 = 0x454E4431

	// WAL header size in bytes
	WALHeaderSize = 64
	// WAL footer size in bytes
	WALFooterSize = 32
	// Redo record header size in bytes
	RedoRecordHeaderSize = 48

	// Default WAL file size (128MB)
	DefaultWALFileSize = 128 * 1024 * 1024
)

// RedoType defines the type of redo log record
type RedoType uint16

const (
	RedoTypeUnknown     RedoType = 0
	RedoTypeInsert      RedoType = 1
	RedoTypeUpdate      RedoType = 2
	RedoTypeDelete      RedoType = 3
	RedoTypePageCreate  RedoType = 4
	RedoTypePageInit    RedoType = 5
	RedoTypeTrxCommit   RedoType = 6
	RedoTypeTrxRollback RedoType = 7
	RedoTypeDDL         RedoType = 8
	RedoTypeCheckpoint  RedoType = 9
	RedoTypeMtrCommit   RedoType = 10
)

// RecordFlags defines flags for redo records
type RecordFlags uint16

const (
	FlagMtrStart RecordFlags = 0x01
	FlagMtrEnd   RecordFlags = 0x02
	FlagSync     RecordFlags = 0x04
)

// WALHeader represents the header of a WAL file
type WALHeader struct {
	Magic       uint32   // Magic number (0x57414C31)
	Version     uint32   // Version (1)
	FileID      uint64   // File ID
	VolumeID    [16]byte // Volume UUID
	StartLSN    uint64   // Start LSN
	EndLSN      uint64   // End LSN (0 if active)
	RecordCount uint32   // Record count
	Flags       uint32   // Flags
	Checksum    uint32   // Header checksum (CRC32)
	Reserved    uint32   // Reserved
}

// WALFooter represents the footer of a WAL file
type WALFooter struct {
	Magic       uint32 // End magic number (0x454E4431)
	EndLSN      uint64 // End LSN
	RecordCount uint32 // Record count
	FileSize    uint64 // File size
	Checksum    uint32 // File checksum (CRC32)
	Reserved    uint32 // Reserved
}

// RedoRecordHeader represents the header of a redo record
type RedoRecordHeader struct {
	LSN     uint64   // Log sequence number
	SpaceID uint64   // Tablespace ID
	PageID  uint64   // Page ID
	TrxID   uint64   // Transaction ID
	MtrID   uint64   // MTR ID
	Type    RedoType // Redo type
	Flags   uint16   // Flags
	DataLen uint32   // Data length
}

// RedoRecord represents a complete redo record
type RedoRecord struct {
	Header   RedoRecordHeader
	Data     []byte
	Checksum uint32
}

// Encode serializes the WAL header to bytes (little-endian)
func (h *WALHeader) Encode() []byte {
	buf := make([]byte, WALHeaderSize)
	binary.LittleEndian.PutUint32(buf[0:4], h.Magic)
	binary.LittleEndian.PutUint32(buf[4:8], h.Version)
	binary.LittleEndian.PutUint64(buf[8:16], h.FileID)
	copy(buf[16:32], h.VolumeID[:])
	binary.LittleEndian.PutUint64(buf[32:40], h.StartLSN)
	binary.LittleEndian.PutUint64(buf[40:48], h.EndLSN)
	binary.LittleEndian.PutUint32(buf[48:52], h.RecordCount)
	binary.LittleEndian.PutUint32(buf[52:56], h.Flags)
	// Calculate checksum (excluding checksum field itself)
	h.Checksum = crc32.ChecksumIEEE(buf[0:56])
	binary.LittleEndian.PutUint32(buf[56:60], h.Checksum)
	binary.LittleEndian.PutUint32(buf[60:64], h.Reserved)
	return buf
}

// Decode deserializes WAL header from bytes
func (h *WALHeader) Decode(buf []byte) error {
	if len(buf) < WALHeaderSize {
		return ErrInvalidHeader
	}
	h.Magic = binary.LittleEndian.Uint32(buf[0:4])
	if h.Magic != WALMagic {
		return ErrInvalidMagic
	}
	h.Version = binary.LittleEndian.Uint32(buf[4:8])
	h.FileID = binary.LittleEndian.Uint64(buf[8:16])
	copy(h.VolumeID[:], buf[16:32])
	h.StartLSN = binary.LittleEndian.Uint64(buf[32:40])
	h.EndLSN = binary.LittleEndian.Uint64(buf[40:48])
	h.RecordCount = binary.LittleEndian.Uint32(buf[48:52])
	h.Flags = binary.LittleEndian.Uint32(buf[52:56])
	h.Checksum = binary.LittleEndian.Uint32(buf[56:60])
	h.Reserved = binary.LittleEndian.Uint32(buf[60:64])

	// Verify checksum
	expectedChecksum := crc32.ChecksumIEEE(buf[0:56])
	if h.Checksum != expectedChecksum {
		return ErrChecksumMismatch
	}
	return nil
}

// Encode serializes the redo record header to bytes
func (h *RedoRecordHeader) Encode() []byte {
	buf := make([]byte, RedoRecordHeaderSize)
	binary.LittleEndian.PutUint64(buf[0:8], h.LSN)
	binary.LittleEndian.PutUint64(buf[8:16], h.SpaceID)
	binary.LittleEndian.PutUint64(buf[16:24], h.PageID)
	binary.LittleEndian.PutUint64(buf[24:32], h.TrxID)
	binary.LittleEndian.PutUint64(buf[32:40], h.MtrID)
	binary.LittleEndian.PutUint16(buf[40:42], uint16(h.Type))
	binary.LittleEndian.PutUint16(buf[42:44], h.Flags)
	binary.LittleEndian.PutUint32(buf[44:48], h.DataLen)
	return buf
}

// Decode deserializes redo record header from bytes
func (h *RedoRecordHeader) Decode(buf []byte) error {
	if len(buf) < RedoRecordHeaderSize {
		return ErrInvalidHeader
	}
	h.LSN = binary.LittleEndian.Uint64(buf[0:8])
	h.SpaceID = binary.LittleEndian.Uint64(buf[8:16])
	h.PageID = binary.LittleEndian.Uint64(buf[16:24])
	h.TrxID = binary.LittleEndian.Uint64(buf[24:32])
	h.MtrID = binary.LittleEndian.Uint64(buf[32:40])
	h.Type = RedoType(binary.LittleEndian.Uint16(buf[40:42]))
	h.Flags = binary.LittleEndian.Uint16(buf[42:44])
	h.DataLen = binary.LittleEndian.Uint32(buf[44:48])
	return nil
}

// Size returns the total size of the redo record
func (r *RedoRecord) Size() int {
	return RedoRecordHeaderSize + len(r.Data) + 4 // header + data + checksum
}

// CalculateChecksum calculates the CRC32 checksum of the record
func (r *RedoRecord) CalculateChecksum() uint32 {
	headerBytes := r.Header.Encode()
	combined := make([]byte, len(headerBytes)+len(r.Data))
	copy(combined, headerBytes)
	copy(combined[len(headerBytes):], r.Data)
	return crc32.ChecksumIEEE(combined)
}
