/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Main Implementation
Main entry point for Aurora integration with InnoDB.

*****************************************************************************/

#include "aurora.h"
#include <cstring>

namespace aurora {

// MySQL system variables (declared in aurora.h)
bool aurora_enabled = false;
char* aurora_mode_str = nullptr;
char* aurora_instance_id = nullptr;
char* aurora_volume_id = nullptr;
char* aurora_cluster_id = nullptr;
char* aurora_storage_nodes = nullptr;
char* aurora_metadata_nodes = nullptr;
int aurora_quorum_n = 6;
int aurora_quorum_vw = 4;
int aurora_quorum_vr = 3;
int aurora_connection_timeout_ms = 5000;
int aurora_request_timeout_ms = 10000;
bool aurora_compression_enabled = true;
int aurora_compression_level = 1;

// Initialization state
static bool g_initialized = false;

bool aurora_init() {
  if (g_initialized) return true;
  if (!aurora_enabled) return true;
  
  // Initialize configuration
  aurora_config_init();
  
  // Set config from system variables
  aurora_config.enabled = aurora_enabled;
  
  if (aurora_mode_str) {
    if (strcmp(aurora_mode_str, "writer") == 0) {
      aurora_config.mode = AuroraMode::WRITER;
    } else if (strcmp(aurora_mode_str, "reader") == 0) {
      aurora_config.mode = AuroraMode::READER;
    }
  }
  
  if (aurora_instance_id) {
    aurora_config.instance_id = aurora_instance_id;
  }
  if (aurora_volume_id) {
    aurora_config.volume_id = aurora_volume_id;
  }
  if (aurora_cluster_id) {
    aurora_config.cluster_id = aurora_cluster_id;
  }
  
  // Parse storage nodes
  if (aurora_storage_nodes) {
    std::string nodes_str = aurora_storage_nodes;
    std::stringstream ss(nodes_str);
    std::string node;
    while (std::getline(ss, node, ',')) {
      node.erase(0, node.find_first_not_of(" \t"));
      node.erase(node.find_last_not_of(" \t") + 1);
      if (!node.empty()) {
        aurora_config.storage_nodes.push_back(node);
      }
    }
  }
  
  // Parse metadata nodes
  if (aurora_metadata_nodes) {
    std::string nodes_str = aurora_metadata_nodes;
    std::stringstream ss(nodes_str);
    std::string node;
    while (std::getline(ss, node, ',')) {
      node.erase(0, node.find_first_not_of(" \t"));
      node.erase(node.find_last_not_of(" \t") + 1);
      if (!node.empty()) {
        aurora_config.metadata_nodes.push_back(node);
      }
    }
  }
  
  aurora_config.quorum_n = aurora_quorum_n;
  aurora_config.quorum_vw = aurora_quorum_vw;
  aurora_config.quorum_vr = aurora_quorum_vr;
  aurora_config.connection_timeout_ms = aurora_connection_timeout_ms;
  aurora_config.request_timeout_ms = aurora_request_timeout_ms;
  aurora_config.enable_compression = aurora_compression_enabled;
  aurora_config.compression_level = aurora_compression_level;
  
  // Initialize clients
  if (!aurora_clients_init()) {
    return false;
  }
  
  // Initialize components based on mode
  if (!aurora_page_reader_init()) {
    aurora_clients_shutdown();
    return false;
  }
  
  if (aurora_is_writer()) {
    if (!aurora_redo_sender_init()) {
      aurora_page_reader_shutdown();
      aurora_clients_shutdown();
      return false;
    }
  }
  
  if (aurora_is_reader()) {
    if (!aurora_reader_sync_init()) {
      aurora_page_reader_shutdown();
      aurora_clients_shutdown();
      return false;
    }
    aurora_reader_sync_start();
  }
  
  // Register instance with metadata service
  if (g_metadata_client) {
    g_metadata_client->register_instance();
  }
  
  g_initialized = true;
  return true;
}

void aurora_shutdown() {
  if (!g_initialized) return;
  
  // Unregister instance
  if (g_metadata_client) {
    g_metadata_client->unregister_instance();
  }
  
  // Shutdown components in reverse order
  aurora_reader_sync_shutdown();
  aurora_redo_sender_shutdown();
  aurora_page_reader_shutdown();
  aurora_clients_shutdown();
  
  g_initialized = false;
}

AuroraStatus aurora_get_status() {
  AuroraStatus status;
  
  status.initialized = g_initialized;
  status.healthy = g_initialized;
  status.mode = aurora_config.mode;
  status.instance_id = aurora_config.instance_id;
  status.volume_id = aurora_config.volume_id;
  
  if (g_redo_sender) {
    status.current_vdl = g_redo_sender->get_vdl();
    status.sent_lsn = g_redo_sender->get_sent_lsn();
    status.healthy = status.healthy && g_redo_sender->is_healthy();
  }
  
  if (g_reader_sync) {
    status.applied_lsn = g_reader_sync->get_applied_lsn();
    status.replication_lag_ms = g_reader_sync->get_lag_ms();
    status.healthy = status.healthy && g_reader_sync->is_healthy();
  }
  
  if (g_storage_client) {
    auto nodes = g_storage_client->get_node_statuses();
    status.storage_nodes_total = nodes.size();
    status.storage_nodes_healthy = 0;
    for (const auto& n : nodes) {
      if (n.is_healthy) status.storage_nodes_healthy++;
    }
  }
  
  status.metadata_connected = g_metadata_client && g_metadata_client->is_healthy();
  
  return status;
}

bool aurora_is_active() {
  return aurora_config.enabled && g_initialized;
}

void aurora_on_redo_write(lsn_t lsn, const uint8_t* log_block, size_t len) {
  if (!aurora_is_active() || !aurora_is_writer()) return;
  
  // Parse redo log block and extract records
  // This is a simplified version - real implementation would parse InnoDB format
  
  RedoRecord record;
  record.lsn = lsn;
  record.space_id = 0;  // Extract from log block
  record.page_id = 0;   // Extract from log block
  record.trx_id = 0;    // Extract from log block
  record.type = RedoType::UNKNOWN;
  record.flags = 0;
  record.data.assign(log_block, log_block + len);
  record.created_at = std::chrono::steady_clock::now();
  
  if (g_redo_sender) {
    g_redo_sender->add_record(std::move(record));
  }
}

bool aurora_on_page_read(uint32_t space_id, uint32_t page_id,
                         uint8_t* buffer, size_t buf_len) {
  if (!aurora_is_active()) return false;
  if (buf_len < PAGE_SIZE) return false;
  
  return aurora_read_page(space_id, page_id, 0, buffer);
}

bool aurora_on_trx_commit(lsn_t commit_lsn, uint32_t timeout_ms) {
  if (!aurora_is_active() || !aurora_is_writer()) {
    return true;  // If not writer or not active, commit proceeds locally
  }
  
  // Flush redo and wait for durability
  if (g_redo_sender) {
    g_redo_sender->flush_and_wait();
    return g_redo_sender->wait_for_lsn(commit_lsn, timeout_ms);
  }
  
  return true;
}

void aurora_on_checkpoint() {
  // Checkpoint handling - update metadata, cleanup old WAL segments, etc.
  if (!aurora_is_active()) return;
  
  // In real implementation:
  // 1. Notify storage nodes about checkpoint
  // 2. Trigger page segment creation
  // 3. Update checkpoint LSN in metadata
}

bool aurora_on_failover(lsn_t* final_lsn) {
  if (!aurora_is_active()) return false;
  
  // Freeze writes to all storage nodes
  if (g_storage_client) {
    return g_storage_client->freeze_writes(final_lsn);
  }
  
  return false;
}

void aurora_invalidate_buffer_pool(space_id_t space_id, page_id_t page_id) {
  // This would call into InnoDB's buffer pool invalidation
  // buf_page_get_gen() with BUF_PEEK_IF_IN_POOL and then invalidate
}

RedoType aurora_convert_redo_type(uint8_t innodb_type) {
  // Map InnoDB redo log types to Aurora types
  // This is a simplified mapping
  switch (innodb_type) {
    case 1:  // MLOG_1BYTE
    case 2:  // MLOG_2BYTES
    case 4:  // MLOG_4BYTES
    case 8:  // MLOG_8BYTES
      return RedoType::UPDATE;
      
    case 20: // MLOG_REC_INSERT
      return RedoType::INSERT;
      
    case 22: // MLOG_REC_DELETE
      return RedoType::DELETE;
      
    case 49: // MLOG_INIT_FILE_PAGE2
      return RedoType::PAGE_INIT;
      
    default:
      return RedoType::UNKNOWN;
  }
}

}  // namespace aurora
