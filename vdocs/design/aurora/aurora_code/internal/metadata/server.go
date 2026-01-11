package metadata

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"time"

	"github.com/hashicorp/raft"
	raftboltdb "github.com/hashicorp/raft-boltdb/v2"
	"google.golang.org/grpc"
)

// Config holds metadata service configuration
type Config struct {
	NodeID      string
	RaftAddr    string
	GRPCPort    int
	DataDir     string
	Peers       []PeerConfig
}

// PeerConfig represents a Raft peer
type PeerConfig struct {
	ID      string
	Address string
}

// Server represents the metadata service server
type Server struct {
	config     Config
	fsm        *FSM
	raft       *raft.Raft
	grpcServer *grpc.Server
}

// NewServer creates a new metadata server
func NewServer(config Config) (*Server, error) {
	s := &Server{
		config: config,
		fsm:    NewFSM(),
	}

	if err := s.setupRaft(); err != nil {
		return nil, fmt.Errorf("setup raft: %w", err)
	}

	return s, nil
}

// setupRaft initializes the Raft consensus
func (s *Server) setupRaft() error {
	config := raft.DefaultConfig()
	config.LocalID = raft.ServerID(s.config.NodeID)

	// Setup Raft communication
	addr, err := net.ResolveTCPAddr("tcp", s.config.RaftAddr)
	if err != nil {
		return fmt.Errorf("resolve raft addr: %w", err)
	}

	transport, err := raft.NewTCPTransport(s.config.RaftAddr, addr, 3, 10*time.Second, nil)
	if err != nil {
		return fmt.Errorf("create transport: %w", err)
	}

	// Create log store and stable store
	logStore, err := raftboltdb.NewBoltStore(s.config.DataDir + "/raft-log.db")
	if err != nil {
		return fmt.Errorf("create log store: %w", err)
	}

	stableStore, err := raftboltdb.NewBoltStore(s.config.DataDir + "/raft-stable.db")
	if err != nil {
		return fmt.Errorf("create stable store: %w", err)
	}

	// Create snapshot store
	snapshotStore, err := raft.NewFileSnapshotStore(s.config.DataDir, 2, nil)
	if err != nil {
		return fmt.Errorf("create snapshot store: %w", err)
	}

	// Create Raft instance
	s.raft, err = raft.NewRaft(config, s.fsm, logStore, stableStore, snapshotStore, transport)
	if err != nil {
		return fmt.Errorf("create raft: %w", err)
	}

	// Bootstrap if first node
	hasState, err := raft.HasExistingState(logStore, stableStore, snapshotStore)
	if err != nil {
		return fmt.Errorf("check existing state: %w", err)
	}

	if !hasState {
		configuration := raft.Configuration{
			Servers: []raft.Server{
				{
					ID:      raft.ServerID(s.config.NodeID),
					Address: raft.ServerAddress(s.config.RaftAddr),
				},
			},
		}
		s.raft.BootstrapCluster(configuration)
	}

	return nil
}

// Start starts the metadata server
func (s *Server) Start() error {
	s.grpcServer = grpc.NewServer()
	// Register metadata service
	// pb.RegisterMetadataServiceServer(s.grpcServer, s)

	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", s.config.GRPCPort))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}

	go func() {
		if err := s.grpcServer.Serve(lis); err != nil {
			fmt.Printf("gRPC server error: %v\n", err)
		}
	}()

	return nil
}

// Stop stops the metadata server
func (s *Server) Stop() error {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
	if s.raft != nil {
		s.raft.Shutdown().Error()
	}
	return nil
}

// IsLeader returns true if this node is the Raft leader
func (s *Server) IsLeader() bool {
	return s.raft.State() == raft.Leader
}

// GetLeader returns the leader address
func (s *Server) GetLeader() (string, string) {
	addr, id := s.raft.LeaderWithID()
	return string(addr), string(id)
}

// apply applies a command to Raft
func (s *Server) apply(cmd Command, timeout time.Duration) error {
	if !s.IsLeader() {
		return ErrNotLeader
	}

	data, err := json.Marshal(cmd)
	if err != nil {
		return fmt.Errorf("marshal command: %w", err)
	}

	future := s.raft.Apply(data, timeout)
	if err := future.Error(); err != nil {
		return fmt.Errorf("apply: %w", err)
	}

	if resp := future.Response(); resp != nil {
		if err, ok := resp.(error); ok {
			return err
		}
	}

	return nil
}

// CreateVolume creates a new volume
func (s *Server) CreateVolume(ctx context.Context, volumeID, clusterID string, sizeBytes int64, pgCount int) error {
	cmdData, _ := json.Marshal(CreateVolumeData{
		VolumeID:  volumeID,
		ClusterID: clusterID,
		SizeBytes: sizeBytes,
		PGCount:   pgCount,
		CreatedAt: time.Now().Unix(),
	})

	return s.apply(Command{Type: CmdCreateVolume, Data: cmdData}, 5*time.Second)
}

// DeleteVolume deletes a volume
func (s *Server) DeleteVolume(ctx context.Context, volumeID string) error {
	cmdData, _ := json.Marshal(DeleteVolumeData{VolumeID: volumeID})
	return s.apply(Command{Type: CmdDeleteVolume, Data: cmdData}, 5*time.Second)
}

// UpdateVDL updates the VDL for a volume
func (s *Server) UpdateVDL(ctx context.Context, volumeID string, newVDL int64, instanceID string) error {
	cmdData, _ := json.Marshal(UpdateVDLData{
		VolumeID:   volumeID,
		NewVDL:     newVDL,
		InstanceID: instanceID,
		UpdatedAt:  time.Now().Unix(),
	})

	return s.apply(Command{Type: CmdUpdateVDL, Data: cmdData}, 5*time.Second)
}

// GetVDL returns the VDL for a volume
func (s *Server) GetVDL(ctx context.Context, volumeID string) (int64, error) {
	vdl, ok := s.fsm.GetVDL(volumeID)
	if !ok {
		return 0, ErrVolumeNotFound
	}
	return vdl, nil
}

// UpdateNodeLSN updates the LSN for a storage node
func (s *Server) UpdateNodeLSN(ctx context.Context, volumeID, nodeID string, lsn int64) error {
	cmdData, _ := json.Marshal(UpdateNodeLSNData{
		VolumeID:   volumeID,
		NodeID:     nodeID,
		CurrentLSN: lsn,
	})

	return s.apply(Command{Type: CmdUpdateNodeLSN, Data: cmdData}, 5*time.Second)
}

// GetNodeLSNs returns all node LSNs for a volume
func (s *Server) GetNodeLSNs(ctx context.Context, volumeID string) (map[string]int64, error) {
	nodeLSNs := s.fsm.GetNodeLSNs(volumeID)
	if nodeLSNs == nil {
		return nil, ErrVolumeNotFound
	}
	return nodeLSNs, nil
}

// GetPageLocation returns the storage nodes for a page
func (s *Server) GetPageLocation(ctx context.Context, volumeID string, spaceID, pageID int64) ([]string, int, error) {
	vol, ok := s.fsm.GetVolume(volumeID)
	if !ok {
		return nil, 0, ErrVolumeNotFound
	}

	// Calculate PG ID
	pgID := int((spaceID<<32 | pageID) % int64(vol.PGCount))

	// In a real implementation, this would return the actual node assignments
	// For now, return placeholder node IDs
	nodes := []string{
		"storage-node-1",
		"storage-node-2",
		"storage-node-3",
		"storage-node-4",
		"storage-node-5",
		"storage-node-6",
	}

	return nodes, pgID, nil
}

// RegisterInstance registers a compute instance
func (s *Server) RegisterInstance(ctx context.Context, instanceID, clusterID, volumeID, role, endpoint string) error {
	cmdData, _ := json.Marshal(RegisterInstanceData{
		InstanceID:   instanceID,
		ClusterID:    clusterID,
		VolumeID:     volumeID,
		Role:         role,
		Endpoint:     endpoint,
		RegisteredAt: time.Now().Unix(),
	})

	return s.apply(Command{Type: CmdRegisterInstance, Data: cmdData}, 5*time.Second)
}

// UnregisterInstance unregisters a compute instance
func (s *Server) UnregisterInstance(ctx context.Context, instanceID string) error {
	cmdData, _ := json.Marshal(UnregisterInstanceData{InstanceID: instanceID})
	return s.apply(Command{Type: CmdUnregisterInstance, Data: cmdData}, 5*time.Second)
}

// GetInstance returns an instance by ID
func (s *Server) GetInstance(ctx context.Context, instanceID string) (*InstanceState, error) {
	inst, ok := s.fsm.GetInstance(instanceID)
	if !ok {
		return nil, ErrInstanceNotFound
	}
	return inst, nil
}

// ListInstances returns all instances for a cluster
func (s *Server) ListInstances(ctx context.Context, clusterID string) ([]*InstanceState, error) {
	return s.fsm.ListInstances(clusterID), nil
}

// HealthCheck returns the health status
func (s *Server) HealthCheck(ctx context.Context) (bool, bool, string, error) {
	leaderAddr, leaderID := s.GetLeader()
	return true, s.IsLeader(), leaderAddr + "/" + leaderID, nil
}

// Errors
var (
	ErrNotLeader        = fmt.Errorf("not the leader")
	ErrVolumeNotFound   = fmt.Errorf("volume not found")
	ErrInstanceNotFound = fmt.Errorf("instance not found")
)
