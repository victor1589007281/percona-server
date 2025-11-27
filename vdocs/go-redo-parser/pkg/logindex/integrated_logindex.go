package logindex

import "fmt"

// IntegratedLogIndex combines memory and persistent storage
type IntegratedLogIndex struct {
	memoryIndex *LogIndex
	cache       *TieredCache
	writer      *PersistentWriter
	config      *Config
}

// NewIntegratedLogIndex creates a new integrated LogIndex
func NewIntegratedLogIndex(config *Config) (*IntegratedLogIndex, error) {
	if !config.Enabled {
		return nil, fmt.Errorf("LogIndex disabled")
	}
	
	writer, err := NewPersistentWriter(config.StorageDir)
	if err != nil {
		return nil, err
	}
	
	cache, err := NewTieredCache(config.HotCacheSize, config.WarmCacheSize, config.StorageDir)
	if err != nil {
		return nil, err
	}
	
	return &IntegratedLogIndex{
		memoryIndex: NewLogIndex(),
		cache:       cache,
		writer:      writer,
		config:      config,
	}, nil
}

// Insert adds LSN with caching and persistence
func (il *IntegratedLogIndex) Insert(spaceID, pageNo uint32, lsn uint64) error {
	// Insert to memory
	if err := il.memoryIndex.Insert(spaceID, pageNo, lsn); err != nil {
		return err
	}
	
	// Update cache
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	if tree, exists := il.cache.hotCache.Get(pageID); exists {
		tree.Insert(lsn)
	}
	
	return nil
}

// Query with caching
func (il *IntegratedLogIndex) Query(spaceID, pageNo uint32) []uint64 {
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	// Try cache first
	if tree, err := il.cache.Get(pageID); err == nil {
		return tree.GetAll()
	}
	
	// Fallback to memory index
	return il.memoryIndex.Query(spaceID, pageNo)
}

// Flush persists to disk
func (il *IntegratedLogIndex) Flush() error {
	// Write all pages to disk
	il.memoryIndex.ForEachPage(func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool {
		lsns := il.memoryIndex.Query(spaceID, pageNo)
		il.writer.WritePageLSNs(spaceID, pageNo, lsns)
		return true
	})
	
	return il.writer.Flush()
}

// Close closes all resources
func (il *IntegratedLogIndex) Close() error {
	il.Flush()
	il.cache.Close()
	il.writer.Close()
	return nil
}
