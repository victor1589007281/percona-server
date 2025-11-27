# Go Binlog Parser API Documentation

## Parser

### NewParser(filename string) (*Parser, error)

Creates a new sequential binlog parser.

```go
parser, err := binlog.NewParser("mysql-bin.000001")
if err != nil {
    log.Fatal(err)
}
defer parser.Close()
```

### ReadEvent() (*types.Event, error)

Reads the next event from the binlog file.

```go
event, err := parser.ReadEvent()
if err != nil {
    if err == io.EOF {
        // End of file
    }
}
```

## ConcurrentParser

### NewConcurrentParser(workers int) *ConcurrentParser

Creates a concurrent parser with specified number of workers.

```go
parser := binlog.NewConcurrentParser(4)
```

### ParseFile(filename string) ([]*types.Event, error)

Parses entire binlog file concurrently.

```go
events, err := parser.ParseFile("mysql-bin.000001")
```

## Event Types

### EventHeader

```go
type EventHeader struct {
    Timestamp uint32
    EventType uint8
    ServerID  uint32
    EventSize uint32
    LogPos    uint32
    Flags     uint16
}
```

### Event

```go
type Event struct {
    Header *EventHeader
    Data   []byte
}
```

## Worker Pool

### NewWorkerPool(workers int) *WorkerPool

Creates a worker pool.

```go
pool := pool.NewWorkerPool(8)
pool.Start()
defer pool.Wait()
```
