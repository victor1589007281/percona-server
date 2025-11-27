package logindex

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
)

// Data file structures (polar_logindex_*.dat)

// PageLSNBlock represents a block of LSN data for a single page
type PageLSNBlock struct {
	SpaceID    uint32   // Tablespace ID
	PageNo     uint32   // Page number
	LSNCount   uint32   // Number of LSNs
	Compressed uint32   // 0=no, 1=yes
	LSNBase    uint64   // Base LSN for delta encoding
	LSNDeltas  []uint32 // Delta values
	Checksum   uint32   // CRC32 checksum
}

// EncodeLSNs encodes LSN sequence with delta encoding
func EncodeLSNs(lsns []uint64) (base uint64, deltas []uint32) {
	if len(lsns) == 0 {
		return 0, nil
	}
	
	base = lsns[0]
	deltas = make([]uint32, len(lsns)-1)
	
	for i := 1; i < len(lsns); i++ {
		deltas[i-1] = uint32(lsns[i] - lsns[i-1])
	}
	
	return base, deltas
}

// DecodeLSNs decodes delta-encoded LSNs
func DecodeLSNs(base uint64, deltas []uint32) []uint64 {
	lsns := make([]uint64, len(deltas)+1)
	lsns[0] = base
	
	for i, delta := range deltas {
		lsns[i+1] = lsns[i] + uint64(delta)
	}
	
	return lsns
}

// MarshalBinary encodes the block to binary
func (b *PageLSNBlock) MarshalBinary() ([]byte, error) {
	size := 4 + 4 + 4 + 4 + 8 + 4*len(b.LSNDeltas) + 4
	buf := make([]byte, size)
	
	offset := 0
	binary.LittleEndian.PutUint32(buf[offset:], b.SpaceID)
	offset += 4
	binary.LittleEndian.PutUint32(buf[offset:], b.PageNo)
	offset += 4
	binary.LittleEndian.PutUint32(buf[offset:], b.LSNCount)
	offset += 4
	binary.LittleEndian.PutUint32(buf[offset:], b.Compressed)
	offset += 4
	binary.LittleEndian.PutUint64(buf[offset:], b.LSNBase)
	offset += 8
	
	for _, delta := range b.LSNDeltas {
		binary.LittleEndian.PutUint32(buf[offset:], delta)
		offset += 4
	}
	
	// Calculate checksum over all data except checksum field
	b.Checksum = crc32.ChecksumIEEE(buf[:offset])
	binary.LittleEndian.PutUint32(buf[offset:], b.Checksum)
	
	return buf, nil
}

// UnmarshalBinary decodes the block from binary
func (b *PageLSNBlock) UnmarshalBinary(buf []byte) error {
	if len(buf) < 28 {
		return fmt.Errorf("buffer too small")
	}
	
	offset := 0
	b.SpaceID = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	b.PageNo = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	b.LSNCount = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	b.Compressed = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	b.LSNBase = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	
	deltaCount := b.LSNCount - 1
	b.LSNDeltas = make([]uint32, deltaCount)
	
	for i := uint32(0); i < deltaCount; i++ {
		if offset+4 > len(buf) {
			return fmt.Errorf("buffer too small for deltas")
		}
		b.LSNDeltas[i] = binary.LittleEndian.Uint32(buf[offset:])
		offset += 4
	}
	
	if offset+4 > len(buf) {
		return fmt.Errorf("buffer too small for checksum")
	}
	b.Checksum = binary.LittleEndian.Uint32(buf[offset:])
	
	// Verify checksum
	savedChecksum := b.Checksum
	calculatedChecksum := crc32.ChecksumIEEE(buf[:offset])
	if savedChecksum != calculatedChecksum {
		return fmt.Errorf("checksum mismatch")
	}
	
	return nil
}

// DataFile represents a data file
type DataFile struct {
	filePath string
	file     *os.File
	offset   uint64
	fileID   uint32
}

// NewDataFile creates a new data file
func NewDataFile(dir string, fileID uint32) (*DataFile, error) {
	filePath := filepath.Join(dir, fmt.Sprintf("%s_%03d.dat", DataFilePrefix, fileID))
	file, err := os.OpenFile(filePath, os.O_CREATE|os.O_RDWR, 0644)
	if err != nil {
		return nil, err
	}
	
	stat, err := file.Stat()
	if err != nil {
		file.Close()
		return nil, err
	}
	
	return &DataFile{
		filePath: filePath,
		file:     file,
		offset:   uint64(stat.Size()),
		fileID:   fileID,
	}, nil
}

// WriteBlock writes a block to the data file
func (df *DataFile) WriteBlock(block *PageLSNBlock) (uint64, error) {
	data, err := block.MarshalBinary()
	if err != nil {
		return 0, err
	}
	
	offset := df.offset
	if _, err := df.file.Write(data); err != nil {
		return 0, err
	}
	
	df.offset += uint64(len(data))
	return offset, nil
}

// ReadBlock reads a block from the data file at the given offset
func (df *DataFile) ReadBlock(offset uint64) (*PageLSNBlock, error) {
	// Read header first to get size
	header := make([]byte, 28)
	if _, err := df.file.ReadAt(header, int64(offset)); err != nil {
		return nil, err
	}
	
	block := &PageLSNBlock{}
	lsnCount := binary.LittleEndian.Uint32(header[8:12])
	
	// Calculate total size
	deltaCount := lsnCount - 1
	totalSize := 28 + int(deltaCount)*4
	
	data := make([]byte, totalSize)
	if _, err := df.file.ReadAt(data, int64(offset)); err != nil {
		return nil, err
	}
	
	if err := block.UnmarshalBinary(data); err != nil {
		return nil, err
	}
	
	return block, nil
}

// Close closes the data file
func (df *DataFile) Close() error {
	if df.file != nil {
		return df.file.Close()
	}
	return nil
}

// Sync syncs the data file to disk
func (df *DataFile) Sync() error {
	if df.file != nil {
		return df.file.Sync()
	}
	return nil
}
