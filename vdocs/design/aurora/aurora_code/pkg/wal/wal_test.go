package wal

import (
	"os"
	"path/filepath"
	"testing"
)

func TestWALHeaderEncodeDecode(t *testing.T) {
	header := WALHeader{
		Magic:       WALMagic,
		Version:     1,
		FileID:      123,
		VolumeID:    [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
		StartLSN:    1000,
		EndLSN:      2000,
		RecordCount: 100,
		Flags:       0,
	}

	encoded := header.Encode()
	if len(encoded) != WALHeaderSize {
		t.Errorf("expected header size %d, got %d", WALHeaderSize, len(encoded))
	}

	var decoded WALHeader
	if err := decoded.Decode(encoded); err != nil {
		t.Fatalf("decode failed: %v", err)
	}

	if decoded.Magic != header.Magic {
		t.Errorf("magic mismatch: expected %x, got %x", header.Magic, decoded.Magic)
	}
	if decoded.FileID != header.FileID {
		t.Errorf("file ID mismatch: expected %d, got %d", header.FileID, decoded.FileID)
	}
	if decoded.StartLSN != header.StartLSN {
		t.Errorf("start LSN mismatch: expected %d, got %d", header.StartLSN, decoded.StartLSN)
	}
}

func TestRedoRecordHeaderEncodeDecode(t *testing.T) {
	header := RedoRecordHeader{
		LSN:     1000,
		SpaceID: 1,
		PageID:  100,
		TrxID:   500,
		MtrID:   10,
		Type:    RedoTypeInsert,
		Flags:   uint16(FlagMtrEnd),
		DataLen: 256,
	}

	encoded := header.Encode()
	if len(encoded) != RedoRecordHeaderSize {
		t.Errorf("expected header size %d, got %d", RedoRecordHeaderSize, len(encoded))
	}

	var decoded RedoRecordHeader
	if err := decoded.Decode(encoded); err != nil {
		t.Fatalf("decode failed: %v", err)
	}

	if decoded.LSN != header.LSN {
		t.Errorf("LSN mismatch: expected %d, got %d", header.LSN, decoded.LSN)
	}
	if decoded.Type != header.Type {
		t.Errorf("type mismatch: expected %d, got %d", header.Type, decoded.Type)
	}
}

func TestWALWriterReader(t *testing.T) {
	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "wal_test")
	if err != nil {
		t.Fatalf("create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	walDir := filepath.Join(tmpDir, "wal")
	volumeID := [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}

	// Create writer
	writer, err := NewWriter(walDir, volumeID, 0)
	if err != nil {
		t.Fatalf("create writer: %v", err)
	}

	// Write some records
	numRecords := 100
	for i := 0; i < numRecords; i++ {
		record := &RedoRecord{
			Header: RedoRecordHeader{
				LSN:     uint64(i),
				SpaceID: 1,
				PageID:  uint64(i % 10),
				TrxID:   uint64(i / 10),
				MtrID:   uint64(i),
				Type:    RedoTypeInsert,
				Flags:   uint16(FlagMtrEnd),
				DataLen: 32,
			},
			Data: make([]byte, 32),
		}
		// Fill data with some pattern
		for j := range record.Data {
			record.Data[j] = byte(i + j)
		}

		if err := writer.Write(record); err != nil {
			t.Fatalf("write record %d: %v", i, err)
		}
	}

	// Sync and close writer
	if err := writer.Sync(); err != nil {
		t.Fatalf("sync writer: %v", err)
	}
	if err := writer.Close(); err != nil {
		t.Fatalf("close writer: %v", err)
	}

	// Create reader
	reader, err := NewReader(walDir)
	if err != nil {
		t.Fatalf("create reader: %v", err)
	}
	defer reader.Close()

	// Read all records
	readCount := 0
	for {
		record, err := reader.ReadNext()
		if err != nil {
			break
		}
		if record.Header.LSN != uint64(readCount) {
			t.Errorf("LSN mismatch at %d: expected %d, got %d", readCount, readCount, record.Header.LSN)
		}
		readCount++
	}

	if readCount != numRecords {
		t.Errorf("record count mismatch: expected %d, got %d", numRecords, readCount)
	}
}

func TestWALSeekToLSN(t *testing.T) {
	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "wal_test")
	if err != nil {
		t.Fatalf("create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	walDir := filepath.Join(tmpDir, "wal")
	volumeID := [16]byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}

	// Create writer and write records
	writer, err := NewWriter(walDir, volumeID, 0)
	if err != nil {
		t.Fatalf("create writer: %v", err)
	}

	for i := 0; i < 50; i++ {
		record := &RedoRecord{
			Header: RedoRecordHeader{
				LSN:     uint64(i),
				SpaceID: 1,
				PageID:  uint64(i),
				Type:    RedoTypeInsert,
				DataLen: 16,
			},
			Data: make([]byte, 16),
		}
		if err := writer.Write(record); err != nil {
			t.Fatalf("write record: %v", err)
		}
	}
	writer.Close()

	// Test seek
	reader, err := NewReader(walDir)
	if err != nil {
		t.Fatalf("create reader: %v", err)
	}
	defer reader.Close()

	targetLSN := uint64(25)
	if err := reader.SeekToLSN(targetLSN); err != nil {
		t.Fatalf("seek to LSN: %v", err)
	}

	record, err := reader.ReadNext()
	if err != nil {
		t.Fatalf("read after seek: %v", err)
	}

	if record.Header.LSN != targetLSN {
		t.Errorf("expected LSN %d after seek, got %d", targetLSN, record.Header.LSN)
	}
}

func TestRedoRecordChecksum(t *testing.T) {
	record := &RedoRecord{
		Header: RedoRecordHeader{
			LSN:     1000,
			SpaceID: 1,
			PageID:  100,
			Type:    RedoTypeInsert,
			DataLen: 8,
		},
		Data: []byte{1, 2, 3, 4, 5, 6, 7, 8},
	}

	checksum1 := record.CalculateChecksum()
	checksum2 := record.CalculateChecksum()

	if checksum1 != checksum2 {
		t.Errorf("checksum should be deterministic: %x != %x", checksum1, checksum2)
	}

	// Modify data and verify checksum changes
	record.Data[0] = 99
	checksum3 := record.CalculateChecksum()

	if checksum1 == checksum3 {
		t.Error("checksum should change when data changes")
	}
}
