// Package raft implements Multi-Raft protocol for Aurora storage layer
package raft

import (
	"sync"
	"time"
)

// RaftState represents the state of a Raft node
type RaftState int

const (
	StateFollower RaftState = iota
	StateCandidate
	StateLeader
)

func (s RaftState) String() string {
	switch s {
	case StateFollower:
		return "follower"
	case StateCandidate:
		return "candidate"
	case StateLeader:
		return "leader"
	default:
		return "unknown"
	}
}

// EntryType represents the type of Raft log entry
type EntryType int

const (
	EntryRedo EntryType = iota
	EntryConfigChange
	EntryNoop
)

// RaftMember represents a member in a Raft group
type RaftMember struct {
	NodeID     string `json:"node_id"`
	Address    string `json:"address"`
	AZ         string `json:"az"`
	MatchIndex uint64 `json:"match_index"`
	NextIndex  uint64 `json:"next_index"`
	IsVoter    bool   `json:"is_voter"`
}

// RaftGroup represents a Raft group for a Protection Group
type RaftGroup struct {
	GroupID     uint64       `json:"group_id"`
	PGID        uint32       `json:"pg_id"`
	ReplicaID   uint32       `json:"replica_id"` // 0 or 1 for dual Raft groups
	Term        uint64       `json:"term"`
	VotedFor    string       `json:"voted_for"`
	CommitIndex uint64       `json:"commit_index"`
	LastApplied uint64       `json:"last_applied"`
	LeaderID    string       `json:"leader_id"`
	Members     []RaftMember `json:"members"`
	State       RaftState    `json:"state"`
	
	mu sync.RWMutex
}

// LogEntry represents a Raft log entry
type LogEntry struct {
	Index     uint64    `json:"index"`
	Term      uint64    `json:"term"`
	LSN       uint64    `json:"lsn"`
	Type      EntryType `json:"type"`
	Data      []byte    `json:"data"`
	Timestamp time.Time `json:"timestamp"`
}

// ReplicationMode defines how replication works
type ReplicationMode int

const (
	ReplicationSync  ReplicationMode = iota // Dual Raft Groups, both must succeed
	ReplicationAsync                        // Single Raft Group + async replicas
)

// PGRaftConfig holds configuration for a PG's Raft groups
type PGRaftConfig struct {
	PGID            uint32
	GroupCount      int // 1 or 2
	Groups          []*RaftGroup
	ReplicationMode ReplicationMode
}

// RaftConfig holds Raft configuration
type RaftConfig struct {
	HeartbeatInterval time.Duration
	ElectionTimeout   time.Duration
	SnapshotInterval  time.Duration
	MaxLogEntries     int
	BatchSize         int
	ReplicationMode   ReplicationMode
}

// DefaultRaftConfig returns default Raft configuration
func DefaultRaftConfig() *RaftConfig {
	return &RaftConfig{
		HeartbeatInterval: 100 * time.Millisecond,
		ElectionTimeout:   1000 * time.Millisecond,
		SnapshotInterval:  10 * time.Minute,
		MaxLogEntries:     100000,
		BatchSize:         100,
		ReplicationMode:   ReplicationSync,
	}
}

// VoteRequest represents a RequestVote RPC
type VoteRequest struct {
	Term         uint64
	CandidateID  string
	LastLogIndex uint64
	LastLogTerm  uint64
	GroupID      uint64
}

// VoteResponse represents response to RequestVote
type VoteResponse struct {
	Term        uint64
	VoteGranted bool
}

// AppendRequest represents an AppendEntries RPC
type AppendRequest struct {
	Term         uint64
	LeaderID     string
	PrevLogIndex uint64
	PrevLogTerm  uint64
	Entries      []LogEntry
	LeaderCommit uint64
	GroupID      uint64
}

// AppendResponse represents response to AppendEntries
type AppendResponse struct {
	Term          uint64
	Success       bool
	MatchIndex    uint64
	ConflictIndex uint64
	ConflictTerm  uint64
}

// MakeGroupID creates a unique group ID from PG and replica ID
func MakeGroupID(pgID uint32, replicaID uint32) uint64 {
	return uint64(pgID)<<32 | uint64(replicaID)
}

// ParseGroupID extracts PG and replica ID from group ID
func ParseGroupID(groupID uint64) (pgID uint32, replicaID uint32) {
	pgID = uint32(groupID >> 32)
	replicaID = uint32(groupID & 0xFFFFFFFF)
	return
}
