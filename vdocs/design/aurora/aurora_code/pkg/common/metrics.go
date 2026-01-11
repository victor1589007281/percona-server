package common

import (
	"sync"
	"sync/atomic"
	"time"
)

// MetricsRegistry holds all metrics
type MetricsRegistry struct {
	mu       sync.RWMutex
	counters map[string]*Counter
	gauges   map[string]*Gauge
	histos   map[string]*Histogram
}

// NewMetricsRegistry creates a new metrics registry
func NewMetricsRegistry() *MetricsRegistry {
	return &MetricsRegistry{
		counters: make(map[string]*Counter),
		gauges:   make(map[string]*Gauge),
		histos:   make(map[string]*Histogram),
	}
}

// Counter is a monotonically increasing counter
type Counter struct {
	name  string
	value atomic.Int64
}

// NewCounter creates or gets a counter
func (r *MetricsRegistry) NewCounter(name string) *Counter {
	r.mu.Lock()
	defer r.mu.Unlock()
	
	if c, ok := r.counters[name]; ok {
		return c
	}
	
	c := &Counter{name: name}
	r.counters[name] = c
	return c
}

// Inc increments the counter
func (c *Counter) Inc() {
	c.value.Add(1)
}

// Add adds a value to the counter
func (c *Counter) Add(v int64) {
	c.value.Add(v)
}

// Value returns the counter value
func (c *Counter) Value() int64 {
	return c.value.Load()
}

// Gauge is a value that can go up and down
type Gauge struct {
	name  string
	value atomic.Int64
}

// NewGauge creates or gets a gauge
func (r *MetricsRegistry) NewGauge(name string) *Gauge {
	r.mu.Lock()
	defer r.mu.Unlock()
	
	if g, ok := r.gauges[name]; ok {
		return g
	}
	
	g := &Gauge{name: name}
	r.gauges[name] = g
	return g
}

// Set sets the gauge value
func (g *Gauge) Set(v int64) {
	g.value.Store(v)
}

// Inc increments the gauge
func (g *Gauge) Inc() {
	g.value.Add(1)
}

// Dec decrements the gauge
func (g *Gauge) Dec() {
	g.value.Add(-1)
}

// Value returns the gauge value
func (g *Gauge) Value() int64 {
	return g.value.Load()
}

// Histogram tracks value distributions
type Histogram struct {
	name    string
	mu      sync.Mutex
	buckets []int64
	counts  []int64
	sum     int64
	count   int64
}

// NewHistogram creates or gets a histogram
func (r *MetricsRegistry) NewHistogram(name string, buckets []int64) *Histogram {
	r.mu.Lock()
	defer r.mu.Unlock()
	
	if h, ok := r.histos[name]; ok {
		return h
	}
	
	h := &Histogram{
		name:    name,
		buckets: buckets,
		counts:  make([]int64, len(buckets)+1),
	}
	r.histos[name] = h
	return h
}

// Observe adds a value to the histogram
func (h *Histogram) Observe(v int64) {
	h.mu.Lock()
	defer h.mu.Unlock()
	
	h.sum += v
	h.count++
	
	for i, bucket := range h.buckets {
		if v <= bucket {
			h.counts[i]++
			return
		}
	}
	h.counts[len(h.buckets)]++
}

// ObserveDuration observes a duration
func (h *Histogram) ObserveDuration(start time.Time) {
	h.Observe(time.Since(start).Microseconds())
}

// Summary returns histogram summary
func (h *Histogram) Summary() HistogramSummary {
	h.mu.Lock()
	defer h.mu.Unlock()
	
	var avg float64
	if h.count > 0 {
		avg = float64(h.sum) / float64(h.count)
	}
	
	return HistogramSummary{
		Count:   h.count,
		Sum:     h.sum,
		Average: avg,
	}
}

// HistogramSummary contains histogram summary data
type HistogramSummary struct {
	Count   int64
	Sum     int64
	Average float64
}

// Timer times operations
type Timer struct {
	start time.Time
	hist  *Histogram
}

// NewTimer starts a new timer
func (h *Histogram) NewTimer() *Timer {
	return &Timer{
		start: time.Now(),
		hist:  h,
	}
}

// Stop stops the timer and records the duration
func (t *Timer) Stop() {
	t.hist.ObserveDuration(t.start)
}

// GetAllMetrics returns all metrics as a map
func (r *MetricsRegistry) GetAllMetrics() map[string]interface{} {
	r.mu.RLock()
	defer r.mu.RUnlock()
	
	result := make(map[string]interface{})
	
	for name, c := range r.counters {
		result["counter_"+name] = c.Value()
	}
	
	for name, g := range r.gauges {
		result["gauge_"+name] = g.Value()
	}
	
	for name, h := range r.histos {
		result["histogram_"+name] = h.Summary()
	}
	
	return result
}

// DefaultMetrics provides default Aurora metrics
var DefaultMetrics = NewMetricsRegistry()

// Common metric names
const (
	MetricRedoWriteTotal       = "redo_write_total"
	MetricRedoWriteBytes       = "redo_write_bytes"
	MetricRedoWriteLatency     = "redo_write_latency_us"
	MetricPageReadTotal        = "page_read_total"
	MetricPageReadLatency      = "page_read_latency_us"
	MetricPageCacheHits        = "page_cache_hits"
	MetricPageCacheMisses      = "page_cache_misses"
	MetricQuorumAckLatency     = "quorum_ack_latency_us"
	MetricActiveConnections    = "active_connections"
	MetricFailoverTotal        = "failover_total"
	MetricReplicationLagMs     = "replication_lag_ms"
)

// Standard latency buckets (in microseconds)
var LatencyBuckets = []int64{
	100, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000,
}
