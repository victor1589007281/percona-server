package logindex

import (
	"bytes"
	"compress/flate"
	"io"
)

// Compression using flate (similar to LZ4 but built-in)
// For production, you can replace with github.com/pierrec/lz4

// Compress compresses data using flate
func Compress(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	w, err := flate.NewWriter(&buf, flate.BestSpeed)
	if err != nil {
		return nil, err
	}
	
	if _, err := w.Write(data); err != nil {
		return nil, err
	}
	
	if err := w.Close(); err != nil {
		return nil, err
	}
	
	return buf.Bytes(), nil
}

// Decompress decompresses data
func Decompress(data []byte) ([]byte, error) {
	r := flate.NewReader(bytes.NewReader(data))
	defer r.Close()
	
	var buf bytes.Buffer
	if _, err := io.Copy(&buf, r); err != nil {
		return nil, err
	}
	
	return buf.Bytes(), nil
}
