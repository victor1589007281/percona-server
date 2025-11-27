package binlog

import (
	"encoding/binary"
	"fmt"
	"io"
	
	"github.com/percona/go-binlog-parser/pkg/types"
)

const (
	// Magic number for binlog files
	BinlogMagicNumber = "\xfe\x62\x69\x6e"
	// Event header size
	EventHeaderSize = 19
)

// ReadFileHeader reads and validates binlog file header
func ReadFileHeader(r io.Reader) error {
	magic := make([]byte, 4)
	if _, err := io.ReadFull(r, magic); err != nil {
		return err
	}
	
	if string(magic) != BinlogMagicNumber {
		return fmt.Errorf("invalid binlog magic number")
	}
	
	return nil
}

// ReadEventHeader reads an event header
func ReadEventHeader(r io.Reader) (*types.EventHeader, error) {
	buf := make([]byte, EventHeaderSize)
	if _, err := io.ReadFull(r, buf); err != nil {
		return nil, err
	}
	
	header := &types.EventHeader{
		Timestamp:  binary.LittleEndian.Uint32(buf[0:4]),
		EventType:  buf[4],
		ServerID:   binary.LittleEndian.Uint32(buf[5:9]),
		EventSize:  binary.LittleEndian.Uint32(buf[9:13]),
		LogPos:     binary.LittleEndian.Uint32(buf[13:17]),
		Flags:      binary.LittleEndian.Uint16(buf[17:19]),
	}
	
	return header, nil
}
