/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Plugin Entry Point Implementation

*****************************************************************************/

#include "aurora_plugin.h"
#include "aurora.h"
#include "aurora_config.h"
#include "aurora_hook.h"
#include "aurora_startup.h"
#include "aurora_sysvars.h"
#include "aurora_gtid.h"
#include "aurora_binlog.h"
#include "aurora_olap_router.h"

#include <cstring>

namespace aurora {

// Global plugin state
static PluginState g_plugin_state = PluginState::UNLOADED;
static std::string g_plugin_error;

//============================================================================
// Configuration Loading
//============================================================================

PluginConfig load_plugin_config() {
  PluginConfig config;
  
  config.enabled = srv_aurora_mode;
  config.volume_id = srv_aurora_volume_id ? srv_aurora_volume_id : "";
  config.storage_endpoints = srv_aurora_storage_nodes ? srv_aurora_storage_nodes : "";
  config.metadata_endpoints = srv_aurora_metadata_nodes ? srv_aurora_metadata_nodes : "";
  config.instance_mode = srv_aurora_instance_mode ? srv_aurora_instance_mode : "writer";
  
  config.quorum_write = srv_aurora_quorum_write;
  config.quorum_read = srv_aurora_quorum_read;
  config.quorum_timeout_ms = srv_aurora_quorum_timeout_ms;
  
  config.redo_buffer_size = srv_aurora_redo_buffer_size;
  config.redo_flush_interval_ms = srv_aurora_redo_flush_interval_ms;
  
  config.binlog_compat = srv_aurora_binlog_compat;
  config.binlog_buffer_size = srv_aurora_binlog_buffer_size;
  
  config.replication_protocol = srv_aurora_replication_protocol ? 
                                 srv_aurora_replication_protocol : "quorum";
  config.transport_type = srv_aurora_transport_type ?
                          srv_aurora_transport_type : "tcp";
  
  config.auto_olap = srv_aurora_auto_olap;
  config.olap_row_threshold = srv_aurora_olap_threshold_rows;
  
  config.debug = srv_aurora_debug;
  
  return config;
}

AuroraConfig to_aurora_config(const PluginConfig& pc) {
  AuroraConfig config;
  
  config.enabled = pc.enabled;
  config.volume_id = pc.volume_id;
  config.storage_endpoints = pc.storage_endpoints;
  config.metadata_endpoints = pc.metadata_endpoints;
  config.is_writer = (pc.instance_mode == "writer");
  config.instance_id = ""; // Generated later
  
  config.quorum_write = pc.quorum_write;
  config.quorum_read = pc.quorum_read;
  config.quorum_timeout_ms = pc.quorum_timeout_ms;
  
  config.redo_buffer_size = pc.redo_buffer_size;
  config.redo_flush_interval_ms = pc.redo_flush_interval_ms;
  
  config.binlog_compat = pc.binlog_compat;
  config.binlog_buffer_size = pc.binlog_buffer_size;
  config.server_id = 1;  // TODO: Get from MySQL
  
  config.replication_protocol = pc.replication_protocol;
  config.transport_type = pc.transport_type;
  
  config.max_reader_lag_lsn = 1000;  // Default
  config.is_first_start = false;  // Determined later
  
  return config;
}

//============================================================================
// Plugin Lifecycle
//============================================================================

int aurora_plugin_init(void* p) {
  g_plugin_state = PluginState::LOADING;
  g_plugin_error.clear();
  
  // Initialize system variable defaults
  aurora_sysvars_init_defaults();
  
  // Load configuration
  PluginConfig plugin_config = load_plugin_config();
  
  // Check if Aurora is enabled
  if (!plugin_config.enabled) {
    g_plugin_state = PluginState::LOADED;
    return 0;  // Success, but not active
  }
  
  // Validate configuration
  if (!aurora_sysvars_validate()) {
    g_plugin_error = "Aurora configuration validation failed";
    g_plugin_state = PluginState::ERROR;
    return 1;
  }
  
  // Convert to Aurora config
  AuroraConfig aurora_config = to_aurora_config(plugin_config);
  
  // Generate instance ID
  aurora_config.instance_id = aurora_config.is_writer ? 
                               "writer-" + aurora_config.volume_id :
                               "reader-" + aurora_config.volume_id;
  
  // Initialize Aurora
  if (!aurora_startup(aurora_config)) {
    g_plugin_error = g_startup_ctx.error_message;
    g_plugin_state = PluginState::ERROR;
    return 1;
  }
  
  // Initialize OLAP router
  if (plugin_config.auto_olap) {
    aurora_olap_router_init();
    if (g_olap_router) {
      g_olap_router->set_auto_olap(true);
      g_olap_router->set_row_threshold(plugin_config.olap_row_threshold);
      // OLAP availability depends on separate OLAP service
      // g_olap_router->set_olap_available(true);
    }
  }
  
  g_plugin_state = PluginState::ACTIVE;
  
  // Log success
  // ib::info() << "Aurora plugin initialized, version " << get_version_string()
  //            << ", mode: " << (aurora_config.is_writer ? "WRITER" : "READER");
  
  return 0;
}

int aurora_plugin_deinit(void* p) {
  if (g_plugin_state == PluginState::UNLOADED) {
    return 0;
  }
  
  g_plugin_state = PluginState::STOPPING;
  
  // Shutdown OLAP router
  aurora_olap_router_shutdown();
  
  // Shutdown Aurora
  aurora_shutdown();
  
  g_plugin_state = PluginState::UNLOADED;
  
  return 0;
}

PluginState aurora_plugin_get_state() {
  return g_plugin_state;
}

const char* aurora_plugin_get_error() {
  return g_plugin_error.c_str();
}

bool aurora_plugin_is_active() {
  return g_plugin_state == PluginState::ACTIVE;
}

}  // namespace aurora

//============================================================================
// MySQL Plugin Declaration
// This would be in a separate file for MySQL plugin system
//============================================================================

/*
#include <mysql/plugin.h>

static int aurora_init(void* p) {
  return aurora::aurora_plugin_init(p);
}

static int aurora_deinit(void* p) {
  return aurora::aurora_plugin_deinit(p);
}

mysql_declare_plugin(aurora)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &aurora_storage_engine_handler,
  "aurora",
  "Aurora Team",
  "Aurora distributed storage plugin for MySQL",
  PLUGIN_LICENSE_GPL,
  aurora_init,
  nullptr,
  aurora_deinit,
  0x0100,
  nullptr,  // status vars
  nullptr,  // system vars
  nullptr,  // config options
  0,
}
mysql_declare_plugin_end;
*/
