package transport

import (
	"context"
	"sync"
	"sync/atomic"
)

// RDMATransport implements Transport using RDMA
// Note: This is a simulation - real RDMA requires libibverbs
type RDMATransport struct {
	transportType TransportType
	config        *TransportConfig
	
	// Queue Pairs per connection
	queuePairs map[string]*QueuePair
	qpMu       sync.RWMutex
	
	// Completion Queue
	completionQueue chan *WorkCompletion
	
	// Memory Regions
	memoryRegions map[uintptr]*MemoryRegion
	mrMu          sync.RWMutex
	
	// Statistics
	stats TransportStats
	
	// State
	initialized bool
	shutdown    bool
	mu          sync.Mutex
}

// QueuePair represents an RDMA Queue Pair
type QueuePair struct {
	RemoteAddr string
	LocalQPN   uint32 // Local Queue Pair Number
	RemoteQPN  uint32 // Remote Queue Pair Number
	PSN        uint32 // Packet Sequence Number
	State      QPState
	
	// Work Queues
	SendQueue chan *WorkRequest
	RecvQueue chan *WorkRequest
}

// QPState represents Queue Pair state
type QPState int

const (
	QPStateInit QPState = iota
	QPStateRTR  // Ready to Receive
	QPStateRTS  // Ready to Send
	QPStateError
)

// WorkRequest represents an RDMA work request
type WorkRequest struct {
	ID        uint64
	OpCode    WROpCode
	LocalAddr uintptr
	Length    uint32
	LKey      uint32
	RemoteAddr uint64
	RKey      uint32
	Flags     uint32
}

// WROpCode represents work request operation code
type WROpCode int

const (
	WRSend WROpCode = iota
	WRRecv
	WRRDMAWrite
	WRRDMARead
	WRAtomic
)

// WorkCompletion represents work completion notification
type WorkCompletion struct {
	ID        uint64
	OpCode    WROpCode
	Status    WCStatus
	ByteLen   uint32
	QPN       uint32
}

// WCStatus represents work completion status
type WCStatus int

const (
	WCSuccess WCStatus = iota
	WCError
	WCFlushError
)

// NewRDMATransport creates a new RDMA transport
func NewRDMATransport(transportType TransportType) *RDMATransport {
	return &RDMATransport{
		transportType:   transportType,
		queuePairs:      make(map[string]*QueuePair),
		completionQueue: make(chan *WorkCompletion, 4096),
		memoryRegions:   make(map[uintptr]*MemoryRegion),
	}
}

// Initialize initializes the RDMA transport
func (t *RDMATransport) Initialize(config *TransportConfig) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	if t.initialized {
		return nil
	}
	
	t.config = config
	
	// In real implementation:
	// 1. Open RDMA device (ibv_open_device)
	// 2. Create Protection Domain (ibv_alloc_pd)
	// 3. Create Completion Queue (ibv_create_cq)
	// 4. Start completion polling thread
	
	t.initialized = true
	return nil
}

// Connect creates a Queue Pair connection to remote address
func (t *RDMATransport) Connect(ctx context.Context, remoteAddr string, port int) error {
	t.qpMu.Lock()
	defer t.qpMu.Unlock()
	
	if _, exists := t.queuePairs[remoteAddr]; exists {
		return nil
	}
	
	// In real implementation:
	// 1. Create Queue Pair (ibv_create_qp)
	// 2. Exchange QP info with remote (via TCP out-of-band)
	// 3. Transition QP to RTR then RTS state
	
	qp := &QueuePair{
		RemoteAddr: remoteAddr,
		LocalQPN:   generateQPN(),
		State:      QPStateRTS,
		SendQueue:  make(chan *WorkRequest, t.config.RDMA.MaxQPWR),
		RecvQueue:  make(chan *WorkRequest, t.config.RDMA.MaxQPWR),
	}
	
	t.queuePairs[remoteAddr] = qp
	t.stats.Connections++
	
	return nil
}

// Disconnect closes the Queue Pair
func (t *RDMATransport) Disconnect(remoteAddr string) error {
	t.qpMu.Lock()
	defer t.qpMu.Unlock()
	
	qp, exists := t.queuePairs[remoteAddr]
	if !exists {
		return nil
	}
	
	qp.State = QPStateError
	close(qp.SendQueue)
	close(qp.RecvQueue)
	delete(t.queuePairs, remoteAddr)
	t.stats.Connections--
	
	return nil
}

// IsConnected checks if connected
func (t *RDMATransport) IsConnected(remoteAddr string) bool {
	t.qpMu.RLock()
	defer t.qpMu.RUnlock()
	
	qp, exists := t.queuePairs[remoteAddr]
	return exists && qp.State == QPStateRTS
}

// Send sends data using RDMA Send operation
func (t *RDMATransport) Send(ctx context.Context, remoteAddr string, data []byte) error {
	t.qpMu.RLock()
	qp, exists := t.queuePairs[remoteAddr]
	t.qpMu.RUnlock()
	
	if !exists || qp.State != QPStateRTS {
		return ErrNotConnected
	}
	
	// In real implementation:
	// 1. Register memory if not already (ibv_reg_mr)
	// 2. Create Send WR with inline data or registered buffer
	// 3. Post to Send Queue (ibv_post_send)
	// 4. Poll CQ for completion
	
	atomic.AddUint64(&t.stats.BytesSent, uint64(len(data)))
	atomic.AddUint64(&t.stats.MessagesSent, 1)
	
	return nil
}

// Recv receives data using RDMA Recv operation
func (t *RDMATransport) Recv(ctx context.Context, buf []byte, timeoutMs int) (remoteAddr string, n int, err error) {
	// In real implementation:
	// 1. Post Receive WR (ibv_post_recv)
	// 2. Poll CQ for receive completion
	// 3. Return data
	
	select {
	case <-ctx.Done():
		return "", 0, ctx.Err()
	case wc := <-t.completionQueue:
		if wc.Status != WCSuccess {
			return "", 0, ErrRDMAError
		}
		return "", int(wc.ByteLen), nil
	}
}

// SendAsync sends data asynchronously
func (t *RDMATransport) SendAsync(remoteAddr string, data []byte, callback SendCallback) {
	go func() {
		ctx := context.Background()
		err := t.Send(ctx, remoteAddr, data)
		if callback != nil {
			callback(err)
		}
	}()
}

// RecvAsync receives data asynchronously
func (t *RDMATransport) RecvAsync(callback RecvCallback) {
	go func() {
		for wc := range t.completionQueue {
			if wc.OpCode == WRRecv {
				if callback != nil {
					callback("", nil, nil)
				}
			}
		}
	}()
}

// RDMAWrite performs RDMA Write (one-sided operation)
func (t *RDMATransport) RDMAWrite(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, data []byte) error {
	t.qpMu.RLock()
	qp, exists := t.queuePairs[remoteAddr]
	t.qpMu.RUnlock()
	
	if !exists || qp.State != QPStateRTS {
		return ErrNotConnected
	}
	
	// In real implementation:
	// 1. Create RDMA Write WR with remote address and rkey
	// 2. Post to Send Queue
	// 3. Poll CQ for completion
	
	wr := &WorkRequest{
		ID:         generateWRID(),
		OpCode:     WRRDMAWrite,
		RemoteAddr: remotePtr,
		RKey:       remoteRKey,
		Length:     uint32(len(data)),
	}
	
	select {
	case qp.SendQueue <- wr:
	case <-ctx.Done():
		return ctx.Err()
	}
	
	atomic.AddUint64(&t.stats.BytesSent, uint64(len(data)))
	return nil
}

// RDMARead performs RDMA Read (one-sided operation)
func (t *RDMATransport) RDMARead(ctx context.Context, remoteAddr string, remotePtr uint64, remoteRKey uint32, buf []byte) error {
	t.qpMu.RLock()
	qp, exists := t.queuePairs[remoteAddr]
	t.qpMu.RUnlock()
	
	if !exists || qp.State != QPStateRTS {
		return ErrNotConnected
	}
	
	// In real implementation:
	// 1. Create RDMA Read WR with remote address and rkey
	// 2. Post to Send Queue
	// 3. Poll CQ for completion
	
	wr := &WorkRequest{
		ID:         generateWRID(),
		OpCode:     WRRDMARead,
		RemoteAddr: remotePtr,
		RKey:       remoteRKey,
		Length:     uint32(len(buf)),
	}
	
	select {
	case qp.SendQueue <- wr:
	case <-ctx.Done():
		return ctx.Err()
	}
	
	atomic.AddUint64(&t.stats.BytesReceived, uint64(len(buf)))
	return nil
}

// RegisterMemory registers a memory region for RDMA
func (t *RDMATransport) RegisterMemory(addr uintptr, length uint64) (*MemoryRegion, error) {
	t.mrMu.Lock()
	defer t.mrMu.Unlock()
	
	// In real implementation:
	// ibv_reg_mr(pd, addr, length, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)
	
	mr := &MemoryRegion{
		Addr:   addr,
		Length: length,
		LKey:   generateKey(),
		RKey:   generateKey(),
	}
	
	t.memoryRegions[addr] = mr
	return mr, nil
}

// DeregisterMemory deregisters a memory region
func (t *RDMATransport) DeregisterMemory(mr *MemoryRegion) error {
	t.mrMu.Lock()
	defer t.mrMu.Unlock()
	
	// In real implementation:
	// ibv_dereg_mr(mr)
	
	delete(t.memoryRegions, mr.Addr)
	return nil
}

// GetType returns the transport type
func (t *RDMATransport) GetType() TransportType {
	return t.transportType
}

// GetStats returns transport statistics
func (t *RDMATransport) GetStats() *TransportStats {
	return &t.stats
}

// Shutdown shuts down the transport
func (t *RDMATransport) Shutdown() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	
	if t.shutdown {
		return nil
	}
	
	// Close all queue pairs
	t.qpMu.Lock()
	for addr := range t.queuePairs {
		t.Disconnect(addr)
	}
	t.qpMu.Unlock()
	
	// Deregister all memory regions
	t.mrMu.Lock()
	for _, mr := range t.memoryRegions {
		delete(t.memoryRegions, mr.Addr)
	}
	t.mrMu.Unlock()
	
	close(t.completionQueue)
	t.shutdown = true
	return nil
}

// Helper functions
var (
	qpnCounter  uint32
	wridCounter uint64
	keyCounter  uint32
)

func generateQPN() uint32 {
	return atomic.AddUint32(&qpnCounter, 1)
}

func generateWRID() uint64 {
	return atomic.AddUint64(&wridCounter, 1)
}

func generateKey() uint32 {
	return atomic.AddUint32(&keyCounter, 1)
}

// Errors
var (
	ErrRDMAError = NewTransportError("rdma error")
)
