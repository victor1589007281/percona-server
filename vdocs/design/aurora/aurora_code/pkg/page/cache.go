package page

import (
	"container/list"
	"sync"
)

// Cache is an LRU cache for pages
type Cache struct {
	mu       sync.RWMutex
	capacity int
	pages    map[PageKey]*list.Element
	lru      *list.List
}

type cacheEntry struct {
	key  PageKey
	page *Page
}

// NewCache creates a new page cache with the given capacity
func NewCache(capacity int) *Cache {
	return &Cache{
		capacity: capacity,
		pages:    make(map[PageKey]*list.Element),
		lru:      list.New(),
	}
}

// Get retrieves a page from the cache
func (c *Cache) Get(key PageKey) (*Page, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if elem, ok := c.pages[key]; ok {
		c.lru.MoveToFront(elem)
		return elem.Value.(*cacheEntry).page, true
	}
	return nil, false
}

// Put adds a page to the cache
func (c *Cache) Put(key PageKey, page *Page) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if elem, ok := c.pages[key]; ok {
		c.lru.MoveToFront(elem)
		elem.Value.(*cacheEntry).page = page
		return
	}

	// Evict if at capacity
	if c.lru.Len() >= c.capacity {
		c.evict()
	}

	// Add new entry
	entry := &cacheEntry{key: key, page: page}
	elem := c.lru.PushFront(entry)
	c.pages[key] = elem
}

// evict removes the least recently used page
func (c *Cache) evict() {
	elem := c.lru.Back()
	if elem != nil {
		c.lru.Remove(elem)
		entry := elem.Value.(*cacheEntry)
		delete(c.pages, entry.key)
	}
}

// Invalidate removes a page from the cache
func (c *Cache) Invalidate(key PageKey) {
	c.mu.Lock()
	defer c.mu.Unlock()

	if elem, ok := c.pages[key]; ok {
		c.lru.Remove(elem)
		delete(c.pages, key)
	}
}

// Clear removes all pages from the cache
func (c *Cache) Clear() {
	c.mu.Lock()
	defer c.mu.Unlock()

	c.pages = make(map[PageKey]*list.Element)
	c.lru = list.New()
}

// Len returns the number of pages in the cache
func (c *Cache) Len() int {
	c.mu.RLock()
	defer c.mu.RUnlock()
	return c.lru.Len()
}

// Stats returns cache statistics
func (c *Cache) Stats() CacheStats {
	c.mu.RLock()
	defer c.mu.RUnlock()

	return CacheStats{
		Size:     c.lru.Len(),
		Capacity: c.capacity,
	}
}

// CacheStats contains cache statistics
type CacheStats struct {
	Size     int
	Capacity int
	Hits     int64
	Misses   int64
}
