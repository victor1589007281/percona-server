/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Management Commands
SQL commands for Aurora status and management.

Reference: 01_compute_layer.md Section 6

Supported commands:
  SHOW AURORA STATUS
  SHOW AURORA REPLICA STATUS
  SHOW AURORA STORAGE NODES
  SHOW AURORA REDO STATS
  SHOW AURORA BUFFER STATS
  AURORA FAILOVER TO 'instance'
  AURORA FREEZE WRITES
  AURORA UNFREEZE WRITES

*****************************************************************************/

#ifndef AURORA_COMMANDS_H
#define AURORA_COMMANDS_H

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace aurora {

/**
 * Aurora Status Info
 */
struct AuroraStatusInfo {
  uint64_t current_lsn;
  uint64_t vdl;
  uint64_t vcl;
  std::string instance_role;  // "WRITER" or "READER"
  int reader_count;
  int storage_nodes;
  int healthy_nodes;
  std::string gtid_executed;
  bool binlog_compat;
  std::string volume_id;
};

/**
 * Aurora Replica Status Info (for Reader)
 */
struct AuroraReplicaStatusInfo {
  uint64_t read_point;
  uint64_t target_vdl;
  uint64_t lag_lsn;
  uint64_t lag_ms;
  uint64_t redo_applied;
  uint64_t pages_invalidated;
  std::string sync_status;
};

/**
 * Storage Node Status Info
 */
struct StorageNodeInfo {
  std::string node_id;
  std::string az;
  bool is_healthy;
  uint64_t lsn;
  uint64_t lag_bytes;
  std::string endpoint;
  uint64_t disk_used_bytes;
  uint64_t disk_total_bytes;
};

/**
 * Redo Statistics
 */
struct RedoStatsInfo {
  uint64_t redo_generated;
  uint64_t redo_bytes;
  uint64_t redo_batches;
  double quorum_latency_avg_ms;
  double quorum_latency_p99_ms;
  uint64_t quorum_success;
  uint64_t quorum_failed;
};

/**
 * Buffer Pool Statistics
 */
struct BufferStatsInfo {
  uint64_t pages_total;
  uint64_t pages_data;
  uint64_t pages_dirty;
  double hit_rate;
  uint64_t remote_reads;
  uint64_t materializations;
};

/**
 * Failover History Entry
 */
struct FailoverHistoryEntry {
  std::string failover_id;
  std::string start_time;
  std::string end_time;
  std::string old_writer;
  std::string new_writer;
  std::string status;
  uint64_t duration_ms;
};

/**
 * Get Aurora status information
 */
AuroraStatusInfo get_aurora_status();

/**
 * Get Aurora replica status (Reader only)
 */
AuroraReplicaStatusInfo get_aurora_replica_status();

/**
 * Get storage nodes status
 */
std::vector<StorageNodeInfo> get_storage_nodes_status();

/**
 * Get Redo statistics
 */
RedoStatsInfo get_redo_stats();

/**
 * Get Buffer Pool statistics
 */
BufferStatsInfo get_buffer_stats();

/**
 * Get failover history
 */
std::vector<FailoverHistoryEntry> get_failover_history();

/**
 * Execute AURORA FAILOVER TO command
 */
bool execute_failover_to(const std::string& target_instance);

/**
 * Execute AURORA FREEZE WRITES command
 */
bool execute_freeze_writes();

/**
 * Execute AURORA UNFREEZE WRITES command
 */
bool execute_unfreeze_writes();

/**
 * Execute AURORA ADD READER command
 */
bool execute_add_reader(const std::string& reader_id, const std::string& az);

/**
 * Execute AURORA REMOVE READER command
 */
bool execute_remove_reader(const std::string& reader_id);

/**
 * Result callback for SQL commands
 */
using RowCallback = std::function<void(const std::vector<std::string>& row)>;

/**
 * Process SHOW AURORA STATUS command
 */
void handle_show_aurora_status(RowCallback callback);

/**
 * Process SHOW AURORA REPLICA STATUS command
 */
void handle_show_aurora_replica_status(RowCallback callback);

/**
 * Process SHOW AURORA STORAGE NODES command
 */
void handle_show_aurora_storage_nodes(RowCallback callback);

/**
 * Process SHOW AURORA REDO STATS command
 */
void handle_show_aurora_redo_stats(RowCallback callback);

/**
 * Process SHOW AURORA BUFFER STATS command
 */
void handle_show_aurora_buffer_stats(RowCallback callback);

/**
 * Process SHOW AURORA FAILOVER HISTORY command
 */
void handle_show_aurora_failover_history(RowCallback callback);

/**
 * Column definitions for SHOW commands
 */
struct ColumnDef {
  std::string name;
  std::string type;  // "VARCHAR", "BIGINT", "DOUBLE", etc.
  int width;
};

/**
 * Get column definitions for SHOW AURORA STATUS
 */
std::vector<ColumnDef> get_aurora_status_columns();

/**
 * Get column definitions for SHOW AURORA REPLICA STATUS
 */
std::vector<ColumnDef> get_aurora_replica_status_columns();

/**
 * Get column definitions for SHOW AURORA STORAGE NODES
 */
std::vector<ColumnDef> get_aurora_storage_nodes_columns();

}  // namespace aurora

#endif  // AURORA_COMMANDS_H
