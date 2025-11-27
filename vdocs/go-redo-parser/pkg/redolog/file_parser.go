package redolog

import (
"fmt"
"os"
"strings"

"github.com/percona/go-redo-parser/pkg/types"
"github.com/percona/go-redo-parser/pkg/utils"
)

// FileParser parses redo log files
type FileParser struct {
blockParser  *BlockParser
recordParser *RecordParser
}

// NewFileParser creates a new file parser
func NewFileParser() *FileParser {
return &FileParser{
blockParser:  NewBlockParser(),
recordParser: NewRecordParser(),
}
}

// ParseFileHeader parses the main file header (first 2048 bytes)
func (p *FileParser) ParseFileHeader(data []byte) (*types.LogFileHeader, error) {
if len(data) < types.LogFileHdrSize {
return nil, fmt.Errorf("insufficient data for file header: %d bytes", len(data))
}

header := &types.LogFileHeader{}

// Parse format (4 bytes, big-endian)
format, err := utils.ReadUint32BE(data[types.LogHeaderFormat:])
if err != nil {
return nil, fmt.Errorf("failed to read format: %w", err)
}
header.Format = types.LogFormat(format)

// Parse log_uuid (4 bytes, big-endian)
logUuid, err := utils.ReadUint32BE(data[types.LogHeaderLogUuid:])
if err != nil {
return nil, fmt.Errorf("failed to read log_uuid: %w", err)
}
header.LogUuid = logUuid

// Parse start_lsn (8 bytes, big-endian)
startLsn, err := utils.ReadUint64BE(data[types.LogHeaderStartLsn:])
if err != nil {
return nil, fmt.Errorf("failed to read start_lsn: %w", err)
}
header.StartLsn = types.LSN(startLsn)

// Parse creator name (null-terminated string)
creatorData := data[types.LogHeaderCreator:types.LogHeaderCreatorEnd]
nullIdx := strings.IndexByte(string(creatorData), 0)
if nullIdx >= 0 {
header.CreatorName = string(creatorData[:nullIdx])
} else {
header.CreatorName = string(creatorData)
}

// Parse flags (4 bytes, big-endian)
flags, err := utils.ReadUint32BE(data[types.LogHeaderFlags:])
if err != nil {
return nil, fmt.Errorf("failed to read flags: %w", err)
}
header.Flags = types.LogFlags(flags)

return header, nil
}

// ParseCheckpointHeader parses a checkpoint header (512 bytes)
func (p *FileParser) ParseCheckpointHeader(data []byte) (*types.LogCheckpointHeader, error) {
if len(data) < types.OSFileLogBlockSize {
return nil, fmt.Errorf("insufficient data for checkpoint header: %d bytes", len(data))
}

header := &types.LogCheckpointHeader{}

// Parse checkpoint_lsn (8 bytes, big-endian)
checkpointLsn, err := utils.ReadUint64BE(data[types.LogCheckpointLsn:])
if err != nil {
return nil, fmt.Errorf("failed to read checkpoint_lsn: %w", err)
}
header.CheckpointLsn = types.LSN(checkpointLsn)

return header, nil
}

// ReadFileInfo reads metadata about a redo log file
func (p *FileParser) ReadFileInfo(filePath string) (*types.LogFileInfo, error) {
// Open file
file, err := os.Open(filePath)
if err != nil {
return nil, fmt.Errorf("failed to open file: %w", err)
}
defer file.Close()

// Get file stats
stat, err := file.Stat()
if err != nil {
return nil, fmt.Errorf("failed to stat file: %w", err)
}

info := &types.LogFileInfo{
FilePath:     filePath,
FileSize:     stat.Size(),
ModifiedTime: stat.ModTime(),
}

// Read file header
headerData := make([]byte, types.LogFileHdrSize)
n, err := file.Read(headerData)
if err != nil {
return nil, fmt.Errorf("failed to read file header: %w", err)
}
if n < types.LogFileHdrSize {
return nil, fmt.Errorf("incomplete file header: read %d bytes", n)
}

header, err := p.ParseFileHeader(headerData)
if err != nil {
return nil, fmt.Errorf("failed to parse file header: %w", err)
}
info.Header = header
info.StartLsn = header.StartLsn

// Calculate end LSN (approximate based on file size)
numBlocks := (info.FileSize - types.LogFileHdrSize) / types.OSFileLogBlockSize
info.EndLsn = info.StartLsn + types.LSN(numBlocks*types.OSFileLogBlockSize)

return info, nil
}

// ReadCheckpoint reads a checkpoint header from the file
func (p *FileParser) ReadCheckpoint(filePath string, checkpointNo int) (*types.LogCheckpointHeader, error) {
file, err := os.Open(filePath)
if err != nil {
return nil, fmt.Errorf("failed to open file: %w", err)
}
defer file.Close()

var offset int64
if checkpointNo == 1 {
offset = types.LogCheckpoint1
} else if checkpointNo == 2 {
offset = types.LogCheckpoint2
} else {
return nil, fmt.Errorf("invalid checkpoint number: %d (must be 1 or 2)", checkpointNo)
}

// Seek to checkpoint position
_, err = file.Seek(offset, 0)
if err != nil {
return nil, fmt.Errorf("failed to seek to checkpoint %d: %w", checkpointNo, err)
}

// Read checkpoint data
checkpointData := make([]byte, types.OSFileLogBlockSize)
n, err := file.Read(checkpointData)
if err != nil {
return nil, fmt.Errorf("failed to read checkpoint %d: %w", checkpointNo, err)
}
if n < types.OSFileLogBlockSize {
return nil, fmt.Errorf("incomplete checkpoint %d: read %d bytes", checkpointNo, n)
}

return p.ParseCheckpointHeader(checkpointData)
}
