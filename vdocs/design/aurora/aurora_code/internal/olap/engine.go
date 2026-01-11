package olap

import (
	"context"
	"encoding/binary"
	"hash/crc32"
	"sync"
	"sync/atomic"
	"time"
)

// OLAPEngine is the main OLAP storage engine
type OLAPEngine struct {
	config  *OLAPConfig
	tables  map[uint64]*ColumnTable
	tablesMu sync.RWMutex
	
	// Write buffer (MemTable)
	memTable    *MemTable
	immutables  []*MemTable
	memMu       sync.RWMutex
	
	// Background tasks
	compactionMgr *CompactionManager
	gcMgr         *GCManager
	
	// Statistics
	stats EngineStats
	
	running bool
	mu      sync.Mutex
}

// EngineStats holds engine statistics
type EngineStats struct {
	RowsWritten    uint64
	BytesWritten   uint64
	RowsRead       uint64
	BytesRead      uint64
	Compactions    uint64
	MemTableFlush  uint64
}

// MemTable is an in-memory write buffer
type MemTable struct {
	data       map[string][]byte // key -> encoded row
	mu         sync.RWMutex
	size       int64
	maxSize    int64
	createTime time.Time
}

// NewMemTable creates a new MemTable
func NewMemTable(maxSize int64) *MemTable {
	return &MemTable{
		data:       make(map[string][]byte),
		maxSize:    maxSize,
		createTime: time.Now(),
	}
}

// CompactionManager manages background compaction
type CompactionManager struct {
	engine   *OLAPEngine
	stopCh   chan struct{}
	running  bool
}

// GCManager manages garbage collection
type GCManager struct {
	engine   *OLAPEngine
	stopCh   chan struct{}
	running  bool
}

// NewOLAPEngine creates a new OLAP engine
func NewOLAPEngine(config *OLAPConfig) *OLAPEngine {
	if config == nil {
		config = DefaultOLAPConfig()
	}
	
	engine := &OLAPEngine{
		config:   config,
		tables:   make(map[uint64]*ColumnTable),
		memTable: NewMemTable(int64(config.WriteBufferSize) * 1024 * 1024),
	}
	
	engine.compactionMgr = &CompactionManager{engine: engine, stopCh: make(chan struct{})}
	engine.gcMgr = &GCManager{engine: engine, stopCh: make(chan struct{})}
	
	return engine
}

// Start starts the OLAP engine
func (e *OLAPEngine) Start() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	
	if e.running {
		return nil
	}
	
	// Start background tasks
	go e.compactionMgr.Run()
	go e.gcMgr.Run()
	
	e.running = true
	return nil
}

// Stop stops the OLAP engine
func (e *OLAPEngine) Stop() error {
	e.mu.Lock()
	defer e.mu.Unlock()
	
	if !e.running {
		return nil
	}
	
	close(e.compactionMgr.stopCh)
	close(e.gcMgr.stopCh)
	
	// Flush remaining data
	e.flushMemTable()
	
	e.running = false
	return nil
}

// CreateTable creates a new column table
func (e *OLAPEngine) CreateTable(table *ColumnTable) error {
	e.tablesMu.Lock()
	defer e.tablesMu.Unlock()
	
	if _, exists := e.tables[table.TableID]; exists {
		return ErrTableExists
	}
	
	e.tables[table.TableID] = table
	return nil
}

// DropTable drops a column table
func (e *OLAPEngine) DropTable(tableID uint64) error {
	e.tablesMu.Lock()
	defer e.tablesMu.Unlock()
	
	if _, exists := e.tables[tableID]; !exists {
		return ErrTableNotFound
	}
	
	delete(e.tables, tableID)
	return nil
}

// WriteColumnBatch writes a batch of columnar data
func (e *OLAPEngine) WriteColumnBatch(ctx context.Context, batch *ColumnBatch) error {
	e.tablesMu.RLock()
	table, exists := e.tables[batch.TableID]
	e.tablesMu.RUnlock()
	
	if !exists {
		return ErrTableNotFound
	}
	
	// Encode batch to bytes
	encoded := e.encodeBatch(table, batch)
	
	// Write to MemTable
	e.memMu.Lock()
	for i := 0; i < batch.RowCount; i++ {
		key := e.buildKey(batch.TableID, batch.SortKeys[i], batch.RowIDs[i])
		e.memTable.data[key] = encoded[i]
		atomic.AddInt64(&e.memTable.size, int64(len(encoded[i])))
	}
	
	// Check if need to flush
	if e.memTable.size >= e.memTable.maxSize {
		e.immutables = append(e.immutables, e.memTable)
		e.memTable = NewMemTable(int64(e.config.WriteBufferSize) * 1024 * 1024)
		go e.flushImmutables()
	}
	e.memMu.Unlock()
	
	atomic.AddUint64(&e.stats.RowsWritten, uint64(batch.RowCount))
	
	return nil
}

// encodeBatch encodes a column batch to bytes
func (e *OLAPEngine) encodeBatch(table *ColumnTable, batch *ColumnBatch) [][]byte {
	result := make([][]byte, batch.RowCount)
	
	for i := 0; i < batch.RowCount; i++ {
		var buf []byte
		
		// Encode each column value
		for colIdx, col := range batch.Columns {
			colMeta := table.Columns[colIdx]
			value := e.getColumnValue(col, i)
			
			// Add column encoding
			buf = append(buf, byte(colMeta.ColumnID))
			buf = append(buf, byte(len(value)))
			buf = append(buf, value...)
		}
		
		result[i] = buf
	}
	
	return result
}

// getColumnValue extracts a single value from column vector
func (e *OLAPEngine) getColumnValue(col ColumnVector, rowIdx int) []byte {
	size := col.DataType.Size()
	if size == 0 {
		// Variable length - stored with length prefix
		return nil
	}
	
	start := rowIdx * size
	end := start + size
	if end > len(col.Data) {
		return nil
	}
	
	return col.Data[start:end]
}

// buildKey builds a key for storage
func (e *OLAPEngine) buildKey(tableID uint64, sortKey []byte, rowID uint64) string {
	key := make([]byte, 8+len(sortKey)+8)
	binary.BigEndian.PutUint64(key[0:8], tableID)
	copy(key[8:8+len(sortKey)], sortKey)
	binary.BigEndian.PutUint64(key[8+len(sortKey):], rowID)
	return string(key)
}

// Query executes an OLAP query
func (e *OLAPEngine) Query(ctx context.Context, query *OLAPQuery) (*QueryResult, error) {
	e.tablesMu.RLock()
	table, exists := e.tables[query.TableID]
	e.tablesMu.RUnlock()
	
	if !exists {
		return nil, ErrTableNotFound
	}
	
	// Create query executor
	executor := NewVectorizedExecutor(e, table)
	
	return executor.Execute(ctx, query)
}

// flushMemTable flushes current MemTable
func (e *OLAPEngine) flushMemTable() {
	e.memMu.Lock()
	if e.memTable.size > 0 {
		e.immutables = append(e.immutables, e.memTable)
		e.memTable = NewMemTable(int64(e.config.WriteBufferSize) * 1024 * 1024)
	}
	e.memMu.Unlock()
	
	e.flushImmutables()
}

// flushImmutables flushes immutable MemTables to SST files
func (e *OLAPEngine) flushImmutables() {
	e.memMu.Lock()
	toFlush := e.immutables
	e.immutables = nil
	e.memMu.Unlock()
	
	for _, mem := range toFlush {
		e.flushToSST(mem)
		atomic.AddUint64(&e.stats.MemTableFlush, 1)
	}
}

// flushToSST flushes a MemTable to SST file
func (e *OLAPEngine) flushToSST(mem *MemTable) error {
	// In real implementation:
	// 1. Sort keys
	// 2. Build column blocks with encoding
	// 3. Compress blocks
	// 4. Write to SST file with index
	return nil
}

// GetStats returns engine statistics
func (e *OLAPEngine) GetStats() *EngineStats {
	return &e.stats
}

// CompactionManager.Run runs compaction loop
func (cm *CompactionManager) Run() {
	ticker := time.NewTicker(10 * time.Second)
	defer ticker.Stop()
	
	cm.running = true
	for {
		select {
		case <-cm.stopCh:
			cm.running = false
			return
		case <-ticker.C:
			cm.maybeCompact()
		}
	}
}

func (cm *CompactionManager) maybeCompact() {
	// In real implementation:
	// 1. Check if compaction is needed (too many L0 files)
	// 2. Pick files to compact
	// 3. Merge and rewrite
	atomic.AddUint64(&cm.engine.stats.Compactions, 1)
}

// GCManager.Run runs GC loop
func (gm *GCManager) Run() {
	ticker := time.NewTicker(1 * time.Minute)
	defer ticker.Stop()
	
	gm.running = true
	for {
		select {
		case <-gm.stopCh:
			gm.running = false
			return
		case <-ticker.C:
			gm.collectGarbage()
		}
	}
}

func (gm *GCManager) collectGarbage() {
	// In real implementation:
	// 1. Find obsolete SST files
	// 2. Delete old versions
}

// ColumnBlockMagic is the magic number for column blocks
const ColumnBlockMagic = 0x434F4C42 // "COLB"

// EncodeColumnBlock encodes a column block
func EncodeColumnBlock(header *ColumnBlockHeader, data []byte) []byte {
	buf := make([]byte, 64+len(data))
	
	binary.LittleEndian.PutUint32(buf[0:4], header.Magic)
	binary.LittleEndian.PutUint16(buf[4:6], header.Version)
	binary.LittleEndian.PutUint32(buf[6:10], header.ColumnID)
	buf[10] = byte(header.ColumnType)
	buf[11] = byte(header.Encoding)
	buf[12] = byte(header.Compression)
	binary.LittleEndian.PutUint32(buf[13:17], header.NumRows)
	binary.LittleEndian.PutUint32(buf[17:21], header.NullCount)
	// ... more header fields
	
	copy(buf[64:], data)
	
	// Calculate checksum
	checksum := crc32.ChecksumIEEE(buf[64:])
	binary.LittleEndian.PutUint32(buf[60:64], checksum)
	
	return buf
}

// Errors
var (
	ErrTableExists   = NewOLAPError("table already exists")
	ErrTableNotFound = NewOLAPError("table not found")
)

type OLAPError struct {
	msg string
}

func NewOLAPError(msg string) *OLAPError {
	return &OLAPError{msg: msg}
}

func (e *OLAPError) Error() string {
	return e.msg
}
