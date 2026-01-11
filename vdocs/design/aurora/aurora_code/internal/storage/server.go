// Package storage implements the storage layer service
package storage

import (
	"context"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"google.golang.org/grpc"

	"github.com/percona/aurora/pkg/page"
	"github.com/percona/aurora/pkg/quorum"
	"github.com/percona/aurora/pkg/wal"
)

// Config holds storage node configuration
type Config struct {
	NodeID      string
	GRPCPort    int
	DataDir     string
	WALDir      string
	MaxWALSize  int64
	PageCacheSize int
}

// Server represents the storage node server
type Server struct {
	config     Config
	grpcServer *grpc.Server
	
	mu         sync.RWMutex
	volumes    map[string]*VolumeStore
	frozen     atomic.Bool
	startTime  time.Time
	currentLSN atomic.Uint64
}

// VolumeStore manages storage for a single volume
type VolumeStore struct {
	volumeID   string
	walWriter  *wal.Writer
	walReader  *wal.Reader
	pageCache  *page.Cache
	materializer *PageMaterializer
	
	mu         sync.RWMutex
	currentLSN uint64
	frozen     bool
}

// NewServer creates a new storage server
func NewServer(config Config) (*Server, error) {
	return &Server{
		config:    config,
		volumes:   make(map[string]*VolumeStore),
		startTime: time.Now(),
	}, nil
}

// Start starts the storage server
func (s *Server) Start() error {
	// Create gRPC server
	s.grpcServer = grpc.NewServer()
	
	// Register storage service
	// pb.RegisterStorageServiceServer(s.grpcServer, s)
	
	// Start listening
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", s.config.GRPCPort))
	if err != nil {
		return fmt.Errorf("failed to listen: %w", err)
	}
	
	go func() {
		if err := s.grpcServer.Serve(lis); err != nil {
			fmt.Printf("gRPC server error: %v\n", err)
		}
	}()
	
	return nil
}

// Stop stops the storage server
func (s *Server) Stop() {
	if s.grpcServer != nil {
		s.grpcServer.GracefulStop()
	}
	
	s.mu.Lock()
	defer s.mu.Unlock()
	
	for _, vs := range s.volumes {
		vs.Close()
	}
}

// GetOrCreateVolume gets or creates a volume store
func (s *Server) GetOrCreateVolume(volumeID string) (*VolumeStore, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	
	if vs, ok := s.volumes[volumeID]; ok {
		return vs, nil
	}
	
	vs, err := NewVolumeStore(volumeID, s.config)
	if err != nil {
		return nil, err
	}
	
	s.volumes[volumeID] = vs
	return vs, nil
}

// WriteRedo writes a redo record
func (s *Server) WriteRedo(ctx context.Context, volumeID string, record *wal.RedoRecord) (uint64, error) {
	if s.frozen.Load() {
		return 0, ErrWritesFrozen
	}
	
	vs, err := s.GetOrCreateVolume(volumeID)
	if err != nil {
		return 0, err
	}
	
	return vs.WriteRedo(record)
}

// ReadPage reads a page with optional materialization
func (s *Server) ReadPage(ctx context.Context, volumeID string, spaceID, pageID uint64, targetLSN uint64) (*page.Page, error) {
	vs, err := s.GetOrCreateVolume(volumeID)
	if err != nil {
		return nil, err
	}
	
	return vs.ReadPage(spaceID, pageID, targetLSN)
}

// GetRedoLogs gets redo logs between LSNs
func (s *Server) GetRedoLogs(ctx context.Context, volumeID string, fromLSN, toLSN uint64, maxCount int) ([]*wal.RedoRecord, uint64, bool, error) {
	vs, err := s.GetOrCreateVolume(volumeID)
	if err != nil {
		return nil, 0, false, err
	}
	
	return vs.GetRedoLogs(fromLSN, toLSN, maxCount)
}

// FreezeWrites freezes all writes and returns the final LSN
func (s *Server) FreezeWrites(volumeID string) (uint64, error) {
	s.frozen.Store(true)
	
	vs, err := s.GetOrCreateVolume(volumeID)
	if err != nil {
		return 0, err
	}
	
	return vs.Freeze()
}

// UnfreezeWrites unfreezes writes
func (s *Server) UnfreezeWrites(volumeID string) error {
	s.frozen.Store(false)
	
	vs, err := s.GetOrCreateVolume(volumeID)
	if err != nil {
		return err
	}
	
	return vs.Unfreeze()
}

// GetStatus returns the node status
func (s *Server) GetStatus() NodeStatus {
	s.mu.RLock()
	defer s.mu.RUnlock()
	
	var totalWALSize int64
	var totalPages int64
	
	for _, vs := range s.volumes {
		stats := vs.GetStats()
		totalWALSize += stats.WALSizeBytes
		totalPages += stats.PageCount
	}
	
	return NodeStatus{
		NodeID:      s.config.NodeID,
		IsHealthy:   true,
		CurrentLSN:  s.currentLSN.Load(),
		WALSizeBytes: totalWALSize,
		PageCount:   totalPages,
		IsFrozen:    s.frozen.Load(),
		UptimeSeconds: int64(time.Since(s.startTime).Seconds()),
	}
}

// NodeStatus represents the status of a storage node
type NodeStatus struct {
	NodeID       string
	IsHealthy    bool
	CurrentLSN   uint64
	WALSizeBytes int64
	PageCount    int64
	IsFrozen     bool
	UptimeSeconds int64
}

// VolumeStats represents volume statistics
type VolumeStats struct {
	WALSizeBytes int64
	PageCount    int64
	CurrentLSN   uint64
}

// NewVolumeStore creates a new volume store
func NewVolumeStore(volumeID string, config Config) (*VolumeStore, error) {
	walDir := fmt.Sprintf("%s/%s/wal", config.WALDir, volumeID)
	
	var volumeUUID [16]byte
	copy(volumeUUID[:], []byte(volumeID))
	
	walWriter, err := wal.NewWriter(walDir, volumeUUID, 0)
	if err != nil {
		return nil, fmt.Errorf("create wal writer: %w", err)
	}
	
	walReader, err := wal.NewReader(walDir)
	if err != nil {
		// WAL reader may fail if no files yet, that's OK
		walReader = nil
	}
	
	return &VolumeStore{
		volumeID:   volumeID,
		walWriter:  walWriter,
		walReader:  walReader,
		pageCache:  page.NewCache(config.PageCacheSize),
		materializer: NewPageMaterializer(),
	}, nil
}

// WriteRedo writes a redo record to the volume
func (vs *VolumeStore) WriteRedo(record *wal.RedoRecord) (uint64, error) {
	vs.mu.Lock()
	defer vs.mu.Unlock()
	
	if vs.frozen {
		return 0, ErrWritesFrozen
	}
	
	if err := vs.walWriter.Write(record); err != nil {
		return 0, err
	}
	
	if err := vs.walWriter.Sync(); err != nil {
		return 0, err
	}
	
	vs.currentLSN = record.Header.LSN
	
	// Invalidate cached page
	pageKey := page.PageKey{SpaceID: record.Header.SpaceID, PageID: record.Header.PageID}
	vs.pageCache.Invalidate(pageKey)
	
	return vs.currentLSN, nil
}

// ReadPage reads a page, materializing if necessary
func (vs *VolumeStore) ReadPage(spaceID, pageID uint64, targetLSN uint64) (*page.Page, error) {
	pageKey := page.PageKey{SpaceID: spaceID, PageID: pageID}
	
	// Check cache first
	if cachedPage, ok := vs.pageCache.Get(pageKey); ok {
		if targetLSN == 0 || cachedPage.GetLSN() >= targetLSN {
			return cachedPage, nil
		}
	}
	
	// Materialize page
	p, err := vs.materializer.Materialize(vs, spaceID, pageID, targetLSN)
	if err != nil {
		return nil, err
	}
	
	// Cache the materialized page
	vs.pageCache.Put(pageKey, p)
	
	return p, nil
}

// GetRedoLogs gets redo logs between LSNs
func (vs *VolumeStore) GetRedoLogs(fromLSN, toLSN uint64, maxCount int) ([]*wal.RedoRecord, uint64, bool, error) {
	if vs.walReader == nil {
		return nil, fromLSN, false, nil
	}
	
	records, err := vs.walReader.GetRecordsBetween(fromLSN, toLSN)
	if err != nil {
		return nil, fromLSN, false, err
	}
	
	hasMore := len(records) > maxCount
	if hasMore {
		records = records[:maxCount]
	}
	
	var nextLSN uint64
	if len(records) > 0 {
		nextLSN = records[len(records)-1].Header.LSN + 1
	} else {
		nextLSN = fromLSN
	}
	
	return records, nextLSN, hasMore, nil
}

// Freeze freezes the volume and returns the final LSN
func (vs *VolumeStore) Freeze() (uint64, error) {
	vs.mu.Lock()
	defer vs.mu.Unlock()
	
	vs.frozen = true
	if err := vs.walWriter.Sync(); err != nil {
		return 0, err
	}
	
	return vs.currentLSN, nil
}

// Unfreeze unfreezes the volume
func (vs *VolumeStore) Unfreeze() error {
	vs.mu.Lock()
	defer vs.mu.Unlock()
	
	vs.frozen = false
	return nil
}

// GetStats returns volume statistics
func (vs *VolumeStore) GetStats() VolumeStats {
	vs.mu.RLock()
	defer vs.mu.RUnlock()
	
	return VolumeStats{
		CurrentLSN: vs.currentLSN,
		PageCount:  int64(vs.pageCache.Len()),
	}
}

// Close closes the volume store
func (vs *VolumeStore) Close() error {
	vs.mu.Lock()
	defer vs.mu.Unlock()
	
	if vs.walWriter != nil {
		vs.walWriter.Close()
	}
	if vs.walReader != nil {
		vs.walReader.Close()
	}
	
	return nil
}

// Errors
var (
	ErrWritesFrozen = fmt.Errorf("writes are frozen")
	ErrVolumeNotFound = fmt.Errorf("volume not found")
)

// QuorumConfig returns the default quorum configuration
func QuorumConfig() quorum.Config {
	return quorum.DefaultConfig()
}
