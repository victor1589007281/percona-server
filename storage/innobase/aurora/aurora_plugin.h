/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Plugin Entry Point
Main entry point for Aurora plugin initialization.

*****************************************************************************/

#ifndef AURORA_PLUGIN_H
#define AURORA_PLUGIN_H

#include <cstdint>
#include <string>

namespace aurora {

// Forward declarations
struct AuroraConfig;

/**
 * Aurora Plugin Version
 */
constexpr uint32_t AURORA_PLUGIN_VERSION_MAJOR = 1;
constexpr uint32_t AURORA_PLUGIN_VERSION_MINOR = 0;
constexpr uint32_t AURORA_PLUGIN_VERSION_PATCH = 0;

inline std::string get_version_string() {
  return std::to_string(AURORA_PLUGIN_VERSION_MAJOR) + "." +
         std::to_string(AURORA_PLUGIN_VERSION_MINOR) + "." +
         std::to_string(AURORA_PLUGIN_VERSION_PATCH);
}

/**
 * Aurora Plugin State
 */
enum class PluginState {
  UNLOADED,
  LOADING,
  LOADED,
  ACTIVE,
  STOPPING,
  ERROR
};

/**
 * Initialize Aurora plugin
 * Called during MySQL plugin initialization
 *
 * @return 0 on success, non-zero on failure
 */
int aurora_plugin_init(void* p);

/**
 * Deinitialize Aurora plugin
 * Called during MySQL plugin shutdown
 *
 * @return 0 on success
 */
int aurora_plugin_deinit(void* p);

/**
 * Get plugin state
 */
PluginState aurora_plugin_get_state();

/**
 * Get last error message
 */
const char* aurora_plugin_get_error();

/**
 * Check if Aurora plugin is active
 */
bool aurora_plugin_is_active();

/**
 * Aurora Plugin Configuration
 * Loaded from MySQL system variables
 */
struct PluginConfig {
  bool enabled;
  std::string volume_id;
  std::string storage_endpoints;
  std::string metadata_endpoints;
  std::string instance_mode;
  
  uint32_t quorum_write;
  uint32_t quorum_read;
  uint32_t quorum_timeout_ms;
  
  uint64_t redo_buffer_size;
  uint32_t redo_flush_interval_ms;
  
  bool binlog_compat;
  uint64_t binlog_buffer_size;
  
  std::string replication_protocol;
  std::string transport_type;
  
  bool auto_olap;
  uint64_t olap_row_threshold;
  
  bool debug;
};

/**
 * Load configuration from MySQL system variables
 */
PluginConfig load_plugin_config();

/**
 * Convert PluginConfig to AuroraConfig
 */
AuroraConfig to_aurora_config(const PluginConfig& pc);

}  // namespace aurora

#endif  // AURORA_PLUGIN_H
