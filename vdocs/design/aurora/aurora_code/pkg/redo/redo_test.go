package redo

import (
	"testing"

	"github.com/percona/aurora/pkg/wal"
)

func TestInsertRedoEncodeDecode(t *testing.T) {
	original := &InsertRedo{
		SlotNo:    5,
		RecordLen: 10,
		RecordData: []byte("test data!"),
	}

	encoded := original.Encode()
	decoded, err := ParseInsert(encoded)
	if err != nil {
		t.Fatalf("parse failed: %v", err)
	}

	if decoded.SlotNo != original.SlotNo {
		t.Errorf("SlotNo mismatch: expected %d, got %d", original.SlotNo, decoded.SlotNo)
	}
	if string(decoded.RecordData) != string(original.RecordData) {
		t.Errorf("RecordData mismatch: expected %s, got %s", original.RecordData, decoded.RecordData)
	}
}

func TestUpdateRedoEncodeDecode(t *testing.T) {
	original := &UpdateRedo{
		SlotNo:  3,
		Offset:  100,
		OldLen:  5,
		NewLen:  6,
		OldData: []byte("hello"),
		NewData: []byte("world!"),
	}

	encoded := original.Encode()
	decoded, err := ParseUpdate(encoded)
	if err != nil {
		t.Fatalf("parse failed: %v", err)
	}

	if decoded.SlotNo != original.SlotNo {
		t.Errorf("SlotNo mismatch: expected %d, got %d", original.SlotNo, decoded.SlotNo)
	}
	if decoded.Offset != original.Offset {
		t.Errorf("Offset mismatch: expected %d, got %d", original.Offset, decoded.Offset)
	}
	if string(decoded.OldData) != string(original.OldData) {
		t.Errorf("OldData mismatch: expected %s, got %s", original.OldData, decoded.OldData)
	}
	if string(decoded.NewData) != string(original.NewData) {
		t.Errorf("NewData mismatch: expected %s, got %s", original.NewData, decoded.NewData)
	}
}

func TestDeleteRedoEncodeDecode(t *testing.T) {
	original := &DeleteRedo{
		SlotNo:    7,
		RecordLen: 8,
		RecordData: []byte("deleted!"),
	}

	encoded := original.Encode()
	decoded, err := ParseDelete(encoded)
	if err != nil {
		t.Fatalf("parse failed: %v", err)
	}

	if decoded.SlotNo != original.SlotNo {
		t.Errorf("SlotNo mismatch: expected %d, got %d", original.SlotNo, decoded.SlotNo)
	}
	if string(decoded.RecordData) != string(original.RecordData) {
		t.Errorf("RecordData mismatch: expected %s, got %s", original.RecordData, decoded.RecordData)
	}
}

func TestNewRedoRecord(t *testing.T) {
	insert := &InsertRedo{
		SlotNo:    1,
		RecordLen: 4,
		RecordData: []byte("test"),
	}
	data := insert.Encode()

	record := NewRedoRecord(1000, 1, 100, 500, 10, wal.RedoTypeInsert, 0, data)

	if record.Header.LSN != 1000 {
		t.Errorf("LSN mismatch: expected 1000, got %d", record.Header.LSN)
	}
	if record.Header.Type != wal.RedoTypeInsert {
		t.Errorf("Type mismatch: expected INSERT, got %d", record.Header.Type)
	}
	if record.Header.DataLen != uint32(len(data)) {
		t.Errorf("DataLen mismatch: expected %d, got %d", len(data), record.Header.DataLen)
	}
}

func TestParseRedoRecord(t *testing.T) {
	// Test INSERT
	insertData := (&InsertRedo{SlotNo: 1, RecordLen: 4, RecordData: []byte("test")}).Encode()
	insertRecord := NewRedoRecord(1000, 1, 100, 500, 10, wal.RedoTypeInsert, 0, insertData)

	parsed, err := ParseRedoRecord(insertRecord)
	if err != nil {
		t.Fatalf("parse INSERT failed: %v", err)
	}
	if insert, ok := parsed.(*InsertRedo); !ok {
		t.Error("expected *InsertRedo")
	} else if insert.SlotNo != 1 {
		t.Errorf("SlotNo mismatch: expected 1, got %d", insert.SlotNo)
	}

	// Test COMMIT (no data)
	commitRecord := NewRedoRecord(1001, 0, 0, 500, 10, wal.RedoTypeTrxCommit, 0, nil)
	parsed, err = ParseRedoRecord(commitRecord)
	if err != nil {
		t.Fatalf("parse COMMIT failed: %v", err)
	}
	if parsed != nil {
		t.Error("expected nil for COMMIT")
	}
}

func TestInvalidRedoData(t *testing.T) {
	// Too short for INSERT
	_, err := ParseInsert([]byte{0, 0})
	if err != ErrInvalidRedoData {
		t.Errorf("expected ErrInvalidRedoData, got %v", err)
	}

	// Too short for UPDATE
	_, err = ParseUpdate([]byte{0, 0, 0, 0})
	if err != ErrInvalidRedoData {
		t.Errorf("expected ErrInvalidRedoData, got %v", err)
	}
}
