// Package olap implements OLAP extension for Aurora (HTAP capability)
package olap

import (
	"time"
)

// DataType represents column data types
type DataType uint8

const (
	TypeInt8 DataType = iota
	TypeInt16
	TypeInt32
	TypeInt64
	TypeFloat32
	TypeFloat64
	TypeString
	TypeBinary
	TypeTimestamp
	TypeDecimal
)

func (d DataType) Size() int {
	switch d {
	case TypeInt8:
		return 1
	case TypeInt16:
		return 2
	case TypeInt32, TypeFloat32:
		return 4
	case TypeInt64, TypeFloat64, TypeTimestamp:
		return 8
	default:
		return 0 // Variable length
	}
}

// EncodingType represents column encoding types
type EncodingType uint8

const (
	EncodingPlain EncodingType = iota
	EncodingRLE              // Run-Length Encoding
	EncodingDictionary       // Dictionary Encoding
	EncodingDelta            // Delta Encoding
	EncodingBitPacking       // Bit Packing
)

// CompressionType represents compression types
type CompressionType uint8

const (
	CompressionNone CompressionType = iota
	CompressionLZ4
	CompressionZstd
	CompressionSnappy
)

// ColumnBlockHeader represents the header of a column block
type ColumnBlockHeader struct {
	Magic            uint32          // 0x434F4C42 "COLB"
	Version          uint16
	ColumnID         uint32
	ColumnType       DataType
	Encoding         EncodingType
	Compression      CompressionType
	NumRows          uint32
	NullCount        uint32
	MinValue         []byte
	MaxValue         []byte
	DataOffset       uint64
	DataLength       uint64
	NullBitmapOffset uint64
	NullBitmapLength uint64
	Checksum         uint32
}

// ColumnMeta holds column metadata
type ColumnMeta struct {
	ColumnID    uint32
	ColumnName  string
	DataType    DataType
	Nullable    bool
	DefaultExpr string
	Encoding    EncodingType
	Compression CompressionType
}

// PartitionSpec defines table partitioning
type PartitionSpec struct {
	Type       PartitionType
	Columns    []uint32
	Expression string
}

// PartitionType defines partition type
type PartitionType int

const (
	PartitionNone PartitionType = iota
	PartitionRange
	PartitionHash
	PartitionList
)

// ColumnTable represents a column-store table
type ColumnTable struct {
	TableID     uint64
	TableName   string
	Columns     []*ColumnMeta
	PrimaryKey  []uint32 // Primary key column indices
	SortKey     []uint32 // Sort key column indices
	PartitionBy *PartitionSpec
}

// ColumnBatch represents a batch of columnar data
type ColumnBatch struct {
	TableID   uint64
	Columns   []ColumnVector
	RowCount  int
	RowIDs    []uint64
	SortKeys  [][]byte
	Timestamp time.Time
}

// ColumnVector represents a vector of column values
type ColumnVector struct {
	ColumnID   uint32
	DataType   DataType
	Data       []byte
	NullBitmap []byte
	Dictionary [][]byte // For dictionary encoding
}

// OLAPConfig holds OLAP engine configuration
type OLAPConfig struct {
	DataPath           string
	WriteBufferSize    int // MB
	MaxWriteBufferNum  int
	BlockSize          int // KB
	BlockCacheSize     int // GB
	CompressionType    CompressionType
	MaxBackgroundJobs  int
	TargetFileSizeBase int // MB
}

// DefaultOLAPConfig returns default OLAP configuration
func DefaultOLAPConfig() *OLAPConfig {
	return &OLAPConfig{
		DataPath:           "/var/lib/aurora/olap",
		WriteBufferSize:    256,
		MaxWriteBufferNum:  4,
		BlockSize:          64,
		BlockCacheSize:     4,
		CompressionType:    CompressionZstd,
		MaxBackgroundJobs:  4,
		TargetFileSizeBase: 256,
	}
}

// SyncConfig holds OLTP->OLAP sync configuration
type SyncConfig struct {
	Enabled         bool
	BatchSize       int
	FlushInterval   time.Duration
	MaxLagMs        int64
	ParallelWorkers int
}

// DefaultSyncConfig returns default sync configuration
func DefaultSyncConfig() *SyncConfig {
	return &SyncConfig{
		Enabled:         true,
		BatchSize:       1000,
		FlushInterval:   100 * time.Millisecond,
		MaxLagMs:        1000,
		ParallelWorkers: 4,
	}
}
