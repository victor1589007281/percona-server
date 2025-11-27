package main

import (
	"fmt"
	"time"

	"github.com/percona/go-redo-parser/pkg/logindex"
)

func main() {
	fmt.Println("=== LogIndex Basic Usage Example ===\n")

	// Create a new LogIndex
	idx := logindex.NewLogIndex()
	fmt.Println("✓ Created new LogIndex")

	// Example 1: Insert LSNs for multiple pages
	fmt.Println("\n--- Example 1: Insert LSNs ---")
	
	// Simulate redo log entries for different pages
	fmt.Println("Inserting LSNs for Page (SpaceID=1, PageNo=100)...")
	idx.Insert(1, 100, 1000)
	idx.Insert(1, 100, 1050)
	idx.Insert(1, 100, 1100)
	idx.Insert(1, 100, 1200)
	
	fmt.Println("Inserting LSNs for Page (SpaceID=1, PageNo=101)...")
	idx.Insert(1, 101, 1150)
	idx.Insert(1, 101, 1250)
	
	fmt.Println("Inserting LSNs for Page (SpaceID=2, PageNo=200)...")
	idx.Insert(2, 200, 2000)
	idx.Insert(2, 200, 2100)
	idx.Insert(2, 200, 2200)

	// Example 2: Query all LSNs for a page
	fmt.Println("\n--- Example 2: Query All LSNs ---")
	lsns := idx.Query(1, 100)
	fmt.Printf("All LSNs for Page (1, 100): %v\n", lsns)

	// Example 3: Range query
	fmt.Println("\n--- Example 3: Range Query ---")
	rangeResult := idx.QueryRange(1, 100, 1050, 1150)
	fmt.Printf("LSNs in range [1050, 1150] for Page (1, 100): %v\n", rangeResult)

	// Example 4: Check if specific LSN exists
	fmt.Println("\n--- Example 4: Contains Check ---")
	exists := idx.Contains(1, 100, 1100)
	fmt.Printf("Does LSN 1100 exist for Page (1, 100)? %v\n", exists)
	
	exists = idx.Contains(1, 100, 9999)
	fmt.Printf("Does LSN 9999 exist for Page (1, 100)? %v\n", exists)

	// Example 5: Get min/max LSN for a page
	fmt.Println("\n--- Example 5: Get Page LSN Range ---")
	minLSN, maxLSN, exists := idx.GetPageLSNRange(1, 100)
	if exists {
		fmt.Printf("Page (1, 100) LSN range: [%d, %d]\n", minLSN, maxLSN)
	}

	// Example 6: Get statistics
	fmt.Println("\n--- Example 6: Statistics ---")
	stats := idx.GetStats()
	fmt.Printf("Total Pages: %d\n", stats.PageCount)
	fmt.Printf("Total LSNs: %d\n", stats.TotalLSNs)
	fmt.Printf("Current LSN: %d\n", stats.CurrentLSN)
	fmt.Printf("Global LSN Range: [%d, %d]\n", stats.MinLSN, stats.MaxLSN)

	// Example 7: Batch insert
	fmt.Println("\n--- Example 7: Batch Insert ---")
	batchLSNs := []uint64{3000, 3100, 3200, 3300, 3400}
	idx.BatchInsert(3, 300, batchLSNs)
	fmt.Printf("Batch inserted %d LSNs for Page (3, 300)\n", len(batchLSNs))

	// Example 8: Iterate over all pages
	fmt.Println("\n--- Example 8: Iterate Over All Pages ---")
	idx.ForEachPage(func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool {
		fmt.Printf("  Page (SpaceID=%d, PageNo=%d): %d LSNs, Range=[%d, %d]\n",
			spaceID, pageNo, count, minLSN, maxLSN)
		return true // Continue iteration
	})

	// Example 9: Purge old LSNs
	fmt.Println("\n--- Example 9: Purge Old LSNs ---")
	fmt.Printf("Before purge: %d LSNs\n", idx.GetTotalLSNCount())
	
	purgeThreshold := uint64(1200)
	purged := idx.PurgeBefore(purgeThreshold)
	fmt.Printf("Purged %d LSNs before LSN %d\n", purged, purgeThreshold)
	fmt.Printf("After purge: %d LSNs\n", idx.GetTotalLSNCount())

	// Example 10: Memory usage
	fmt.Println("\n--- Example 10: Memory Usage ---")
	memUsage := idx.GetMemoryUsage()
	fmt.Printf("Estimated memory usage: %.2f KB\n", float64(memUsage)/1024)

	fmt.Println("\n=== Example Complete ===")
}

