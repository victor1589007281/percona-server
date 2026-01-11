// Package crossregion implements the cross-region disaster recovery service
package crossregion

import (
	"context"
	"fmt"
	"net"
	"sync"
	"time"

	"google.golang.org/grpc"
)

// Config holds cross-region service configuration
type Config struct {
	NodeID   string
	RegionID string
	GRPCPort int
}

// Server represents the cross-region server
type Server struct {
	config     Config
	grpcServer *grpc.Server
	
	mu              sync.RWMutex
	regions         map[string]*RegionInfo
	replications    map[string]*ReplicationState
	failovers       map[string]*DRFailoverStatus
	
	replicator      *Replicator
}

// RegionInfo represents a region
type RegionInfo struct {
	RegionID   string
	RegionName string
	Endpoint   string
	IsPrimary  bool
	IsHealthy  bool
	LastCheck  time.Time
}

// ReplicationState represents replication state
type ReplicationState struct {
	ClusterID     string
	SourceRegion  string
	TargetRegion  string
	CurrentGTID   string
	DelayMs       int64
	IsHealthy     bool
	BytesReplicated int64
	LastUpdated   time.Time
}

// DRFailoverStatus represents DR failover status
type DRFailoverStatus struct {
	FailoverID    string
	ClusterID     string
	SourceRegion  string
	TargetRegion  string
	State         DRFailoverState
	FinalGTID     string
	StartedAt     time.Time
	CompletedAt   time.Time
	ErrorMessage  string
}

// DRFailoverState represents DR failover state
type DRFailoverState int

const (
	DRFailoverStateUnknown DRFailoverState = iota
	DRFailoverStateStarted
	DRFailoverStateSyncing
	DRFailoverStateSwitching
	DRFailoverStateCompleted
	DRFailoverStateFailed
)

// NewServer creates a new cross-region server
func NewServer(config Config) (*Server, error) {
	s := &Server{
		config:       config,
		regions:      make(map[string]*RegionInfo),
		replications: make(map[string]*ReplicationState),
		failovers:    make(map[string]*DRFailoverStatus),
	}
	
	s.replicator = NewReplicator(s)
	
	// Register self
	s.regions[config.RegionID] = &RegionInfo{
		RegionID:  config.RegionID,
		IsPrimary: true,
		IsHealthy: true,
		LastCheck: time.Now(),
	}
	
	return s, nil
}

// Start starts the cross-region server
func (s *Server) Start() error {
	s.grpcServer = grpc.NewServer()
	// pb.RegisterCrossRegionServiceServer(s.grpcServer, s)
	
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", s.config.GRPCPort))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	
	go func() {
		if err := s.grpcServer.Serve(lis); err != nil {
			fmt.Printf("gRPC server error: %v\n", err)
		}
	}()
	
	// Start replicator
	go s.replicator.Start(context.Background())
	
	return nil
}

// Stop stops the cross-region server
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}

// RegisterRegion registers a region
func (s *Server) RegisterRegion(ctx context.Context, regionID, regionName, endpoint string, isPrimary bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	s.regions[regionID] = &RegionInfo{
		RegionID:   regionID,
		RegionName: regionName,
		Endpoint:   endpoint,
		IsPrimary:  isPrimary,
		IsHealthy:  true,
		LastCheck:  time.Now(),
	}
	
	return nil
}

// GetRegions returns all regions
func (s *Server) GetRegions(ctx context.Context) ([]*RegionInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	var regions []*RegionInfo
	for _, r := range s.regions {
		regions = append(regions, r)
	}
	return regions, nil
}

// StartReplication starts replication for a cluster
func (s *Server) StartReplication(ctx context.Context, clusterID, sourceRegion, targetRegion string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	key := fmt.Sprintf("%s:%s->%s", clusterID, sourceRegion, targetRegion)
	
	s.replications[key] = &ReplicationState{
		ClusterID:    clusterID,
		SourceRegion: sourceRegion,
		TargetRegion: targetRegion,
		IsHealthy:    true,
		LastUpdated:  time.Now(),
	}
	
	// Start replication stream
	go s.replicator.StartStream(ctx, clusterID, sourceRegion, targetRegion)
	
	return nil
}

// GetReplicationStatus returns replication status
func (s *Server) GetReplicationStatus(ctx context.Context, clusterID string) (*ReplicationState, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	for _, rep := range s.replications {
		if rep.ClusterID == clusterID {
			return rep, nil
		}
	}
	return nil, ErrReplicationNotFound
}

// UpdateReplicationState updates replication state
func (s *Server) UpdateReplicationState(clusterID, gtid string, delay int64, bytesReplicated int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	for key, rep := range s.replications {
		if rep.ClusterID == clusterID {
			s.replications[key].CurrentGTID = gtid
			s.replications[key].DelayMs = delay
			s.replications[key].BytesReplicated = bytesReplicated
			s.replications[key].LastUpdated = time.Now()
			break
		}
	}
}

// TriggerDRFailover triggers a DR failover
func (s *Server) TriggerDRFailover(ctx context.Context, clusterID, targetRegion string, force bool) (*DRFailoverStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	// Find source region
	var sourceRegion string
	for _, rep := range s.replications {
		if rep.ClusterID == clusterID {
			sourceRegion = rep.SourceRegion
			break
		}
	}
	
	if sourceRegion == "" {
		sourceRegion = s.config.RegionID
	}
	
	failoverID := generateID("drfo")
	
	failover := &DRFailoverStatus{
		FailoverID:   failoverID,
		ClusterID:    clusterID,
		SourceRegion: sourceRegion,
		TargetRegion: targetRegion,
		State:        DRFailoverStateStarted,
		StartedAt:    time.Now(),
	}
	
	s.failovers[failoverID] = failover
	
	// Execute failover
	go s.executeDRFailover(ctx, failover, force)
	
	return failover, nil
}

func (s *Server) executeDRFailover(ctx context.Context, failover *DRFailoverStatus, force bool) {
	// Step 1: Sync pending data
	s.updateDRState(failover, DRFailoverStateSyncing)
	
	if !force {
		// Wait for replication to catch up
		time.Sleep(200 * time.Millisecond)
	}
	
	// Step 2: Switch primary
	s.updateDRState(failover, DRFailoverStateSwitching)
	
	// Update regions
	s.mu.Lock()
	for _, r := range s.regions {
		if r.RegionID == failover.SourceRegion {
			r.IsPrimary = false
		}
		if r.RegionID == failover.TargetRegion {
			r.IsPrimary = true
		}
	}
	s.mu.Unlock()
	
	time.Sleep(100 * time.Millisecond)
	
	// Step 3: Complete
	s.mu.Lock()
	failover.State = DRFailoverStateCompleted
	failover.CompletedAt = time.Now()
	failover.FinalGTID = "uuid:final"
	s.mu.Unlock()
}

func (s *Server) updateDRState(failover *DRFailoverStatus, state DRFailoverState) {
	s.mu.Lock()
	failover.State = state
	s.mu.Unlock()
}

// GetDRFailoverStatus returns DR failover status
func (s *Server) GetDRFailoverStatus(ctx context.Context, failoverID string) (*DRFailoverStatus, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	failover, ok := s.failovers[failoverID]
	if !ok {
		return nil, ErrFailoverNotFound
	}
	return failover, nil
}

// Errors
var (
	ErrReplicationNotFound = fmt.Errorf("replication not found")
	ErrFailoverNotFound    = fmt.Errorf("failover not found")
	ErrRegionNotFound      = fmt.Errorf("region not found")
)

var idCounter int64

func generateID(prefix string) string {
	idCounter++
	return fmt.Sprintf("%s-%d-%d", prefix, time.Now().Unix(), idCounter)
}
