package common

import (
	"context"
	"math/rand"
	"time"
)

// RetryConfig configures retry behavior
type RetryConfig struct {
	MaxRetries    int
	InitialDelay  time.Duration
	MaxDelay      time.Duration
	BackoffFactor float64
	Jitter        bool
}

// DefaultRetryConfig returns default retry configuration
func DefaultRetryConfig() RetryConfig {
	return RetryConfig{
		MaxRetries:    3,
		InitialDelay:  100 * time.Millisecond,
		MaxDelay:      10 * time.Second,
		BackoffFactor: 2.0,
		Jitter:        true,
	}
}

// RetryFunc is a function that can be retried
type RetryFunc func(ctx context.Context) error

// Retry executes a function with retries
func Retry(ctx context.Context, config RetryConfig, fn RetryFunc) error {
	var lastErr error
	delay := config.InitialDelay
	
	for attempt := 0; attempt <= config.MaxRetries; attempt++ {
		if err := fn(ctx); err == nil {
			return nil
		} else {
			lastErr = err
		}
		
		if attempt == config.MaxRetries {
			break
		}
		
		// Calculate delay with jitter
		waitDelay := delay
		if config.Jitter {
			jitter := time.Duration(rand.Float64() * float64(delay) * 0.3)
			waitDelay = delay + jitter
		}
		
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(waitDelay):
		}
		
		// Increase delay with backoff
		delay = time.Duration(float64(delay) * config.BackoffFactor)
		if delay > config.MaxDelay {
			delay = config.MaxDelay
		}
	}
	
	return lastErr
}

// RetryWithResult executes a function that returns a result with retries
func RetryWithResult[T any](ctx context.Context, config RetryConfig, fn func(ctx context.Context) (T, error)) (T, error) {
	var result T
	var lastErr error
	delay := config.InitialDelay
	
	for attempt := 0; attempt <= config.MaxRetries; attempt++ {
		res, err := fn(ctx)
		if err == nil {
			return res, nil
		}
		lastErr = err
		
		if attempt == config.MaxRetries {
			break
		}
		
		waitDelay := delay
		if config.Jitter {
			jitter := time.Duration(rand.Float64() * float64(delay) * 0.3)
			waitDelay = delay + jitter
		}
		
		select {
		case <-ctx.Done():
			return result, ctx.Err()
		case <-time.After(waitDelay):
		}
		
		delay = time.Duration(float64(delay) * config.BackoffFactor)
		if delay > config.MaxDelay {
			delay = config.MaxDelay
		}
	}
	
	return result, lastErr
}

// Batcher batches operations for efficiency
type Batcher[T any] struct {
	maxSize   int
	maxWait   time.Duration
	batch     []T
	batchChan chan []T
	itemChan  chan T
	done      chan struct{}
}

// NewBatcher creates a new batcher
func NewBatcher[T any](maxSize int, maxWait time.Duration) *Batcher[T] {
	b := &Batcher[T]{
		maxSize:   maxSize,
		maxWait:   maxWait,
		batchChan: make(chan []T, 10),
		itemChan:  make(chan T, maxSize*2),
		done:      make(chan struct{}),
	}
	go b.run()
	return b
}

func (b *Batcher[T]) run() {
	ticker := time.NewTicker(b.maxWait)
	defer ticker.Stop()
	
	for {
		select {
		case item := <-b.itemChan:
			b.batch = append(b.batch, item)
			if len(b.batch) >= b.maxSize {
				b.flush()
			}
		case <-ticker.C:
			if len(b.batch) > 0 {
				b.flush()
			}
		case <-b.done:
			if len(b.batch) > 0 {
				b.flush()
			}
			close(b.batchChan)
			return
		}
	}
}

func (b *Batcher[T]) flush() {
	if len(b.batch) == 0 {
		return
	}
	batch := b.batch
	b.batch = nil
	b.batchChan <- batch
}

// Add adds an item to the batch
func (b *Batcher[T]) Add(item T) {
	b.itemChan <- item
}

// Batches returns the channel of batches
func (b *Batcher[T]) Batches() <-chan []T {
	return b.batchChan
}

// Close closes the batcher
func (b *Batcher[T]) Close() {
	close(b.done)
}
