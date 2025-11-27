package logindex

import (
	"fmt"
	"sync"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestLogIndex_NewLogIndex(t *testing.T) {
	idx := NewLogIndex()
	assert.NotNil(t, idx)
	assert.Equal(t, 0, idx.GetPageCount())
	assert.Equal(t, 0, idx.GetTotalLSNCount())
	assert.Equal(t, uint64(0), idx.GetCurrentLSN())
}

func TestLogIndex_Insert(t *testing.T) {
	idx := NewLogIndex()

	// Insert LSNs for different pages
	err := idx.Insert(1, 100, 1000)
	assert.NoError(t, err)

	err = idx.Insert(1, 100, 1050)
	assert.NoError(t, err)

	err = idx.Insert(1, 101, 1100)
	assert.NoError(t, err)

	// Verify stats
	assert.Equal(t, 2, idx.GetPageCount())
	assert.Equal(t, 3, idx.GetTotalLSNCount())
	assert.Equal(t, uint64(1100), idx.GetCurrentLSN())
}

func TestLogIndex_InsertInvalidLSN(t *testing.T) {
	idx := NewLogIndex()

	// Insert invalid LSN (0)
	err := idx.Insert(1, 100, 0)
	assert.Error(t, err)
}

func TestLogIndex_Contains(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 1050)
	idx.Insert(1, 101, 1100)

	// Test contains
	assert.True(t, idx.Contains(1, 100, 1000))
	assert.True(t, idx.Contains(1, 100, 1050))
	assert.True(t, idx.Contains(1, 101, 1100))

	// Test non-existent
	assert.False(t, idx.Contains(1, 100, 999))
	assert.False(t, idx.Contains(1, 102, 1000))
	assert.False(t, idx.Contains(2, 100, 1000))
}

func TestLogIndex_Query(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	lsns := []uint64{1000, 1050, 1100, 1200, 1300}
	for _, lsn := range lsns {
		idx.Insert(1, 100, lsn)
	}

	// Query all LSNs
	result := idx.Query(1, 100)
	assert.Equal(t, len(lsns), len(result))

	// Verify sorted order
	for i := 1; i < len(result); i++ {
		assert.True(t, result[i-1] < result[i])
	}

	// Query non-existent page
	result = idx.Query(1, 999)
	assert.Nil(t, result)
}

func TestLogIndex_QueryRange(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	for i := uint64(1000); i <= 2000; i += 100 {
		idx.Insert(1, 100, i)
	}

	// Test range queries
	testCases := []struct {
		name     string
		start    uint64
		end      uint64
		expected int
	}{
		{"Full range", 1000, 2000, 11},
		{"Partial range", 1200, 1600, 5},
		{"Single value", 1500, 1500, 1},
		{"Before all", 100, 500, 0},
		{"After all", 3000, 4000, 0},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			result := idx.QueryRange(1, 100, tc.start, tc.end)
			assert.Equal(t, tc.expected, len(result))
		})
	}
}

func TestLogIndex_GetPageLSNRange(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 1500)
	idx.Insert(1, 100, 2000)

	// Get range
	minLSN, maxLSN, exists := idx.GetPageLSNRange(1, 100)
	assert.True(t, exists)
	assert.Equal(t, uint64(1000), minLSN)
	assert.Equal(t, uint64(2000), maxLSN)

	// Get range for non-existent page
	_, _, exists = idx.GetPageLSNRange(1, 999)
	assert.False(t, exists)
}

func TestLogIndex_PurgeBefore(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data for multiple pages
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 1100)
	idx.Insert(1, 100, 1200)
	idx.Insert(1, 101, 1050)
	idx.Insert(1, 101, 1150)
	idx.Insert(1, 102, 900) // This page will be completely purged

	initialCount := idx.GetTotalLSNCount()
	assert.Equal(t, 6, initialCount)

	// Purge LSNs before 1100
	purged := idx.PurgeBefore(1100)
	assert.True(t, purged > 0)

	// Verify purge results
	assert.False(t, idx.Contains(1, 100, 1000))
	assert.True(t, idx.Contains(1, 100, 1100))
	assert.True(t, idx.Contains(1, 100, 1200))

	assert.False(t, idx.Contains(1, 101, 1050))
	assert.True(t, idx.Contains(1, 101, 1150))

	// Page 102 should be completely removed
	_, _, exists := idx.GetPageLSNRange(1, 102)
	assert.False(t, exists)

	// Total count should be less
	assert.True(t, idx.GetTotalLSNCount() < initialCount)
}

func TestLogIndex_BatchInsert(t *testing.T) {
	idx := NewLogIndex()

	// Batch insert
	lsns := []uint64{1000, 1100, 1200, 1300, 1400}
	err := idx.BatchInsert(1, 100, lsns)
	assert.NoError(t, err)

	// Verify all LSNs inserted
	for _, lsn := range lsns {
		assert.True(t, idx.Contains(1, 100, lsn))
	}

	assert.Equal(t, 1, idx.GetPageCount())
	assert.Equal(t, len(lsns), idx.GetTotalLSNCount())
}

func TestLogIndex_GetStats(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 2000)
	idx.Insert(1, 101, 1500)

	stats := idx.GetStats()
	assert.Equal(t, 2, stats.PageCount)
	assert.Equal(t, 3, stats.TotalLSNs)
	assert.Equal(t, uint64(2000), stats.CurrentLSN)
	assert.Equal(t, uint64(1000), stats.MinLSN)
	assert.Equal(t, uint64(2000), stats.MaxLSN)

	t.Logf("Stats: %s", stats.String())
}

func TestLogIndex_Clear(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 101, 1100)
	assert.Equal(t, 2, idx.GetPageCount())

	// Clear
	idx.Clear()
	assert.Equal(t, 0, idx.GetPageCount())
	assert.Equal(t, 0, idx.GetTotalLSNCount())
	assert.Equal(t, uint64(0), idx.GetCurrentLSN())
}

func TestLogIndex_ForEachPage(t *testing.T) {
	idx := NewLogIndex()

	// Insert test data
	pages := []struct {
		spaceID uint32
		pageNo  uint32
		lsns    []uint64
	}{
		{1, 100, []uint64{1000, 1100, 1200}},
		{1, 101, []uint64{1050, 1150}},
		{2, 200, []uint64{2000, 2100, 2200, 2300}},
	}

	for _, page := range pages {
		for _, lsn := range page.lsns {
			idx.Insert(page.spaceID, page.pageNo, lsn)
		}
	}

	// Iterate and verify
	visitedPages := make(map[PageID]bool)
	idx.ForEachPage(func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool {
		pageID := PageID{SpaceID: spaceID, PageNo: pageNo}
		visitedPages[pageID] = true

		// Verify count
		var expectedCount int
		for _, page := range pages {
			if page.spaceID == spaceID && page.pageNo == pageNo {
				expectedCount = len(page.lsns)
				break
			}
		}
		assert.Equal(t, expectedCount, count)

		return true
	})

	assert.Equal(t, len(pages), len(visitedPages))
}

func TestLogIndex_ConcurrentAccess(t *testing.T) {
	idx := NewLogIndex()

	const numGoroutines = 10
	const opsPerGoroutine = 100

	var wg sync.WaitGroup
	wg.Add(numGoroutines)

	// Concurrent insertions
	for g := 0; g < numGoroutines; g++ {
		go func(goroutineID int) {
			defer wg.Done()
			for i := 0; i < opsPerGoroutine; i++ {
				spaceID := uint32(goroutineID % 3)
				pageNo := uint32(i % 10)
				lsn := uint64(goroutineID*1000 + i)
				idx.Insert(spaceID, pageNo, lsn)
			}
		}(g)
	}

	wg.Wait()

	// Verify data integrity
	assert.True(t, idx.GetPageCount() > 0)
	assert.True(t, idx.GetTotalLSNCount() > 0)
	assert.True(t, idx.GetCurrentLSN() > 0)
}

func TestLogIndex_RealWorldScenario(t *testing.T) {
	// Simulate a real-world scenario
	idx := NewLogIndex()

	// Simulate primary node writing redo logs
	t.Log("Simulating primary node writes...")
	for spaceID := uint32(1); spaceID <= 3; spaceID++ {
		for pageNo := uint32(100); pageNo <= 110; pageNo++ {
			for lsn := uint64(1000); lsn <= 5000; lsn += 100 {
				idx.Insert(spaceID, pageNo, lsn)
			}
		}
	}

	stats := idx.GetStats()
	t.Logf("After writes: %s", stats.String())

	// Simulate read-only node querying
	t.Log("Simulating read-only node queries...")
	for pageNo := uint32(100); pageNo <= 110; pageNo++ {
		// Get the page's LSN range
		minLSN, maxLSN, exists := idx.GetPageLSNRange(1, pageNo)
		require.True(t, exists)
		assert.True(t, minLSN <= maxLSN)

		// Query range
		lsns := idx.QueryRange(1, pageNo, 2000, 4000)
		assert.True(t, len(lsns) > 0)

		// Verify all LSNs are in range
		for _, lsn := range lsns {
			assert.True(t, lsn >= 2000 && lsn <= 4000)
		}
	}

	// Simulate purging old LSNs
	t.Log("Simulating purge operation...")
	purgeThreshold := uint64(3000)
	purged := idx.PurgeBefore(purgeThreshold)
	t.Logf("Purged %d LSNs before %d", purged, purgeThreshold)

	statsAfterPurge := idx.GetStats()
	t.Logf("After purge: %s", statsAfterPurge.String())

	// Verify purge worked
	assert.True(t, statsAfterPurge.TotalLSNs < stats.TotalLSNs)
	assert.True(t, statsAfterPurge.MinLSN >= purgeThreshold)
}

func TestLogIndex_GetMemoryUsage(t *testing.T) {
	idx := NewLogIndex()

	// Insert some data
	for i := uint32(0); i < 100; i++ {
		for lsn := uint64(1000); lsn < 2000; lsn += 100 {
			idx.Insert(1, i, lsn)
		}
	}

	memUsage := idx.GetMemoryUsage()
	t.Logf("Memory usage: %d bytes (%.2f MB)", memUsage, float64(memUsage)/(1024*1024))
	assert.True(t, memUsage > 0)
}

func BenchmarkLogIndex_Insert(b *testing.B) {
	idx := NewLogIndex()

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		idx.Insert(1, uint32(i%1000), uint64(i))
	}
}

func BenchmarkLogIndex_QueryRange(b *testing.B) {
	idx := NewLogIndex()

	// Prepare data
	for i := uint32(0); i < 100; i++ {
		for lsn := uint64(1000); lsn < 10000; lsn += 100 {
			idx.Insert(1, i, lsn)
		}
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		idx.QueryRange(1, uint32(i%100), 2000, 8000)
	}
}

func BenchmarkLogIndex_Contains(b *testing.B) {
	idx := NewLogIndex()

	// Prepare data
	for i := uint32(0); i < 100; i++ {
		for lsn := uint64(1000); lsn < 10000; lsn += 100 {
			idx.Insert(1, i, lsn)
		}
	}

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		idx.Contains(1, uint32(i%100), uint64(5000))
	}
}

func BenchmarkLogIndex_PurgeBefore(b *testing.B) {
	for i := 0; i < b.N; i++ {
		b.StopTimer()
		idx := NewLogIndex()

		// Prepare data
		for j := uint32(0); j < 100; j++ {
			for lsn := uint64(1000); lsn < 10000; lsn += 100 {
				idx.Insert(1, j, lsn)
			}
		}

		b.StartTimer()
		idx.PurgeBefore(5000)
	}
}

func ExampleLogIndex() {
	// Create a new LogIndex
	idx := NewLogIndex()

	// Insert LSNs for different pages
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 1050)
	idx.Insert(1, 100, 1100)
	idx.Insert(1, 101, 1200)

	// Query all LSNs for a page
	lsns := idx.Query(1, 100)
	fmt.Printf("LSNs for page (1, 100): %v\n", lsns)

	// Query LSN range
	rangeResult := idx.QueryRange(1, 100, 1000, 1080)
	fmt.Printf("LSNs in range [1000, 1080]: %v\n", rangeResult)

	// Check if specific LSN exists
	exists := idx.Contains(1, 100, 1050)
	fmt.Printf("LSN 1050 exists: %v\n", exists)

	// Get statistics
	stats := idx.GetStats()
	fmt.Printf("Stats: %s\n", stats.String())

	// Output:
	// LSNs for page (1, 100): [1000 1050 1100]
	// LSNs in range [1000, 1080]: [1000 1050]
	// LSN 1050 exists: true
	// Stats: LogIndex Stats: Pages=2, LSNs=4, CurrentLSN=1200, Range=[1000, 1200]
}

