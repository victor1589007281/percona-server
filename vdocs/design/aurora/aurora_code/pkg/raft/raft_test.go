package raft

import (
	"context"
	"testing"
	"time"
)

func TestMakeGroupID(t *testing.T) {
	pgID := uint32(100)
	replicaID := uint32(1)
	
	groupID := MakeGroupID(pgID, replicaID)
	
	gotPG, gotReplica := ParseGroupID(groupID)
	
	if gotPG != pgID {
		t.Errorf("ParseGroupID pgID = %d, want %d", gotPG, pgID)
	}
	if gotReplica != replicaID {
		t.Errorf("ParseGroupID replicaID = %d, want %d", gotReplica, replicaID)
	}
}

func TestRaftGroupState(t *testing.T) {
	group := &RaftGroup{
		GroupID:   MakeGroupID(1, 0),
		PGID:      1,
		ReplicaID: 0,
		State:     StateFollower,
		Term:      1,
	}
	
	if group.State != StateFollower {
		t.Errorf("Initial state should be Follower")
	}
	
	if group.State.String() != "follower" {
		t.Errorf("State.String() = %s, want follower", group.State.String())
	}
}

func TestLogEntry(t *testing.T) {
	entry := LogEntry{
		Index:     1,
		Term:      1,
		LSN:       1000,
		Type:      EntryRedo,
		Data:      []byte("test data"),
		Timestamp: time.Now(),
	}
	
	if entry.Type != EntryRedo {
		t.Errorf("EntryType = %d, want %d", entry.Type, EntryRedo)
	}
}

// MockTransport for testing
type MockTransport struct {
	voteResponses   map[string]*VoteResponse
	appendResponses map[string]*AppendResponse
}

func NewMockTransport() *MockTransport {
	return &MockTransport{
		voteResponses:   make(map[string]*VoteResponse),
		appendResponses: make(map[string]*AppendResponse),
	}
}

func (m *MockTransport) SendVote(ctx context.Context, target string, req *VoteRequest) (*VoteResponse, error) {
	if resp, ok := m.voteResponses[target]; ok {
		return resp, nil
	}
	return &VoteResponse{Term: req.Term, VoteGranted: false}, nil
}

func (m *MockTransport) SendAppend(ctx context.Context, target string, req *AppendRequest) (*AppendResponse, error) {
	if resp, ok := m.appendResponses[target]; ok {
		return resp, nil
	}
	return &AppendResponse{Term: req.Term, Success: true, MatchIndex: req.PrevLogIndex + uint64(len(req.Entries))}, nil
}

func (m *MockTransport) Start() error { return nil }
func (m *MockTransport) Stop() error  { return nil }

// MockStorage for testing
type MockStorage struct {
	logs   map[uint64][]LogEntry
	states map[uint64]*RaftGroup
}

func NewMockStorage() *MockStorage {
	return &MockStorage{
		logs:   make(map[uint64][]LogEntry),
		states: make(map[uint64]*RaftGroup),
	}
}

func (m *MockStorage) GetLastIndex(groupID uint64) (uint64, error) {
	if logs, ok := m.logs[groupID]; ok && len(logs) > 0 {
		return logs[len(logs)-1].Index, nil
	}
	return 0, nil
}

func (m *MockStorage) GetLastTerm(groupID uint64) (uint64, error) {
	if logs, ok := m.logs[groupID]; ok && len(logs) > 0 {
		return logs[len(logs)-1].Term, nil
	}
	return 0, nil
}

func (m *MockStorage) GetEntry(groupID uint64, index uint64) (*LogEntry, error) {
	if logs, ok := m.logs[groupID]; ok {
		for _, entry := range logs {
			if entry.Index == index {
				return &entry, nil
			}
		}
	}
	return nil, nil
}

func (m *MockStorage) GetEntries(groupID uint64, from, to uint64) ([]LogEntry, error) {
	var result []LogEntry
	if logs, ok := m.logs[groupID]; ok {
		for _, entry := range logs {
			if entry.Index >= from && entry.Index <= to {
				result = append(result, entry)
			}
		}
	}
	return result, nil
}

func (m *MockStorage) AppendEntries(groupID uint64, entries []LogEntry) error {
	m.logs[groupID] = append(m.logs[groupID], entries...)
	return nil
}

func (m *MockStorage) TruncateAfter(groupID uint64, index uint64) error {
	if logs, ok := m.logs[groupID]; ok {
		var newLogs []LogEntry
		for _, entry := range logs {
			if entry.Index <= index {
				newLogs = append(newLogs, entry)
			}
		}
		m.logs[groupID] = newLogs
	}
	return nil
}

func (m *MockStorage) GetState(groupID uint64) (*RaftGroup, error) {
	return m.states[groupID], nil
}

func (m *MockStorage) SaveState(groupID uint64, state *RaftGroup) error {
	m.states[groupID] = state
	return nil
}

func (m *MockStorage) SaveSnapshot(groupID uint64, index uint64, data []byte) error {
	return nil
}

func (m *MockStorage) LoadSnapshot(groupID uint64) (uint64, []byte, error) {
	return 0, nil, nil
}

func TestRaftManager(t *testing.T) {
	transport := NewMockTransport()
	storage := NewMockStorage()
	config := DefaultRaftConfig()
	
	manager := NewRaftManager("node-1", config, transport, storage)
	
	if err := manager.Start(); err != nil {
		t.Fatalf("Failed to start manager: %v", err)
	}
	defer manager.Stop()
	
	// Create a group
	members := []RaftMember{
		{NodeID: "node-1", Address: "localhost:9001", IsVoter: true},
		{NodeID: "node-2", Address: "localhost:9002", IsVoter: true},
		{NodeID: "node-3", Address: "localhost:9003", IsVoter: true},
	}
	
	if err := manager.CreateGroup(1, 0, members); err != nil {
		t.Fatalf("Failed to create group: %v", err)
	}
	
	// Verify group exists
	if _, exists := manager.GetGroup(MakeGroupID(1, 0)); !exists {
		t.Error("Group should exist")
	}
	
	// Remove group
	if err := manager.RemoveGroup(1, 0); err != nil {
		t.Fatalf("Failed to remove group: %v", err)
	}
	
	if _, exists := manager.GetGroup(MakeGroupID(1, 0)); exists {
		t.Error("Group should not exist after removal")
	}
}
