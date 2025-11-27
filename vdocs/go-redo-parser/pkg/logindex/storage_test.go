package logindex

import (
	"os"
	"testing"
)

func TestMetaFileHeaderMarshal(t *testing.T) {
	header := NewMetaFileHeader()
	header.CheckpointLSN = 1000
	header.NumPages = 100
	header.TotalLSNs = 5000
	
	buf := make([]byte, MetaHeaderSize)
	if err := header.MarshalBinary(buf); err != nil {
		t.Fatal(err)
	}
	
	header2 := &MetaFileHeader{}
	if err := header2.UnmarshalBinary(buf); err != nil {
		t.Fatal(err)
	}
	
	if header2.CheckpointLSN != 1000 {
		t.Errorf("expected 1000, got %d", header2.CheckpointLSN)
	}
}

func TestMetaFileSaveLoad(t *testing.T) {
	tmpDir := t.TempDir()
	metaPath := tmpDir + "/test.meta"
	
	// Create and save
	meta := NewMetaFile(metaPath)
	meta.AddEntry(PageEntry{SpaceID: 1, PageNo: 100, FileOffset: 0, LSNCount: 10, MinLSN: 1000, MaxLSN: 2000})
	meta.AddEntry(PageEntry{SpaceID: 1, PageNo: 101, FileOffset: 1000, LSNCount: 15, MinLSN: 1050, MaxLSN: 2100})
	
	if err := meta.Save(); err != nil {
		t.Fatal(err)
	}
	
	// Load and verify
	meta2 := NewMetaFile(metaPath)
	if err := meta2.Load(); err != nil {
		t.Fatal(err)
	}
	
	if len(meta2.Entries) != 2 {
		t.Errorf("expected 2 entries, got %d", len(meta2.Entries))
	}
}

func TestDeltaEncoding(t *testing.T) {
	lsns := []uint64{1000, 1050, 1100, 1200, 1250}
	
	base, deltas := EncodeLSNs(lsns)
	decoded := DecodeLSNs(base, deltas)
	
	if len(decoded) != len(lsns) {
		t.Fatalf("length mismatch")
	}
	
	for i, lsn := range decoded {
		if lsn != lsns[i] {
			t.Errorf("index %d: expected %d, got %d", i, lsns[i], lsn)
		}
	}
}

func TestDataFileWrite Read(t *testing.T) {
	tmpDir := t.TempDir()
	df, err := NewDataFile(tmpDir, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer df.Close()
	
	// Write block
	lsns := []uint64{1000, 1100, 1200}
	base, deltas := EncodeLSNs(lsns)
	
	block := &PageLSNBlock{
		SpaceID:   1,
		PageNo:    100,
		LSNCount:  uint32(len(lsns)),
		LSNBase:   base,
		LSNDeltas: deltas,
	}
	
	offset, err := df.WriteBlock(block)
	if err != nil {
		t.Fatal(err)
	}
	
	// Read block
	block2, err := df.ReadBlock(offset)
	if err != nil {
		t.Fatal(err)
	}
	
	if block2.SpaceID != 1 || block2.PageNo != 100 {
		t.Error("block mismatch")
	}
	
	decoded := DecodeLSNs(block2.LSNBase, block2.LSNDeltas)
	if len(decoded) != 3 {
		t.Errorf("expected 3 LSNs, got %d", len(decoded))
	}
}

func TestWriterReader(t *testing.T) {
	tmpDir := t.TempDir()
	
	// Write
	writer, err := NewPersistentWriter(tmpDir)
	if err != nil {
		t.Fatal(err)
	}
	
	lsns := []uint64{1000, 1100, 1200, 1300}
	if err := writer.WritePageLSNs(1, 100, lsns); err != nil {
		t.Fatal(err)
	}
	
	if err := writer.Flush(); err != nil {
		t.Fatal(err)
	}
	writer.Close()
	
	// Read
	reader, err := NewPersistentReader(tmpDir)
	if err != nil {
		t.Fatal(err)
	}
	defer reader.Close()
	
	readLSNs, err := reader.ReadPageLSNs(1, 100)
	if err != nil {
		t.Fatal(err)
	}
	
	if len(readLSNs) != 4 {
		t.Errorf("expected 4 LSNs, got %d", len(readLSNs))
	}
}

func TestCacheLRU(t *testing.T) {
	cache := NewLRUCache(2)
	
	tree1 := NewLSNBPlusTree()
	tree1.Insert(1000)
	
	tree2 := NewLSNBPlusTree()
	tree2.Insert(2000)
	
	tree3 := NewLSNBPlusTree()
	tree3.Insert(3000)
	
	pid1 := PageID{SpaceID: 1, PageNo: 100}
	pid2 := PageID{SpaceID: 1, PageNo: 101}
	pid3 := PageID{SpaceID: 1, PageNo: 102}
	
	cache.Put(pid1, tree1)
	cache.Put(pid2, tree2)
	
	if cache.Size() != 2 {
		t.Errorf("expected size 2, got %d", cache.Size())
	}
	
	// This should evict pid1
	cache.Put(pid3, tree3)
	
	if _, ok := cache.Get(pid1); ok {
		t.Error("pid1 should have been evicted")
	}
	
	if _, ok := cache.Get(pid2); !ok {
		t.Error("pid2 should still be in cache")
	}
}
