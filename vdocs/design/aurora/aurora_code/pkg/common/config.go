// Package common provides common utilities and configurations
package common

import (
	"encoding/json"
	"os"
	"time"
)

// ServiceConfig represents common service configuration
type ServiceConfig struct {
	NodeID      string        `json:"node_id"`
	GRPCPort    int           `json:"grpc_port"`
	MetricsPort int           `json:"metrics_port"`
	LogLevel    string        `json:"log_level"`
	DataDir     string        `json:"data_dir"`
	Timeout     time.Duration `json:"timeout"`
}

// LoadConfig loads configuration from a JSON file
func LoadConfig(path string, config interface{}) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	return json.Unmarshal(data, config)
}

// SaveConfig saves configuration to a JSON file
func SaveConfig(path string, config interface{}) error {
	data, err := json.MarshalIndent(config, "", "  ")
	if err != nil {
		return err
	}
	return os.WriteFile(path, data, 0644)
}

// NetworkConfig represents network configuration
type NetworkConfig struct {
	// Connection pool settings
	MaxConnections    int           `json:"max_connections"`
	MinConnections    int           `json:"min_connections"`
	ConnectionTimeout time.Duration `json:"connection_timeout"`
	IdleTimeout       time.Duration `json:"idle_timeout"`
	
	// Retry settings
	MaxRetries     int           `json:"max_retries"`
	RetryInterval  time.Duration `json:"retry_interval"`
	RetryBackoff   float64       `json:"retry_backoff"`
	
	// Compression
	EnableCompression bool   `json:"enable_compression"`
	CompressionLevel  int    `json:"compression_level"`
	
	// Buffer sizes
	ReadBufferSize  int `json:"read_buffer_size"`
	WriteBufferSize int `json:"write_buffer_size"`
}

// DefaultNetworkConfig returns default network configuration
func DefaultNetworkConfig() NetworkConfig {
	return NetworkConfig{
		MaxConnections:    100,
		MinConnections:    10,
		ConnectionTimeout: 5 * time.Second,
		IdleTimeout:       60 * time.Second,
		MaxRetries:        3,
		RetryInterval:     100 * time.Millisecond,
		RetryBackoff:      2.0,
		EnableCompression: true,
		CompressionLevel:  6,
		ReadBufferSize:    64 * 1024,
		WriteBufferSize:   64 * 1024,
	}
}

// QuorumConfig represents quorum configuration
type QuorumConfig struct {
	N  int `json:"n"`   // Total replicas
	Vw int `json:"vw"`  // Write quorum
	Vr int `json:"vr"`  // Read quorum
}

// DefaultQuorumConfig returns the default Aurora quorum configuration
func DefaultQuorumConfig() QuorumConfig {
	return QuorumConfig{
		N:  6,
		Vw: 4,
		Vr: 3,
	}
}
