package binlog

import (
	"fmt"
	"sync"
	
	"github.com/percona/go-binlog-parser/pkg/pool"
	"github.com/percona/go-binlog-parser/pkg/types"
)

type ConcurrentParser struct {
	workers int
	pool    *pool.WorkerPool
}

func NewConcurrentParser(workers int) *ConcurrentParser {
	return &ConcurrentParser{
		workers: workers,
		pool:    pool.NewWorkerPool(workers),
	}
}

func (cp *ConcurrentParser) ParseFile(filename string) ([]*types.Event, error) {
	parser, err := NewParser(filename)
	if err != nil {
		return nil, err
	}
	defer parser.Close()
	
	var events []*types.Event
	var mu sync.Mutex
	
	cp.pool.Start()
	
	// Read events sequentially, process concurrently
	for {
		event, err := parser.ReadEvent()
		if err != nil {
			break
		}
		
		// Copy event for concurrent processing
		evt := event
		cp.pool.Submit(func() error {
			// Process event
			mu.Lock()
			events = append(events, evt)
			mu.Unlock()
			return nil
		})
	}
	
	cp.pool.Wait()
	
	// Check for errors
	select {
	case err := <-cp.pool.Errors():
		return nil, err
	default:
	}
	
	return events, nil
}
