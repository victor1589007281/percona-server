package page

import (
	"testing"
)

func TestPageEncodeDecode(t *testing.T) {
	p := NewPage(1, 100, PageTypeData)
	p.Header.PageLSN = 5000
	p.Header.RecordCount = 10
	p.Header.TrxID = 999

	// Write some data to body
	copy(p.Body[0:10], []byte("test data!"))

	// Encode
	encoded := p.Encode()
	if len(encoded) != PageSize {
		t.Errorf("expected page size %d, got %d", PageSize, len(encoded))
	}

	// Decode
	decoded := &Page{}
	if err := decoded.Decode(encoded); err != nil {
		t.Fatalf("decode failed: %v", err)
	}

	// Verify
	if decoded.Header.SpaceID != p.Header.SpaceID {
		t.Errorf("SpaceID mismatch: expected %d, got %d", p.Header.SpaceID, decoded.Header.SpaceID)
	}
	if decoded.Header.PageID != p.Header.PageID {
		t.Errorf("PageID mismatch: expected %d, got %d", p.Header.PageID, decoded.Header.PageID)
	}
	if decoded.Header.PageLSN != p.Header.PageLSN {
		t.Errorf("PageLSN mismatch: expected %d, got %d", p.Header.PageLSN, decoded.Header.PageLSN)
	}
	if decoded.Header.RecordCount != p.Header.RecordCount {
		t.Errorf("RecordCount mismatch: expected %d, got %d", p.Header.RecordCount, decoded.Header.RecordCount)
	}
	if string(decoded.Body[0:10]) != "test data!" {
		t.Errorf("body mismatch: expected 'test data!', got '%s'", string(decoded.Body[0:10]))
	}
}

func TestPageChecksumValidation(t *testing.T) {
	p := NewPage(1, 100, PageTypeData)
	encoded := p.Encode()

	// Corrupt the data
	encoded[100] ^= 0xFF

	// Decode should fail
	decoded := &Page{}
	if err := decoded.Decode(encoded); err != ErrInvalidPageChecksum {
		t.Errorf("expected checksum error, got %v", err)
	}
}

func TestPageKey(t *testing.T) {
	p := NewPage(5, 1000, PageTypeIndex)
	key := p.GetKey()

	if key.SpaceID != 5 {
		t.Errorf("SpaceID mismatch: expected 5, got %d", key.SpaceID)
	}
	if key.PageID != 1000 {
		t.Errorf("PageID mismatch: expected 1000, got %d", key.PageID)
	}

	hash := key.ToUint64()
	expectedHash := uint64(5<<32 | 1000)
	if hash != expectedHash {
		t.Errorf("hash mismatch: expected %d, got %d", expectedHash, hash)
	}
}

func TestCache(t *testing.T) {
	cache := NewCache(3)

	// Add pages
	for i := uint64(0); i < 5; i++ {
		p := NewPage(1, i, PageTypeData)
		cache.Put(p.GetKey(), p)
	}

	// Cache should only have last 3 pages
	if cache.Len() != 3 {
		t.Errorf("expected cache len 3, got %d", cache.Len())
	}

	// Pages 0 and 1 should be evicted
	_, ok := cache.Get(PageKey{SpaceID: 1, PageID: 0})
	if ok {
		t.Error("page 0 should have been evicted")
	}

	_, ok = cache.Get(PageKey{SpaceID: 1, PageID: 1})
	if ok {
		t.Error("page 1 should have been evicted")
	}

	// Pages 2, 3, 4 should be present
	for i := uint64(2); i < 5; i++ {
		_, ok := cache.Get(PageKey{SpaceID: 1, PageID: i})
		if !ok {
			t.Errorf("page %d should be in cache", i)
		}
	}
}

func TestCacheInvalidate(t *testing.T) {
	cache := NewCache(10)

	p := NewPage(1, 100, PageTypeData)
	key := p.GetKey()
	cache.Put(key, p)

	// Verify page is in cache
	_, ok := cache.Get(key)
	if !ok {
		t.Error("page should be in cache")
	}

	// Invalidate
	cache.Invalidate(key)

	// Verify page is no longer in cache
	_, ok = cache.Get(key)
	if ok {
		t.Error("page should not be in cache after invalidation")
	}
}
