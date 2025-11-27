package logindex

import (
	"math/rand"
	"sort"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestBPlusTree_InsertAndContains(t *testing.T) {
	tree := NewLSNBPlusTree()
	assert.NotNil(t, tree)
	assert.Equal(t, 0, tree.Count())

	// Insert LSNs
	lsns := []uint64{1000, 1050, 1100, 1200, 1300}
	for _, lsn := range lsns {
		tree.Insert(lsn)
	}

	assert.Equal(t, len(lsns), tree.Count())
	assert.Equal(t, uint64(1000), tree.GetMinLSN())
	assert.Equal(t, uint64(1300), tree.GetMaxLSN())

	// Check contains
	for _, lsn := range lsns {
		assert.True(t, tree.Contains(lsn), "LSN %d should exist", lsn)
	}

	// Check non-existent LSN
	assert.False(t, tree.Contains(999))
	assert.False(t, tree.Contains(1150))
}

func TestBPlusTree_DuplicateInsert(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert same LSN multiple times
	tree.Insert(1000)
	tree.Insert(1000)
	tree.Insert(1000)

	// Should only count once
	assert.Equal(t, 1, tree.Count())
	assert.True(t, tree.Contains(1000))
}

func TestBPlusTree_GetAll(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert unsorted LSNs
	lsns := []uint64{1300, 1000, 1200, 1050, 1100}
	for _, lsn := range lsns {
		tree.Insert(lsn)
	}

	// GetAll should return sorted LSNs
	result := tree.GetAll()
	assert.Equal(t, len(lsns), len(result))

	// Check if sorted
	for i := 1; i < len(result); i++ {
		assert.True(t, result[i-1] < result[i], "LSNs should be sorted")
	}

	// Verify all LSNs are present
	expected := make([]uint64, len(lsns))
	copy(expected, lsns)
	sort.Slice(expected, func(i, j int) bool { return expected[i] < expected[j] })
	assert.Equal(t, expected, result)
}

func TestBPlusTree_RangeQuery(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert LSNs
	for i := uint64(1000); i <= 2000; i += 100 {
		tree.Insert(i)
	}

	// Test various ranges
	testCases := []struct {
		name     string
		start    uint64
		end      uint64
		expected []uint64
	}{
		{
			name:     "Full range",
			start:    1000,
			end:      2000,
			expected: []uint64{1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000},
		},
		{
			name:     "Partial range",
			start:    1200,
			end:      1600,
			expected: []uint64{1200, 1300, 1400, 1500, 1600},
		},
		{
			name:     "Single value",
			start:    1500,
			end:      1500,
			expected: []uint64{1500},
		},
		{
			name:     "Before all",
			start:    100,
			end:      500,
			expected: nil,
		},
		{
			name:     "After all",
			start:    3000,
			end:      4000,
			expected: nil,
		},
		{
			name:     "Empty range",
			start:    1550,
			end:      1650,
			expected: nil,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			result := tree.RangeQuery(tc.start, tc.end)
			assert.Equal(t, tc.expected, result)
		})
	}
}

func TestBPlusTree_PurgeBefore(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert LSNs
	lsns := []uint64{1000, 1100, 1200, 1300, 1400, 1500}
	for _, lsn := range lsns {
		tree.Insert(lsn)
	}

	// Purge LSNs before 1300
	purged := tree.PurgeBefore(1300)
	assert.Equal(t, 3, purged) // 1000, 1100, 1200 should be purged
	assert.Equal(t, 3, tree.Count())

	// Check remaining LSNs
	assert.False(t, tree.Contains(1000))
	assert.False(t, tree.Contains(1100))
	assert.False(t, tree.Contains(1200))
	assert.True(t, tree.Contains(1300))
	assert.True(t, tree.Contains(1400))
	assert.True(t, tree.Contains(1500))

	// Check min/max after purge
	assert.Equal(t, uint64(1300), tree.GetMinLSN())
	assert.Equal(t, uint64(1500), tree.GetMaxLSN())
}

func TestBPlusTree_PurgeAll(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert LSNs
	lsns := []uint64{1000, 1100, 1200}
	for _, lsn := range lsns {
		tree.Insert(lsn)
	}

	// Purge all LSNs
	purged := tree.PurgeBefore(2000)
	assert.Equal(t, 3, purged)
	assert.Equal(t, 0, tree.Count())
}

func TestBPlusTree_LargeDataset(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert many LSNs
	const count = 10000
	for i := uint64(0); i < count; i++ {
		tree.Insert(i * 100)
	}

	assert.Equal(t, count, tree.Count())
	assert.Equal(t, uint64(0), tree.GetMinLSN())
	assert.Equal(t, uint64((count-1)*100), tree.GetMaxLSN())

	// Test range query on large dataset
	result := tree.RangeQuery(50000, 60000)
	assert.True(t, len(result) > 0)
	for _, lsn := range result {
		assert.True(t, lsn >= 50000 && lsn <= 60000)
	}
}

func TestBPlusTree_RandomInsertOrder(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Generate random LSNs
	const count = 1000
	lsns := make([]uint64, count)
	for i := 0; i < count; i++ {
		lsns[i] = uint64(i * 100)
	}

	// Shuffle
	rand.Shuffle(len(lsns), func(i, j int) {
		lsns[i], lsns[j] = lsns[j], lsns[i]
	})

	// Insert in random order
	for _, lsn := range lsns {
		tree.Insert(lsn)
	}

	// Verify all LSNs exist
	for _, lsn := range lsns {
		assert.True(t, tree.Contains(lsn))
	}

	// Verify GetAll returns sorted result
	result := tree.GetAll()
	assert.Equal(t, count, len(result))
	for i := 1; i < len(result); i++ {
		assert.True(t, result[i-1] < result[i], "LSNs should be sorted")
	}
}

func TestBPlusTree_MultiplePurges(t *testing.T) {
	tree := NewLSNBPlusTree()

	// Insert LSNs
	for i := uint64(1000); i < 2000; i += 50 {
		tree.Insert(i)
	}

	initialCount := tree.Count()

	// Multiple purges
	purged1 := tree.PurgeBefore(1300)
	assert.True(t, purged1 > 0)
	count1 := tree.Count()
	assert.True(t, count1 < initialCount)

	purged2 := tree.PurgeBefore(1600)
	assert.True(t, purged2 > 0)
	count2 := tree.Count()
	assert.True(t, count2 < count1)

	// Verify min LSN after purges
	assert.True(t, tree.GetMinLSN() >= 1600)
}

func BenchmarkBPlusTree_Insert(b *testing.B) {
	tree := NewLSNBPlusTree()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		tree.Insert(uint64(i))
	}
}

func BenchmarkBPlusTree_Contains(b *testing.B) {
	tree := NewLSNBPlusTree()

	// Prepare data
	for i := uint64(0); i < 10000; i++ {
		tree.Insert(i * 100)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		tree.Contains(uint64(i%10000) * 100)
	}
}

func BenchmarkBPlusTree_RangeQuery(b *testing.B) {
	tree := NewLSNBPlusTree()

	// Prepare data
	for i := uint64(0); i < 10000; i++ {
		tree.Insert(i * 100)
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		tree.RangeQuery(50000, 60000)
	}
}

