package common

import (
	"bytes"
	"compress/gzip"
	"io"
	"sync"

	"github.com/klauspost/compress/lz4"
	"github.com/klauspost/compress/zstd"
)

// CompressionType defines compression algorithm
type CompressionType int

const (
	CompressionNone CompressionType = iota
	CompressionGzip
	CompressionLZ4
	CompressionZstd
)

// Compressor handles data compression
type Compressor struct {
	compType CompressionType
	level    int
	
	// Buffer pools for efficiency
	gzipWriterPool sync.Pool
	gzipReaderPool sync.Pool
}

// NewCompressor creates a new compressor
func NewCompressor(compType CompressionType, level int) *Compressor {
	return &Compressor{
		compType: compType,
		level:    level,
	}
}

// Compress compresses data
func (c *Compressor) Compress(data []byte) ([]byte, error) {
	switch c.compType {
	case CompressionNone:
		return data, nil
	case CompressionGzip:
		return c.compressGzip(data)
	case CompressionLZ4:
		return c.compressLZ4(data)
	case CompressionZstd:
		return c.compressZstd(data)
	default:
		return data, nil
	}
}

// Decompress decompresses data
func (c *Compressor) Decompress(data []byte) ([]byte, error) {
	switch c.compType {
	case CompressionNone:
		return data, nil
	case CompressionGzip:
		return c.decompressGzip(data)
	case CompressionLZ4:
		return c.decompressLZ4(data)
	case CompressionZstd:
		return c.decompressZstd(data)
	default:
		return data, nil
	}
}

func (c *Compressor) compressGzip(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	
	var writer *gzip.Writer
	if w := c.gzipWriterPool.Get(); w != nil {
		writer = w.(*gzip.Writer)
		writer.Reset(&buf)
	} else {
		var err error
		writer, err = gzip.NewWriterLevel(&buf, c.level)
		if err != nil {
			return nil, err
		}
	}
	
	if _, err := writer.Write(data); err != nil {
		return nil, err
	}
	if err := writer.Close(); err != nil {
		return nil, err
	}
	
	c.gzipWriterPool.Put(writer)
	
	return buf.Bytes(), nil
}

func (c *Compressor) decompressGzip(data []byte) ([]byte, error) {
	reader, err := gzip.NewReader(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	defer reader.Close()
	
	return io.ReadAll(reader)
}

func (c *Compressor) compressLZ4(data []byte) ([]byte, error) {
	// LZ4 maximum compressed size
	maxSize := lz4.CompressBlockBound(len(data))
	compressed := make([]byte, maxSize)
	
	n, err := lz4.CompressBlock(data, compressed, nil)
	if err != nil {
		return nil, err
	}
	
	return compressed[:n], nil
}

func (c *Compressor) decompressLZ4(data []byte) ([]byte, error) {
	// Estimate decompressed size (usually 2-4x compressed)
	decompressed := make([]byte, len(data)*4)
	
	n, err := lz4.UncompressBlock(data, decompressed)
	if err != nil {
		// Try with larger buffer
		decompressed = make([]byte, len(data)*10)
		n, err = lz4.UncompressBlock(data, decompressed)
		if err != nil {
			return nil, err
		}
	}
	
	return decompressed[:n], nil
}

func (c *Compressor) compressZstd(data []byte) ([]byte, error) {
	encoder, err := zstd.NewWriter(nil, zstd.WithEncoderLevel(zstd.EncoderLevelFromZstd(c.level)))
	if err != nil {
		return nil, err
	}
	defer encoder.Close()
	
	return encoder.EncodeAll(data, nil), nil
}

func (c *Compressor) decompressZstd(data []byte) ([]byte, error) {
	decoder, err := zstd.NewReader(nil)
	if err != nil {
		return nil, err
	}
	defer decoder.Close()
	
	return decoder.DecodeAll(data, nil)
}

// CompressionRatio calculates the compression ratio
func CompressionRatio(original, compressed []byte) float64 {
	if len(original) == 0 {
		return 1.0
	}
	return float64(len(compressed)) / float64(len(original))
}

// DefaultCompressor is the default LZ4 compressor for Aurora
var DefaultCompressor = NewCompressor(CompressionLZ4, 0)
