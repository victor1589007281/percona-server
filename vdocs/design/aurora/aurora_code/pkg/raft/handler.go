package raft

import (
	"context"
	"math/rand"
	"sync"
	"time"
)

// RaftGroupHandler handles a single Raft group
type RaftGroupHandler struct {
	manager *RaftManager
	group   *RaftGroup
	
	// Log state
	log        []LogEntry
	logMu      sync.RWMutex
	
	// Channels
	proposeCh  chan proposeRequest
	voteCh     chan voteRequest
	appendCh   chan appendRequest
	stopCh     chan struct{}
	
	// Timers
	electionTimer  *time.Timer
	heartbeatTimer *time.Timer
	
	// Pending proposals
	pending    map[uint64]chan error
	pendingMu  sync.Mutex
	
	running bool
	mu      sync.Mutex
}

type proposeRequest struct {
	entry   LogEntry
	resultCh chan error
}

type voteRequest struct {
	req      *VoteRequest
	resultCh chan *VoteResponse
}

type appendRequest struct {
	req      *AppendRequest
	resultCh chan *AppendResponse
}

// NewRaftGroupHandler creates a new Raft group handler
func NewRaftGroupHandler(manager *RaftManager, group *RaftGroup) *RaftGroupHandler {
	return &RaftGroupHandler{
		manager:   manager,
		group:     group,
		proposeCh: make(chan proposeRequest, 1000),
		voteCh:    make(chan voteRequest, 10),
		appendCh:  make(chan appendRequest, 100),
		stopCh:    make(chan struct{}),
		pending:   make(map[uint64]chan error),
	}
}

// Run starts the Raft group handler main loop
func (h *RaftGroupHandler) Run() {
	h.mu.Lock()
	if h.running {
		h.mu.Unlock()
		return
	}
	h.running = true
	h.mu.Unlock()
	
	// Initialize timers
	h.resetElectionTimer()
	
	for {
		select {
		case <-h.stopCh:
			return
			
		case req := <-h.proposeCh:
			h.handlePropose(req)
			
		case req := <-h.voteCh:
			h.handleVote(req)
			
		case req := <-h.appendCh:
			h.handleAppend(req)
			
		case <-h.electionTimer.C:
			h.handleElectionTimeout()
			
		case <-h.getHeartbeatChan():
			h.handleHeartbeatTimeout()
		}
	}
}

// Stop stops the Raft group handler
func (h *RaftGroupHandler) Stop() {
	h.mu.Lock()
	defer h.mu.Unlock()
	
	if !h.running {
		return
	}
	
	close(h.stopCh)
	h.running = false
	
	if h.electionTimer != nil {
		h.electionTimer.Stop()
	}
	if h.heartbeatTimer != nil {
		h.heartbeatTimer.Stop()
	}
}

// Propose proposes a new log entry
func (h *RaftGroupHandler) Propose(ctx context.Context, entry LogEntry) error {
	resultCh := make(chan error, 1)
	
	select {
	case h.proposeCh <- proposeRequest{entry: entry, resultCh: resultCh}:
	case <-ctx.Done():
		return ctx.Err()
	}
	
	select {
	case err := <-resultCh:
		return err
	case <-ctx.Done():
		return ctx.Err()
	}
}

// handlePropose handles a propose request
func (h *RaftGroupHandler) handlePropose(req proposeRequest) {
	h.group.mu.Lock()
	defer h.group.mu.Unlock()
	
	if h.group.State != StateLeader {
		req.resultCh <- ErrNotLeader
		return
	}
	
	// Append to local log
	h.logMu.Lock()
	index := uint64(len(h.log)) + 1
	req.entry.Index = index
	req.entry.Term = h.group.Term
	h.log = append(h.log, req.entry)
	h.logMu.Unlock()
	
	// Track pending proposal
	h.pendingMu.Lock()
	h.pending[index] = req.resultCh
	h.pendingMu.Unlock()
	
	// Replicate to followers
	go h.replicateToFollowers(index)
}

// replicateToFollowers replicates log entries to followers
func (h *RaftGroupHandler) replicateToFollowers(upToIndex uint64) {
	h.group.mu.RLock()
	members := h.group.Members
	term := h.group.Term
	h.group.mu.RUnlock()
	
	var wg sync.WaitGroup
	successCount := 1 // Count self
	var countMu sync.Mutex
	
	for _, member := range members {
		if member.NodeID == h.manager.GetNodeID() {
			continue
		}
		
		wg.Add(1)
		go func(m RaftMember) {
			defer wg.Done()
			
			h.logMu.RLock()
			entries := h.log[m.NextIndex-1:]
			prevLogIndex := m.NextIndex - 1
			var prevLogTerm uint64
			if prevLogIndex > 0 && prevLogIndex <= uint64(len(h.log)) {
				prevLogTerm = h.log[prevLogIndex-1].Term
			}
			h.logMu.RUnlock()
			
			h.group.mu.RLock()
			req := &AppendRequest{
				Term:         term,
				LeaderID:     h.manager.GetNodeID(),
				PrevLogIndex: prevLogIndex,
				PrevLogTerm:  prevLogTerm,
				Entries:      entries,
				LeaderCommit: h.group.CommitIndex,
				GroupID:      h.group.GroupID,
			}
			h.group.mu.RUnlock()
			
			ctx, cancel := context.WithTimeout(context.Background(), 
				h.manager.GetConfig().HeartbeatInterval*2)
			defer cancel()
			
			resp, err := h.manager.transport.SendAppend(ctx, m.Address, req)
			if err != nil {
				return
			}
			
			if resp.Success {
				countMu.Lock()
				successCount++
				countMu.Unlock()
				
				h.group.mu.Lock()
				for i := range h.group.Members {
					if h.group.Members[i].NodeID == m.NodeID {
						h.group.Members[i].MatchIndex = resp.MatchIndex
						h.group.Members[i].NextIndex = resp.MatchIndex + 1
						break
					}
				}
				h.group.mu.Unlock()
			}
		}(member)
	}
	
	wg.Wait()
	
	// Check if majority achieved
	majority := len(members)/2 + 1
	if successCount >= majority {
		h.advanceCommitIndex(upToIndex)
	}
}

// advanceCommitIndex advances the commit index
func (h *RaftGroupHandler) advanceCommitIndex(newIndex uint64) {
	h.group.mu.Lock()
	if newIndex > h.group.CommitIndex {
		h.group.CommitIndex = newIndex
	}
	h.group.mu.Unlock()
	
	// Notify pending proposals
	h.pendingMu.Lock()
	for idx, ch := range h.pending {
		if idx <= newIndex {
			ch <- nil
			delete(h.pending, idx)
		}
	}
	h.pendingMu.Unlock()
}

// handleVote handles a vote request
func (h *RaftGroupHandler) handleVote(req voteRequest) {
	h.group.mu.Lock()
	defer h.group.mu.Unlock()
	
	resp := &VoteResponse{
		Term:        h.group.Term,
		VoteGranted: false,
	}
	
	// If request term is older, reject
	if req.req.Term < h.group.Term {
		req.resultCh <- resp
		return
	}
	
	// If request term is newer, become follower
	if req.req.Term > h.group.Term {
		h.group.Term = req.req.Term
		h.group.State = StateFollower
		h.group.VotedFor = ""
	}
	
	// Check if we can vote for this candidate
	if h.group.VotedFor == "" || h.group.VotedFor == req.req.CandidateID {
		h.logMu.RLock()
		var lastLogIndex, lastLogTerm uint64
		if len(h.log) > 0 {
			lastLogIndex = uint64(len(h.log))
			lastLogTerm = h.log[len(h.log)-1].Term
		}
		h.logMu.RUnlock()
		
		// Check if candidate's log is at least as up-to-date
		if req.req.LastLogTerm > lastLogTerm ||
			(req.req.LastLogTerm == lastLogTerm && req.req.LastLogIndex >= lastLogIndex) {
			h.group.VotedFor = req.req.CandidateID
			resp.VoteGranted = true
			h.resetElectionTimer()
		}
	}
	
	req.resultCh <- resp
}

// handleAppend handles an append request
func (h *RaftGroupHandler) handleAppend(req appendRequest) {
	h.group.mu.Lock()
	defer h.group.mu.Unlock()
	
	resp := &AppendResponse{
		Term:    h.group.Term,
		Success: false,
	}
	
	// If request term is older, reject
	if req.req.Term < h.group.Term {
		req.resultCh <- resp
		return
	}
	
	// Update term and become follower
	if req.req.Term > h.group.Term {
		h.group.Term = req.req.Term
		h.group.VotedFor = ""
	}
	h.group.State = StateFollower
	h.group.LeaderID = req.req.LeaderID
	h.resetElectionTimer()
	
	// Check log consistency
	h.logMu.Lock()
	if req.req.PrevLogIndex > 0 {
		if req.req.PrevLogIndex > uint64(len(h.log)) {
			resp.ConflictIndex = uint64(len(h.log)) + 1
			h.logMu.Unlock()
			req.resultCh <- resp
			return
		}
		if h.log[req.req.PrevLogIndex-1].Term != req.req.PrevLogTerm {
			resp.ConflictTerm = h.log[req.req.PrevLogIndex-1].Term
			resp.ConflictIndex = req.req.PrevLogIndex
			h.logMu.Unlock()
			req.resultCh <- resp
			return
		}
	}
	
	// Append new entries
	for i, entry := range req.req.Entries {
		idx := req.req.PrevLogIndex + uint64(i) + 1
		if idx <= uint64(len(h.log)) {
			if h.log[idx-1].Term != entry.Term {
				h.log = h.log[:idx-1]
				h.log = append(h.log, req.req.Entries[i:]...)
				break
			}
		} else {
			h.log = append(h.log, req.req.Entries[i:]...)
			break
		}
	}
	h.logMu.Unlock()
	
	// Update commit index
	if req.req.LeaderCommit > h.group.CommitIndex {
		h.logMu.RLock()
		lastIndex := uint64(len(h.log))
		h.logMu.RUnlock()
		
		if req.req.LeaderCommit < lastIndex {
			h.group.CommitIndex = req.req.LeaderCommit
		} else {
			h.group.CommitIndex = lastIndex
		}
	}
	
	resp.Success = true
	h.logMu.RLock()
	resp.MatchIndex = uint64(len(h.log))
	h.logMu.RUnlock()
	
	req.resultCh <- resp
	
	// Update leader cache
	h.manager.UpdateLeaderCache(h.group.PGID, req.req.LeaderID)
}

// handleElectionTimeout handles election timeout
func (h *RaftGroupHandler) handleElectionTimeout() {
	h.group.mu.Lock()
	
	if h.group.State == StateLeader {
		h.group.mu.Unlock()
		return
	}
	
	// Become candidate
	h.group.State = StateCandidate
	h.group.Term++
	h.group.VotedFor = h.manager.GetNodeID()
	currentTerm := h.group.Term
	members := h.group.Members
	h.group.mu.Unlock()
	
	h.resetElectionTimer()
	
	// Start election
	h.logMu.RLock()
	var lastLogIndex, lastLogTerm uint64
	if len(h.log) > 0 {
		lastLogIndex = uint64(len(h.log))
		lastLogTerm = h.log[len(h.log)-1].Term
	}
	h.logMu.RUnlock()
	
	votes := 1 // Vote for self
	var votesMu sync.Mutex
	var wg sync.WaitGroup
	
	for _, member := range members {
		if member.NodeID == h.manager.GetNodeID() {
			continue
		}
		
		wg.Add(1)
		go func(m RaftMember) {
			defer wg.Done()
			
			req := &VoteRequest{
				Term:         currentTerm,
				CandidateID:  h.manager.GetNodeID(),
				LastLogIndex: lastLogIndex,
				LastLogTerm:  lastLogTerm,
				GroupID:      h.group.GroupID,
			}
			
			ctx, cancel := context.WithTimeout(context.Background(),
				h.manager.GetConfig().ElectionTimeout/2)
			defer cancel()
			
			resp, err := h.manager.transport.SendVote(ctx, m.Address, req)
			if err != nil {
				return
			}
			
			if resp.VoteGranted {
				votesMu.Lock()
				votes++
				votesMu.Unlock()
			}
		}(member)
	}
	
	wg.Wait()
	
	// Check if won election
	majority := len(members)/2 + 1
	h.group.mu.Lock()
	if votes >= majority && h.group.Term == currentTerm && h.group.State == StateCandidate {
		h.group.State = StateLeader
		h.group.LeaderID = h.manager.GetNodeID()
		
		// Initialize nextIndex for all followers
		h.logMu.RLock()
		lastIndex := uint64(len(h.log))
		h.logMu.RUnlock()
		
		for i := range h.group.Members {
			h.group.Members[i].NextIndex = lastIndex + 1
			h.group.Members[i].MatchIndex = 0
		}
		
		h.startHeartbeat()
	}
	h.group.mu.Unlock()
}

// handleHeartbeatTimeout handles heartbeat timeout (leader only)
func (h *RaftGroupHandler) handleHeartbeatTimeout() {
	h.group.mu.RLock()
	if h.group.State != StateLeader {
		h.group.mu.RUnlock()
		return
	}
	h.group.mu.RUnlock()
	
	// Send empty AppendEntries to all followers
	h.logMu.RLock()
	lastIndex := uint64(len(h.log))
	h.logMu.RUnlock()
	
	h.replicateToFollowers(lastIndex)
	h.resetHeartbeatTimer()
}

// resetElectionTimer resets the election timer
func (h *RaftGroupHandler) resetElectionTimer() {
	timeout := h.manager.GetConfig().ElectionTimeout +
		time.Duration(rand.Int63n(int64(h.manager.GetConfig().ElectionTimeout)))
	
	if h.electionTimer == nil {
		h.electionTimer = time.NewTimer(timeout)
	} else {
		h.electionTimer.Reset(timeout)
	}
}

// startHeartbeat starts the heartbeat timer
func (h *RaftGroupHandler) startHeartbeat() {
	h.heartbeatTimer = time.NewTimer(h.manager.GetConfig().HeartbeatInterval)
}

// resetHeartbeatTimer resets the heartbeat timer
func (h *RaftGroupHandler) resetHeartbeatTimer() {
	if h.heartbeatTimer != nil {
		h.heartbeatTimer.Reset(h.manager.GetConfig().HeartbeatInterval)
	}
}

// getHeartbeatChan returns the heartbeat channel
func (h *RaftGroupHandler) getHeartbeatChan() <-chan time.Time {
	if h.heartbeatTimer == nil {
		return nil
	}
	return h.heartbeatTimer.C
}

// GetLeader returns the current leader ID
func (h *RaftGroupHandler) GetLeader() string {
	h.group.mu.RLock()
	defer h.group.mu.RUnlock()
	return h.group.LeaderID
}

// GetState returns the current state
func (h *RaftGroupHandler) GetState() RaftState {
	h.group.mu.RLock()
	defer h.group.mu.RUnlock()
	return h.group.State
}

// Errors
var (
	ErrNotLeader = NewRaftError("not leader")
)

// RaftError is a Raft-specific error
type RaftError struct {
	msg string
}

func NewRaftError(msg string) *RaftError {
	return &RaftError{msg: msg}
}

func (e *RaftError) Error() string {
	return e.msg
}
