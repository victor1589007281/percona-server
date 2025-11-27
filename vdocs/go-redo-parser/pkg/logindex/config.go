package logindex

// Config holds LogIndex configuration
type Config struct {
	Enabled          bool   // Enable/disable LogIndex
	StorageDir       string // Storage directory
	HotCacheSize     int    // Hot cache size (number of pages)
	WarmCacheSize    int    // Warm cache size
	MaxMemoryMB      int    // Max memory usage in MB
	FlushIntervalSec int    // Flush interval in seconds
	EnableCompression bool   // Enable compression
	PurgeThresholdMB int    // Purge threshold
}

// DefaultConfig returns default configuration
func DefaultConfig() *Config {
	return &Config{
		Enabled:           true,
		StorageDir:        "/tmp/logindex",
		HotCacheSize:      10000,
		WarmCacheSize:     50000,
		MaxMemoryMB:       2048,
		FlushIntervalSec:  5,
		EnableCompression: true,
		PurgeThresholdMB:  100,
	}
}
