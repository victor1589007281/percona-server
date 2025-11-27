package main

import (
	"fmt"
	"log"
	
	"github.com/percona/go-binlog-parser/pkg/binlog"
)

func main() {
	parser, err := binlog.NewParser("mysql-bin.000001")
	if err != nil {
		log.Fatal(err)
	}
	defer parser.Close()
	
	for {
		event, err := parser.ReadEvent()
		if err != nil {
			break
		}
		
		fmt.Printf("Event Type: %d, Size: %d\n", 
			event.Header.EventType, event.Header.EventSize)
	}
}
