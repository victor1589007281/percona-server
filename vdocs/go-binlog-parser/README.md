# Go Binlog Parser

High-performance concurrent binlog parser for Percona Server 8.4.3-3.

## Features

- ✅ Concurrent binlog reading with goroutine pool
- ✅ Support for all major event types
- ✅ Event-driven parsing architecture
- ✅ Memory-efficient streaming
- ✅ Comprehensive testing

## Usage

```go
parser := binlog.NewConcurrentParser(4) // 4 workers
events, err := parser.ParseFile("mysql-bin.000001")
```
