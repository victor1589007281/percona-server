package pool

import (
	"sync/atomic"
	"testing"
	"time"
)

func TestWorkerPool(t *testing.T) {
	pool := NewWorkerPool(4)
	pool.Start()
	
	var counter int32
	
	for i := 0; i < 10; i++ {
		pool.Submit(func() error {
			atomic.AddInt32(&counter, 1)
			time.Sleep(10 * time.Millisecond)
			return nil
		})
	}
	
	pool.Wait()
	
	if counter != 10 {
		t.Errorf("expected 10, got %d", counter)
	}
}
