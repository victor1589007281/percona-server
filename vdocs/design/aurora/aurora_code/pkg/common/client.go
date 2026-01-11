package common

import (
	"context"
	"sync"
	"time"
)

// StorageClient is a client for storage nodes
type StorageClient struct {
	pool     *ConnectionPool
	config   NetworkConfig
	nodes    []string
	mu       sync.RWMutex
}

// NewStorageClient creates a new storage client
func NewStorageClient(nodes []string, config NetworkConfig) *StorageClient {
	return &StorageClient{
		pool:   NewConnectionPool(config),
		config: config,
		nodes:  nodes,
	}
}

// WriteRedoToAll writes redo to all storage nodes in parallel
func (c *StorageClient) WriteRedoToAll(ctx context.Context, volumeID string, lsn int64, data []byte) ([]WriteResult, error) {
	c.mu.RLock()
	nodes := make([]string, len(c.nodes))
	copy(nodes, c.nodes)
	c.mu.RUnlock()
	
	results := make([]WriteResult, len(nodes))
	var wg sync.WaitGroup
	
	for i, node := range nodes {
		wg.Add(1)
		go func(idx int, addr string) {
			defer wg.Done()
			
			start := time.Now()
			err := c.writeRedoToNode(ctx, addr, volumeID, lsn, data)
			
			results[idx] = WriteResult{
				NodeID:   addr,
				Success:  err == nil,
				Error:    err,
				Latency:  time.Since(start),
			}
		}(i, node)
	}
	
	wg.Wait()
	return results, nil
}

func (c *StorageClient) writeRedoToNode(ctx context.Context, addr, volumeID string, lsn int64, data []byte) error {
	conn, err := c.pool.Get(ctx, addr)
	if err != nil {
		return err
	}
	defer c.pool.Put(addr, conn)
	
	// In real implementation, call gRPC StorageService.WriteRedo
	_ = conn
	return nil
}

// ReadPage reads a page from a storage node
func (c *StorageClient) ReadPage(ctx context.Context, volumeID string, spaceID, pageID int64, targetLSN int64) ([]byte, error) {
	c.mu.RLock()
	nodes := make([]string, len(c.nodes))
	copy(nodes, c.nodes)
	c.mu.RUnlock()
	
	// Try nodes in order until one succeeds
	var lastErr error
	for _, node := range nodes {
		data, err := c.readPageFromNode(ctx, node, volumeID, spaceID, pageID, targetLSN)
		if err == nil {
			return data, nil
		}
		lastErr = err
	}
	
	return nil, lastErr
}

func (c *StorageClient) readPageFromNode(ctx context.Context, addr, volumeID string, spaceID, pageID, targetLSN int64) ([]byte, error) {
	conn, err := c.pool.Get(ctx, addr)
	if err != nil {
		return nil, err
	}
	defer c.pool.Put(addr, conn)
	
	// In real implementation, call gRPC StorageService.ReadPage
	_ = conn
	return nil, nil
}

// Close closes the client
func (c *StorageClient) Close() {
	c.pool.Close()
}

// WriteResult represents the result of a write operation
type WriteResult struct {
	NodeID  string
	Success bool
	Error   error
	Latency time.Duration
	LSN     int64
}

// MetadataClient is a client for metadata service
type MetadataClient struct {
	pool    *ConnectionPool
	config  NetworkConfig
	leader  string
	peers   []string
	mu      sync.RWMutex
}

// NewMetadataClient creates a new metadata client
func NewMetadataClient(peers []string, config NetworkConfig) *MetadataClient {
	return &MetadataClient{
		pool:   NewConnectionPool(config),
		config: config,
		peers:  peers,
	}
}

// UpdateVDL updates the VDL
func (c *MetadataClient) UpdateVDL(ctx context.Context, volumeID string, newVDL int64, instanceID string) error {
	return Retry(ctx, DefaultRetryConfig(), func(ctx context.Context) error {
		return c.doUpdateVDL(ctx, volumeID, newVDL, instanceID)
	})
}

func (c *MetadataClient) doUpdateVDL(ctx context.Context, volumeID string, newVDL int64, instanceID string) error {
	c.mu.RLock()
	leader := c.leader
	c.mu.RUnlock()
	
	if leader == "" {
		leader = c.peers[0]
	}
	
	conn, err := c.pool.Get(ctx, leader)
	if err != nil {
		return err
	}
	defer c.pool.Put(leader, conn)
	
	// In real implementation, call gRPC MetadataService.UpdateVDL
	_ = conn
	return nil
}

// GetVDL gets the current VDL
func (c *MetadataClient) GetVDL(ctx context.Context, volumeID string) (int64, error) {
	c.mu.RLock()
	peers := make([]string, len(c.peers))
	copy(peers, c.peers)
	c.mu.RUnlock()
	
	for _, peer := range peers {
		vdl, err := c.getVDLFromNode(ctx, peer, volumeID)
		if err == nil {
			return vdl, nil
		}
	}
	
	return 0, ErrNoAvailableNodes
}

func (c *MetadataClient) getVDLFromNode(ctx context.Context, addr, volumeID string) (int64, error) {
	conn, err := c.pool.Get(ctx, addr)
	if err != nil {
		return 0, err
	}
	defer c.pool.Put(addr, conn)
	
	// In real implementation, call gRPC MetadataService.GetVDL
	_ = conn
	return 0, nil
}

// Close closes the client
func (c *MetadataClient) Close() {
	c.pool.Close()
}

// Errors
var (
	ErrNoAvailableNodes = NewError("no available nodes")
)

// Error is a custom error type
type Error struct {
	message string
}

func NewError(msg string) *Error {
	return &Error{message: msg}
}

func (e *Error) Error() string {
	return e.message
}
