package wal

import (
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"os"
	"path/filepath"
	"sync"
)

// Writer writes redo records to WAL files
type Writer struct {
	mu          sync.Mutex
	dir         string
	volumeID    [16]byte
	currentFile *os.File
	header      WALHeader
	offset      int64
	maxFileSize int64
	closed      bool
}

// NewWriter creates a new WAL writer
func NewWriter(dir string, volumeID [16]byte, startLSN uint64) (*Writer, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("create wal dir: %w", err)
	}

	w := &Writer{
		dir:         dir,
		volumeID:    volumeID,
		maxFileSize: DefaultWALFileSize,
	}

	if err := w.createNewFile(startLSN); err != nil {
		return nil, err
	}

	return w, nil
}

// createNewFile creates a new WAL file
func (w *Writer) createNewFile(startLSN uint64) error {
	// Close current file if exists
	if w.currentFile != nil {
		if err := w.sealCurrentFile(); err != nil {
			return err
		}
	}

	// Generate file ID and name
	fileID := startLSN / uint64(w.maxFileSize)
	fileName := fmt.Sprintf("wal_%06d.wal", fileID)
	filePath := filepath.Join(w.dir, fileName)

	// Create new file
	f, err := os.OpenFile(filePath, os.O_CREATE|os.O_RDWR|os.O_EXCL, 0644)
	if err != nil {
		return fmt.Errorf("create wal file: %w", err)
	}

	// Initialize header
	w.header = WALHeader{
		Magic:       WALMagic,
		Version:     1,
		FileID:      fileID,
		VolumeID:    w.volumeID,
		StartLSN:    startLSN,
		EndLSN:      0, // Will be set when sealed
		RecordCount: 0,
		Flags:       0,
	}

	// Write header
	headerBytes := w.header.Encode()
	if _, err := f.Write(headerBytes); err != nil {
		f.Close()
		return fmt.Errorf("write wal header: %w", err)
	}

	w.currentFile = f
	w.offset = WALHeaderSize

	return nil
}

// sealCurrentFile seals the current WAL file with footer
func (w *Writer) sealCurrentFile() error {
	if w.currentFile == nil {
		return nil
	}

	// Update header with end LSN
	w.header.EndLSN = w.header.StartLSN + uint64(w.header.RecordCount)

	// Seek to beginning and rewrite header
	if _, err := w.currentFile.Seek(0, 0); err != nil {
		return err
	}
	headerBytes := w.header.Encode()
	if _, err := w.currentFile.Write(headerBytes); err != nil {
		return err
	}

	// Seek to end and write footer
	if _, err := w.currentFile.Seek(w.offset, 0); err != nil {
		return err
	}

	footer := WALFooter{
		Magic:       WALEndMagic,
		EndLSN:      w.header.EndLSN,
		RecordCount: w.header.RecordCount,
		FileSize:    uint64(w.offset + WALFooterSize),
	}
	footerBytes := make([]byte, WALFooterSize)
	binary.LittleEndian.PutUint32(footerBytes[0:4], footer.Magic)
	binary.LittleEndian.PutUint64(footerBytes[4:12], footer.EndLSN)
	binary.LittleEndian.PutUint32(footerBytes[12:16], footer.RecordCount)
	binary.LittleEndian.PutUint64(footerBytes[16:24], footer.FileSize)
	footer.Checksum = crc32.ChecksumIEEE(footerBytes[0:24])
	binary.LittleEndian.PutUint32(footerBytes[24:28], footer.Checksum)

	if _, err := w.currentFile.Write(footerBytes); err != nil {
		return err
	}

	// Sync and close
	if err := w.currentFile.Sync(); err != nil {
		return err
	}
	if err := w.currentFile.Close(); err != nil {
		return err
	}

	w.currentFile = nil
	return nil
}

// Write writes a redo record to the WAL
func (w *Writer) Write(record *RedoRecord) error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.closed {
		return ErrWALClosed
	}

	recordSize := int64(record.Size())

	// Check if we need a new file
	if w.offset+recordSize+WALFooterSize > w.maxFileSize {
		if err := w.createNewFile(record.Header.LSN); err != nil {
			return err
		}
	}

	// Write record header
	headerBytes := record.Header.Encode()
	if _, err := w.currentFile.Write(headerBytes); err != nil {
		return fmt.Errorf("write record header: %w", err)
	}

	// Write record data
	if _, err := w.currentFile.Write(record.Data); err != nil {
		return fmt.Errorf("write record data: %w", err)
	}

	// Write checksum
	record.Checksum = record.CalculateChecksum()
	checksumBytes := make([]byte, 4)
	binary.LittleEndian.PutUint32(checksumBytes, record.Checksum)
	if _, err := w.currentFile.Write(checksumBytes); err != nil {
		return fmt.Errorf("write record checksum: %w", err)
	}

	w.offset += recordSize
	w.header.RecordCount++

	return nil
}

// Sync syncs the WAL file to disk
func (w *Writer) Sync() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.currentFile != nil {
		return w.currentFile.Sync()
	}
	return nil
}

// Close closes the WAL writer
func (w *Writer) Close() error {
	w.mu.Lock()
	defer w.mu.Unlock()

	if w.closed {
		return nil
	}

	w.closed = true
	return w.sealCurrentFile()
}

// CurrentLSN returns the current LSN
func (w *Writer) CurrentLSN() uint64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.header.StartLSN + uint64(w.header.RecordCount)
}

// FileID returns the current file ID
func (w *Writer) FileID() uint64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.header.FileID
}
