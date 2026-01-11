// Package transport provides network transport abstraction for Aurora
package transport

import (
	"context"
	"time"
)

// TransportType defines the type of network transport
type TransportType int

const (
	TransportTCPGRPC TransportType = iota // TCP/IP + gRPC
	TransportRDMARC                       // RDMA Reliable Connection
	TransportRDMAUD                       // RDMA Unreliable Datagram
)

func (t TransportType) String() string {
	switch t {
	case TransportTCPGRPC:
		return "tcp_grpc"
	case TransportRDMARC:
		return "rdma_rc"
	case TransportRDMAUD:
		return "rdma_ud"
	default:
		return "unknown"
	}
}

// TransportConfig holds transport configuration
type TransportConfig struct {
	Type      TransportType
	LocalAddr string
	LocalPort int
	
	// TCP/gRPC configuration
	TCP TCPConfig
	
	// RDMA configuration
	RDMA RDMAConfig
}

// TCPConfig holds TCP/gRPC specific configuration
type TCPConfig struct {
	MaxConnections   int
	KeepaliveTimeMs  int
	MaxMessageSize   int
	ConnectionPool   int
	IdleTimeout      time.Duration
}

// RDMAConfig holds RDMA specific configuration
type RDMAConfig struct {
	DeviceName    string // e.g., "mlx5_0"
	PortNum       int    // IB port number
	GIDIndex      int    // GID index
	MaxQPWR       int    // Max QP work requests
	MaxCQEntries  int    // Max CQ entries
	MaxSendSGE    int    // Max send scatter/gather elements
	MaxRecvSGE    int    // Max receive scatter/gather elements
	InlineSize    int    // Inline data size
	UseSRQ        bool   // Use Shared Receive Queue
	MaxMRSize     uint64 // Max memory region size
}

// DefaultTCPConfig returns default TCP configuration
func DefaultTCPConfig() TCPConfig {
	return TCPConfig{
		MaxConnections:  100,
		KeepaliveTimeMs: 30000,
		MaxMessageSize:  64 * 1024 * 1024, // 64MB
		ConnectionPool:  10,
		IdleTimeout:     5 * time.Minute,
	}
}

// DefaultRDMAConfig returns default RDMA configuration
func DefaultRDMAConfig() RDMAConfig {
	return RDMAConfig{
		DeviceName:   "mlx5_0",
		PortNum:      1,
		GIDIndex:     0,
		MaxQPWR:      1024,
		MaxCQEntries: 4096,
		MaxSendSGE:   16,
		MaxRecvSGE:   16,
		InlineSize:   256,
		UseSRQ:       true,
		MaxMRSize:    1 << 30, // 1GB
	}
}

// SendCallback is called when async send completes
type SendCallback func(err error)

// RecvCallback is called when async receive completes
type RecvCallback func(remoteAddr string, data []byte, err error)

// MemoryRegion represents a registered memory region for RDMA
type MemoryRegion struct {
	Addr   uintptr
	Length uint64
	LKey   uint32
	RKey   uint32
}

// Transport defines the abstract transport interface
type Transport interface {
	// Initialize initializes the transport
	Initialize(config *TransportConfig) error
	
	// Connection management
	Connect(ctx context.Context, remoteAddr string, port int) error
	Disconnect(remoteAddr string) error
	IsConnected(remoteAddr string) bool
	
	// Synchronous send/receive
	Send(ctx context.Context, remoteAddr string, data []byte) error
	Recv(ctx context.Context, buf []byte, timeoutMs int) (remoteAddr string, n int, err error)
	
	// Asynchronous send/receive
	SendAsync(remoteAddr string, data []byte, callback SendCallback)
	RecvAsync(callback RecvCallback)
	
	// RDMA-specific operations (only work in RDMA mode)
	RDMAWrite(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, data []byte) error
	RDMARead(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, buf []byte) error
	
	// Memory registration (only for RDMA)
	RegisterMemory(addr uintptr, length uint64) (*MemoryRegion, error)
	DeregisterMemory(mr *MemoryRegion) error
	
	// Status and control
	GetType() TransportType
	GetStats() *TransportStats
	Shutdown() error
}

// TransportStats holds transport statistics
type TransportStats struct {
	BytesSent       uint64
	BytesReceived   uint64
	MessagesSent    uint64
	MessagesRecv    uint64
	Errors          uint64
	Connections     int
	AvgLatencyUs    float64
	P99LatencyUs    float64
}

// CreateTransport creates a transport of the specified type
func CreateTransport(transportType TransportType) Transport {
	switch transportType {
	case TransportTCPGRPC:
		return NewTCPTransport()
	case TransportRDMARC, TransportRDMAUD:
		return NewRDMATransport(transportType)
	default:
		return NewTCPTransport()
	}
}
