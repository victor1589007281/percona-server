package logindex

import (
	"container/list"
	"sync"
	"time"
)

// Cache levels: Hot (memory) -> Warm (compressed memory) -> Cold (disk)

// CacheEntry represents a cached page
type CacheEntry struct {
	PageID    PageID
	Tree      *LSNBPlusTree
	LastAccess time.Time
	Hot       bool
}

// LRUCache implements LRU caching strategy
type LRUCache struct {
	capacity int
	items    map[PageID]*list.Element
	lruList  *list.List
	mu       sync.RWMutex
	hits     uint64
	misses   uint64
}

// NewLRUCache creates a new LRU cache
func NewLRUCache(capacity int) *LRUCache {
	return &LRUCache{
		capacity: capacity,
		items:    make(map[PageID]*list.Element),
		lruList:  list.New(),
	}
}

// Get retrieves an item from cache
func (c *LRUCache) Get(pageID PageID) (*LSNBPlusTree, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	
	if elem, exists := c.items[pageID]; exists {
		c.lruList.MoveToFront(elem)
		entry := elem.Value.(*CacheEntry)
		entry.LastAccess = time.Now()
		c.hits++
		return entry.Tree, true
	}
	
	c.misses++
	return nil, false
}

// Put adds an item to cache
func (c *LRUCache) Put(pageID PageID, tree *LSNBPlusTree) {
	c.mu.Lock()
	defer c.mu.Unlock()
	
	if elem, exists := c.items[pageID]; exists {
		c.lruList.MoveToFront(elem)
		entry := elem.Value.(*CacheEntry)
		entry.Tree = tree
		entry.LastAccess = time.Now()
		return
	}
	
	entry := &CacheEntry{
		PageID:     pageID,
		Tree:       tree,
		LastAccess: time.Now(),
		Hot:        true,
	}
	
	elem := c.lruList.PushFront(entry)
	c.items[pageID] = elem
	
	// Evict if over capacity
	if c.lruList.Len() > c.capacity {
		c.evict()
	}
}

func (c *LRUCache) evict() {
	elem := c.lruList.Back()
	if elem != nil {
		c.lruList.Remove(elem)
		entry := elem.Value.(*CacheEntry)
		delete(c.items, entry.PageID)
	}
}

// Remove removes an item from cache
func (c *LRUCache) Remove(pageID PageID) {
	c.mu.Lock()
	defer c.mu.Unlock()
	
	if elem, exists := c.items[pageID]; exists {
		c.lruList.Remove(elem)
		delete(c.items, pageID)
	}
}

// Size returns current cache size
func (c *LRUCache) Size() int {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return len(c.items)
}

// Stats returns cache statistics
func (c *LRUCache) Stats() (hits, misses uint64) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.hits, c.misses
}

// Clear clears the cache
func (c *LRUCache) Clear() {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.items = make(map[PageID]*list.Element)
	c.lruList = list.New()
}

// TieredCache implements multi-level caching
type TieredCache struct {
	hotCache  *LRUCache
	warmCache *LRUCache
	reader    *PersistentReader
	writer    *PersistentWriter
	mu        sync.RWMutex
}

// NewTieredCache creates a new tiered cache
func NewTieredCache(hotSize, warmSize int, dir string) (*TieredCache, error) {
	writer, err := NewPersistentWriter(dir)
	if err != nil {
		return nil, err
	}
	
	reader, err := NewPersistentReader(dir)
	if err != nil {
		// If no existing data, that's okay
		reader = nil
	}
	
	return &TieredCache{
		hotCache:  NewLRUCache(hotSize),
		warmCache: NewLRUCache(warmSize),
		reader:    reader,
		writer:    writer,
	}, nil
}

// Get retrieves from tiered cache
func (tc *TieredCache) Get(pageID PageID) (*LSNBPlusTree, error) {
	// Try hot cache
	if tree, ok := tc.hotCache.Get(pageID); ok {
		return tree, nil
	}
	
	// Try warm cache
	if tree, ok := tc.warmCache.Get(pageID); ok {
		// Promote to hot
		tc.hotCache.Put(pageID, tree)
		return tree, nil
	}
	
	// Load from disk (cold)
	if tc.reader != nil {
		lsns, err := tc.reader.ReadPageLSNs(pageID.SpaceID, pageID.PageNo)
		if err == nil {
			tree := NewLSNBPlusTree()
			for _, lsn := range lsns {
				tree.Insert(lsn)
			}
			tc.hotCache.Put(pageID, tree)
			return tree, nil
		}
	}
	
	return nil, fmt.Errorf("page not found")
}

// Put adds to tiered cache
func (tc *TieredCache) Put(pageID PageID, tree *LSNBPlusTree) {
	tc.hotCache.Put(pageID, tree)
}

// Flush writes hot data to disk
func (tc *TieredCache) Flush() error {
	if tc.writer == nil {
		return nil
	}
	return tc.writer.Flush()
}

// Close closes the cache
func (tc *TieredCache) Close() error {
	if tc.writer != nil {
		tc.writer.Close()
	}
	if tc.reader != nil {
		tc.reader.Close()
	}
	return nil
}
