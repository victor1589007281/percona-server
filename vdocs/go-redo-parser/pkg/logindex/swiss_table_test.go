package logindex

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestSwissTable_Basic(t *testing.T) {
	st := NewSwissTable(16)
	assert.NotNil(t, st)
	assert.Equal(t, 0, st.Size())

	// Test insertion
	pageID := PageID{SpaceID: 1, PageNo: 100}
	tree := NewLSNBPlusTree()
	st.Set(pageID, tree)

	assert.Equal(t, 1, st.Size())

	// Test retrieval
	retrieved, exists := st.Get(pageID)
	assert.True(t, exists)
	assert.Equal(t, tree, retrieved)

	// Test non-existent key
	nonExistent := PageID{SpaceID: 2, PageNo: 200}
	_, exists = st.Get(nonExistent)
	assert.False(t, exists)
}

func TestSwissTable_Update(t *testing.T) {
	st := NewSwissTable(16)
	pageID := PageID{SpaceID: 1, PageNo: 100}

	// Insert initial value
	tree1 := NewLSNBPlusTree()
	tree1.Insert(1000)
	st.Set(pageID, tree1)

	// Update with new value
	tree2 := NewLSNBPlusTree()
	tree2.Insert(2000)
	st.Set(pageID, tree2)

	// Should still have only 1 entry
	assert.Equal(t, 1, st.Size())

	// Should get the updated value
	retrieved, exists := st.Get(pageID)
	assert.True(t, exists)
	assert.Equal(t, tree2, retrieved)
	assert.True(t, retrieved.Contains(2000))
}

func TestSwissTable_Delete(t *testing.T) {
	st := NewSwissTable(16)
	pageID := PageID{SpaceID: 1, PageNo: 100}

	// Insert and then delete
	tree := NewLSNBPlusTree()
	st.Set(pageID, tree)
	assert.Equal(t, 1, st.Size())

	deleted := st.Delete(pageID)
	assert.True(t, deleted)
	assert.Equal(t, 0, st.Size())

	// Delete non-existent key
	deleted = st.Delete(pageID)
	assert.False(t, deleted)
}

func TestSwissTable_MultipleEntries(t *testing.T) {
	st := NewSwissTable(16)

	// Insert multiple entries
	for i := uint32(0); i < 100; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		tree := NewLSNBPlusTree()
		tree.Insert(uint64(i * 100))
		st.Set(pageID, tree)
	}

	assert.Equal(t, 100, st.Size())

	// Verify all entries
	for i := uint32(0); i < 100; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		tree, exists := st.Get(pageID)
		require.True(t, exists, "Page %d should exist", i)
		assert.True(t, tree.Contains(uint64(i*100)))
	}
}

func TestSwissTable_Resize(t *testing.T) {
	// Start with small capacity to trigger resize
	st := NewSwissTable(4)

	// Insert many entries to trigger resize
	for i := uint32(0); i < 50; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		tree := NewLSNBPlusTree()
		st.Set(pageID, tree)
	}

	assert.Equal(t, 50, st.Size())
	assert.True(t, st.capacity > 4, "Table should have resized")

	// Verify all entries still exist after resize
	for i := uint32(0); i < 50; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		_, exists := st.Get(pageID)
		assert.True(t, exists, "Page %d should exist after resize", i)
	}
}

func TestSwissTable_ForEach(t *testing.T) {
	st := NewSwissTable(16)

	// Insert test data
	expectedPages := make(map[PageID]bool)
	for i := uint32(0); i < 10; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		expectedPages[pageID] = true
		tree := NewLSNBPlusTree()
		st.Set(pageID, tree)
	}

	// Iterate and verify
	count := 0
	st.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		assert.True(t, expectedPages[pageID], "Unexpected page: %v", pageID)
		count++
		return true
	})

	assert.Equal(t, 10, count)
}

func TestSwissTable_ForEachEarlyExit(t *testing.T) {
	st := NewSwissTable(16)

	// Insert test data
	for i := uint32(0); i < 10; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		tree := NewLSNBPlusTree()
		st.Set(pageID, tree)
	}

	// Exit early
	count := 0
	st.ForEach(func(pageID PageID, tree *LSNBPlusTree) bool {
		count++
		return count < 5 // Stop after 5 iterations
	})

	assert.Equal(t, 5, count)
}

func TestSwissTable_ConcurrentAccess(t *testing.T) {
	st := NewSwissTable(16)

	// Test concurrent reads and writes
	const numGoroutines = 10
	const opsPerGoroutine = 100

	done := make(chan bool, numGoroutines)

	// Writers
	for g := 0; g < numGoroutines/2; g++ {
		go func(id int) {
			for i := 0; i < opsPerGoroutine; i++ {
				pageID := PageID{SpaceID: uint32(id), PageNo: uint32(i)}
				tree := NewLSNBPlusTree()
				st.Set(pageID, tree)
			}
			done <- true
		}(g)
	}

	// Readers
	for g := numGoroutines / 2; g < numGoroutines; g++ {
		go func(id int) {
			for i := 0; i < opsPerGoroutine; i++ {
				pageID := PageID{SpaceID: uint32(id % (numGoroutines / 2)), PageNo: uint32(i)}
				st.Get(pageID)
			}
			done <- true
		}(g)
	}

	// Wait for all goroutines
	for i := 0; i < numGoroutines; i++ {
		<-done
	}

	// Verify data
	assert.True(t, st.Size() > 0)
}

func BenchmarkSwissTable_Insert(b *testing.B) {
	st := NewSwissTable(1024)
	tree := NewLSNBPlusTree()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		pageID := PageID{SpaceID: 1, PageNo: uint32(i)}
		st.Set(pageID, tree)
	}
}

func BenchmarkSwissTable_Get(b *testing.B) {
	st := NewSwissTable(1024)

	// Prepare data
	for i := uint32(0); i < 1000; i++ {
		pageID := PageID{SpaceID: 1, PageNo: i}
		tree := NewLSNBPlusTree()
		st.Set(pageID, tree)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		pageID := PageID{SpaceID: 1, PageNo: uint32(i % 1000)}
		st.Get(pageID)
	}
}

