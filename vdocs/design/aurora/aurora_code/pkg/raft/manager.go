package raft

import (
	"context"
	"errors"
	"sync"
	"time"
)

// RaftManager manages multiple Raft groups
type RaftManager struct {
	nodeID      string
	groups      map[uint64]*RaftGroupHandler
	transport   Transport
	storage     Storage
	config      *RaftConfig
	
	// Leader cache for routing
	leaderCache map[uint32]string // pgID -> leaderNodeID
	cacheMu     sync.RWMutex
	
	mu      sync.RWMutex
	running bool
}

// Transport defines the network transport interface for Raft
type Transport interface {
	SendVote(ctx context.Context, target string, req *VoteRequest) (*VoteResponse, error)
	SendAppend(ctx context.Context, target string, req *AppendRequest) (*AppendResponse, error)
	Start() error
	Stop() error
}

// Storage defines the persistent storage interface for Raft
type Storage interface {
	// Log operations
	GetLastIndex(groupID uint64) (uint64, error)
	GetLastTerm(groupID uint64) (uint64, error)
	GetEntry(groupID uint64, index uint64) (*LogEntry, error)
	GetEntries(groupID uint64, from, to uint64) ([]LogEntry, error)
	AppendEntries(groupID uint64, entries []LogEntry) error
	TruncateAfter(groupID uint64, index uint64) error
	
	// State operations
	GetState(groupID uint64) (*RaftGroup, error)
	SaveState(groupID uint64, state *RaftGroup) error
	
	// Snapshot operations
	SaveSnapshot(groupID uint64, index uint64, data []byte) error
	LoadSnapshot(groupID uint64) (index uint64, data []byte, err error)
}

// NewRaftManager creates a new Raft manager
func NewRaftManager(nodeID string, config *RaftConfig, transport Transport, storage Storage) *RaftManager {
	if config == nil {
		config = DefaultRaftConfig()
	}
	
	return &RaftManager{
		nodeID:      nodeID,
		groups:      make(map[uint64]*RaftGroupHandler),
		transport:   transport,
		storage:     storage,
		config:      config,
		leaderCache: make(map[uint32]string),
	}
}

// Start starts the Raft manager
func (rm *RaftManager) Start() error {
	rm.mu.Lock()
	defer rm.mu.Unlock()
	
	if rm.running {
		return nil
	}
	
	if err := rm.transport.Start(); err != nil {
		return err
	}
	
	rm.running = true
	return nil
}

// Stop stops the Raft manager
func (rm *RaftManager) Stop() error {
	rm.mu.Lock()
	defer rm.mu.Unlock()
	
	if !rm.running {
		return nil
	}
	
	// Stop all group handlers
	for _, handler := range rm.groups {
		handler.Stop()
	}
	
	if err := rm.transport.Stop(); err != nil {
		return err
	}
	
	rm.running = false
	return nil
}

// CreateGroup creates a new Raft group
func (rm *RaftManager) CreateGroup(pgID uint32, replicaID uint32, members []RaftMember) error {
	groupID := MakeGroupID(pgID, replicaID)
	
	rm.mu.Lock()
	defer rm.mu.Unlock()
	
	if _, exists := rm.groups[groupID]; exists {
		return errors.New("raft group already exists")
	}
	
	group := &RaftGroup{
		GroupID:   groupID,
		PGID:      pgID,
		ReplicaID: replicaID,
		Members:   members,
		State:     StateFollower,
		Term:      0,
	}
	
	handler := NewRaftGroupHandler(rm, group)
	rm.groups[groupID] = handler
	
	go handler.Run()
	
	return nil
}

// RemoveGroup removes a Raft group
func (rm *RaftManager) RemoveGroup(pgID uint32, replicaID uint32) error {
	groupID := MakeGroupID(pgID, replicaID)
	
	rm.mu.Lock()
	defer rm.mu.Unlock()
	
	handler, exists := rm.groups[groupID]
	if !exists {
		return errors.New("raft group not found")
	}
	
	handler.Stop()
	delete(rm.groups, groupID)
	
	return nil
}

// WriteRedo writes a Redo record through Raft
func (rm *RaftManager) WriteRedo(ctx context.Context, pgID uint32, lsn uint64, data []byte) error {
	rm.mu.RLock()
	defer rm.mu.RUnlock()
	
	groups := rm.getGroupsForPG(pgID)
	if len(groups) == 0 {
		return errors.New("no raft group found for pg")
	}
	
	entry := LogEntry{
		LSN:       lsn,
		Type:      EntryRedo,
		Data:      data,
		Timestamp: time.Now(),
	}
	
	if rm.config.ReplicationMode == ReplicationSync {
		// Dual Raft Group sync mode: both must succeed
		var wg sync.WaitGroup
		errChan := make(chan error, len(groups))
		
		for _, group := range groups {
			wg.Add(1)
			go func(g *RaftGroupHandler) {
				defer wg.Done()
				if err := g.Propose(ctx, entry); err != nil {
					errChan <- err
				}
			}(group)
		}
		
		wg.Wait()
		close(errChan)
		
		for err := range errChan {
			if err != nil {
				return err
			}
		}
		
		return nil
	}
	
	// Async mode: only first group needs to succeed
	return groups[0].Propose(ctx, entry)
}

// getGroupsForPG returns all Raft groups for a PG
func (rm *RaftManager) getGroupsForPG(pgID uint32) []*RaftGroupHandler {
	var groups []*RaftGroupHandler
	
	// Check for both replica IDs (0 and 1)
	for replicaID := uint32(0); replicaID <= 1; replicaID++ {
		groupID := MakeGroupID(pgID, replicaID)
		if handler, exists := rm.groups[groupID]; exists {
			groups = append(groups, handler)
		}
	}
	
	return groups
}

// GetLeader returns the leader for a PG
func (rm *RaftManager) GetLeader(pgID uint32) (string, error) {
	rm.cacheMu.RLock()
	if leader, ok := rm.leaderCache[pgID]; ok {
		rm.cacheMu.RUnlock()
		return leader, nil
	}
	rm.cacheMu.RUnlock()
	
	// Find leader from groups
	groups := rm.getGroupsForPG(pgID)
	if len(groups) == 0 {
		return "", errors.New("no raft group found")
	}
	
	leader := groups[0].GetLeader()
	if leader != "" {
		rm.cacheMu.Lock()
		rm.leaderCache[pgID] = leader
		rm.cacheMu.Unlock()
	}
	
	return leader, nil
}

// UpdateLeaderCache updates the leader cache
func (rm *RaftManager) UpdateLeaderCache(pgID uint32, leaderID string) {
	rm.cacheMu.Lock()
	defer rm.cacheMu.Unlock()
	rm.leaderCache[pgID] = leaderID
}

// GetGroup returns a specific Raft group
func (rm *RaftManager) GetGroup(groupID uint64) (*RaftGroupHandler, bool) {
	rm.mu.RLock()
	defer rm.mu.RUnlock()
	handler, exists := rm.groups[groupID]
	return handler, exists
}

// GetNodeID returns the node ID
func (rm *RaftManager) GetNodeID() string {
	return rm.nodeID
}

// GetConfig returns the Raft configuration
func (rm *RaftManager) GetConfig() *RaftConfig {
	return rm.config
}
