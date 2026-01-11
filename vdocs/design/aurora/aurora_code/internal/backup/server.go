// Package backup implements the backup and PITR service
package backup

import (
	"context"
	"fmt"
	"net"
	"sync"
	"time"

	"google.golang.org/grpc"
)

// Config holds backup service configuration
type Config struct {
	NodeID       string
	GRPCPort     int
	S3Endpoint   string
	S3Bucket     string
	S3AccessKey  string
	S3SecretKey  string
	DataDir      string
}

// Server represents the backup service server
type Server struct {
	config      Config
	grpcServer  *grpc.Server
	
	mu          sync.RWMutex
	snapshots   map[string]*SnapshotInfo
	restores    map[string]*RestoreStatus
	policies    map[string]*BackupPolicy
	
	snapshotMgr *SnapshotManager
	archiver    *RedoArchiver
	restoreMgr  *RestoreManager
}

// SnapshotInfo represents snapshot information
type SnapshotInfo struct {
	SnapshotID   string
	ClusterID    string
	SnapshotName string
	State        SnapshotState
	LSN          int64
	SizeBytes    int64
	CreatedAt    time.Time
	CompletedAt  time.Time
	S3Path       string
}

// SnapshotState represents snapshot state
type SnapshotState int

const (
	SnapshotStateUnknown SnapshotState = iota
	SnapshotStateCreating
	SnapshotStateAvailable
	SnapshotStateDeleting
	SnapshotStateFailed
)

// RestoreStatus represents restore operation status
type RestoreStatus struct {
	RestoreID       string
	State           RestoreState
	ProgressPercent int
	TargetClusterID string
	SourceSnapshotID string
	TargetLSN       int64
	StartedAt       time.Time
	CompletedAt     time.Time
	ErrorMessage    string
}

// RestoreState represents restore state
type RestoreState int

const (
	RestoreStateUnknown RestoreState = iota
	RestoreStatePending
	RestoreStateRestoringSnapshot
	RestoreStateApplyingRedo
	RestoreStateStartingCluster
	RestoreStateCompleted
	RestoreStateFailed
)

// BackupPolicy represents backup policy
type BackupPolicy struct {
	ClusterID          string
	Enabled            bool
	Schedule           string // cron expression
	RetentionDays      int
	RedoRetentionHours int64
}

// NewServer creates a new backup server
func NewServer(config Config) (*Server, error) {
	s := &Server{
		config:    config,
		snapshots: make(map[string]*SnapshotInfo),
		restores:  make(map[string]*RestoreStatus),
		policies:  make(map[string]*BackupPolicy),
	}
	
	s.snapshotMgr = NewSnapshotManager(s)
	s.archiver = NewRedoArchiver(config)
	s.restoreMgr = NewRestoreManager(s)
	
	return s, nil
}

// Start starts the backup server
func (s *Server) Start() error {
	s.grpcServer = grpc.NewServer()
	// pb.RegisterBackupServiceServer(s.grpcServer, s)
	
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", s.config.GRPCPort))
	if err != nil {
		return fmt.Errorf("listen: %w", err)
	}
	
	go func() {
		if err := s.grpcServer.Serve(lis); err != nil {
			fmt.Printf("gRPC server error: %v\n", err)
		}
	}()
	
	// Start background archiver
	go s.archiver.Start(context.Background())
	
	return nil
}

// Stop stops the backup server
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}

// CreateSnapshot creates a new snapshot
func (s *Server) CreateSnapshot(ctx context.Context, clusterID, snapshotName string) (*SnapshotInfo, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	snapshotID := generateID("snap")
	
	snapshot := &SnapshotInfo{
		SnapshotID:   snapshotID,
		ClusterID:    clusterID,
		SnapshotName: snapshotName,
		State:        SnapshotStateCreating,
		CreatedAt:    time.Now(),
	}
	
	s.snapshots[snapshotID] = snapshot
	
	// Start async snapshot creation
	go s.snapshotMgr.CreateSnapshot(ctx, snapshot)
	
	return snapshot, nil
}

// GetSnapshot returns snapshot information
func (s *Server) GetSnapshot(ctx context.Context, snapshotID string) (*SnapshotInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	snap, ok := s.snapshots[snapshotID]
	if !ok {
		return nil, ErrSnapshotNotFound
	}
	return snap, nil
}

// DeleteSnapshot deletes a snapshot
func (s *Server) DeleteSnapshot(ctx context.Context, snapshotID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	snap, ok := s.snapshots[snapshotID]
	if !ok {
		return ErrSnapshotNotFound
	}
	
	snap.State = SnapshotStateDeleting
	
	// Delete from S3
	go s.snapshotMgr.DeleteSnapshot(ctx, snap)
	
	return nil
}

// ListSnapshots lists all snapshots for a cluster
func (s *Server) ListSnapshots(ctx context.Context, clusterID string) ([]*SnapshotInfo, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	var result []*SnapshotInfo
	for _, snap := range s.snapshots {
		if snap.ClusterID == clusterID {
			result = append(result, snap)
		}
	}
	return result, nil
}

// RestoreToPointInTime performs PITR restore
func (s *Server) RestoreToPointInTime(ctx context.Context, sourceClusterID, targetClusterName string, targetLSN int64, targetTimestamp int64, snapshotID string) (*RestoreStatus, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	restoreID := generateID("restore")
	
	restore := &RestoreStatus{
		RestoreID:        restoreID,
		State:            RestoreStatePending,
		SourceSnapshotID: snapshotID,
		TargetLSN:        targetLSN,
		StartedAt:        time.Now(),
	}
	
	s.restores[restoreID] = restore
	
	// Start async restore
	go s.restoreMgr.ExecuteRestore(ctx, restore, sourceClusterID, targetClusterName)
	
	return restore, nil
}

// GetRestoreStatus returns restore status
func (s *Server) GetRestoreStatus(ctx context.Context, restoreID string) (*RestoreStatus, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	restore, ok := s.restores[restoreID]
	if !ok {
		return nil, ErrRestoreNotFound
	}
	return restore, nil
}

// SetBackupPolicy sets the backup policy for a cluster
func (s *Server) SetBackupPolicy(ctx context.Context, clusterID string, policy *BackupPolicy) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	policy.ClusterID = clusterID
	s.policies[clusterID] = policy
	return nil
}

// GetBackupPolicy returns the backup policy for a cluster
func (s *Server) GetBackupPolicy(ctx context.Context, clusterID string) (*BackupPolicy, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	policy, ok := s.policies[clusterID]
	if !ok {
		return nil, ErrPolicyNotFound
	}
	return policy, nil
}

// Errors
var (
	ErrSnapshotNotFound = fmt.Errorf("snapshot not found")
	ErrRestoreNotFound  = fmt.Errorf("restore not found")
	ErrPolicyNotFound   = fmt.Errorf("policy not found")
)

var idCounter int64

func generateID(prefix string) string {
	idCounter++
	return fmt.Sprintf("%s-%d-%d", prefix, time.Now().Unix(), idCounter)
}
