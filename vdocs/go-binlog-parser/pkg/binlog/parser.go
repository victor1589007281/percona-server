package binlog

import (
	"bufio"
	"encoding/binary"
	"fmt"
	"io"
	"os"
	
	"github.com/percona/go-binlog-parser/pkg/types"
)

const BinlogMagic = "\xfe\x62\x69\x6e"

type Parser struct {
	file *os.File
	reader *bufio.Reader
}

func NewParser(filename string) (*Parser, error) {
	file, err := os.Open(filename)
	if err != nil {
		return nil, err
	}
	
	p := &Parser{
		file:   file,
		reader: bufio.NewReader(file),
	}
	
	// Read magic
	magic := make([]byte, 4)
	if _, err := io.ReadFull(p.reader, magic); err != nil {
		return nil, err
	}
	
	if string(magic) != BinlogMagic {
		return nil, fmt.Errorf("invalid binlog magic")
	}
	
	return p, nil
}

func (p *Parser) ReadEvent() (*types.Event, error) {
	// Read header (19 bytes)
	headerBuf := make([]byte, 19)
	if _, err := io.ReadFull(p.reader, headerBuf); err != nil {
		return nil, err
	}
	
	header := &types.EventHeader{
		Timestamp: binary.LittleEndian.Uint32(headerBuf[0:4]),
		EventType: headerBuf[4],
		ServerID:  binary.LittleEndian.Uint32(headerBuf[5:9]),
		EventSize: binary.LittleEndian.Uint32(headerBuf[9:13]),
		LogPos:    binary.LittleEndian.Uint32(headerBuf[13:17]),
		Flags:     binary.LittleEndian.Uint16(headerBuf[17:19]),
	}
	
	// Read event data
	dataSize := header.EventSize - 19
	data := make([]byte, dataSize)
	if _, err := io.ReadFull(p.reader, data); err != nil {
		return nil, err
	}
	
	return &types.Event{
		Header: header,
		Data:   data,
	}, nil
}

func (p *Parser) Close() error {
	if p.file != nil {
		return p.file.Close()
	}
	return nil
}
