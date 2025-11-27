package utils

import (
"hash/crc32"
)

// CalculateBlockChecksum calculates the CRC32 checksum for a log block
// The checksum is calculated over the first 508 bytes (header + data)
func CalculateBlockChecksum(blockData []byte) uint32 {
if len(blockData) < 508 {
return 0
}

// Calculate CRC32 over first 508 bytes (excluding the 4-byte trailer)
return crc32.ChecksumIEEE(blockData[:508])
}

// VerifyBlockChecksum verifies the checksum of a log block
func VerifyBlockChecksum(blockData []byte, expectedChecksum uint32, noChecksumMagic uint32) bool {
// If checksums are disabled, check for magic value
if expectedChecksum == noChecksumMagic {
return true
}

calculated := CalculateBlockChecksum(blockData)
return calculated == expectedChecksum
}
