/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora System Variables
MySQL system variables for Aurora configuration.

Reference: 01_compute_layer.md Section 11.9

These variables should be registered in sql/sys_vars.cc with
#ifdef HAVE_AURORA conditional compilation.

*****************************************************************************/

#ifndef AURORA_SYSVARS_H
#define AURORA_SYSVARS_H

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Aurora mode enable flag
 */
extern bool srv_aurora_mode;

/**
 * Aurora volume ID
 */
extern char* srv_aurora_volume_id;

/**
 * Aurora storage node endpoints (comma-separated)
 */
extern char* srv_aurora_storage_nodes;

/**
 * Aurora metadata node endpoints (comma-separated)
 */
extern char* srv_aurora_metadata_nodes;

/**
 * Aurora instance mode: "writer" or "reader"
 */
extern char* srv_aurora_instance_mode;

/**
 * Redo buffer size in bytes
 */
extern unsigned long srv_aurora_redo_buffer_size;

/**
 * Redo flush interval in milliseconds
 */
extern unsigned int srv_aurora_redo_flush_interval_ms;

/**
 * Number of nodes required for write quorum (typically 4 out of 6)
 */
extern unsigned int srv_aurora_quorum_write;

/**
 * Number of nodes required for read quorum (typically 3 out of 6)
 */
extern unsigned int srv_aurora_quorum_read;

/**
 * Quorum timeout in milliseconds
 */
extern unsigned int srv_aurora_quorum_timeout_ms;

/**
 * Reader sync interval in milliseconds
 */
extern unsigned int srv_aurora_reader_sync_interval_ms;

/**
 * Reader sync batch size
 */
extern unsigned int srv_aurora_reader_sync_batch_size;

/**
 * Enable binlog compatibility layer
 */
extern bool srv_aurora_binlog_compat;

/**
 * Binlog buffer size in bytes
 */
extern unsigned long srv_aurora_binlog_buffer_size;

/**
 * Maximum binlog events in buffer
 */
extern unsigned int srv_aurora_binlog_buffer_count;

/**
 * Virtual binlog file prefix
 */
extern char* srv_aurora_binlog_file_prefix;

/**
 * Virtual binlog rotate size
 */
extern unsigned long srv_aurora_binlog_rotate_size;

/**
 * Replication protocol: "quorum" or "raft"
 */
extern char* srv_aurora_replication_protocol;

/**
 * Raft heartbeat interval in milliseconds
 */
extern unsigned int srv_aurora_raft_heartbeat_ms;

/**
 * Raft election timeout in milliseconds
 */
extern unsigned int srv_aurora_raft_election_timeout_ms;

/**
 * Raft replication mode: "sync" or "async"
 */
extern char* srv_aurora_raft_replication_mode;

/**
 * Network transport type: "tcp" or "rdma"
 */
extern char* srv_aurora_transport_type;

/**
 * RDMA device name (e.g., "mlx5_0")
 */
extern char* srv_aurora_rdma_device;

/**
 * RDMA redo buffer size in MB
 */
extern unsigned int srv_aurora_rdma_redo_buffer_mb;

/**
 * Enable TCP fallback when RDMA is unavailable
 */
extern bool srv_aurora_tcp_fallback;

/**
 * Enable automatic OLAP routing
 */
extern bool srv_aurora_auto_olap;

/**
 * OLAP query threshold (use OLAP if estimated rows exceed this)
 */
extern unsigned long srv_aurora_olap_threshold_rows;

/**
 * Page read cache size in pages
 */
extern unsigned int srv_aurora_page_cache_size;

/**
 * Maximum concurrent gRPC connections per storage node
 */
extern unsigned int srv_aurora_grpc_max_connections;

/**
 * gRPC connection timeout in seconds
 */
extern unsigned int srv_aurora_grpc_timeout_sec;

/**
 * Enable compression for redo data
 */
extern bool srv_aurora_redo_compression;

/**
 * Compression algorithm: "lz4", "zstd", "snappy"
 */
extern char* srv_aurora_compression_algorithm;

/**
 * Enable debug logging
 */
extern bool srv_aurora_debug;

/**
 * Initialize Aurora system variables with defaults
 */
void aurora_sysvars_init_defaults();

/**
 * Validate Aurora system variable configuration
 * Returns true if configuration is valid
 */
bool aurora_sysvars_validate();

/**
 * Apply Aurora system variable changes
 * Called when variables are modified at runtime
 */
void aurora_sysvars_apply();

#ifdef __cplusplus
}
#endif

#endif  // AURORA_SYSVARS_H
