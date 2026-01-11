package storage

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/percona/aurora/pkg/page"
	"github.com/percona/aurora/pkg/redo"
	"github.com/percona/aurora/pkg/wal"
)

func TestVolumeStoreWriteRead(t *testing.T) {
	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "storage_test")
	if err != nil {
		t.Fatalf("create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	config := Config{
		NodeID:        "test-node",
		DataDir:       filepath.Join(tmpDir, "data"),
		WALDir:        filepath.Join(tmpDir, "wal"),
		PageCacheSize: 100,
	}

	vs, err := NewVolumeStore("test-volume", config)
	if err != nil {
		t.Fatalf("create volume store: %v", err)
	}
	defer vs.Close()

	// Write some redo records
	for i := 0; i < 10; i++ {
		insertData := (&redo.InsertRedo{
			SlotNo:     uint16(i),
			RecordLen:  8,
			RecordData: []byte("testdata"),
		}).Encode()

		record := redo.NewRedoRecord(
			uint64(i),
			1, // spaceID
			1, // pageID
			uint64(i),
			uint64(i),
			wal.RedoTypeInsert,
			0,
			insertData,
		)

		lsn, err := vs.WriteRedo(record)
		if err != nil {
			t.Fatalf("write redo %d: %v", i, err)
		}
		if lsn != uint64(i) {
			t.Errorf("expected LSN %d, got %d", i, lsn)
		}
	}

	// Read page (will trigger materialization)
	p, err := vs.ReadPage(1, 1, 0)
	if err != nil {
		t.Fatalf("read page: %v", err)
	}

	if p.Header.SpaceID != 1 {
		t.Errorf("expected SpaceID 1, got %d", p.Header.SpaceID)
	}
}

func TestVolumeStoreFreeze(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "storage_test")
	if err != nil {
		t.Fatalf("create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	config := Config{
		NodeID:        "test-node",
		DataDir:       filepath.Join(tmpDir, "data"),
		WALDir:        filepath.Join(tmpDir, "wal"),
		PageCacheSize: 100,
	}

	vs, err := NewVolumeStore("test-volume", config)
	if err != nil {
		t.Fatalf("create volume store: %v", err)
	}
	defer vs.Close()

	// Write a record
	insertData := (&redo.InsertRedo{SlotNo: 1, RecordLen: 4, RecordData: []byte("test")}).Encode()
	record := redo.NewRedoRecord(1000, 1, 1, 1, 1, wal.RedoTypeInsert, 0, insertData)
	_, err = vs.WriteRedo(record)
	if err != nil {
		t.Fatalf("write redo: %v", err)
	}

	// Freeze
	finalLSN, err := vs.Freeze()
	if err != nil {
		t.Fatalf("freeze: %v", err)
	}
	if finalLSN != 1000 {
		t.Errorf("expected final LSN 1000, got %d", finalLSN)
	}

	// Write should fail
	_, err = vs.WriteRedo(record)
	if err != ErrWritesFrozen {
		t.Errorf("expected ErrWritesFrozen, got %v", err)
	}

	// Unfreeze
	if err := vs.Unfreeze(); err != nil {
		t.Fatalf("unfreeze: %v", err)
	}

	// Write should succeed again
	record.Header.LSN = 1001
	_, err = vs.WriteRedo(record)
	if err != nil {
		t.Errorf("write after unfreeze failed: %v", err)
	}
}

func TestPageMaterializer(t *testing.T) {
	m := NewPageMaterializer()

	// Create a base page
	basePage := page.NewPage(1, 100, page.PageTypeData)
	basePage.SetLSN(1000)
	m.SetBasePage(basePage)

	// Check base page exists
	p, ok := m.GetBasePage(1, 100)
	if !ok {
		t.Error("base page should exist")
	}
	if p.GetLSN() != 1000 {
		t.Errorf("expected LSN 1000, got %d", p.GetLSN())
	}

	// Apply insert
	insertRedo := &redo.InsertRedo{SlotNo: 1, RecordLen: 10, RecordData: make([]byte, 10)}
	insertRecord := redo.NewRedoRecord(1001, 1, 100, 1, 1, wal.RedoTypeInsert, 0, insertRedo.Encode())

	if err := m.ApplyRedo(basePage, insertRecord); err != nil {
		t.Fatalf("apply redo: %v", err)
	}

	if basePage.Header.RecordCount != 1 {
		t.Errorf("expected record count 1, got %d", basePage.Header.RecordCount)
	}
}

func TestCoalescing(t *testing.T) {
	m := NewPageMaterializer()
	c := NewCoalescing(m, 100)

	// Should not coalesce with few records
	if c.ShouldCoalesce(50) {
		t.Error("should not coalesce with 50 records")
	}

	// Should coalesce at threshold
	if !c.ShouldCoalesce(100) {
		t.Error("should coalesce with 100 records")
	}

	// Coalesce a page
	p := page.NewPage(1, 1, page.PageTypeData)
	p.SetLSN(5000)
	c.Coalesce(p)

	// Check base page was updated
	basePage, ok := m.GetBasePage(1, 1)
	if !ok {
		t.Error("base page should exist after coalescing")
	}
	if basePage.GetLSN() != 5000 {
		t.Errorf("expected LSN 5000, got %d", basePage.GetLSN())
	}
}

func TestServerStatus(t *testing.T) {
	tmpDir, err := os.MkdirTemp("", "storage_test")
	if err != nil {
		t.Fatalf("create temp dir: %v", err)
	}
	defer os.RemoveAll(tmpDir)

	config := Config{
		NodeID:        "test-node",
		GRPCPort:      9002,
		DataDir:       filepath.Join(tmpDir, "data"),
		WALDir:        filepath.Join(tmpDir, "wal"),
		PageCacheSize: 100,
	}

	server, err := NewServer(config)
	if err != nil {
		t.Fatalf("create server: %v", err)
	}

	status := server.GetStatus()
	if status.NodeID != "test-node" {
		t.Errorf("expected node ID 'test-node', got '%s'", status.NodeID)
	}
	if !status.IsHealthy {
		t.Error("server should be healthy")
	}
	if status.IsFrozen {
		t.Error("server should not be frozen initially")
	}
}
