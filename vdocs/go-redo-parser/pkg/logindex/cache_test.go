package logindex

import "testing"

func TestLRUCacheBasic(t *testing.T) {
	cache := NewLRUCache(3)
	
	pid1 := PageID{1, 100}
	tree1 := NewLSNBPlusTree()
	tree1.Insert(1000)
	
	cache.Put(pid1, tree1)
	
	tree, ok := cache.Get(pid1)
	if !ok || tree == nil {
		t.Error("expected to find tree in cache")
	}
	
	hits, misses := cache.Stats()
	if hits != 1 || misses != 0 {
		t.Errorf("expected 1 hit, 0 misses, got %d hits, %d misses", hits, misses)
	}
}

func TestLRUCacheEviction(t *testing.T) {
	cache := NewLRUCache(2)
	
	cache.Put(PageID{1, 100}, NewLSNBPlusTree())
	cache.Put(PageID{1, 101}, NewLSNBPlusTree())
	cache.Put(PageID{1, 102}, NewLSNBPlusTree())
	
	if cache.Size() != 2 {
		t.Errorf("expected size 2, got %d", cache.Size())
	}
	
	_, ok := cache.Get(PageID{1, 100})
	if ok {
		t.Error("first entry should have been evicted")
	}
}
