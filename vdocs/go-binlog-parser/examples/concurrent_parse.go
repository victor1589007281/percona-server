package main

import (
	"fmt"
	"log"
	
	"github.com/percona/go-binlog-parser/pkg/binlog"
)

func main() {
	parser := binlog.NewConcurrentParser(4)
	
	events, err := parser.ParseFile("mysql-bin.000001")
	if err != nil {
		log.Fatal(err)
	}
	
	fmt.Printf("Parsed %d events\n", len(events))
}
