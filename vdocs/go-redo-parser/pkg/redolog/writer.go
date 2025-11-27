package redolog

import (
"fmt"
"os"

"github.com/percona/go-redo-parser/pkg/types"
"github.com/percona/go-redo-parser/pkg/utils"
)

// Writer provides redo log writing functionality
type Writer struct {
file          *os.File
currentLsn    types.LSN
currentBlock  []byte
blockOffset   int
currentHdrNo  uint32
currentEpoch  uint32
}

// NewWriter creates a new redo log writer
func NewWriter(filePath string, startLsn types.LSN) (*Writer, error) {
file, err := os.Create(filePath)
if err != nil {
return nil, fmt.Errorf("failed to create file: %w", err)
}

w := &Writer{
file:         file,
currentLsn:   startLsn,
currentBlock: make([]byte, types.OSFileLogBlockSize),
blockOffset:  types.LogBlockHdrSize,
currentHdrNo: uint32(startLsn / types.OSFileLogBlockSize),
currentEpoch: uint32(startLsn / (types.LogBlockMaxNo * types.OSFileLogBlockSize)),
}

// Write file header
if err := w.writeFileHeader(startLsn); err != nil {
file.Close()
return nil, err
}

return w, nil
}

// writeFileHeader writes the redo log file header
func (w *Writer) writeFileHeader(startLsn types.LSN) error {
header := make([]byte, types.LogFileHdrSize)

// Write format
utils.WriteUint32BE(types.LogFormatCurrent, header[types.LogHeaderFormat:])

// Write log_uuid (use dummy value for testing)
utils.WriteUint32BE(0x12345678, header[types.LogHeaderLogUuid:])

// Write start_lsn
utils.WriteUint64BE(uint64(startLsn), header[types.LogHeaderStartLsn:])

// Write creator
creator := "Go-Redo-Parser v1.0"
copy(header[types.LogHeaderCreator:], []byte(creator))

// Write flags (no special flags)
utils.WriteUint32BE(0, header[types.LogHeaderFlags:])

// Write checkpoint 1 (dummy)
cp1 := header[types.LogCheckpoint1:types.LogCheckpoint1+types.OSFileLogBlockSize]
utils.WriteUint64BE(uint64(startLsn), cp1[types.LogCheckpointLsn:])

// Write checkpoint 2 (dummy)
cp2 := header[types.LogCheckpoint2:types.LogCheckpoint2+types.OSFileLogBlockSize]
utils.WriteUint64BE(uint64(startLsn), cp2[types.LogCheckpointLsn:])

// Write to file
_, err := w.file.Write(header)
return err
}

// WriteRecord writes a single redo log record
func (w *Writer) WriteRecord(record *types.LogRecord) error {
// Build record data
recordData := make([]byte, 0, 256)
recordData = append(recordData, byte(record.Type))

// For most types, write space_id and page_no
needsSpaceAndPage := record.Type != types.MlogMultiRecEnd && 
                     record.Type != types.MlogDummyRecord

if needsSpaceAndPage {
// Write space_id
buf := make([]byte, 5)
n, err := utils.WriteCompressed(uint64(record.SpaceID), buf)
if err != nil {
return err
}
recordData = append(recordData, buf[:n]...)

// Write page_no
n, err = utils.WriteCompressed(uint64(record.PageNo), buf)
if err != nil {
return err
}
recordData = append(recordData, buf[:n]...)
}

// Write type-specific data
baseType := record.Type.BaseType()
if baseType >= types.Mlog1Byte && baseType <= types.Mlog8Bytes {
// Simple write: offset + value
offsetBuf := make([]byte, 2)
utils.WriteUint16BE(record.Offset, offsetBuf)
recordData = append(recordData, offsetBuf...)

// Convert data to value and write compressed
value := uint64(0)
for _, b := range record.Data {
value = (value << 8) | uint64(b)
}
buf := make([]byte, 5)
n, _ := utils.WriteCompressed(value, buf)
recordData = append(recordData, buf[:n]...)

} else if baseType == types.MlogWriteString {
// Write length
lenBuf := make([]byte, 2)
utils.WriteUint16BE(uint16(len(record.Data)), lenBuf)
recordData = append(recordData, lenBuf...)

// Write offset
offsetBuf := make([]byte, 2)
utils.WriteUint16BE(record.Offset, offsetBuf)
recordData = append(recordData, offsetBuf...)

// Write data
recordData = append(recordData, record.Data...)
} else {
// For other types, just append data
recordData = append(recordData, record.Data...)
}

// Write record data to current block
return w.writeToBlock(recordData)
}

// writeToBlock writes data to the current block, flushing if needed
func (w *Writer) writeToBlock(data []byte) error {
for len(data) > 0 {
// Calculate available space in current block
availSpace := types.OSFileLogBlockSize - types.LogBlockTrlSize - w.blockOffset

if availSpace <= 0 {
// Block is full, flush it
if err := w.flushBlock(); err != nil {
return err
}
continue
}

// Copy as much as possible
toCopy := len(data)
if toCopy > availSpace {
toCopy = availSpace
}

copy(w.currentBlock[w.blockOffset:], data[:toCopy])
w.blockOffset += toCopy
w.currentLsn += types.LSN(toCopy)
data = data[toCopy:]
}

return nil
}

// flushBlock flushes the current block to file
func (w *Writer) flushBlock() error {
// Update block header
utils.WriteUint32BE(w.currentHdrNo, w.currentBlock[types.LogBlockHdrNo:])
utils.WriteUint16BE(uint16(w.blockOffset), w.currentBlock[types.LogBlockHdrDataLen:])
utils.WriteUint16BE(types.LogBlockHdrSize, w.currentBlock[types.LogBlockFirstRecGroup:]) // First record at start
utils.WriteUint32BE(w.currentEpoch, w.currentBlock[types.LogBlockEpochNo:])

// Calculate and write checksum
checksum := utils.CalculateBlockChecksum(w.currentBlock)
checksumOffset := types.OSFileLogBlockSize - types.LogBlockChecksum
utils.WriteUint32BE(checksum, w.currentBlock[checksumOffset:])

// Write block to file
_, err := w.file.Write(w.currentBlock)
if err != nil {
return fmt.Errorf("failed to write block: %w", err)
}

// Reset block for next write
w.currentBlock = make([]byte, types.OSFileLogBlockSize)
w.blockOffset = types.LogBlockHdrSize
w.currentHdrNo++
if w.currentHdrNo >= types.LogBlockMaxNo {
w.currentHdrNo = 0
w.currentEpoch++
}

return nil
}

// WriteMTR writes a complete MTR (mini-transaction)
func (w *Writer) WriteMTR(mtr *types.MTR) error {
for _, record := range mtr.Records {
if err := w.WriteRecord(record); err != nil {
return err
}
}

// Write MLOG_MULTI_REC_END if not single record
if len(mtr.Records) > 1 {
endRecord := &types.LogRecord{
Type: types.MlogMultiRecEnd,
}
if err := w.WriteRecord(endRecord); err != nil {
return err
}
}

return nil
}

// Close closes the writer and flushes any remaining data
func (w *Writer) Close() error {
// Flush current block if it has data
if w.blockOffset > types.LogBlockHdrSize {
if err := w.flushBlock(); err != nil {
return err
}
}

return w.file.Close()
}

// GetCurrentLSN returns the current LSN
func (w *Writer) GetCurrentLSN() types.LSN {
return w.currentLsn
}
