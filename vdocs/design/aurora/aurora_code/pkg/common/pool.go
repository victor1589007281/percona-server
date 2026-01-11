package common

import (
	"context"
	"errors"
	"sync"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// ConnectionPool manages a pool of gRPC connections
type ConnectionPool struct {
	mu       sync.Mutex
	config   NetworkConfig
	pools    map[string]*singlePool
}

type singlePool struct {
	address     string
	connections chan *grpc.ClientConn
	created     int
	maxSize     int
}

// NewConnectionPool creates a new connection pool
func NewConnectionPool(config NetworkConfig) *ConnectionPool {
	return &ConnectionPool{
		config: config,
		pools:  make(map[string]*singlePool),
	}
}

// Get gets a connection from the pool
func (p *ConnectionPool) Get(ctx context.Context, address string) (*grpc.ClientConn, error) {
	p.mu.Lock()
	pool, ok := p.pools[address]
	if !ok {
		pool = &singlePool{
			address:     address,
			connections: make(chan *grpc.ClientConn, p.config.MaxConnections),
			maxSize:     p.config.MaxConnections,
		}
		p.pools[address] = pool
	}
	p.mu.Unlock()
	
	// Try to get from pool
	select {
	case conn := <-pool.connections:
		return conn, nil
	default:
	}
	
	// Create new connection if under limit
	p.mu.Lock()
	if pool.created < pool.maxSize {
		pool.created++
		p.mu.Unlock()
		
		return p.createConnection(ctx, address)
	}
	p.mu.Unlock()
	
	// Wait for available connection
	select {
	case conn := <-pool.connections:
		return conn, nil
	case <-ctx.Done():
		return nil, ctx.Err()
	case <-time.After(p.config.ConnectionTimeout):
		return nil, ErrPoolTimeout
	}
}

// Put returns a connection to the pool
func (p *ConnectionPool) Put(address string, conn *grpc.ClientConn) {
	p.mu.Lock()
	pool, ok := p.pools[address]
	p.mu.Unlock()
	
	if !ok {
		conn.Close()
		return
	}
	
	select {
	case pool.connections <- conn:
	default:
		// Pool is full, close connection
		conn.Close()
		p.mu.Lock()
		pool.created--
		p.mu.Unlock()
	}
}

// createConnection creates a new gRPC connection
func (p *ConnectionPool) createConnection(ctx context.Context, address string) (*grpc.ClientConn, error) {
	opts := []grpc.DialOption{
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	}
	
	dialCtx, cancel := context.WithTimeout(ctx, p.config.ConnectionTimeout)
	defer cancel()
	
	conn, err := grpc.DialContext(dialCtx, address, opts...)
	if err != nil {
		p.mu.Lock()
		if pool, ok := p.pools[address]; ok {
			pool.created--
		}
		p.mu.Unlock()
		return nil, err
	}
	
	return conn, nil
}

// Close closes all connections in the pool
func (p *ConnectionPool) Close() {
	p.mu.Lock()
	defer p.mu.Unlock()
	
	for _, pool := range p.pools {
		close(pool.connections)
		for conn := range pool.connections {
			conn.Close()
		}
	}
	
	p.pools = make(map[string]*singlePool)
}

// Stats returns pool statistics
func (p *ConnectionPool) Stats() map[string]PoolStats {
	p.mu.Lock()
	defer p.mu.Unlock()
	
	stats := make(map[string]PoolStats)
	for addr, pool := range p.pools {
		stats[addr] = PoolStats{
			Address:     addr,
			Total:       pool.created,
			Available:   len(pool.connections),
			MaxSize:     pool.maxSize,
		}
	}
	return stats
}

// PoolStats contains connection pool statistics
type PoolStats struct {
	Address   string
	Total     int
	Available int
	MaxSize   int
}

// Errors
var (
	ErrPoolTimeout   = errors.New("connection pool timeout")
	ErrPoolClosed    = errors.New("connection pool closed")
)
