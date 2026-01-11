package wal

import (
	"encoding/binary"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

// Reader reads redo records from WAL files
type Reader struct {
	dir         string
	currentFile *os.File
	header      WALHeader
	offset      int64
	fileIndex   int
	files       []string
}

// NewReader creates a new WAL reader
func NewReader(dir string) (*Reader, error) {
	r := &Reader{
		dir:       dir,
		fileIndex: -1,
	}

	// List WAL files
	if err := r.scanFiles(); err != nil {
		return nil, err
	}

	return r, nil
}

// scanFiles scans the directory for WAL files
func (r *Reader) scanFiles() error {
	entries, err := os.ReadDir(r.dir)
	if err != nil {
		return fmt.Errorf("read wal dir: %w", err)
	}

	r.files = nil
	for _, entry := range entries {
		if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".wal") {
			r.files = append(r.files, entry.Name())
		}
	}

	// Sort files by name (which includes sequence number)
	sort.Strings(r.files)

	return nil
}

// SeekToLSN seeks to the first record with LSN >= targetLSN
func (r *Reader) SeekToLSN(targetLSN uint64) error {
	// Find the file containing the target LSN
	for i, fileName := range r.files {
		filePath := filepath.Join(r.dir, fileName)
		f, err := os.Open(filePath)
		if err != nil {
			continue
		}

		// Read header
		headerBuf := make([]byte, WALHeaderSize)
		if _, err := io.ReadFull(f, headerBuf); err != nil {
			f.Close()
			continue
		}

		var header WALHeader
		if err := header.Decode(headerBuf); err != nil {
			f.Close()
			continue
		}

		// Check if target LSN is in this file
		if header.EndLSN > 0 && targetLSN > header.EndLSN {
			f.Close()
			continue
		}

		if targetLSN >= header.StartLSN {
			// Found the right file
			if r.currentFile != nil {
				r.currentFile.Close()
			}
			r.currentFile = f
			r.header = header
			r.offset = WALHeaderSize
			r.fileIndex = i

			// Scan to find the exact record
			for {
				record, err := r.readRecord()
				if err != nil {
					break
				}
				if record.Header.LSN >= targetLSN {
					// Seek back to this record
					recordSize := int64(record.Size())
					r.offset -= recordSize
					r.currentFile.Seek(r.offset, 0)
					return nil
				}
			}
			return nil
		}

		f.Close()
	}

	return ErrInvalidLSN
}

// ReadNext reads the next redo record
func (r *Reader) ReadNext() (*RedoRecord, error) {
	for {
		// Open next file if needed
		if r.currentFile == nil {
			if err := r.openNextFile(); err != nil {
				return nil, err
			}
		}

		// Try to read a record
		record, err := r.readRecord()
		if err == io.EOF {
			// Try next file
			r.currentFile.Close()
			r.currentFile = nil
			continue
		}
		if err != nil {
			return nil, err
		}

		return record, nil
	}
}

// openNextFile opens the next WAL file
func (r *Reader) openNextFile() error {
	r.fileIndex++
	if r.fileIndex >= len(r.files) {
		return io.EOF
	}

	filePath := filepath.Join(r.dir, r.files[r.fileIndex])
	f, err := os.Open(filePath)
	if err != nil {
		return fmt.Errorf("open wal file: %w", err)
	}

	// Read header
	headerBuf := make([]byte, WALHeaderSize)
	if _, err := io.ReadFull(f, headerBuf); err != nil {
		f.Close()
		return fmt.Errorf("read wal header: %w", err)
	}

	if err := r.header.Decode(headerBuf); err != nil {
		f.Close()
		return err
	}

	r.currentFile = f
	r.offset = WALHeaderSize

	return nil
}

// readRecord reads a single redo record
func (r *Reader) readRecord() (*RedoRecord, error) {
	// Read record header
	headerBuf := make([]byte, RedoRecordHeaderSize)
	n, err := io.ReadFull(r.currentFile, headerBuf)
	if err != nil {
		if err == io.EOF || n == 0 {
			return nil, io.EOF
		}
		return nil, fmt.Errorf("read record header: %w", err)
	}

	// Check if we hit the footer
	if binary.LittleEndian.Uint32(headerBuf[0:4]) == WALEndMagic {
		return nil, io.EOF
	}

	var header RedoRecordHeader
	if err := header.Decode(headerBuf); err != nil {
		return nil, err
	}

	// Read data
	data := make([]byte, header.DataLen)
	if _, err := io.ReadFull(r.currentFile, data); err != nil {
		return nil, fmt.Errorf("read record data: %w", err)
	}

	// Read checksum
	checksumBuf := make([]byte, 4)
	if _, err := io.ReadFull(r.currentFile, checksumBuf); err != nil {
		return nil, fmt.Errorf("read record checksum: %w", err)
	}
	storedChecksum := binary.LittleEndian.Uint32(checksumBuf)

	record := &RedoRecord{
		Header:   header,
		Data:     data,
		Checksum: storedChecksum,
	}

	// Verify checksum
	expectedChecksum := record.CalculateChecksum()
	if storedChecksum != expectedChecksum {
		return nil, ErrChecksumMismatch
	}

	r.offset += int64(record.Size())

	return record, nil
}

// Close closes the reader
func (r *Reader) Close() error {
	if r.currentFile != nil {
		return r.currentFile.Close()
	}
	return nil
}

// GetRecordsBetween returns all records between fromLSN and toLSN
func (r *Reader) GetRecordsBetween(fromLSN, toLSN uint64) ([]*RedoRecord, error) {
	if err := r.SeekToLSN(fromLSN); err != nil {
		return nil, err
	}

	var records []*RedoRecord
	for {
		record, err := r.ReadNext()
		if err == io.EOF {
			break
		}
		if err != nil {
			return nil, err
		}
		if record.Header.LSN > toLSN {
			break
		}
		records = append(records, record)
	}

	return records, nil
}

// GetFileInfo returns information about WAL files
func (r *Reader) GetFileInfo() []WALFileInfo {
	var infos []WALFileInfo
	for _, fileName := range r.files {
		filePath := filepath.Join(r.dir, fileName)
		f, err := os.Open(filePath)
		if err != nil {
			continue
		}

		headerBuf := make([]byte, WALHeaderSize)
		if _, err := io.ReadFull(f, headerBuf); err != nil {
			f.Close()
			continue
		}

		var header WALHeader
		if err := header.Decode(headerBuf); err != nil {
			f.Close()
			continue
		}

		stat, _ := f.Stat()
		f.Close()

		infos = append(infos, WALFileInfo{
			FileName:    fileName,
			FileID:      header.FileID,
			StartLSN:    header.StartLSN,
			EndLSN:      header.EndLSN,
			RecordCount: header.RecordCount,
			SizeBytes:   stat.Size(),
		})
	}
	return infos
}

// WALFileInfo contains information about a WAL file
type WALFileInfo struct {
	FileName    string
	FileID      uint64
	StartLSN    uint64
	EndLSN      uint64
	RecordCount uint32
	SizeBytes   int64
}
