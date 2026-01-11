/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora System Variables Implementation

*****************************************************************************/

#include "aurora_sysvars.h"
#include <cstring>

// ============================================================================
// Variable Definitions
// ============================================================================

bool srv_aurora_mode = false;
char* srv_aurora_volume_id = nullptr;
char* srv_aurora_storage_nodes = nullptr;
char* srv_aurora_metadata_nodes = nullptr;
char* srv_aurora_instance_mode = nullptr;

unsigned long srv_aurora_redo_buffer_size = 16 * 1024 * 1024;  // 16MB
unsigned int srv_aurora_redo_flush_interval_ms = 10;
unsigned int srv_aurora_quorum_write = 4;
unsigned int srv_aurora_quorum_read = 3;
unsigned int srv_aurora_quorum_timeout_ms = 5000;

unsigned int srv_aurora_reader_sync_interval_ms = 10;
unsigned int srv_aurora_reader_sync_batch_size = 1000;

bool srv_aurora_binlog_compat = true;
unsigned long srv_aurora_binlog_buffer_size = 256 * 1024 * 1024;  // 256MB
unsigned int srv_aurora_binlog_buffer_count = 65536;
char* srv_aurora_binlog_file_prefix = nullptr;
unsigned long srv_aurora_binlog_rotate_size = 1073741824;  // 1GB

char* srv_aurora_replication_protocol = nullptr;
unsigned int srv_aurora_raft_heartbeat_ms = 100;
unsigned int srv_aurora_raft_election_timeout_ms = 500;
char* srv_aurora_raft_replication_mode = nullptr;

char* srv_aurora_transport_type = nullptr;
char* srv_aurora_rdma_device = nullptr;
unsigned int srv_aurora_rdma_redo_buffer_mb = 256;
bool srv_aurora_tcp_fallback = true;

bool srv_aurora_auto_olap = false;
unsigned long srv_aurora_olap_threshold_rows = 10000;

unsigned int srv_aurora_page_cache_size = 10000;
unsigned int srv_aurora_grpc_max_connections = 10;
unsigned int srv_aurora_grpc_timeout_sec = 30;

bool srv_aurora_redo_compression = true;
char* srv_aurora_compression_algorithm = nullptr;

bool srv_aurora_debug = false;

// ============================================================================
// Default String Values
// ============================================================================

static const char* DEFAULT_VOLUME_ID = "";
static const char* DEFAULT_STORAGE_NODES = "";
static const char* DEFAULT_METADATA_NODES = "";
static const char* DEFAULT_INSTANCE_MODE = "writer";
static const char* DEFAULT_BINLOG_PREFIX = "mysql-bin";
static const char* DEFAULT_REPLICATION_PROTOCOL = "quorum";
static const char* DEFAULT_RAFT_REPLICATION_MODE = "sync";
static const char* DEFAULT_TRANSPORT_TYPE = "tcp";
static const char* DEFAULT_RDMA_DEVICE = "mlx5_0";
static const char* DEFAULT_COMPRESSION_ALGORITHM = "lz4";

// ============================================================================
// Helper Functions
// ============================================================================

static char* copy_string(const char* src) {
  if (!src) return nullptr;
  size_t len = strlen(src) + 1;
  char* dst = new char[len];
  memcpy(dst, src, len);
  return dst;
}

// ============================================================================
// Initialization and Validation
// ============================================================================

void aurora_sysvars_init_defaults() {
  // Initialize string variables with defaults
  srv_aurora_volume_id = copy_string(DEFAULT_VOLUME_ID);
  srv_aurora_storage_nodes = copy_string(DEFAULT_STORAGE_NODES);
  srv_aurora_metadata_nodes = copy_string(DEFAULT_METADATA_NODES);
  srv_aurora_instance_mode = copy_string(DEFAULT_INSTANCE_MODE);
  srv_aurora_binlog_file_prefix = copy_string(DEFAULT_BINLOG_PREFIX);
  srv_aurora_replication_protocol = copy_string(DEFAULT_REPLICATION_PROTOCOL);
  srv_aurora_raft_replication_mode = copy_string(DEFAULT_RAFT_REPLICATION_MODE);
  srv_aurora_transport_type = copy_string(DEFAULT_TRANSPORT_TYPE);
  srv_aurora_rdma_device = copy_string(DEFAULT_RDMA_DEVICE);
  srv_aurora_compression_algorithm = copy_string(DEFAULT_COMPRESSION_ALGORITHM);
}

bool aurora_sysvars_validate() {
  if (!srv_aurora_mode) {
    return true;  // Aurora not enabled, skip validation
  }
  
  // Check required settings when Aurora is enabled
  if (!srv_aurora_volume_id || strlen(srv_aurora_volume_id) == 0) {
    // Error: volume_id is required when aurora_mode=ON
    return false;
  }
  
  if (!srv_aurora_storage_nodes || strlen(srv_aurora_storage_nodes) == 0) {
    // Error: storage_nodes is required when aurora_mode=ON
    return false;
  }
  
  if (!srv_aurora_metadata_nodes || strlen(srv_aurora_metadata_nodes) == 0) {
    // Error: metadata_nodes is required when aurora_mode=ON
    return false;
  }
  
  // Validate quorum settings
  if (srv_aurora_quorum_write < 1 || srv_aurora_quorum_write > 6) {
    // Error: quorum_write must be between 1 and 6
    return false;
  }
  
  if (srv_aurora_quorum_read < 1 || srv_aurora_quorum_read > 6) {
    // Error: quorum_read must be between 1 and 6
    return false;
  }
  
  // Validate instance mode
  if (srv_aurora_instance_mode) {
    if (strcmp(srv_aurora_instance_mode, "writer") != 0 &&
        strcmp(srv_aurora_instance_mode, "reader") != 0) {
      // Error: instance_mode must be "writer" or "reader"
      return false;
    }
  }
  
  // Validate replication protocol
  if (srv_aurora_replication_protocol) {
    if (strcmp(srv_aurora_replication_protocol, "quorum") != 0 &&
        strcmp(srv_aurora_replication_protocol, "raft") != 0) {
      // Error: replication_protocol must be "quorum" or "raft"
      return false;
    }
  }
  
  // Validate transport type
  if (srv_aurora_transport_type) {
    if (strcmp(srv_aurora_transport_type, "tcp") != 0 &&
        strcmp(srv_aurora_transport_type, "rdma") != 0) {
      // Error: transport_type must be "tcp" or "rdma"
      return false;
    }
  }
  
  return true;
}

void aurora_sysvars_apply() {
  // Apply configuration changes
  // This is called when variables are modified at runtime
  
  // Note: Most Aurora configuration changes require restart
  // Only a few variables can be changed at runtime:
  // - aurora_debug
  // - aurora_reader_sync_interval_ms
  // - aurora_reader_sync_batch_size
}
