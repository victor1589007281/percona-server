package logindex

import (
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// PersistentWriter handles writing LogIndex to disk
type PersistentWriter struct {
	dir           string
	metaFile      *MetaFile
	dataFiles     map[uint32]*DataFile
	currentFileID uint32
	mu            sync.Mutex
	flushChan     chan *flushRequest
	stopChan      chan struct{}
	wg            sync.WaitGroup
}

type flushRequest struct {
	done chan error
}

// NewPersistentWriter creates a new writer
func NewPersistentWriter(dir string) (*PersistentWriter, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	
	metaPath := filepath.Join(dir, MetaFileName)
	writer := &PersistentWriter{
		dir:           dir,
		metaFile:      NewMetaFile(metaPath),
		dataFiles:     make(map[uint32]*DataFile),
		currentFileID: 0,
		flushChan:     make(chan *flushRequest, 100),
		stopChan:      make(chan struct{}),
	}
	
	// Start async flush worker
	writer.wg.Add(1)
	go writer.flushWorker()
	
	return writer, nil
}

// WritePageLSNs writes LSNs for a page
func (w *PersistentWriter) WritePageLSNs(spaceID, pageNo uint32, lsns []uint64) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	
	// Encode LSNs with delta encoding
	base, deltas := EncodeLSNs(lsns)
	
	block := &PageLSNBlock{
		SpaceID:    spaceID,
		PageNo:     pageNo,
		LSNCount:   uint32(len(lsns)),
		Compressed: 0,
		LSNBase:    base,
		LSNDeltas:  deltas,
	}
	
	// Get or create data file
	dataFile, err := w.getOrCreateDataFile()
	if err != nil {
		return err
	}
	
	// Write block
	offset, err := dataFile.WriteBlock(block)
	if err != nil {
		return err
	}
	
	// Update meta file
	entry := PageEntry{
		SpaceID:    spaceID,
		PageNo:     pageNo,
		FileOffset: offset,
		LSNCount:   uint32(len(lsns)),
		MinLSN:     lsns[0],
		MaxLSN:     lsns[len(lsns)-1],
	}
	w.metaFile.UpdateEntry(entry)
	
	return nil
}

func (w *PersistentWriter) getOrCreateDataFile() (*DataFile, error) {
	if df, exists := w.dataFiles[w.currentFileID]; exists {
		// Check if current file is too large
		if df.offset < DataFileMaxSize {
			return df, nil
		}
		// Current file is full, create new one
		w.currentFileID++
	}
	
	df, err := NewDataFile(w.dir, w.currentFileID)
	if err != nil {
		return nil, err
	}
	
	w.dataFiles[w.currentFileID] = df
	return df, nil
}

// Flush syncs all data to disk asynchronously
func (w *PersistentWriter) Flush() error {
	req := &flushRequest{
		done: make(chan error, 1),
	}
	
	select {
	case w.flushChan <- req:
		return <-req.done
	case <-w.stopChan:
		return fmt.Errorf("writer stopped")
	}
}

func (w *PersistentWriter) flushWorker() {
	defer w.wg.Done()
	
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	
	for {
		select {
		case req := <-w.flushChan:
			req.done <- w.doFlush()
		case <-ticker.C:
			w.doFlush()
		case <-w.stopChan:
			w.doFlush()
			return
		}
	}
}

func (w *PersistentWriter) doFlush() error {
	w.mu.Lock()
	defer w.mu.Unlock()
	
	// Sync all data files
	for _, df := range w.dataFiles {
		if err := df.Sync(); err != nil {
			return err
		}
	}
	
	// Save meta file
	w.metaFile.Header.DataFileCount = w.currentFileID + 1
	if err := w.metaFile.Save(); err != nil {
		return err
	}
	
	return nil
}

// Close closes the writer
func (w *PersistentWriter) Close() error {
	close(w.stopChan)
	w.wg.Wait()
	
	w.mu.Lock()
	defer w.mu.Unlock()
	
	for _, df := range w.dataFiles {
		df.Close()
	}
	
	return nil
}
