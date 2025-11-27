package redolog

import (
"fmt"

"github.com/percona/go-redo-parser/pkg/types"
"github.com/percona/go-redo-parser/pkg/utils"
)

// RecordParser parses individual redo log records
type RecordParser struct{}

// NewRecordParser creates a new record parser
func NewRecordParser() *RecordParser {
return &RecordParser{}
}

// ParseRecord parses a single redo log record from the data buffer
// Returns the record and number of bytes consumed
func (p *RecordParser) ParseRecord(data []byte, currentLsn types.LSN) (*types.LogRecord, int, error) {
if len(data) == 0 {
return nil, 0, fmt.Errorf("empty data buffer")
}

record := &types.LogRecord{
LSN: currentLsn,
}

offset := 0

// Parse type (1 byte)
record.Type = types.MlogType(data[offset])
offset++

// Check for MLOG_MULTI_REC_END (no space_id/page_no)
if record.Type == types.MlogMultiRecEnd {
return record, offset, nil
}

// Check for dummy record (no space_id/page_no)
if record.Type == types.MlogDummyRecord {
return record, offset, nil
}

// For simple byte write types (1-8 bytes), parse directly
baseType := record.Type.BaseType()
if baseType >= types.Mlog1Byte && baseType <= types.Mlog8Bytes {
return p.parseSimpleWrite(record, data, offset)
}

// For most other types, parse space_id and page_no
if p.needsSpaceAndPage(record.Type) {
var err error
var consumed int

// Parse space_id
spaceID, consumed, err := utils.ParseCompressed(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to parse space_id: %w", err)
}
record.SpaceID = types.SpaceID(spaceID)
offset += consumed

// Parse page_no
pageNo, consumed, err := utils.ParseCompressed(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to parse page_no: %w", err)
}
record.PageNo = types.PageNo(pageNo)
offset += consumed
}

// Parse type-specific data
consumed, err := p.parseTypeSpecificData(record, data[offset:])
if err != nil {
return nil, 0, err
}
offset += consumed

return record, offset, nil
}

// parseSimpleWrite parses MLOG_1BYTE, MLOG_2BYTES, MLOG_4BYTES, MLOG_8BYTES
func (p *RecordParser) parseSimpleWrite(record *types.LogRecord, data []byte, offset int) (*types.LogRecord, int, error) {
// Parse space_id
spaceID, consumed, err := utils.ParseCompressed(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to parse space_id: %w", err)
}
record.SpaceID = types.SpaceID(spaceID)
offset += consumed

// Parse page_no
pageNo, consumed, err := utils.ParseCompressed(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to parse page_no: %w", err)
}
record.PageNo = types.PageNo(pageNo)
offset += consumed

// Parse offset within page (2 bytes big-endian)
if len(data[offset:]) < 2 {
return nil, 0, fmt.Errorf("incomplete offset field")
}
pageOffset, err := utils.ReadUint16BE(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to read offset: %w", err)
}
record.Offset = pageOffset
offset += 2

// Parse value (compressed)
value, consumed, err := utils.ParseCompressed(data[offset:])
if err != nil {
return nil, 0, fmt.Errorf("failed to parse value: %w", err)
}
offset += consumed

// Store value as data
baseType := record.Type.BaseType()
numBytes := int(baseType)
record.Data = make([]byte, numBytes)
for i := 0; i < numBytes; i++ {
record.Data[numBytes-1-i] = byte(value >> (i * 8))
}

return record, offset, nil
}

// parseTypeSpecificData parses data specific to each record type
func (p *RecordParser) parseTypeSpecificData(record *types.LogRecord, data []byte) (int, error) {
baseType := record.Type.BaseType()
offset := 0

switch baseType {
case types.MlogWriteString:
// Parse length
if len(data) < 2 {
return 0, fmt.Errorf("incomplete write_string length")
}
length, err := utils.ReadUint16BE(data[offset:])
if err != nil {
return 0, fmt.Errorf("failed to read string length: %w", err)
}
offset += 2

// Parse offset
if len(data[offset:]) < 2 {
return 0, fmt.Errorf("incomplete write_string offset")
}
pageOffset, err := utils.ReadUint16BE(data[offset:])
if err != nil {
return 0, fmt.Errorf("failed to read string offset: %w", err)
}
record.Offset = pageOffset
offset += 2

// Parse string data
if len(data[offset:]) < int(length) {
return 0, fmt.Errorf("incomplete write_string data")
}
record.Data = make([]byte, length)
copy(record.Data, data[offset:offset+int(length)])
offset += int(length)

case types.MlogFileCreate, types.MlogFileRename, types.MlogFileDelete:
// These records have variable length paths
// For now, just copy remaining data
record.Data = make([]byte, len(data))
copy(record.Data, data)
offset = len(data)

default:
// For other types, we don't parse specific data yet
// Just mark as having no additional data
record.Data = []byte{}
}

return offset, nil
}

// needsSpaceAndPage determines if a record type needs space_id and page_no
func (p *RecordParser) needsSpaceAndPage(mType types.MlogType) bool {
baseType := mType.BaseType()

// Types that don't need space_id/page_no
switch baseType {
case types.MlogMultiRecEnd, types.MlogDummyRecord:
return false
}

// Most other types need space_id and page_no
return true
}

// ParseRecords parses all records from a block's data
func (p *RecordParser) ParseRecords(blockData []byte, startLsn types.LSN) ([]*types.LogRecord, error) {
var records []*types.LogRecord
offset := 0
currentLsn := startLsn

for offset < len(blockData) {
record, consumed, err := p.ParseRecord(blockData[offset:], currentLsn)
if err != nil {
// If we can't parse, might be padding or end of data
break
}

records = append(records, record)
offset += consumed
currentLsn += types.LSN(consumed)

// Stop at MLOG_MULTI_REC_END
if record.Type == types.MlogMultiRecEnd {
break
}
}

return records, nil
}
