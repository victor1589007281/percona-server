package redolog

import (
"fmt"
"io"
"os"

"github.com/percona/go-redo-parser/pkg/types"
)

// Reader provides high-level redo log reading functionality
type Reader struct {
fileParser   *FileParser
blockParser  *BlockParser
recordParser *RecordParser
}

// NewReader creates a new redo log reader
func NewReader() *Reader {
return &Reader{
fileParser:   NewFileParser(),
blockParser:  NewBlockParser(),
recordParser: NewRecordParser(),
}
}

// ReadFile reads an entire redo log file and returns all MTRs
func (r *Reader) ReadFile(filePath string) ([]*types.MTR, *types.RecoveryStats, error) {
stats := &types.RecoveryStats{}

// Open file
file, err := os.Open(filePath)
if err != nil {
return nil, nil, fmt.Errorf("failed to open file: %w", err)
}
defer file.Close()

// Read file header
headerData := make([]byte, types.LogFileHdrSize)
_, err = file.Read(headerData)
if err != nil {
return nil, nil, fmt.Errorf("failed to read header: %w", err)
}

header, err := r.fileParser.ParseFileHeader(headerData)
if err != nil {
return nil, nil, fmt.Errorf("failed to parse header: %w", err)
}

// Start reading from first data block
currentLsn := header.StartLsn
var mtrs []*types.MTR
var currentMtr *types.MTR

for {
// Read next block
blockData := make([]byte, types.OSFileLogBlockSize)
n, err := file.Read(blockData)
if err == io.EOF {
break
}
if err != nil {
return nil, stats, fmt.Errorf("failed to read block: %w", err)
}
if n != types.OSFileLogBlockSize {
break
}

stats.BlocksRead++
stats.BytesProcessed += uint64(n)

// Parse block
block, err := r.blockParser.ParseBlock(blockData)
if err != nil {
stats.CorruptBlocks++
continue
}

// Check for empty block (end of log)
if r.blockParser.IsEmptyBlock(block) {
break
}

// Parse records from block data
if len(block.Data) > 0 {
records, err := r.recordParser.ParseRecords(block.Data, currentLsn)
if err != nil {
continue
}

for _, record := range records {
stats.RecordsParsed++

// Start new MTR if needed
if currentMtr == nil {
currentMtr = &types.MTR{
StartLsn: record.LSN,
Records:  []*types.LogRecord{},
}
}

currentMtr.Records = append(currentMtr.Records, record)

// Check for MTR end
isSingleRec := record.Type.IsSingleRec()
isMultiEnd := record.Type == types.MlogMultiRecEnd

if isSingleRec || isMultiEnd {
// MTR complete
currentMtr.EndLsn = record.LSN + types.LSN(len(record.Data))
mtrs = append(mtrs, currentMtr)
stats.MtrsProcessed++
currentMtr = nil
}
}
}

currentLsn += types.LSN(len(block.Data))
}

// Handle incomplete MTR at end
if currentMtr != nil && len(currentMtr.Records) > 0 {
currentMtr.EndLsn = currentLsn
mtrs = append(mtrs, currentMtr)
stats.MtrsProcessed++
}

return mtrs, stats, nil
}

// ReadFromLSN reads redo log starting from a specific LSN
func (r *Reader) ReadFromLSN(filePath string, startLsn types.LSN) ([]*types.MTR, error) {
// Read file info to determine block offset
info, err := r.fileParser.ReadFileInfo(filePath)
if err != nil {
return nil, err
}

if startLsn < info.StartLsn || startLsn >= info.EndLsn {
return nil, fmt.Errorf("LSN %d out of range [%d, %d)", startLsn, info.StartLsn, info.EndLsn)
}

// Calculate block offset
lsnOffset := startLsn - info.StartLsn
blockNo := lsnOffset / types.OSFileLogBlockSize
fileOffset := types.LogFileHdrSize + int64(blockNo*types.OSFileLogBlockSize)

// Open file and seek
file, err := os.Open(filePath)
if err != nil {
return nil, fmt.Errorf("failed to open file: %w", err)
}
defer file.Close()

_, err = file.Seek(fileOffset, 0)
if err != nil {
return nil, fmt.Errorf("failed to seek to LSN %d: %w", startLsn, err)
}

// Read from this point
var mtrs []*types.MTR
var currentMtr *types.MTR
currentLsn := info.StartLsn + types.LSN(blockNo*types.OSFileLogBlockSize)

for {
blockData := make([]byte, types.OSFileLogBlockSize)
n, err := file.Read(blockData)
if err == io.EOF {
break
}
if err != nil {
return nil, err
}
if n != types.OSFileLogBlockSize {
break
}

block, err := r.blockParser.ParseBlock(blockData)
if err != nil {
continue
}

if r.blockParser.IsEmptyBlock(block) {
break
}

if len(block.Data) > 0 {
records, _ := r.recordParser.ParseRecords(block.Data, currentLsn)
for _, record := range records {
if record.LSN < startLsn {
continue // Skip records before target LSN
}

if currentMtr == nil {
currentMtr = &types.MTR{StartLsn: record.LSN}
}

currentMtr.Records = append(currentMtr.Records, record)

if record.Type.IsSingleRec() || record.Type == types.MlogMultiRecEnd {
currentMtr.EndLsn = record.LSN
mtrs = append(mtrs, currentMtr)
currentMtr = nil
}
}
}

currentLsn += types.LSN(len(block.Data))
}

if currentMtr != nil && len(currentMtr.Records) > 0 {
currentMtr.EndLsn = currentLsn
mtrs = append(mtrs, currentMtr)
}

return mtrs, nil
}
