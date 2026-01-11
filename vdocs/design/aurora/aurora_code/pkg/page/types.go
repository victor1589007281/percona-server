// Package page implements page format handling
package page

import (
	"encoding/binary"
	"errors"
	"hash/crc32"
)

const (
	// PageSize is the standard page size (16KB)
	PageSize = 16 * 1024

	// PageHeaderSize is the size of page header
	PageHeaderSize = 56

	// PageTrailerSize is the size of page trailer
	PageTrailerSize = 8

	// PageBodySize is the usable body size
	PageBodySize = PageSize - PageHeaderSize - PageTrailerSize
)

// PageType defines the type of page
type PageType uint32

const (
	PageTypeData     PageType = 1 // Data page
	PageTypeIndex    PageType = 2 // Index page
	PageTypeFSP      PageType = 3 // File space page
	PageTypeXDES     PageType = 4 // Extent descriptor page
	PageTypeBlob     PageType = 5 // BLOB page
	PageTypeUndo     PageType = 6 // Undo page
	PageTypeSysTable PageType = 7 // System table page
)

// Errors
var (
	ErrInvalidPageSize     = errors.New("invalid page size")
	ErrInvalidPageChecksum = errors.New("invalid page checksum")
	ErrInvalidPageType     = errors.New("invalid page type")
)

// PageHeader represents the header of a page (56 bytes)
type PageHeader struct {
	SpaceID     uint64   // Tablespace ID
	PageID      uint64   // Page ID
	PageLSN     uint64   // Page LSN
	PageType    PageType // Page type
	Checksum    uint32   // Page checksum
	RecordCount uint16   // Record count
	FreeSpace   uint16   // Available space
	HeapTop     uint16   // Heap top offset
	SlotCount   uint16   // Slot count
	TrxID       uint64   // Last modifying transaction ID
	PrevPage    uint64   // Previous page ID
}

// PageTrailer represents the trailer of a page (8 bytes)
type PageTrailer struct {
	OldChecksum uint32 // Old checksum (for compatibility)
	LSNLow      uint32 // Low 32 bits of LSN
}

// Page represents a complete 16KB page
type Page struct {
	Header  PageHeader
	Body    []byte // 16272 bytes
	Trailer PageTrailer
}

// NewPage creates a new empty page
func NewPage(spaceID, pageID uint64, pageType PageType) *Page {
	return &Page{
		Header: PageHeader{
			SpaceID:     spaceID,
			PageID:      pageID,
			PageType:    pageType,
			RecordCount: 0,
			FreeSpace:   PageBodySize,
			HeapTop:     0,
			SlotCount:   0,
		},
		Body: make([]byte, PageBodySize),
	}
}

// Encode serializes the page to bytes
func (p *Page) Encode() []byte {
	buf := make([]byte, PageSize)

	// Encode header
	binary.LittleEndian.PutUint64(buf[0:8], p.Header.SpaceID)
	binary.LittleEndian.PutUint64(buf[8:16], p.Header.PageID)
	binary.LittleEndian.PutUint64(buf[16:24], p.Header.PageLSN)
	binary.LittleEndian.PutUint32(buf[24:28], uint32(p.Header.PageType))
	// Checksum will be calculated after body is copied
	binary.LittleEndian.PutUint16(buf[32:34], p.Header.RecordCount)
	binary.LittleEndian.PutUint16(buf[34:36], p.Header.FreeSpace)
	binary.LittleEndian.PutUint16(buf[36:38], p.Header.HeapTop)
	binary.LittleEndian.PutUint16(buf[38:40], p.Header.SlotCount)
	binary.LittleEndian.PutUint64(buf[40:48], p.Header.TrxID)
	binary.LittleEndian.PutUint64(buf[48:56], p.Header.PrevPage)

	// Copy body
	copy(buf[PageHeaderSize:PageHeaderSize+PageBodySize], p.Body)

	// Encode trailer
	p.Trailer.LSNLow = uint32(p.Header.PageLSN & 0xFFFFFFFF)
	binary.LittleEndian.PutUint32(buf[PageSize-8:PageSize-4], p.Trailer.OldChecksum)
	binary.LittleEndian.PutUint32(buf[PageSize-4:PageSize], p.Trailer.LSNLow)

	// Calculate and write checksum
	p.Header.Checksum = crc32.ChecksumIEEE(buf[0:28])
	p.Header.Checksum = crc32.Update(p.Header.Checksum, crc32.IEEETable, buf[32:PageSize])
	binary.LittleEndian.PutUint32(buf[28:32], p.Header.Checksum)

	return buf
}

// Decode deserializes a page from bytes
func (p *Page) Decode(buf []byte) error {
	if len(buf) != PageSize {
		return ErrInvalidPageSize
	}

	// Decode header
	p.Header.SpaceID = binary.LittleEndian.Uint64(buf[0:8])
	p.Header.PageID = binary.LittleEndian.Uint64(buf[8:16])
	p.Header.PageLSN = binary.LittleEndian.Uint64(buf[16:24])
	p.Header.PageType = PageType(binary.LittleEndian.Uint32(buf[24:28]))
	p.Header.Checksum = binary.LittleEndian.Uint32(buf[28:32])
	p.Header.RecordCount = binary.LittleEndian.Uint16(buf[32:34])
	p.Header.FreeSpace = binary.LittleEndian.Uint16(buf[34:36])
	p.Header.HeapTop = binary.LittleEndian.Uint16(buf[36:38])
	p.Header.SlotCount = binary.LittleEndian.Uint16(buf[38:40])
	p.Header.TrxID = binary.LittleEndian.Uint64(buf[40:48])
	p.Header.PrevPage = binary.LittleEndian.Uint64(buf[48:56])

	// Copy body
	p.Body = make([]byte, PageBodySize)
	copy(p.Body, buf[PageHeaderSize:PageHeaderSize+PageBodySize])

	// Decode trailer
	p.Trailer.OldChecksum = binary.LittleEndian.Uint32(buf[PageSize-8 : PageSize-4])
	p.Trailer.LSNLow = binary.LittleEndian.Uint32(buf[PageSize-4 : PageSize])

	// Verify checksum
	expectedChecksum := crc32.ChecksumIEEE(buf[0:28])
	expectedChecksum = crc32.Update(expectedChecksum, crc32.IEEETable, buf[32:PageSize])
	if p.Header.Checksum != expectedChecksum {
		return ErrInvalidPageChecksum
	}

	return nil
}

// SetLSN updates the page LSN
func (p *Page) SetLSN(lsn uint64) {
	p.Header.PageLSN = lsn
	p.Trailer.LSNLow = uint32(lsn & 0xFFFFFFFF)
}

// GetLSN returns the page LSN
func (p *Page) GetLSN() uint64 {
	return p.Header.PageLSN
}

// GetKey returns the page key (space_id, page_id)
func (p *Page) GetKey() PageKey {
	return PageKey{
		SpaceID: p.Header.SpaceID,
		PageID:  p.Header.PageID,
	}
}

// PageKey uniquely identifies a page
type PageKey struct {
	SpaceID uint64
	PageID  uint64
}

// ToUint64 converts PageKey to a single uint64 for hashing
func (k PageKey) ToUint64() uint64 {
	return (k.SpaceID << 32) | (k.PageID & 0xFFFFFFFF)
}
