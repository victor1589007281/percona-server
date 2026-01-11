/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Management Commands Implementation

*****************************************************************************/

#include "aurora_commands.h"
#include "aurora.h"
#include "aurora_config.h"
#include "aurora_types.h"
#include "aurora_gtid.h"
#include "aurora_binlog.h"
#include <sstream>
#include <iomanip>

namespace aurora {

// External references (defined in aurora.cc)
extern AuroraConfig g_config;
extern std::atomic<uint64_t> g_current_lsn;
extern std::atomic<uint64_t> g_vdl;
extern std::atomic<bool> g_is_writer;
extern std::atomic<bool> g_writes_frozen;

//============================================================================
// Status Query Functions
//============================================================================

AuroraStatusInfo get_aurora_status() {
  AuroraStatusInfo info;
  
  info.current_lsn = g_current_lsn.load();
  info.vdl = g_vdl.load();
  info.vcl = info.vdl;  // VCL >= VDL
  info.instance_role = g_is_writer.load() ? "WRITER" : "READER";
  info.reader_count = 0;  // TODO: Query from metadata service
  info.storage_nodes = 6;
  info.healthy_nodes = 6;  // TODO: Query actual status
  info.volume_id = g_config.volume_id;
  info.binlog_compat = g_config.binlog_compat;
  
  if (g_gtid_manager) {
    info.gtid_executed = g_gtid_manager->get_executed_set().to_string();
  }
  
  return info;
}

AuroraReplicaStatusInfo get_aurora_replica_status() {
  AuroraReplicaStatusInfo info;
  
  info.read_point = 0;  // TODO: Get from reader sync module
  info.target_vdl = g_vdl.load();
  info.lag_lsn = info.target_vdl - info.read_point;
  info.lag_ms = 0;  // TODO: Calculate based on timestamps
  info.redo_applied = 0;
  info.pages_invalidated = 0;
  info.sync_status = "SYNCED";
  
  return info;
}

std::vector<StorageNodeInfo> get_storage_nodes_status() {
  std::vector<StorageNodeInfo> nodes;
  
  // Parse storage nodes from config
  std::stringstream ss(g_config.storage_endpoints);
  std::string endpoint;
  int node_num = 1;
  
  while (std::getline(ss, endpoint, ',')) {
    StorageNodeInfo node;
    node.node_id = "node-" + std::to_string(node_num);
    
    // Assign AZ based on node number
    if (node_num <= 2) node.az = "az-a";
    else if (node_num <= 4) node.az = "az-b";
    else node.az = "az-c";
    
    node.is_healthy = true;  // TODO: Query actual status
    node.lsn = g_vdl.load();
    node.lag_bytes = 0;
    node.endpoint = endpoint;
    node.disk_used_bytes = 0;
    node.disk_total_bytes = 0;
    
    nodes.push_back(node);
    node_num++;
  }
  
  return nodes;
}

RedoStatsInfo get_redo_stats() {
  RedoStatsInfo stats;
  
  // TODO: Get actual stats from redo sender
  stats.redo_generated = 0;
  stats.redo_bytes = 0;
  stats.redo_batches = 0;
  stats.quorum_latency_avg_ms = 0.0;
  stats.quorum_latency_p99_ms = 0.0;
  stats.quorum_success = 0;
  stats.quorum_failed = 0;
  
  return stats;
}

BufferStatsInfo get_buffer_stats() {
  BufferStatsInfo stats;
  
  // TODO: Get actual stats from buffer pool
  stats.pages_total = 0;
  stats.pages_data = 0;
  stats.pages_dirty = 0;
  stats.hit_rate = 99.5;
  stats.remote_reads = 0;
  stats.materializations = 0;
  
  return stats;
}

std::vector<FailoverHistoryEntry> get_failover_history() {
  std::vector<FailoverHistoryEntry> history;
  // TODO: Query from control plane
  return history;
}

//============================================================================
// Command Execution Functions
//============================================================================

bool execute_failover_to(const std::string& target_instance) {
  // TODO: Send failover request to control plane
  return true;
}

bool execute_freeze_writes() {
  g_writes_frozen.store(true);
  return true;
}

bool execute_unfreeze_writes() {
  g_writes_frozen.store(false);
  return true;
}

bool execute_add_reader(const std::string& reader_id, const std::string& az) {
  // TODO: Send request to control plane
  return true;
}

bool execute_remove_reader(const std::string& reader_id) {
  // TODO: Send request to control plane
  return true;
}

//============================================================================
// SHOW Command Handlers
//============================================================================

void handle_show_aurora_status(RowCallback callback) {
  auto status = get_aurora_status();
  
  callback({"aurora_current_lsn", std::to_string(status.current_lsn)});
  callback({"aurora_vdl", std::to_string(status.vdl)});
  callback({"aurora_vcl", std::to_string(status.vcl)});
  callback({"aurora_instance_role", status.instance_role});
  callback({"aurora_reader_count", std::to_string(status.reader_count)});
  callback({"aurora_storage_nodes", std::to_string(status.storage_nodes)});
  callback({"aurora_healthy_nodes", std::to_string(status.healthy_nodes)});
  callback({"aurora_volume_id", status.volume_id});
  callback({"aurora_gtid_executed", status.gtid_executed});
  callback({"aurora_binlog_compat", status.binlog_compat ? "ON" : "OFF"});
}

void handle_show_aurora_replica_status(RowCallback callback) {
  auto status = get_aurora_replica_status();
  
  callback({"aurora_read_point", std::to_string(status.read_point)});
  callback({"aurora_target_vdl", std::to_string(status.target_vdl)});
  callback({"aurora_lag_lsn", std::to_string(status.lag_lsn)});
  callback({"aurora_lag_ms", std::to_string(status.lag_ms)});
  callback({"aurora_redo_applied", std::to_string(status.redo_applied)});
  callback({"aurora_pages_invalidated", std::to_string(status.pages_invalidated)});
  callback({"aurora_sync_status", status.sync_status});
}

void handle_show_aurora_storage_nodes(RowCallback callback) {
  auto nodes = get_storage_nodes_status();
  
  for (const auto& node : nodes) {
    callback({
      node.node_id,
      node.az,
      node.is_healthy ? "YES" : "NO",
      std::to_string(node.lsn),
      std::to_string(node.lag_bytes),
      node.endpoint
    });
  }
}

void handle_show_aurora_redo_stats(RowCallback callback) {
  auto stats = get_redo_stats();
  
  callback({"aurora_redo_generated", std::to_string(stats.redo_generated)});
  callback({"aurora_redo_bytes", std::to_string(stats.redo_bytes)});
  callback({"aurora_redo_batches", std::to_string(stats.redo_batches)});
  
  std::ostringstream avg_ss, p99_ss;
  avg_ss << std::fixed << std::setprecision(2) << stats.quorum_latency_avg_ms;
  p99_ss << std::fixed << std::setprecision(2) << stats.quorum_latency_p99_ms;
  
  callback({"aurora_quorum_latency_avg", avg_ss.str()});
  callback({"aurora_quorum_latency_p99", p99_ss.str()});
  callback({"aurora_quorum_success", std::to_string(stats.quorum_success)});
  callback({"aurora_quorum_failed", std::to_string(stats.quorum_failed)});
}

void handle_show_aurora_buffer_stats(RowCallback callback) {
  auto stats = get_buffer_stats();
  
  callback({"aurora_bp_pages_total", std::to_string(stats.pages_total)});
  callback({"aurora_bp_pages_data", std::to_string(stats.pages_data)});
  callback({"aurora_bp_pages_dirty", std::to_string(stats.pages_dirty)});
  
  std::ostringstream rate_ss;
  rate_ss << std::fixed << std::setprecision(1) << stats.hit_rate;
  
  callback({"aurora_bp_hit_rate", rate_ss.str()});
  callback({"aurora_bp_remote_reads", std::to_string(stats.remote_reads)});
  callback({"aurora_bp_materializations", std::to_string(stats.materializations)});
}

void handle_show_aurora_failover_history(RowCallback callback) {
  auto history = get_failover_history();
  
  for (const auto& entry : history) {
    callback({
      entry.failover_id,
      entry.start_time,
      entry.end_time,
      entry.old_writer,
      entry.new_writer,
      entry.status
    });
  }
}

//============================================================================
// Column Definitions
//============================================================================

std::vector<ColumnDef> get_aurora_status_columns() {
  return {
    {"Variable_name", "VARCHAR", 40},
    {"Value", "VARCHAR", 100}
  };
}

std::vector<ColumnDef> get_aurora_replica_status_columns() {
  return {
    {"Variable_name", "VARCHAR", 40},
    {"Value", "VARCHAR", 100}
  };
}

std::vector<ColumnDef> get_aurora_storage_nodes_columns() {
  return {
    {"node_id", "VARCHAR", 20},
    {"az", "VARCHAR", 10},
    {"is_healthy", "VARCHAR", 5},
    {"lsn", "BIGINT", 20},
    {"lag_bytes", "BIGINT", 20},
    {"endpoint", "VARCHAR", 50}
  };
}

}  // namespace aurora
