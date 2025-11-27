package logindex

import (
	"encoding/binary"
	"fmt"
	"os"
)

// Checkpoint file structure (polar_logindex.ckpt)

// CheckpointHeader represents the checkpoint file header
type CheckpointHeader struct {
	CheckpointLSN uint64 // Checkpoint LSN
	Timestamp     uint64 // Unix timestamp
	NumEntries    uint32 // Number of entries
	Checksum      uint32 // CRC32 checksum
}

// CheckpointEntry represents a checkpoint entry (lightweight)
type CheckpointEntry struct {
	SpaceID uint32 // Tablespace ID
	PageNo  uint32 // Page number
	MaxLSN  uint64 // Maximum LSN only
}

// CheckpointFile represents a checkpoint file
type CheckpointFile struct {
	Header  CheckpointHeader
	Entries []CheckpointEntry
	path    string
}

// NewCheckpointFile creates a new checkpoint file
func NewCheckpointFile(path string) *CheckpointFile {
	return &CheckpointFile{
		Entries: make([]CheckpointEntry, 0),
		path:    path,
	}
}

// Save writes checkpoint to disk
func (cf *CheckpointFile) Save() error {
	file, err := os.Create(cf.path)
	if err != nil {
		return err
	}
	defer file.Close()
	
	cf.Header.NumEntries = uint32(len(cf.Entries))
	
	// Write header
	headerBuf := make([]byte, 24)
	binary.LittleEndian.PutUint64(headerBuf[0:], cf.Header.CheckpointLSN)
	binary.LittleEndian.PutUint64(headerBuf[8:], cf.Header.Timestamp)
	binary.LittleEndian.PutUint32(headerBuf[16:], cf.Header.NumEntries)
	binary.LittleEndian.PutUint32(headerBuf[20:], cf.Header.Checksum)
	
	if _, err := file.Write(headerBuf); err != nil {
		return err
	}
	
	// Write entries
	for _, entry := range cf.Entries {
		entryBuf := make([]byte, 16)
		binary.LittleEndian.PutUint32(entryBuf[0:], entry.SpaceID)
		binary.LittleEndian.PutUint32(entryBuf[4:], entry.PageNo)
		binary.LittleEndian.PutUint64(entryBuf[8:], entry.MaxLSN)
		
		if _, err := file.Write(entryBuf); err != nil {
			return err
		}
	}
	
	return file.Sync()
}

// Load reads checkpoint from disk
func (cf *CheckpointFile) Load() error {
	file, err := os.Open(cf.path)
	if err != nil {
		return err
	}
	defer file.Close()
	
	// Read header
	headerBuf := make([]byte, 24)
	if _, err := file.Read(headerBuf); err != nil {
		return err
	}
	
	cf.Header.CheckpointLSN = binary.LittleEndian.Uint64(headerBuf[0:])
	cf.Header.Timestamp = binary.LittleEndian.Uint64(headerBuf[8:])
	cf.Header.NumEntries = binary.LittleEndian.Uint32(headerBuf[16:])
	cf.Header.Checksum = binary.LittleEndian.Uint32(headerBuf[20:])
	
	// Read entries
	cf.Entries = make([]CheckpointEntry, cf.Header.NumEntries)
	for i := uint32(0); i < cf.Header.NumEntries; i++ {
		entryBuf := make([]byte, 16)
		if _, err := file.Read(entryBuf); err != nil {
			return err
		}
		
		cf.Entries[i].SpaceID = binary.LittleEndian.Uint32(entryBuf[0:])
		cf.Entries[i].PageNo = binary.LittleEndian.Uint32(entryBuf[4:])
		cf.Entries[i].MaxLSN = binary.LittleEndian.Uint64(entryBuf[8:])
	}
	
	return nil
}
