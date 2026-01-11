// Package dts implements the Data Transmission Service
package dts

import (
	"context"
	"fmt"
	"net"
	"sync"
	"time"

	"google.golang.org/grpc"
)

// Config holds DTS service configuration
type Config struct {
	NodeID   string
	GRPCPort int
}

// Server represents the DTS server
type Server struct {
	config     Config
	grpcServer *grpc.Server
	
	mu             sync.RWMutex
	migrationTasks map[string]*MigrationTask
	syncTasks      map[string]*SyncTask
}

// MigrationTask represents a data migration task
type MigrationTask struct {
	TaskID    string
	TaskName  string
	Source    *SourceEndpoint
	Target    *TargetEndpoint
	Mode      MigrationMode
	State     MigrationState
	Progress  *MigrationProgress
	Databases []string
	CreatedAt time.Time
	StartedAt time.Time
	Error     string
}

// SourceEndpoint represents source database
type SourceEndpoint struct {
	Host     string
	Port     int
	Username string
	Password string
	Database string
}

// TargetEndpoint represents target Aurora cluster
type TargetEndpoint struct {
	ClusterID string
	Database  string
}

// MigrationMode represents migration mode
type MigrationMode int

const (
	MigrationModeUnknown MigrationMode = iota
	MigrationModeStructureOnly
	MigrationModeFull
	MigrationModeIncremental
	MigrationModeFullAndIncremental
)

// MigrationState represents migration state
type MigrationState int

const (
	MigrationStateUnknown MigrationState = iota
	MigrationStateCreated
	MigrationStateRunning
	MigrationStatePaused
	MigrationStateCompleted
	MigrationStateFailed
)

// MigrationProgress represents migration progress
type MigrationProgress struct {
	Phase              MigrationPhase
	StructurePercent   int
	FullPercent        int
	IncrementalDelayMs int64
	CurrentGTID        string
}

// MigrationPhase represents migration phase
type MigrationPhase int

const (
	MigrationPhaseUnknown MigrationPhase = iota
	MigrationPhaseStructure
	MigrationPhaseFull
	MigrationPhaseIncremental
)

// SyncTask represents a real-time sync task
type SyncTask struct {
	TaskID      string
	TaskName    string
	Source      *SourceEndpoint
	Target      *TargetEndpoint
	Databases   []string
	State       SyncState
	DelayMs     int64
	CurrentGTID string
	CreatedAt   time.Time
	StartedAt   time.Time
	Error       string
}

// SyncState represents sync state
type SyncState int

const (
	SyncStateUnknown SyncState = iota
	SyncStateCreated
	SyncStateRunning
	SyncStatePaused
	SyncStateStopped
	SyncStateFailed
)

// NewServer creates a new DTS server
func NewServer(config Config) (*Server, error) {
	return &Server{
		config:         config,
		migrationTasks: make(map[string]*MigrationTask),
		syncTasks:      make(map[string]*SyncTask),
	}, nil
}

// Start starts the DTS server
func (s *Server) Start() error {
	s.grpcServer = grpc.NewServer()
	// pb.RegisterDTSServiceServer(s.grpcServer, s)
	
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

// Stop stops the DTS server
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
}

// CreateMigrationTask creates a new migration task
func (s *Server) CreateMigrationTask(ctx context.Context, taskName string, source *SourceEndpoint, target *TargetEndpoint, mode MigrationMode, databases []string) (*MigrationTask, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	taskID := generateID("mig")
	
	task := &MigrationTask{
		TaskID:    taskID,
		TaskName:  taskName,
		Source:    source,
		Target:    target,
		Mode:      mode,
		Databases: databases,
		State:     MigrationStateCreated,
		Progress:  &MigrationProgress{},
		CreatedAt: time.Now(),
	}
	
	s.migrationTasks[taskID] = task
	
	return task, nil
}

// StartMigrationTask starts a migration task
func (s *Server) StartMigrationTask(ctx context.Context, taskID string) error {
	s.mu.Lock()
	task, ok := s.migrationTasks[taskID]
	if !ok {
		s.mu.Unlock()
		return ErrTaskNotFound
	}
	task.State = MigrationStateRunning
	task.StartedAt = time.Now()
	s.mu.Unlock()
	
	// Start migration in background
	go s.executeMigration(ctx, task)
	
	return nil
}

// executeMigration executes the migration
func (s *Server) executeMigration(ctx context.Context, task *MigrationTask) {
	executor := &MigrationExecutor{server: s}
	
	// Phase 1: Structure migration
	if task.Mode != MigrationModeIncremental {
		s.updateProgress(task, MigrationPhaseStructure, 0, 0, 0)
		if err := executor.MigrateStructure(ctx, task); err != nil {
			s.failTask(task, err)
			return
		}
		s.updateProgress(task, MigrationPhaseStructure, 100, 0, 0)
	}
	
	// Phase 2: Full data migration
	if task.Mode == MigrationModeFull || task.Mode == MigrationModeFullAndIncremental {
		s.updateProgress(task, MigrationPhaseFull, 100, 0, 0)
		if err := executor.MigrateFullData(ctx, task); err != nil {
			s.failTask(task, err)
			return
		}
		s.updateProgress(task, MigrationPhaseFull, 100, 100, 0)
	}
	
	// Phase 3: Incremental sync
	if task.Mode == MigrationModeIncremental || task.Mode == MigrationModeFullAndIncremental {
		s.updateProgress(task, MigrationPhaseIncremental, 100, 100, 0)
		executor.StartIncrementalSync(ctx, task)
	} else {
		// Complete
		s.mu.Lock()
		task.State = MigrationStateCompleted
		s.mu.Unlock()
	}
}

func (s *Server) updateProgress(task *MigrationTask, phase MigrationPhase, structPct, fullPct int, delay int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	task.Progress.Phase = phase
	task.Progress.StructurePercent = structPct
	task.Progress.FullPercent = fullPct
	task.Progress.IncrementalDelayMs = delay
}

func (s *Server) failTask(task *MigrationTask, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	task.State = MigrationStateFailed
	task.Error = err.Error()
}

// StopMigrationTask stops a migration task
func (s *Server) StopMigrationTask(ctx context.Context, taskID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	task, ok := s.migrationTasks[taskID]
	if !ok {
		return ErrTaskNotFound
	}
	
	task.State = MigrationStatePaused
	return nil
}

// GetMigrationTask returns a migration task
func (s *Server) GetMigrationTask(ctx context.Context, taskID string) (*MigrationTask, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	task, ok := s.migrationTasks[taskID]
	if !ok {
		return nil, ErrTaskNotFound
	}
	return task, nil
}

// ListMigrationTasks returns all migration tasks
func (s *Server) ListMigrationTasks(ctx context.Context) ([]*MigrationTask, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	var tasks []*MigrationTask
	for _, t := range s.migrationTasks {
		tasks = append(tasks, t)
	}
	return tasks, nil
}

// CreateSyncTask creates a new sync task
func (s *Server) CreateSyncTask(ctx context.Context, taskName string, source *SourceEndpoint, target *TargetEndpoint, databases []string) (*SyncTask, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	taskID := generateID("sync")
	
	task := &SyncTask{
		TaskID:    taskID,
		TaskName:  taskName,
		Source:    source,
		Target:    target,
		Databases: databases,
		State:     SyncStateCreated,
		CreatedAt: time.Now(),
	}
	
	s.syncTasks[taskID] = task
	
	return task, nil
}

// StartSyncTask starts a sync task
func (s *Server) StartSyncTask(ctx context.Context, taskID string) error {
	s.mu.Lock()
	task, ok := s.syncTasks[taskID]
	if !ok {
		s.mu.Unlock()
		return ErrTaskNotFound
	}
	task.State = SyncStateRunning
	task.StartedAt = time.Now()
	s.mu.Unlock()
	
	// Start sync in background
	go s.executeSyncTask(ctx, task)
	
	return nil
}

func (s *Server) executeSyncTask(ctx context.Context, task *SyncTask) {
	executor := &SyncExecutor{server: s}
	
	for task.State == SyncStateRunning {
		select {
		case <-ctx.Done():
			return
		default:
			delay, gtid, err := executor.SyncBatch(ctx, task)
			if err != nil {
				s.mu.Lock()
				task.State = SyncStateFailed
				task.Error = err.Error()
				s.mu.Unlock()
				return
			}
			
			s.mu.Lock()
			task.DelayMs = delay
			task.CurrentGTID = gtid
			s.mu.Unlock()
			
			time.Sleep(100 * time.Millisecond)
		}
	}
}

// StopSyncTask stops a sync task
func (s *Server) StopSyncTask(ctx context.Context, taskID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	task, ok := s.syncTasks[taskID]
	if !ok {
		return ErrTaskNotFound
	}
	
	task.State = SyncStateStopped
	return nil
}

// GetSyncTask returns a sync task
func (s *Server) GetSyncTask(ctx context.Context, taskID string) (*SyncTask, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	task, ok := s.syncTasks[taskID]
	if !ok {
		return nil, ErrTaskNotFound
	}
	return task, nil
}

// Errors
var (
	ErrTaskNotFound = fmt.Errorf("task not found")
)

var idCounter int64

func generateID(prefix string) string {
	idCounter++
	return fmt.Sprintf("%s-%d-%d", prefix, time.Now().Unix(), idCounter)
}
