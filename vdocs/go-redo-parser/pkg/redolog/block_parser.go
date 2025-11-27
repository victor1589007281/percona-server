package redolog

import (
"fmt"

"github.com/percona/go-redo-parser/pkg/types"
"github.com/percona/go-redo-parser/pkg/utils"
)

// BlockParser parses 512-byte redo log blocks
type BlockParser struct{}

// NewBlockParser creates a new block parser
func NewBlockParser() *BlockParser {
return &BlockParser{}
}

// ParseBlock parses a 512-byte log block
func (p *BlockParser) ParseBlock(blockData []byte) (*types.LogBlock, error) {
if len(blockData) != types.OSFileLogBlockSize {
return nil, fmt.Errorf("invalid block size: %d, expected %d", 
len(blockData), types.OSFileLogBlockSize)
}

block := &types.LogBlock{}

// Parse header (12 bytes)
hdrNo, err := utils.ReadUint32BE(blockData[types.LogBlockHdrNo:])
if err != nil {
return nil, fmt.Errorf("failed to read hdr_no: %w", err)
}
block.Header.HdrNo = hdrNo & uint32(^types.LogBlockFlushBitMask) // Remove flush bit

dataLen, err := utils.ReadUint16BE(blockData[types.LogBlockHdrDataLen:])
if err != nil {
return nil, fmt.Errorf("failed to read data_len: %w", err)
}
block.Header.DataLen = dataLen & 0x7FFF // Remove encryption bit if present

firstRecGroup, err := utils.ReadUint16BE(blockData[types.LogBlockFirstRecGroup:])
if err != nil {
return nil, fmt.Errorf("failed to read first_rec_group: %w", err)
}
block.Header.FirstRecGroup = firstRecGroup

epochNo, err := utils.ReadUint32BE(blockData[types.LogBlockEpochNo:])
if err != nil {
return nil, fmt.Errorf("failed to read epoch_no: %w", err)
}
block.Header.EpochNo = epochNo

// Validate data_len
if block.Header.DataLen > types.OSFileLogBlockSize {
return nil, fmt.Errorf("invalid data_len: %d", block.Header.DataLen)
}

// Extract data part (496 bytes max)
if int(block.Header.DataLen) > types.LogBlockHdrSize {
actualDataLen := int(block.Header.DataLen) - types.LogBlockHdrSize
if actualDataLen > types.LogBlockDataSize {
actualDataLen = types.LogBlockDataSize
}
block.Data = make([]byte, actualDataLen)
copy(block.Data, blockData[types.LogBlockHdrSize:types.LogBlockHdrSize+actualDataLen])
} else {
block.Data = []byte{}
}

// Parse trailer (4 bytes from end)
checksumOffset := types.OSFileLogBlockSize - types.LogBlockChecksum
checksum, err := utils.ReadUint32BE(blockData[checksumOffset:])
if err != nil {
return nil, fmt.Errorf("failed to read checksum: %w", err)
}
block.Checksum = checksum

// Verify checksum
if !utils.VerifyBlockChecksum(blockData, checksum, types.LogNoChecksumMagic) {
return nil, fmt.Errorf("block checksum mismatch: hdr_no=%d, expected=0x%08x, calculated=0x%08x",
block.Header.HdrNo, checksum, utils.CalculateBlockChecksum(blockData))
}

return block, nil
}

// IsEmptyBlock checks if a block is empty (data_len == 0)
func (p *BlockParser) IsEmptyBlock(block *types.LogBlock) bool {
return block.Header.DataLen == 0
}

// IsFullBlock checks if a block is completely full
func (p *BlockParser) IsFullBlock(block *types.LogBlock) bool {
return block.Header.DataLen == types.OSFileLogBlockSize
}

// GetBlockLSN calculates the starting LSN for a block given its header number and epoch
func (p *BlockParser) GetBlockLSN(hdrNo uint32, epochNo uint32) types.LSN {
// LSN = epoch_no * LOG_BLOCK_MAX_NO * OS_FILE_LOG_BLOCK_SIZE + hdr_no * OS_FILE_LOG_BLOCK_SIZE
lsn := uint64(epochNo) * uint64(types.LogBlockMaxNo) * uint64(types.OSFileLogBlockSize)
lsn += uint64(hdrNo) * uint64(types.OSFileLogBlockSize)
return types.LSN(lsn)
}
