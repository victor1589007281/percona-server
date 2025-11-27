package main

import (
	"flag"
	"fmt"
	"log"
	
	"github.com/percona/go-binlog-parser/pkg/binlog"
)

func main() {
	filename := flag.String("file", "", "Binlog file to parse")
	workers := flag.Int("workers", 4, "Number of workers")
	flag.Parse()
	
	if *filename == "" {
		log.Fatal("Please specify binlog file with -file")
	}
	
	parser := binlog.NewConcurrentParser(*workers)
	events, err := parser.ParseFile(*filename)
	if err != nil {
		log.Fatal(err)
	}
	
	fmt.Printf("Successfully parsed %d events\n", len(events))
	
	// Print event statistics
	eventCounts := make(map[uint8]int)
	for _, event := range events {
		eventCounts[event.Header.EventType]++
	}
	
	fmt.Println("\nEvent Type Distribution:")
	for eventType, count := range eventCounts {
		fmt.Printf("  Type %d: %d events\n", eventType, count)
	}
}
