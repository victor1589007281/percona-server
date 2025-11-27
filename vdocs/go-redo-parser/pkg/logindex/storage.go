package logindex

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"time"
)

// Storage constants
const (
	// File names
	MetaFileName       = "polar_logindex.meta"
	DataFilePrefix     = "polar_logindex"
	CheckpointFileName = "polar_logindex.ckpt"

	// File sizes
	MetaHeaderSize = 4096                    // 4KB header
	DataFileMaxSize = 1 * 1024 * 1024 * 1024 // 1GB per data file
	PageEntrySize   = 64                     // 64 bytes per page entry

	// Magic number
	MetaFileMagic  = "POLARDB_LOGIDX\x00"
	MetaFileVersion = 1
)

// MetaFileHeader represents the 4KB header of meta file
type MetaFileHeader struct {
	Magic          [16]byte  // "POLARDB_LOGIDX\0"
	Version        uint32    // File format version
	CreateTime     uint64    // Unix timestamp
	LastUpdate     uint64    // Unix timestamp
	CheckpointLSN  uint64    // Checkpoint LSN
	NumPages       uint64    // Number of pages indexed
	TotalLSNs      uint64    // Total number of LSNs
	DataFileCount  uint32    // Number of data files
	Checksum       uint32    // CRC32 checksum
	Reserved       [3952]byte // Padding to 4KB
}

// NewMetaFileHeader creates a new meta file header
func NewMetaFileHeader() *MetaFileHeader {
	header := &MetaFileHeader{
		Version:    MetaFileVersion,
		CreateTime: uint64(time.Now().Unix()),
	}
	copy(header.Magic[:], MetaFileMagic)
	return header
}

// Validate checks if the header is valid
func (h *MetaFileHeader) Validate() error {
	if string(h.Magic[:15]) != MetaFileMagic[:15] {
		return fmt.Errorf("invalid magic number")
	}
	if h.Version != MetaFileVersion {
		return fmt.Errorf("unsupported version: %d", h.Version)
	}
	return nil
}

// PageEntry represents an entry in the meta file index
type PageEntry struct {
	SpaceID    uint32 // Tablespace ID
	PageNo     uint32 // Page number
	FileOffset uint64 // Offset in data file
	LSNCount   uint32 // Number of LSNs
	MinLSN     uint64 // Minimum LSN
	MaxLSN     uint64 // Maximum LSN
	Reserved   [28]byte // Reserved for future use (total 64 bytes)
}

// MarshalBinary encodes the header to binary
func (h *MetaFileHeader) MarshalBinary(buf []byte) error {
	if len(buf) < MetaHeaderSize {
		return fmt.Errorf("buffer too small")
	}
	offset := 0
	copy(buf[offset:], h.Magic[:])
	offset += 16
	binary.LittleEndian.PutUint32(buf[offset:], h.Version)
	offset += 4
	binary.LittleEndian.PutUint64(buf[offset:], h.CreateTime)
	offset += 8
	binary.LittleEndian.PutUint64(buf[offset:], h.LastUpdate)
	offset += 8
	binary.LittleEndian.PutUint64(buf[offset:], h.CheckpointLSN)
	offset += 8
	binary.LittleEndian.PutUint64(buf[offset:], h.NumPages)
	offset += 8
	binary.LittleEndian.PutUint64(buf[offset:], h.TotalLSNs)
	offset += 8
	binary.LittleEndian.PutUint32(buf[offset:], h.DataFileCount)
	offset += 4
	binary.LittleEndian.PutUint32(buf[offset:], h.Checksum)
	offset += 4
	copy(buf[offset:], h.Reserved[:])
	return nil
}

// UnmarshalBinary decodes the header from binary
func (h *MetaFileHeader) UnmarshalBinary(buf []byte) error {
	if len(buf) < MetaHeaderSize {
		return fmt.Errorf("buffer too small")
	}
	offset := 0
	copy(h.Magic[:], buf[offset:offset+16])
	offset += 16
	h.Version = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	h.CreateTime = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	h.LastUpdate = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	h.CheckpointLSN = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	h.NumPages = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	h.TotalLSNs = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	h.DataFileCount = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	h.Checksum = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	copy(h.Reserved[:], buf[offset:offset+3952])
	return nil
}

// UpdateChecksum calculates and updates the checksum
func (h *MetaFileHeader) UpdateChecksum() {
	data := make([]byte, MetaHeaderSize)
	h.MarshalBinary(data)
	binary.LittleEndian.PutUint32(data[48:52], 0)
	h.Checksum = crc32.ChecksumIEEE(data)
}

// MarshalBinary encodes the entry to binary
func (e *PageEntry) MarshalBinary(buf []byte) error {
	if len(buf) < PageEntrySize {
		return fmt.Errorf("buffer too small")
	}
	offset := 0
	binary.LittleEndian.PutUint32(buf[offset:], e.SpaceID)
	offset += 4
	binary.LittleEndian.PutUint32(buf[offset:], e.PageNo)
	offset += 4
	binary.LittleEndian.PutUint64(buf[offset:], e.FileOffset)
	offset += 8
	binary.LittleEndian.PutUint32(buf[offset:], e.LSNCount)
	offset += 4
	binary.LittleEndian.PutUint64(buf[offset:], e.MinLSN)
	offset += 8
	binary.LittleEndian.PutUint64(buf[offset:], e.MaxLSN)
	offset += 8
	copy(buf[offset:], e.Reserved[:])
	return nil
}

// UnmarshalBinary decodes the entry from binary
func (e *PageEntry) UnmarshalBinary(buf []byte) error {
	if len(buf) < PageEntrySize {
		return fmt.Errorf("buffer too small")
	}
	offset := 0
	e.SpaceID = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	e.PageNo = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	e.FileOffset = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	e.LSNCount = binary.LittleEndian.Uint32(buf[offset:])
	offset += 4
	e.MinLSN = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	e.MaxLSN = binary.LittleEndian.Uint64(buf[offset:])
	offset += 8
	copy(e.Reserved[:], buf[offset:offset+28])
	return nil
}

// MetaFile represents the complete meta file
type MetaFile struct {
	Header   *MetaFileHeader
	Entries  []PageEntry
	filePath string
}

// NewMetaFile creates a new meta file
func NewMetaFile(filePath string) *MetaFile {
	return &MetaFile{
		Header:   NewMetaFileHeader(),
		Entries:  make([]PageEntry, 0),
		filePath: filePath,
	}
}

// Save writes the meta file to disk
func (m *MetaFile) Save() error {
	m.Header.LastUpdate = uint64(time.Now().Unix())
	m.Header.NumPages = uint64(len(m.Entries))
	m.Header.UpdateChecksum()

	file, err := os.OpenFile(m.filePath, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0644)
	if err != nil {
		return err
	}
	defer file.Close()

	headerBuf := make([]byte, MetaHeaderSize)
	if err := m.Header.MarshalBinary(headerBuf); err != nil {
		return err
	}
	if _, err := file.Write(headerBuf); err != nil {
		return err
	}

	for _, entry := range m.Entries {
		entryBuf := make([]byte, PageEntrySize)
		if err := entry.MarshalBinary(entryBuf); err != nil {
			return err
		}
		if _, err := file.Write(entryBuf); err != nil {
			return err
		}
	}
	return file.Sync()
}

// Load reads the meta file from disk
func (m *MetaFile) Load() error {
	file, err := os.Open(m.filePath)
	if err != nil {
		return err
	}
	defer file.Close()

	headerBuf := make([]byte, MetaHeaderSize)
	if _, err := file.Read(headerBuf); err != nil {
		return err
	}

	m.Header = &MetaFileHeader{}
	if err := m.Header.UnmarshalBinary(headerBuf); err != nil {
		return err
	}
	if err := m.Header.Validate(); err != nil {
		return err
	}

	m.Entries = make([]PageEntry, m.Header.NumPages)
	for i := uint64(0); i < m.Header.NumPages; i++ {
		entryBuf := make([]byte, PageEntrySize)
		if _, err := file.Read(entryBuf); err != nil {
			return err
		}
		if err := m.Entries[i].UnmarshalBinary(entryBuf); err != nil {
			return err
		}
	}
	return nil
}

// AddEntry adds a page entry to meta file
func (m *MetaFile) AddEntry(entry PageEntry) {
	m.Entries = append(m.Entries, entry)
}

// UpdateEntry updates or adds a page entry
func (m *MetaFile) UpdateEntry(entry PageEntry) {
	for i, e := range m.Entries {
		if e.SpaceID == entry.SpaceID && e.PageNo == entry.PageNo {
			m.Entries[i] = entry
			return
		}
	}
	m.AddEntry(entry)
}

// FindEntry finds a page entry
func (m *MetaFile) FindEntry(spaceID, pageNo uint32) (*PageEntry, int) {
	for i, e := range m.Entries {
		if e.SpaceID == spaceID && e.PageNo == pageNo {
			return &e, i
		}
	}
	return nil, -1
}
