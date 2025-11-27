package logindex

import (
	"fmt"
	"sync"
)

// PageID uniquely identifies a page in the database
type PageID struct {
	SpaceID uint32 // Tablespace ID
	PageNo  uint32 // Page number within the tablespace
}

// String returns a string representation of PageID
func (p PageID) String() string {
	return fmt.Sprintf("PageID{SpaceID:%d, PageNo:%d}", p.SpaceID, p.PageNo)
}

// LogIndex is the main structure that maps PageID to LSN sequences
// It uses Swiss Table for O(1) lookups and B+Tree for efficient LSN range queries
type LogIndex struct {
	table       *SwissTable
	currentLSN  uint64
	mu          sync.RWMutex
	minLSNCache map[PageID]uint64 // Cache for quick min LSN lookups
}

// NewLogIndex creates a new LogIndex with the given initial capacity
func NewLogIndex() *LogIndex {
	return NewLogIndexWithCapacity(1024)
}

// NewLogIndexWithCapacity creates a new LogIndex with a specific initial capacity
func NewLogIndexWithCapacity(capacity int) *LogIndex {
	return &LogIndex{
		table:       NewSwissTable(capacity),
		currentLSN:  0,
		minLSNCache: make(map[PageID]uint64),
	}
}

// Insert adds a new LSN for the given page
// If the page doesn't exist in the index, it creates a new B+Tree for it
func (idx *LogIndex) Insert(spaceID, pageNo uint32, lsn uint64) error {
	if lsn == 0 {
		return fmt.Errorf("invalid LSN: 0")
	}
	
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	// Get or create the B+Tree for this page
	tree, exists := idx.table.Get(pageID)
	if !exists {
		tree = NewLSNBPlusTree()
		idx.table.Set(pageID, tree)
	}
	
	// Insert the LSN into the tree
	tree.Insert(lsn)
	
	// Update current LSN
	idx.mu.Lock()
	if lsn > idx.currentLSN {
		idx.currentLSN = lsn
	}
	idx.mu.Unlock()
	
	return nil
}

// Contains checks if a specific LSN exists for a given page
func (idx *LogIndex) Contains(spaceID, pageNo uint32, lsn uint64) bool {
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	tree, exists := idx.table.Get(pageID)
	if !exists {
		return false
	}
	
	return tree.Contains(lsn)
}

// Query returns all LSNs for a given page
func (idx *LogIndex) Query(spaceID, pageNo uint32) []uint64 {
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	tree, exists := idx.table.Get(pageID)
	if !exists {
		return nil
	}
	
	return tree.GetAll()
}

// QueryRange returns all LSNs for a given page within the specified range [startLSN, endLSN]
func (idx *LogIndex) QueryRange(spaceID, pageNo uint32, startLSN, endLSN uint64) []uint64 {
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	tree, exists := idx.table.Get(pageID)
	if !exists {
		return nil
	}
	
	return tree.RangeQuery(startLSN, endLSN)
}

// GetPageLSNRange returns the min and max LSN for a given page
func (idx *LogIndex) GetPageLSNRange(spaceID, pageNo uint32) (minLSN, maxLSN uint64, exists bool) {
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	tree, exists := idx.table.Get(pageID)
	if !exists {
		return 0, 0, false
	}
	
	return tree.GetMinLSN(), tree.GetMaxLSN(), true
}

// PurgeBefore removes all LSNs less than the given threshold across all pages
// This is used for memory management - removing old LSNs that have been applied by all read-only nodes
// Returns the total number of LSNs purged
func (idx *LogIndex) PurgeBefore(threshold uint64) int {
	totalPurged := 0
	pagesToDelete := make([]PageID, 0)
	
	// Iterate through all pages
	idx.table.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		// If all LSNs for this page are below threshold, mark page for deletion
		if tree.GetMaxLSN() < threshold {
			pagesToDelete = append(pagesToDelete, pageID)
			totalPurged += tree.Count()
		} else {
			// Otherwise, purge only LSNs below threshold
			purged := tree.PurgeBefore(threshold)
			totalPurged += purged
			
			// If tree becomes empty, mark for deletion
			if tree.Count() == 0 {
				pagesToDelete = append(pagesToDelete, pageID)
			}
		}
		return true // Continue iteration
	})
	
	// Delete empty pages
	for _, pageID := range pagesToDelete {
		idx.table.Delete(pageID)
		
		// Clean up cache
		idx.mu.Lock()
		delete(idx.minLSNCache, pageID)
		idx.mu.Unlock()
	}
	
	return totalPurged
}

// GetCurrentLSN returns the maximum LSN seen so far
func (idx *LogIndex) GetCurrentLSN() uint64 {
	idx.mu.RLock()
	defer idx.mu.RUnlock()
	return idx.currentLSN
}

// GetPageCount returns the number of pages in the index
func (idx *LogIndex) GetPageCount() int {
	return idx.table.Size()
}

// GetTotalLSNCount returns the total number of LSNs across all pages
func (idx *LogIndex) GetTotalLSNCount() int {
	total := 0
	idx.table.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		total += tree.Count()
		return true
	})
	return total
}

// GetStats returns statistics about the LogIndex
func (idx *LogIndex) GetStats() Stats {
	stats := Stats{
		PageCount:  idx.GetPageCount(),
		TotalLSNs:  idx.GetTotalLSNCount(),
		CurrentLSN: idx.GetCurrentLSN(),
		MinLSN:     ^uint64(0),
		MaxLSN:     0,
	}
	
	idx.table.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		minLSN := tree.GetMinLSN()
		maxLSN := tree.GetMaxLSN()
		
		if minLSN < stats.MinLSN {
			stats.MinLSN = minLSN
		}
		if maxLSN > stats.MaxLSN {
			stats.MaxLSN = maxLSN
		}
		
		return true
	})
	
	if stats.PageCount == 0 {
		stats.MinLSN = 0
	}
	
	return stats
}

// Stats contains statistics about the LogIndex
type Stats struct {
	PageCount  int    // Number of pages indexed
	TotalLSNs  int    // Total number of LSNs
	CurrentLSN uint64 // Maximum LSN seen
	MinLSN     uint64 // Minimum LSN across all pages
	MaxLSN     uint64 // Maximum LSN across all pages
}

// String returns a string representation of the stats
func (s Stats) String() string {
	return fmt.Sprintf("LogIndex Stats: Pages=%d, LSNs=%d, CurrentLSN=%d, Range=[%d, %d]",
		s.PageCount, s.TotalLSNs, s.CurrentLSN, s.MinLSN, s.MaxLSN)
}

// Clear removes all entries from the LogIndex
func (idx *LogIndex) Clear() {
	idx.table = NewSwissTable(1024)
	idx.mu.Lock()
	idx.currentLSN = 0
	idx.minLSNCache = make(map[PageID]uint64)
	idx.mu.Unlock()
}

// BatchInsert inserts multiple LSNs for a given page efficiently
func (idx *LogIndex) BatchInsert(spaceID, pageNo uint32, lsns []uint64) error {
	if len(lsns) == 0 {
		return nil
	}
	
	pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
	
	// Get or create the B+Tree for this page
	tree, exists := idx.table.Get(pageID)
	if !exists {
		tree = NewLSNBPlusTree()
		idx.table.Set(pageID, tree)
	}
	
	// Insert all LSNs
	maxLSN := uint64(0)
	for _, lsn := range lsns {
		if lsn == 0 {
			continue
		}
		tree.Insert(lsn)
		if lsn > maxLSN {
			maxLSN = lsn
		}
	}
	
	// Update current LSN
	if maxLSN > 0 {
		idx.mu.Lock()
		if maxLSN > idx.currentLSN {
			idx.currentLSN = maxLSN
		}
		idx.mu.Unlock()
	}
	
	return nil
}

// ForEachPage iterates over all pages in the index
func (idx *LogIndex) ForEachPage(fn func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool) {
	idx.table.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		return fn(pageID.SpaceID, pageID.PageNo, tree.GetMinLSN(), tree.GetMaxLSN(), tree.Count())
	})
}

// GetMemoryUsage estimates the memory usage of the LogIndex in bytes
func (idx *LogIndex) GetMemoryUsage() int64 {
	// Base structures
	var memUsage int64 = 0
	
	// Swiss Table overhead: entries array + metadata
	memUsage += int64(idx.table.capacity) * 32 // Rough estimate for entry pointers
	
	// B+Trees
	idx.table.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		// Each LSN is 8 bytes, plus node overhead
		// Rough estimate: 16 bytes per LSN (8 bytes data + 8 bytes overhead)
		memUsage += int64(tree.Count()) * 16
		return true
	})
	
	return memUsage
}

