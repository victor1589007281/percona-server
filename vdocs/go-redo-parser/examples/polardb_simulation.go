package main

import (
	"fmt"
	"math/rand"
	"time"

	"github.com/percona/go-redo-parser/pkg/logindex"
)

// PolarDBSimulation simulates a PolarDB-style redo log replay scenario
func main() {
	fmt.Println("=== PolarDB LogIndex Simulation ===\n")

	// Initialize LogIndex
	idx := logindex.NewLogIndex()
	fmt.Println("✓ Initialized LogIndex")

	// Simulation parameters
	const (
		numPages       = 500
		redoLogWindow  = 10000
		numTransactions = 5000
	)

	fmt.Printf("\nSimulation Parameters:\n")
	fmt.Printf("  - Number of pages: %d\n", numPages)
	fmt.Printf("  - Redo log window: %d LSNs\n", redoLogWindow)
	fmt.Printf("  - Number of transactions: %d\n\n", numTransactions)

	// Phase 1: Simulate primary node generating redo logs
	fmt.Println("--- Phase 1: Primary Node Redo Log Generation ---")
	
	rand.Seed(time.Now().UnixNano())
	currentLSN := uint64(1000)
	
	start := time.Now()
	for txn := 0; txn < numTransactions; txn++ {
		// Each transaction modifies 1-5 pages
		numPagesModified := rand.Intn(5) + 1
		
		for i := 0; i < numPagesModified; i++ {
			spaceID := uint32(1)
			pageNo := uint32(rand.Intn(numPages))
			
			// Generate LSN for this modification
			currentLSN += uint64(rand.Intn(10) + 1)
			idx.Insert(spaceID, pageNo, currentLSN)
		}
	}
	
	phase1Duration := time.Since(start)
	stats := idx.GetStats()
	
	fmt.Printf("Generated %d redo log entries\n", stats.TotalLSNs)
	fmt.Printf("Time: %v (%.0f ops/sec)\n", phase1Duration,
		float64(stats.TotalLSNs)/phase1Duration.Seconds())
	fmt.Printf("LSN range: [%d, %d]\n", stats.MinLSN, stats.MaxLSN)
	fmt.Printf("Pages modified: %d\n\n", stats.PageCount)

	// Phase 2: Simulate read-only node replay
	fmt.Println("--- Phase 2: Read-Only Node Page Replay ---")
	
	// Simulate read-only node reading pages
	// For each page, we need to know:
	// 1. The Page LSN (last LSN when page was written to disk)
	// 2. The current max LSN for that page
	
	const numPageReads = 100
	totalReplayed := 0
	
	start = time.Now()
	for i := 0; i < numPageReads; i++ {
		spaceID := uint32(1)
		pageNo := uint32(rand.Intn(numPages))
		
		// Simulate Page LSN (some point in the past)
		minLSN, maxLSN, exists := idx.GetPageLSNRange(spaceID, pageNo)
		if !exists {
			continue
		}
		
		// Simulate that page was written to disk at 70% of its LSN range
		pageLSN := minLSN + uint64(float64(maxLSN-minLSN)*0.7)
		
		// Query the LSN range that needs to be replayed
		lsnsToReplay := idx.QueryRange(spaceID, pageNo, pageLSN+1, maxLSN)
		
		if i < 5 { // Print first 5 examples
			fmt.Printf("  Page (%d, %d): ", spaceID, pageNo)
			fmt.Printf("Page LSN=%d, Max LSN=%d, ", pageLSN, maxLSN)
			fmt.Printf("Need to replay %d LSNs\n", len(lsnsToReplay))
		}
		
		totalReplayed += len(lsnsToReplay)
	}
	
	phase2Duration := time.Since(start)
	
	if numPageReads > 5 {
		fmt.Printf("  ... (%d more pages)\n", numPageReads-5)
	}
	
	fmt.Printf("\nTotal LSNs replayed: %d\n", totalReplayed)
	fmt.Printf("Average LSNs per page: %.1f\n", float64(totalReplayed)/float64(numPageReads))
	fmt.Printf("Replay time: %v (%.0f pages/sec)\n\n",
		phase2Duration, float64(numPageReads)/phase2Duration.Seconds())

	// Phase 3: Simulate checkpoint and purge
	fmt.Println("--- Phase 3: Checkpoint and Purge ---")
	
	// Find minimum checkpoint LSN across all read-only nodes
	// In real scenario, this would be the minimum of all read-only node replay positions
	// For simulation, assume all read-only nodes have caught up to 80% of current LSN
	minCheckpointLSN := stats.MinLSN + uint64(float64(stats.MaxLSN-stats.MinLSN)*0.8)
	
	fmt.Printf("Minimum checkpoint LSN: %d\n", minCheckpointLSN)
	fmt.Printf("Before purge: %d LSNs, Memory: %.2f MB\n",
		stats.TotalLSNs, float64(idx.GetMemoryUsage())/(1024*1024))
	
	start = time.Now()
	purged := idx.PurgeBefore(minCheckpointLSN)
	purgeDuration := time.Since(start)
	
	statsAfterPurge := idx.GetStats()
	
	fmt.Printf("Purged: %d LSNs in %v\n", purged, purgeDuration)
	fmt.Printf("After purge: %d LSNs, Memory: %.2f MB\n",
		statsAfterPurge.TotalLSNs, float64(idx.GetMemoryUsage())/(1024*1024))
	fmt.Printf("Memory saved: %.2f MB (%.1f%%)\n\n",
		float64(stats.TotalLSNs-statsAfterPurge.TotalLSNs)*16/1024/1024,
		float64(purged)/float64(stats.TotalLSNs)*100)

	// Phase 4: Analyze hot pages
	fmt.Println("--- Phase 4: Hot Page Analysis ---")
	
	type PageInfo struct {
		SpaceID uint32
		PageNo  uint32
		Count   int
		Range   uint64
	}
	
	hotPages := make([]PageInfo, 0)
	
	idx.ForEachPage(func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool {
		hotPages = append(hotPages, PageInfo{
			SpaceID: spaceID,
			PageNo:  pageNo,
			Count:   count,
			Range:   maxLSN - minLSN,
		})
		return true
	})
	
	// Sort by count (most modified pages first)
	for i := 0; i < len(hotPages)-1; i++ {
		for j := i + 1; j < len(hotPages); j++ {
			if hotPages[j].Count > hotPages[i].Count {
				hotPages[i], hotPages[j] = hotPages[j], hotPages[i]
			}
		}
	}
	
	fmt.Println("Top 10 Hot Pages:")
	for i := 0; i < 10 && i < len(hotPages); i++ {
		page := hotPages[i]
		fmt.Printf("  %2d. Page (%d, %3d): %3d modifications, LSN range: %d\n",
			i+1, page.SpaceID, page.PageNo, page.Count, page.Range)
	}

	// Summary
	fmt.Println("\n--- Simulation Summary ---")
	fmt.Printf("✓ Simulated %d transactions\n", numTransactions)
	fmt.Printf("✓ Generated %d redo log entries for %d pages\n",
		statsAfterPurge.TotalLSNs, statsAfterPurge.PageCount)
	fmt.Printf("✓ Replayed %d LSNs for %d page reads\n", totalReplayed, numPageReads)
	fmt.Printf("✓ Purged %d old LSNs\n", purged)
	fmt.Printf("✓ Current memory usage: %.2f MB\n",
		float64(idx.GetMemoryUsage())/(1024*1024))
	
	fmt.Println("\n=== Simulation Complete ===")
}

