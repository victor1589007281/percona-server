package main

import (
	"fmt"
	"math/rand"
	"time"

	"github.com/percona/go-redo-parser/pkg/logindex"
)

func main() {
	fmt.Println("=== LogIndex Performance Demo ===\n")

	idx := logindex.NewLogIndex()

	// Simulate a workload with many pages and LSNs
	const (
		numSpaces    = 5
		pagesPerSpace = 1000
		lsnsPerPage   = 100
	)

	fmt.Printf("Simulating workload:\n")
	fmt.Printf("  - %d tablespaces\n", numSpaces)
	fmt.Printf("  - %d pages per tablespace\n", pagesPerSpace)
	fmt.Printf("  - %d LSNs per page\n", lsnsPerPage)
	fmt.Printf("  - Total: %d pages, %d LSNs\n\n",
		numSpaces*pagesPerSpace, numSpaces*pagesPerSpace*lsnsPerPage)

	// Insert benchmark
	fmt.Println("--- Insert Performance ---")
	start := time.Now()
	
	totalInserts := 0
	for spaceID := uint32(1); spaceID <= numSpaces; spaceID++ {
		for pageNo := uint32(1); pageNo <= pagesPerSpace; pageNo++ {
			baseLSN := uint64(spaceID*1000000 + pageNo*1000)
			for i := 0; i < lsnsPerPage; i++ {
				lsn := baseLSN + uint64(i*10)
				idx.Insert(spaceID, pageNo, lsn)
				totalInserts++
			}
		}
	}
	
	insertDuration := time.Since(start)
	fmt.Printf("Inserted %d LSNs in %v\n", totalInserts, insertDuration)
	fmt.Printf("Insert rate: %.0f ops/sec\n", float64(totalInserts)/insertDuration.Seconds())
	fmt.Printf("Average latency: %.2f μs/op\n\n", float64(insertDuration.Microseconds())/float64(totalInserts))

	// Statistics
	stats := idx.GetStats()
	fmt.Println("--- Statistics After Insert ---")
	fmt.Printf("Pages: %d\n", stats.PageCount)
	fmt.Printf("Total LSNs: %d\n", stats.TotalLSNs)
	fmt.Printf("LSN Range: [%d, %d]\n", stats.MinLSN, stats.MaxLSN)
	fmt.Printf("Memory Usage: %.2f MB\n\n", float64(idx.GetMemoryUsage())/(1024*1024))

	// Query benchmark
	fmt.Println("--- Query Performance ---")
	
	// Random query test
	const numQueries = 10000
	rand.Seed(time.Now().UnixNano())
	
	start = time.Now()
	for i := 0; i < numQueries; i++ {
		spaceID := uint32(rand.Intn(numSpaces) + 1)
		pageNo := uint32(rand.Intn(pagesPerSpace) + 1)
		baseLSN := uint64(spaceID*1000000 + pageNo*1000)
		
		// Query a range of 20 LSNs
		idx.QueryRange(spaceID, pageNo, baseLSN, baseLSN+200)
	}
	
	queryDuration := time.Since(start)
	fmt.Printf("Executed %d range queries in %v\n", numQueries, queryDuration)
	fmt.Printf("Query rate: %.0f ops/sec\n", float64(numQueries)/queryDuration.Seconds())
	fmt.Printf("Average latency: %.2f μs/op\n\n", float64(queryDuration.Microseconds())/float64(numQueries))

	// Contains benchmark
	fmt.Println("--- Contains Performance ---")
	
	start = time.Now()
	for i := 0; i < numQueries; i++ {
		spaceID := uint32(rand.Intn(numSpaces) + 1)
		pageNo := uint32(rand.Intn(pagesPerSpace) + 1)
		baseLSN := uint64(spaceID*1000000 + pageNo*1000)
		lsnOffset := uint64(rand.Intn(lsnsPerPage) * 10)
		
		idx.Contains(spaceID, pageNo, baseLSN+lsnOffset)
	}
	
	containsDuration := time.Since(start)
	fmt.Printf("Executed %d contains checks in %v\n", numQueries, containsDuration)
	fmt.Printf("Contains rate: %.0f ops/sec\n", float64(numQueries)/containsDuration.Seconds())
	fmt.Printf("Average latency: %.2f μs/op\n\n", float64(containsDuration.Microseconds())/float64(numQueries))

	// Purge benchmark
	fmt.Println("--- Purge Performance ---")
	
	// Calculate a purge threshold (keep 50% of LSNs)
	purgeThreshold := (stats.MinLSN + stats.MaxLSN) / 2
	
	start = time.Now()
	purged := idx.PurgeBefore(purgeThreshold)
	purgeDuration := time.Since(start)
	
	fmt.Printf("Purged %d LSNs (threshold: %d) in %v\n", purged, purgeThreshold, purgeDuration)
	fmt.Printf("Purge rate: %.0f LSNs/sec\n", float64(purged)/purgeDuration.Seconds())
	
	statsAfterPurge := idx.GetStats()
	fmt.Printf("Remaining LSNs: %d\n", statsAfterPurge.TotalLSNs)
	fmt.Printf("Memory Usage After Purge: %.2f MB\n", float64(idx.GetMemoryUsage())/(1024*1024))

	fmt.Println("\n=== Performance Demo Complete ===")
}

