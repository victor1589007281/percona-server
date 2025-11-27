package logindex

import (
	"fmt"
	"os"
	"path/filepath"
)

// PersistentReader handles reading LogIndex from disk
type PersistentReader struct {
	dir       string
	metaFile  *MetaFile
	dataFiles map[uint32]*DataFile
}

// NewPersistentReader creates a new reader
func NewPersistentReader(dir string) (*PersistentReader, error) {
	metaPath := filepath.Join(dir, MetaFileName)
	if _, err := os.Stat(metaPath); os.IsNotExist(err) {
		return nil, fmt.Errorf("meta file not found")
	}
	
	reader := &PersistentReader{
		dir:       dir,
		metaFile:  NewMetaFile(metaPath),
		dataFiles: make(map[uint32]*DataFile),
	}
	
	// Load meta file
	if err := reader.metaFile.Load(); err != nil {
		return nil, err
	}
	
	return reader, nil
}

// ReadPageLSNs reads LSNs for a page
func (r *PersistentReader) ReadPageLSNs(spaceID, pageNo uint32) ([]uint64, error) {
	// Find entry in meta file
	entry, _ := r.metaFile.FindEntry(spaceID, pageNo)
	if entry == nil {
		return nil, fmt.Errorf("page not found")
	}
	
	// Calculate which data file
	fileID := uint32(entry.FileOffset / DataFileMaxSize)
	offsetInFile := entry.FileOffset % DataFileMaxSize
	
	// Get or open data file
	dataFile, err := r.getDataFile(fileID)
	if err != nil {
		return nil, err
	}
	
	// Read block
	block, err := dataFile.ReadBlock(offsetInFile)
	if err != nil {
		return nil, err
	}
	
	// Decode LSNs
	lsns := DecodeLSNs(block.LSNBase, block.LSNDeltas)
	return lsns, nil
}

func (r *PersistentReader) getDataFile(fileID uint32) (*DataFile, error) {
	if df, exists := r.dataFiles[fileID]; exists {
		return df, nil
	}
	
	df, err := NewDataFile(r.dir, fileID)
	if err != nil {
		return nil, err
	}
	
	r.dataFiles[fileID] = df
	return df, nil
}

// Close closes the reader
func (r *PersistentReader) Close() error {
	for _, df := range r.dataFiles {
		df.Close()
	}
	return nil
}
