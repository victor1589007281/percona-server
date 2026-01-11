// Package metadata implements the metadata service with Raft
package metadata

import (
	"encoding/json"
	"fmt"
	"io"
	"sort"
	"sync"

	"github.com/hashicorp/raft"
)

// CommandType defines the type of FSM command
type CommandType uint8

const (
	CmdCreateVolume CommandType = iota + 1
	CmdDeleteVolume
	CmdUpdateVDL
	CmdUpdateNodeLSN
	CmdRegisterInstance
	CmdUnregisterInstance
)

// Command represents a command to be applied to the FSM
type Command struct {
	Type CommandType     `json:"type"`
	Data json.RawMessage `json:"data"`
}

// VolumeState represents the state of a volume
type VolumeState struct {
	VolumeID   string   `json:"volume_id"`
	ClusterID  string   `json:"cluster_id"`
	SizeBytes  int64    `json:"size_bytes"`
	CurrentVDL int64    `json:"current_vdl"`
	PGCount    int      `json:"pg_count"`
	State      string   `json:"state"`
	CreatedAt  int64    `json:"created_at"`
	UpdatedAt  int64    `json:"updated_at"`
}

// InstanceState represents the state of a compute instance
type InstanceState struct {
	InstanceID   string `json:"instance_id"`
	ClusterID    string `json:"cluster_id"`
	VolumeID     string `json:"volume_id"`
	Role         string `json:"role"` // "writer" or "reader"
	Endpoint     string `json:"endpoint"`
	State        string `json:"state"`
	CurrentLSN   int64  `json:"current_lsn"`
	RegisteredAt int64  `json:"registered_at"`
}

// FSM implements the Raft FSM interface
type FSM struct {
	mu sync.RWMutex

	// State storage
	volumes   map[string]*VolumeState
	nodeLSNs  map[string]map[string]int64 // volume_id -> node_id -> lsn
	instances map[string]*InstanceState

	// VDL configuration
	vwQuorum int // Write quorum (default 4)
}

// NewFSM creates a new FSM
func NewFSM() *FSM {
	return &FSM{
		volumes:   make(map[string]*VolumeState),
		nodeLSNs:  make(map[string]map[string]int64),
		instances: make(map[string]*InstanceState),
		vwQuorum:  4,
	}
}

// Apply applies a Raft log entry to the FSM
func (f *FSM) Apply(log *raft.Log) interface{} {
	var cmd Command
	if err := json.Unmarshal(log.Data, &cmd); err != nil {
		return fmt.Errorf("unmarshal command: %w", err)
	}

	switch cmd.Type {
	case CmdCreateVolume:
		return f.applyCreateVolume(cmd.Data)
	case CmdDeleteVolume:
		return f.applyDeleteVolume(cmd.Data)
	case CmdUpdateVDL:
		return f.applyUpdateVDL(cmd.Data)
	case CmdUpdateNodeLSN:
		return f.applyUpdateNodeLSN(cmd.Data)
	case CmdRegisterInstance:
		return f.applyRegisterInstance(cmd.Data)
	case CmdUnregisterInstance:
		return f.applyUnregisterInstance(cmd.Data)
	default:
		return fmt.Errorf("unknown command type: %d", cmd.Type)
	}
}

// Snapshot returns a snapshot of the FSM
func (f *FSM) Snapshot() (raft.FSMSnapshot, error) {
	f.mu.RLock()
	defer f.mu.RUnlock()

	// Create deep copy of state
	volumes := make(map[string]*VolumeState)
	for k, v := range f.volumes {
		copied := *v
		volumes[k] = &copied
	}

	nodeLSNs := make(map[string]map[string]int64)
	for k, v := range f.nodeLSNs {
		nodeLSNs[k] = make(map[string]int64)
		for nk, nv := range v {
			nodeLSNs[k][nk] = nv
		}
	}

	instances := make(map[string]*InstanceState)
	for k, v := range f.instances {
		copied := *v
		instances[k] = &copied
	}

	return &fsmSnapshot{
		volumes:   volumes,
		nodeLSNs:  nodeLSNs,
		instances: instances,
	}, nil
}

// Restore restores the FSM from a snapshot
func (f *FSM) Restore(rc io.ReadCloser) error {
	defer rc.Close()

	var snapshot fsmSnapshot
	if err := json.NewDecoder(rc).Decode(&snapshot); err != nil {
		return fmt.Errorf("decode snapshot: %w", err)
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	f.volumes = snapshot.volumes
	f.nodeLSNs = snapshot.nodeLSNs
	f.instances = snapshot.instances

	return nil
}

// CreateVolumeData represents create volume command data
type CreateVolumeData struct {
	VolumeID  string `json:"volume_id"`
	ClusterID string `json:"cluster_id"`
	SizeBytes int64  `json:"size_bytes"`
	PGCount   int    `json:"pg_count"`
	CreatedAt int64  `json:"created_at"`
}

func (f *FSM) applyCreateVolume(data json.RawMessage) interface{} {
	var req CreateVolumeData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	f.volumes[req.VolumeID] = &VolumeState{
		VolumeID:   req.VolumeID,
		ClusterID:  req.ClusterID,
		SizeBytes:  req.SizeBytes,
		PGCount:    req.PGCount,
		State:      "available",
		CreatedAt:  req.CreatedAt,
		UpdatedAt:  req.CreatedAt,
	}

	f.nodeLSNs[req.VolumeID] = make(map[string]int64)

	return nil
}

// DeleteVolumeData represents delete volume command data
type DeleteVolumeData struct {
	VolumeID string `json:"volume_id"`
}

func (f *FSM) applyDeleteVolume(data json.RawMessage) interface{} {
	var req DeleteVolumeData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	delete(f.volumes, req.VolumeID)
	delete(f.nodeLSNs, req.VolumeID)

	return nil
}

// UpdateVDLData represents update VDL command data
type UpdateVDLData struct {
	VolumeID   string `json:"volume_id"`
	NewVDL     int64  `json:"new_vdl"`
	InstanceID string `json:"instance_id"`
	UpdatedAt  int64  `json:"updated_at"`
}

func (f *FSM) applyUpdateVDL(data json.RawMessage) interface{} {
	var req UpdateVDLData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	if vol, ok := f.volumes[req.VolumeID]; ok {
		if req.NewVDL > vol.CurrentVDL {
			vol.CurrentVDL = req.NewVDL
			vol.UpdatedAt = req.UpdatedAt
		}
	}

	return nil
}

// UpdateNodeLSNData represents update node LSN command data
type UpdateNodeLSNData struct {
	VolumeID   string `json:"volume_id"`
	NodeID     string `json:"node_id"`
	CurrentLSN int64  `json:"current_lsn"`
}

func (f *FSM) applyUpdateNodeLSN(data json.RawMessage) interface{} {
	var req UpdateNodeLSNData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	if f.nodeLSNs[req.VolumeID] == nil {
		f.nodeLSNs[req.VolumeID] = make(map[string]int64)
	}

	// Only update if new LSN is higher
	if current, ok := f.nodeLSNs[req.VolumeID][req.NodeID]; !ok || req.CurrentLSN > current {
		f.nodeLSNs[req.VolumeID][req.NodeID] = req.CurrentLSN
	}

	return nil
}

// RegisterInstanceData represents register instance command data
type RegisterInstanceData struct {
	InstanceID   string `json:"instance_id"`
	ClusterID    string `json:"cluster_id"`
	VolumeID     string `json:"volume_id"`
	Role         string `json:"role"`
	Endpoint     string `json:"endpoint"`
	RegisteredAt int64  `json:"registered_at"`
}

func (f *FSM) applyRegisterInstance(data json.RawMessage) interface{} {
	var req RegisterInstanceData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	f.instances[req.InstanceID] = &InstanceState{
		InstanceID:   req.InstanceID,
		ClusterID:    req.ClusterID,
		VolumeID:     req.VolumeID,
		Role:         req.Role,
		Endpoint:     req.Endpoint,
		State:        "running",
		RegisteredAt: req.RegisteredAt,
	}

	return nil
}

// UnregisterInstanceData represents unregister instance command data
type UnregisterInstanceData struct {
	InstanceID string `json:"instance_id"`
}

func (f *FSM) applyUnregisterInstance(data json.RawMessage) interface{} {
	var req UnregisterInstanceData
	if err := json.Unmarshal(data, &req); err != nil {
		return err
	}

	f.mu.Lock()
	defer f.mu.Unlock()

	delete(f.instances, req.InstanceID)

	return nil
}

// GetVolume returns a volume by ID
func (f *FSM) GetVolume(volumeID string) (*VolumeState, bool) {
	f.mu.RLock()
	defer f.mu.RUnlock()
	v, ok := f.volumes[volumeID]
	return v, ok
}

// GetVDL returns the current VDL for a volume
func (f *FSM) GetVDL(volumeID string) (int64, bool) {
	f.mu.RLock()
	defer f.mu.RUnlock()
	if v, ok := f.volumes[volumeID]; ok {
		return v.CurrentVDL, true
	}
	return 0, false
}

// CalculateVDL calculates the VDL from node LSNs
func (f *FSM) CalculateVDL(volumeID string) int64 {
	f.mu.RLock()
	defer f.mu.RUnlock()

	nodeLSNs, ok := f.nodeLSNs[volumeID]
	if !ok || len(nodeLSNs) < f.vwQuorum {
		return 0
	}

	// Sort LSNs descending
	lsns := make([]int64, 0, len(nodeLSNs))
	for _, lsn := range nodeLSNs {
		lsns = append(lsns, lsn)
	}
	sort.Slice(lsns, func(i, j int) bool {
		return lsns[i] > lsns[j]
	})

	// Return the Vw-th highest (0-indexed)
	if len(lsns) >= f.vwQuorum {
		return lsns[f.vwQuorum-1]
	}
	return lsns[len(lsns)-1]
}

// GetNodeLSNs returns all node LSNs for a volume
func (f *FSM) GetNodeLSNs(volumeID string) map[string]int64 {
	f.mu.RLock()
	defer f.mu.RUnlock()

	if nodeLSNs, ok := f.nodeLSNs[volumeID]; ok {
		result := make(map[string]int64)
		for k, v := range nodeLSNs {
			result[k] = v
		}
		return result
	}
	return nil
}

// GetInstance returns an instance by ID
func (f *FSM) GetInstance(instanceID string) (*InstanceState, bool) {
	f.mu.RLock()
	defer f.mu.RUnlock()
	i, ok := f.instances[instanceID]
	return i, ok
}

// ListInstances returns all instances for a cluster
func (f *FSM) ListInstances(clusterID string) []*InstanceState {
	f.mu.RLock()
	defer f.mu.RUnlock()

	var result []*InstanceState
	for _, inst := range f.instances {
		if inst.ClusterID == clusterID {
			copied := *inst
			result = append(result, &copied)
		}
	}
	return result
}

// fsmSnapshot represents an FSM snapshot
type fsmSnapshot struct {
	volumes   map[string]*VolumeState
	nodeLSNs  map[string]map[string]int64
	instances map[string]*InstanceState
}

// Persist persists the snapshot
func (s *fsmSnapshot) Persist(sink raft.SnapshotSink) error {
	if err := json.NewEncoder(sink).Encode(s); err != nil {
		sink.Cancel()
		return fmt.Errorf("encode snapshot: %w", err)
	}
	return sink.Close()
}

// Release releases the snapshot
func (s *fsmSnapshot) Release() {}
