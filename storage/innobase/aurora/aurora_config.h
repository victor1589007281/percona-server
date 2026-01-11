/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Configuration Header
Defines configuration parameters for Aurora distributed storage integration.

*****************************************************************************/

#ifndef AURORA_CONFIG_H
#define AURORA_CONFIG_H

#include <string>
#include <vector>
#include <cstdint>

namespace aurora {

/**
 * Aurora operation mode
 */
enum class AuroraMode {
  DISABLED = 0,    // Aurora disabled, use local storage
  WRITER = 1,      // Writer instance (primary)
  READER = 2       // Reader instance (replica)
};

/**
 * Aurora configuration
 */
struct AuroraConfig {
  // Basic settings
  bool enabled = false;
  AuroraMode mode = AuroraMode::DISABLED;
  std::string instance_id;
  std::string volume_id;
  std::string cluster_id;
  
  // Storage nodes
  std::vector<std::string> storage_nodes;
  
  // Metadata service
  std::vector<std::string> metadata_nodes;
  
  // Control plane
  std::string control_plane_endpoint;
  
  // Network settings
  int grpc_port = 9001;
  int connection_timeout_ms = 5000;
  int request_timeout_ms = 10000;
  int max_connections_per_node = 10;
  
  // Quorum settings
  int quorum_n = 6;   // Total replicas
  int quorum_vw = 4;  // Write quorum
  int quorum_vr = 3;  // Read quorum
  
  // Buffer settings
  size_t redo_buffer_size = 16 * 1024 * 1024;  // 16MB
  size_t max_batch_size = 1024 * 1024;          // 1MB
  int batch_timeout_ms = 1;
  
  // Compression
  bool enable_compression = true;
  int compression_level = 1;  // LZ4 fast
  
  // Retry settings
  int max_retries = 3;
  int retry_delay_ms = 100;
  
  // Health check
  int health_check_interval_ms = 5000;
  int health_check_timeout_ms = 3000;
  
  // VDL settings
  int vdl_update_interval_ms = 100;
};

/**
 * Global Aurora configuration instance
 */
extern AuroraConfig aurora_config;

/**
 * Initialize Aurora configuration from MySQL system variables
 */
void aurora_config_init();

/**
 * Check if Aurora is enabled
 */
inline bool aurora_is_enabled() {
  return aurora_config.enabled;
}

/**
 * Check if this is a writer instance
 */
inline bool aurora_is_writer() {
  return aurora_config.enabled && aurora_config.mode == AuroraMode::WRITER;
}

/**
 * Check if this is a reader instance
 */
inline bool aurora_is_reader() {
  return aurora_config.enabled && aurora_config.mode == AuroraMode::READER;
}

}  // namespace aurora

#endif  // AURORA_CONFIG_H
