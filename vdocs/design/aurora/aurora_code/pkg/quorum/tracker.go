// Package quorum implements Quorum protocol tracking
package quorum

import (
	"sort"
	"sync"
)

const (
	// DefaultN is the default number of replicas
	DefaultN = 6
	// DefaultVw is the default write quorum
	DefaultVw = 4
	// DefaultVr is the default read quorum
	DefaultVr = 3
)

// Config holds quorum configuration
type Config struct {
	N  int // Total replicas
	Vw int // Write quorum
	Vr int // Read quorum
}

// DefaultConfig returns the default quorum configuration
func DefaultConfig() Config {
	return Config{
		N:  DefaultN,
		Vw: DefaultVw,
		Vr: DefaultVr,
	}
}

// Tracker tracks LSN across storage nodes and calculates VDL
type Tracker struct {
	mu       sync.RWMutex
	config   Config
	nodeLSNs map[string]uint64 // nodeID -> LSN
}

// NewTracker creates a new quorum tracker
func NewTracker(config Config) *Tracker {
	return &Tracker{
		config:   config,
		nodeLSNs: make(map[string]uint64),
	}
}

// UpdateNodeLSN updates the LSN for a storage node
func (t *Tracker) UpdateNodeLSN(nodeID string, lsn uint64) {
	t.mu.Lock()
	defer t.mu.Unlock()

	// Only update if new LSN is higher
	if current, ok := t.nodeLSNs[nodeID]; !ok || lsn > current {
		t.nodeLSNs[nodeID] = lsn
	}
}

// GetNodeLSN returns the LSN for a specific node
func (t *Tracker) GetNodeLSN(nodeID string) (uint64, bool) {
	t.mu.RLock()
	defer t.mu.RUnlock()
	lsn, ok := t.nodeLSNs[nodeID]
	return lsn, ok
}

// GetAllNodeLSNs returns all node LSNs
func (t *Tracker) GetAllNodeLSNs() map[string]uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()

	result := make(map[string]uint64, len(t.nodeLSNs))
	for k, v := range t.nodeLSNs {
		result[k] = v
	}
	return result
}

// CalculateVDL calculates the Volume Durable LSN
// VDL is the Vw-th highest LSN (guaranteeing Vw nodes have this LSN or higher)
func (t *Tracker) CalculateVDL() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()

	if len(t.nodeLSNs) < t.config.Vw {
		return 0
	}

	// Get all LSNs and sort descending
	lsns := make([]uint64, 0, len(t.nodeLSNs))
	for _, lsn := range t.nodeLSNs {
		lsns = append(lsns, lsn)
	}
	sort.Slice(lsns, func(i, j int) bool {
		return lsns[i] > lsns[j]
	})

	// Return the Vw-th highest (0-indexed, so Vw-1)
	if len(lsns) >= t.config.Vw {
		return lsns[t.config.Vw-1]
	}
	return lsns[len(lsns)-1]
}

// GetMinLSN returns the minimum LSN across all nodes
func (t *Tracker) GetMinLSN() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()

	if len(t.nodeLSNs) == 0 {
		return 0
	}

	var min uint64 = ^uint64(0)
	for _, lsn := range t.nodeLSNs {
		if lsn < min {
			min = lsn
		}
	}
	return min
}

// GetMaxLSN returns the maximum LSN across all nodes
func (t *Tracker) GetMaxLSN() uint64 {
	t.mu.RLock()
	defer t.mu.RUnlock()

	var max uint64
	for _, lsn := range t.nodeLSNs {
		if lsn > max {
			max = lsn
		}
	}
	return max
}

// WriteAcker tracks ACKs for a single write operation
type WriteAcker struct {
	mu        sync.Mutex
	config    Config
	targetLSN uint64
	acks      map[string]bool
	done      chan struct{}
}

// NewWriteAcker creates a new write ACK tracker
func NewWriteAcker(config Config, targetLSN uint64) *WriteAcker {
	return &WriteAcker{
		config:    config,
		targetLSN: targetLSN,
		acks:      make(map[string]bool),
		done:      make(chan struct{}),
	}
}

// Ack records an ACK from a node
func (a *WriteAcker) Ack(nodeID string, lsn uint64) bool {
	a.mu.Lock()
	defer a.mu.Unlock()

	if lsn >= a.targetLSN {
		a.acks[nodeID] = true
	}

	if len(a.acks) >= a.config.Vw {
		select {
		case <-a.done:
			// Already done
		default:
			close(a.done)
		}
		return true
	}
	return false
}

// Done returns a channel that is closed when quorum is reached
func (a *WriteAcker) Done() <-chan struct{} {
	return a.done
}

// AckCount returns the current number of ACKs
func (a *WriteAcker) AckCount() int {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.acks)
}

// IsComplete returns true if quorum has been reached
func (a *WriteAcker) IsComplete() bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	return len(a.acks) >= a.config.Vw
}

// GetAckedNodes returns the list of nodes that have ACKed
func (a *WriteAcker) GetAckedNodes() []string {
	a.mu.Lock()
	defer a.mu.Unlock()

	nodes := make([]string, 0, len(a.acks))
	for nodeID := range a.acks {
		nodes = append(nodes, nodeID)
	}
	return nodes
}
