/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Configuration Implementation

*****************************************************************************/

#include "aurora_config.h"
#include <sstream>
#include <algorithm>

namespace aurora {

// Global configuration instance
AuroraConfig aurora_config;

// Parse comma-separated string into vector
static std::vector<std::string> parse_nodes(const std::string& nodes_str) {
  std::vector<std::string> result;
  std::stringstream ss(nodes_str);
  std::string node;
  
  while (std::getline(ss, node, ',')) {
    // Trim whitespace
    node.erase(0, node.find_first_not_of(" \t"));
    node.erase(node.find_last_not_of(" \t") + 1);
    
    if (!node.empty()) {
      result.push_back(node);
    }
  }
  
  return result;
}

void aurora_config_init() {
  // These would be populated from MySQL system variables
  // For now, use defaults
  
  aurora_config.enabled = false;
  aurora_config.mode = AuroraMode::DISABLED;
  aurora_config.instance_id = "";
  aurora_config.volume_id = "";
  aurora_config.cluster_id = "";
  
  aurora_config.storage_nodes.clear();
  aurora_config.metadata_nodes.clear();
  aurora_config.control_plane_endpoint = "";
  
  // Default quorum settings (Aurora style)
  aurora_config.quorum_n = 6;
  aurora_config.quorum_vw = 4;
  aurora_config.quorum_vr = 3;
  
  // Default network settings
  aurora_config.grpc_port = 9001;
  aurora_config.connection_timeout_ms = 5000;
  aurora_config.request_timeout_ms = 10000;
  aurora_config.max_connections_per_node = 10;
  
  // Default buffer settings
  aurora_config.redo_buffer_size = 16 * 1024 * 1024;  // 16MB
  aurora_config.max_batch_size = 1024 * 1024;          // 1MB
  aurora_config.batch_timeout_ms = 1;
  
  // Default compression
  aurora_config.enable_compression = true;
  aurora_config.compression_level = 1;
  
  // Default retry settings
  aurora_config.max_retries = 3;
  aurora_config.retry_delay_ms = 100;
  
  // Default health check
  aurora_config.health_check_interval_ms = 5000;
  aurora_config.health_check_timeout_ms = 3000;
  
  // Default VDL settings
  aurora_config.vdl_update_interval_ms = 100;
}

}  // namespace aurora
