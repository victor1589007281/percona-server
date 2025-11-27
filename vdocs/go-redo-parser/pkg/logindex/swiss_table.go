package logindex

import (
	"hash/fnv"
	"sync"
)

// SwissTable is a high-performance hash table inspired by Google's Swiss Table
// Uses open addressing with quadratic probing for collision resolution
type SwissTable struct {
	mu       sync.RWMutex
	entries  []*swissEntry
	capacity int
	size     int
	maxLoad  float64 // Load factor threshold for resizing
}

type swissEntry struct {
	key   PageID
	value *LSNBPlusTree
	hash  uint64
	valid bool
}

// NewSwissTable creates a new Swiss Table with the given initial capacity
func NewSwissTable(initialCapacity int) *SwissTable {
	if initialCapacity < 16 {
		initialCapacity = 16
	}
	// Round up to next power of 2 for efficient modulo operations
	capacity := nextPowerOf2(initialCapacity)
	
	return &SwissTable{
		entries:  make([]*swissEntry, capacity),
		capacity: capacity,
		size:     0,
		maxLoad:  0.875, // 87.5% load factor (Google's Swiss Table uses this)
	}
}

// Get retrieves a value by key
func (st *SwissTable) Get(key PageID) (*LSNBPlusTree, bool) {
	st.mu.RLock()
	defer st.mu.RUnlock()
	
	hash := st.hashKey(key)
	idx := st.findEntry(key, hash)
	
	if idx == -1 {
		return nil, false
	}
	
	return st.entries[idx].value, true
}

// Set inserts or updates a key-value pair
func (st *SwissTable) Set(key PageID, value *LSNBPlusTree) {
	st.mu.Lock()
	defer st.mu.Unlock()
	
	// Check if we need to resize
	if float64(st.size+1)/float64(st.capacity) > st.maxLoad {
		st.resize(st.capacity * 2)
	}
	
	hash := st.hashKey(key)
	idx := st.findEntry(key, hash)
	
	if idx != -1 {
		// Update existing entry
		st.entries[idx].value = value
		return
	}
	
	// Insert new entry
	idx = st.findEmptySlot(key, hash)
	st.entries[idx] = &swissEntry{
		key:   key,
		value: value,
		hash:  hash,
		valid: true,
	}
	st.size++
}

// Delete removes a key from the table
func (st *SwissTable) Delete(key PageID) bool {
	st.mu.Lock()
	defer st.mu.Unlock()
	
	hash := st.hashKey(key)
	idx := st.findEntry(key, hash)
	
	if idx == -1 {
		return false
	}
	
	st.entries[idx].valid = false
	st.size--
	return true
}

// Size returns the number of entries in the table
func (st *SwissTable) Size() int {
	st.mu.RLock()
	defer st.mu.RUnlock()
	return st.size
}

// ForEach iterates over all valid entries
func (st *SwissTable) ForEach(fn func(key PageID, value *LSNBPlusTree) bool) {
	st.mu.RLock()
	defer st.mu.RUnlock()
	
	for _, entry := range st.entries {
		if entry != nil && entry.valid {
			if !fn(entry.key, entry.value) {
				break
			}
		}
	}
}

// findEntry finds the index of an entry with the given key
// Returns -1 if not found
func (st *SwissTable) findEntry(key PageID, hash uint64) int {
	mask := st.capacity - 1
	idx := int(hash & uint64(mask))
	
	// Quadratic probing
	for i := 0; i < st.capacity; i++ {
		entry := st.entries[idx]
		
		if entry == nil {
			return -1 // Empty slot, not found
		}
		
		if entry.valid && entry.hash == hash && entry.key == key {
			return idx // Found
		}
		
		// Quadratic probing: h(i) = (h + i^2) mod capacity
		idx = (idx + 2*i + 1) & mask
	}
	
	return -1 // Table is full (shouldn't happen due to load factor)
}

// findEmptySlot finds an empty or deleted slot for insertion
func (st *SwissTable) findEmptySlot(key PageID, hash uint64) int {
	mask := st.capacity - 1
	idx := int(hash & uint64(mask))
	
	// Quadratic probing
	for i := 0; i < st.capacity; i++ {
		entry := st.entries[idx]
		
		if entry == nil || !entry.valid {
			return idx // Found empty or deleted slot
		}
		
		// Quadratic probing
		idx = (idx + 2*i + 1) & mask
	}
	
	// Should never reach here due to load factor control
	panic("swiss table: no empty slot found")
}

// resize grows the table to a new capacity
func (st *SwissTable) resize(newCapacity int) {
	oldEntries := st.entries
	st.entries = make([]*swissEntry, newCapacity)
	st.capacity = newCapacity
	st.size = 0
	
	// Rehash all valid entries
	for _, entry := range oldEntries {
		if entry != nil && entry.valid {
			// Recompute position in new table
			idx := st.findEmptySlot(entry.key, entry.hash)
			st.entries[idx] = entry
			st.size++
		}
	}
}

// hashKey computes the hash of a PageID
func (st *SwissTable) hashKey(key PageID) uint64 {
	h := fnv.New64a()
	// Combine SpaceID and PageNo
	b := make([]byte, 8)
	b[0] = byte(key.SpaceID >> 24)
	b[1] = byte(key.SpaceID >> 16)
	b[2] = byte(key.SpaceID >> 8)
	b[3] = byte(key.SpaceID)
	b[4] = byte(key.PageNo >> 24)
	b[5] = byte(key.PageNo >> 16)
	b[6] = byte(key.PageNo >> 8)
	b[7] = byte(key.PageNo)
	h.Write(b)
	return h.Sum64()
}

// nextPowerOf2 returns the next power of 2 greater than or equal to n
func nextPowerOf2(n int) int {
	if n <= 1 {
		return 1
	}
	n--
	n |= n >> 1
	n |= n >> 2
	n |= n >> 4
	n |= n >> 8
	n |= n >> 16
	n |= n >> 32
	n++
	return n
}

