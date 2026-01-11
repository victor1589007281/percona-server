// Package control implements the control plane service
package control

import (
	"context"
	"fmt"
	"net"
	"sync"
	"time"

	"google.golang.org/grpc"
)

// Config holds control plane configuration
type Config struct {
	NodeID   string
	GRPCPort int
	RESTPort int
	EtcdEndpoints []string
}

// Server represents the control plane server
type Server struct {
	config     Config
	grpcServer *grpc.Server
	
	mu        sync.RWMutex
	clusters  map[string]*ClusterInfo
	failovers map[string]*FailoverContext
	
	monitor   *MonitorService
	failover  *FailoverController
}

// ClusterInfo represents cluster information
type ClusterInfo struct {
	ClusterID   string
	ClusterName string
	VolumeID    string
	Writer      *InstanceInfo
	Readers     []*InstanceInfo
	State       ClusterState
	CreatedAt   time.Time
	UpdatedAt   time.Time
}

// InstanceInfo represents instance information
type InstanceInfo struct {
	InstanceID       string
	InstanceClass    string
	Endpoint         string
	AvailabilityZone string
	State            InstanceState
	Role             InstanceRole
	CurrentLSN       int64
}

// ClusterState represents cluster state
type ClusterState int

const (
	ClusterStateUnknown ClusterState = iota
	ClusterStateCreating
	ClusterStateAvailable
	ClusterStateModifying
	ClusterStateDeleting
	ClusterStateFailed
	ClusterStateFailover
)

// InstanceState represents instance state
type InstanceState int

const (
	InstanceStateUnknown InstanceState = iota
	InstanceStateCreating
	InstanceStateAvailable
	InstanceStateModifying
	InstanceStateDeleting
	InstanceStateFailed
	InstanceStateRebooting
)

// InstanceRole represents instance role
type InstanceRole int

const (
	InstanceRoleUnknown InstanceRole = iota
	InstanceRoleWriter
	InstanceRoleReader
)

// NewServer creates a new control plane server
func NewServer(config Config) (*Server, error) {
	s := &Server{
		config:    config,
		clusters:  make(map[string]*ClusterInfo),
		failovers: make(map[string]*FailoverContext),
	}

	s.monitor = NewMonitorService(s)
	s.failover = NewFailoverController(s)

	return s, nil
}

// Start starts the control plane server
func (s *Server) Start() error {
	s.grpcServer = grpc.NewServer()
	// Register services
	// pb.RegisterClusterServiceServer(s.grpcServer, s)
	// pb.RegisterFailoverServiceServer(s.grpcServer, s)
	// pb.RegisterMonitorServiceServer(s.grpcServer, s)

	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", s.config.GRPCPort))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}

	go func() {
		if err := s.grpcServer.Serve(lis); err != nil {
			fmt.Printf("gRPC server error: %v\n", err)
		}
	}()

	// Start monitor service
	go s.monitor.Start(context.Background())

	return nil
}

// Stop stops the control plane server
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}

// CreateCluster creates a new cluster
func (s *Server) CreateCluster(ctx context.Context, name, instanceClass string, readerCount int, azs []string, volumeSize int64) (*ClusterInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	clusterID := generateID("cluster")
	volumeID := generateID("vol")

	cluster := &ClusterInfo{
		ClusterID:   clusterID,
		ClusterName: name,
		VolumeID:    volumeID,
		State:       ClusterStateCreating,
		CreatedAt:   time.Now(),
		UpdatedAt:   time.Now(),
	}

	// Create writer instance
	cluster.Writer = &InstanceInfo{
		InstanceID:    generateID("writer"),
		InstanceClass: instanceClass,
		Role:          InstanceRoleWriter,
		State:         InstanceStateCreating,
	}
	if len(azs) > 0 {
		cluster.Writer.AvailabilityZone = azs[0]
	}

	// Create reader instances
	for i := 0; i < readerCount; i++ {
		reader := &InstanceInfo{
			InstanceID:    generateID("reader"),
			InstanceClass: instanceClass,
			Role:          InstanceRoleReader,
			State:         InstanceStateCreating,
		}
		if len(azs) > 0 {
			reader.AvailabilityZone = azs[(i+1)%len(azs)]
		}
		cluster.Readers = append(cluster.Readers, reader)
	}

	s.clusters[clusterID] = cluster

	// In a real implementation, this would:
	// 1. Create volume via metadata service
	// 2. Provision compute instances
	// 3. Initialize storage
	// 4. Update state to Available

	cluster.State = ClusterStateAvailable
	cluster.Writer.State = InstanceStateAvailable
	for _, r := range cluster.Readers {
		r.State = InstanceStateAvailable
	}

	return cluster, nil
}

// DeleteCluster deletes a cluster
func (s *Server) DeleteCluster(ctx context.Context, clusterID string, force bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	cluster, ok := s.clusters[clusterID]
	if !ok {
		return ErrClusterNotFound
	}

	cluster.State = ClusterStateDeleting

	// In a real implementation, this would:
	// 1. Stop all instances
	// 2. Delete volume
	// 3. Clean up resources

	delete(s.clusters, clusterID)

	return nil
}

// GetCluster returns cluster information
func (s *Server) GetCluster(ctx context.Context, clusterID string) (*ClusterInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	cluster, ok := s.clusters[clusterID]
	if !ok {
		return nil, ErrClusterNotFound
	}

	return cluster, nil
}

// ListClusters returns all clusters
func (s *Server) ListClusters(ctx context.Context) ([]*ClusterInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	result := make([]*ClusterInfo, 0, len(s.clusters))
	for _, c := range s.clusters {
		result = append(result, c)
	}
	return result, nil
}

// AddReader adds a reader to a cluster
func (s *Server) AddReader(ctx context.Context, clusterID, instanceClass, az string) (*InstanceInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	cluster, ok := s.clusters[clusterID]
	if !ok {
		return nil, ErrClusterNotFound
	}

	reader := &InstanceInfo{
		InstanceID:       generateID("reader"),
		InstanceClass:    instanceClass,
		AvailabilityZone: az,
		Role:             InstanceRoleReader,
		State:            InstanceStateCreating,
	}

	cluster.Readers = append(cluster.Readers, reader)
	cluster.UpdatedAt = time.Now()

	// In a real implementation, this would provision the instance
	reader.State = InstanceStateAvailable

	return reader, nil
}

// RemoveReader removes a reader from a cluster
func (s *Server) RemoveReader(ctx context.Context, clusterID, instanceID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	cluster, ok := s.clusters[clusterID]
	if !ok {
		return ErrClusterNotFound
	}

	for i, r := range cluster.Readers {
		if r.InstanceID == instanceID {
			cluster.Readers = append(cluster.Readers[:i], cluster.Readers[i+1:]...)
			cluster.UpdatedAt = time.Now()
			return nil
		}
	}

	return ErrInstanceNotFound
}

// TriggerFailover triggers a failover
func (s *Server) TriggerFailover(ctx context.Context, clusterID, targetInstanceID string) (*FailoverContext, error) {
	return s.failover.TriggerFailover(ctx, clusterID, targetInstanceID)
}

// GetFailoverStatus returns the status of a failover
func (s *Server) GetFailoverStatus(ctx context.Context, failoverID string) (*FailoverContext, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	fo, ok := s.failovers[failoverID]
	if !ok {
		return nil, ErrFailoverNotFound
	}
	return fo, nil
}

// GetClusterStatus returns the cluster status
func (s *Server) GetClusterStatus(ctx context.Context, clusterID string) (*ClusterStatus, error) {
	cluster, err := s.GetCluster(ctx, clusterID)
	if err != nil {
		return nil, err
	}

	status := &ClusterStatus{
		ClusterID: clusterID,
		State:     cluster.State,
	}

	if cluster.Writer != nil {
		status.Writer = &WriterStatus{
			InstanceID: cluster.Writer.InstanceID,
			IsHealthy:  cluster.Writer.State == InstanceStateAvailable,
			CurrentLSN: cluster.Writer.CurrentLSN,
		}
	}

	for _, r := range cluster.Readers {
		status.Readers = append(status.Readers, &ReaderStatus{
			InstanceID: r.InstanceID,
			IsHealthy:  r.State == InstanceStateAvailable,
			CurrentLSN: r.CurrentLSN,
		})
	}

	return status, nil
}

// ClusterStatus represents cluster status
type ClusterStatus struct {
	ClusterID string
	State     ClusterState
	Writer    *WriterStatus
	Readers   []*ReaderStatus
	Storage   *StorageStatus
}

// WriterStatus represents writer status
type WriterStatus struct {
	InstanceID  string
	IsHealthy   bool
	CurrentLSN  int64
	Connections int64
	CPUUsage    float64
	MemoryUsage float64
}

// ReaderStatus represents reader status
type ReaderStatus struct {
	InstanceID string
	IsHealthy  bool
	CurrentLSN int64
	LagBytes   int64
	LagMs      int64
}

// StorageStatus represents storage status
type StorageStatus struct {
	CurrentVDL   int64
	UsedBytes    int64
	TotalBytes   int64
	HealthyNodes int
	TotalNodes   int
}

// Errors
var (
	ErrClusterNotFound  = fmt.Errorf("cluster not found")
	ErrInstanceNotFound = fmt.Errorf("instance not found")
	ErrFailoverNotFound = fmt.Errorf("failover not found")
)

var idCounter int64

func generateID(prefix string) string {
	idCounter++
	return fmt.Sprintf("%s-%d", prefix, idCounter)
}
