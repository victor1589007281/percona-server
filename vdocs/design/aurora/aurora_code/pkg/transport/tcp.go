package transport

import (
	"context"
	"sync"
	"sync/atomic"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/keepalive"
)

// TCPTransport implements Transport using TCP/gRPC
type TCPTransport struct {
	config      *TransportConfig
	connections map[string]*grpc.ClientConn
	connMu      sync.RWMutex
	
	// Statistics
	stats TransportStats
	
	// State
	initialized bool
	shutdown    bool
	mu          sync.Mutex
}

// NewTCPTransport creates a new TCP transport
func NewTCPTransport() *TCPTransport {
	return &TCPTransport{
		connections: make(map[string]*grpc.ClientConn),
	}
}

// Initialize initializes the TCP transport
func (t *TCPTransport) Initialize(config *TransportConfig) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	if t.initialized {
		return nil
	}
	
	t.config = config
	t.initialized = true
	return nil
}

// Connect connects to a remote address
func (t *TCPTransport) Connect(ctx context.Context, remoteAddr string, port int) error {
	t.connMu.Lock()
	defer t.connMu.Unlock()
	
	if _, exists := t.connections[remoteAddr]; exists {
		return nil // Already connected
	}
	
	opts := []grpc.DialOption{
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithKeepaliveParams(keepalive.ClientParameters{
			Time:                time.Duration(t.config.TCP.KeepaliveTimeMs) * time.Millisecond,
			Timeout:             20 * time.Second,
			PermitWithoutStream: true,
		}),
		grpc.WithDefaultCallOptions(
			grpc.MaxCallRecvMsgSize(t.config.TCP.MaxMessageSize),
			grpc.MaxCallSendMsgSize(t.config.TCP.MaxMessageSize),
		),
	}
	
	target := remoteAddr
	if port > 0 {
		target = remoteAddr + ":" + string(rune(port))
	}
	
	conn, err := grpc.DialContext(ctx, target, opts...)
	if err != nil {
		return err
	}
	
	t.connections[remoteAddr] = conn
	t.stats.Connections++
	return nil
}

// Disconnect disconnects from a remote address
func (t *TCPTransport) Disconnect(remoteAddr string) error {
	t.connMu.Lock()
	defer t.connMu.Unlock()
	
	conn, exists := t.connections[remoteAddr]
	if !exists {
		return nil
	}
	
	delete(t.connections, remoteAddr)
	t.stats.Connections--
	return conn.Close()
}

// IsConnected checks if connected to a remote address
func (t *TCPTransport) IsConnected(remoteAddr string) bool {
	t.connMu.RLock()
	defer t.connMu.RUnlock()
	
	_, exists := t.connections[remoteAddr]
	return exists
}

// Send sends data to a remote address
func (t *TCPTransport) Send(ctx context.Context, remoteAddr string, data []byte) error {
	t.connMu.RLock()
	conn, exists := t.connections[remoteAddr]
	t.connMu.RUnlock()
	
	if !exists {
		return ErrNotConnected
	}
	
	// In real implementation, this would use a gRPC stub
	_ = conn
	
	atomic.AddUint64(&t.stats.BytesSent, uint64(len(data)))
	atomic.AddUint64(&t.stats.MessagesSent, 1)
	
	return nil
}

// Recv receives data from any remote address
func (t *TCPTransport) Recv(ctx context.Context, buf []byte, timeoutMs int) (remoteAddr string, n int, err error) {
	// In real implementation, this would use gRPC server streaming
	select {
	case <-ctx.Done():
		return "", 0, ctx.Err()
	case <-time.After(time.Duration(timeoutMs) * time.Millisecond):
		return "", 0, ErrTimeout
	}
}

// SendAsync sends data asynchronously
func (t *TCPTransport) SendAsync(remoteAddr string, data []byte, callback SendCallback) {
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		
		err := t.Send(ctx, remoteAddr, data)
		if callback != nil {
			callback(err)
		}
	}()
}

// RecvAsync receives data asynchronously
func (t *TCPTransport) RecvAsync(callback RecvCallback) {
	// In real implementation, this would start a background receiver
}

// RDMA operations not supported in TCP mode
func (t *TCPTransport) RDMAWrite(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, data []byte) error {
	return ErrNotSupported
}

func (t *TCPTransport) RDMARead(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, buf []byte) error {
	return ErrNotSupported
}

func (t *TCPTransport) RegisterMemory(addr uintptr, length uint64) (*MemoryRegion, error) {
	return nil, ErrNotSupported
}

func (t *TCPTransport) DeregisterMemory(mr *MemoryRegion) error {
	return ErrNotSupported
}

// GetType returns the transport type
func (t *TCPTransport) GetType() TransportType {
	return TransportTCPGRPC
}

// GetStats returns transport statistics
func (t *TCPTransport) GetStats() *TransportStats {
	return &t.stats
}

// Shutdown shuts down the transport
func (t *TCPTransport) Shutdown() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	if t.shutdown {
		return nil
	}
	
	t.connMu.Lock()
	for addr, conn := range t.connections {
		conn.Close()
		delete(t.connections, addr)
	}
	t.connMu.Unlock()
	
	t.shutdown = true
	return nil
}

// Errors
var (
	ErrNotConnected = NewTransportError("not connected")
	ErrNotSupported = NewTransportError("not supported")
	ErrTimeout      = NewTransportError("timeout")
)

type TransportError struct {
	msg string
}

func NewTransportError(msg string) *TransportError {
	return &TransportError{msg: msg}
}

func (e *TransportError) Error() string {
	return e.msg
}
