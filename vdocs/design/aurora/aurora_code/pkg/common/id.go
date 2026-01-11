package common

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"sync/atomic"
	"time"
)

var globalCounter atomic.Int64

// GenerateID generates a unique ID with prefix
func GenerateID(prefix string) string {
	counter := globalCounter.Add(1)
	timestamp := time.Now().UnixNano()
	return fmt.Sprintf("%s-%d-%d", prefix, timestamp, counter)
}

// GenerateUUID generates a UUID-like string
func GenerateUUID() string {
	bytes := make([]byte, 16)
	rand.Read(bytes)
	
	// Set version (4) and variant (2)
	bytes[6] = (bytes[6] & 0x0f) | 0x40
	bytes[8] = (bytes[8] & 0x3f) | 0x80
	
	return fmt.Sprintf("%x-%x-%x-%x-%x",
		bytes[0:4], bytes[4:6], bytes[6:8], bytes[8:10], bytes[10:16])
}

// GenerateVolumeID generates a volume ID
func GenerateVolumeID() string {
	return GenerateID("vol")
}

// GenerateClusterID generates a cluster ID
func GenerateClusterID() string {
	return GenerateID("cluster")
}

// GenerateInstanceID generates an instance ID
func GenerateInstanceID(role string) string {
	return GenerateID(role)
}

// GenerateSnapshotID generates a snapshot ID
func GenerateSnapshotID() string {
	return GenerateID("snap")
}

// GenerateTaskID generates a task ID
func GenerateTaskID(taskType string) string {
	return GenerateID(taskType)
}

// GenerateRandomBytes generates random bytes
func GenerateRandomBytes(n int) []byte {
	bytes := make([]byte, n)
	rand.Read(bytes)
	return bytes
}

// GenerateRandomHex generates a random hex string
func GenerateRandomHex(n int) string {
	bytes := GenerateRandomBytes(n)
	return hex.EncodeToString(bytes)
}

// ParseVolumeUUID parses a volume ID into UUID bytes
func ParseVolumeUUID(volumeID string) [16]byte {
	var uuid [16]byte
	
	// Simple hash of the volumeID
	for i, c := range volumeID {
		uuid[i%16] ^= byte(c)
	}
	
	return uuid
}
