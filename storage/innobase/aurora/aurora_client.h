/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora gRPC Client
Client for communicating with Aurora storage and metadata services.

*****************************************************************************/

#ifndef AURORA_CLIENT_H
#define AURORA_CLIENT_H

#include "aurora_config.h"
#include "aurora_types.h"

#include <memory>
#include <mutex>
#include <vector>
#include <functional>
#include <future>
#include <atomic>

namespace aurora {

// Forward declarations
class ConnectionPool;
class StorageServiceClient;
class MetadataServiceClient;

/**
 * Callback for async write completion
 */
using WriteCallback = std::function<void(const std::vector<WriteResult>&)>;

/**
 * Aurora Storage Client
 * Manages connections to storage nodes and provides write/read operations.
 */
class AuroraStorageClient {
public:
  explicit AuroraStorageClient(const AuroraConfig& config);
  ~AuroraStorageClient();
  
  // Disable copy
  AuroraStorageClient(const AuroraStorageClient&) = delete;
  AuroraStorageClient& operator=(const AuroraStorageClient&) = delete;
  
  /**
   * Initialize the client
   */
  bool init();
  
  /**
   * Shutdown the client
   */
  void shutdown();
  
  /**
   * Write redo records to all storage nodes
   * Returns when quorum is achieved
   */
  bool write_redo(const RedoBatch& batch, lsn_t* persisted_lsn);
  
  /**
   * Write redo records asynchronously
   */
  void write_redo_async(const RedoBatch& batch, WriteCallback callback);
  
  /**
   * Read a page from storage
   */
  bool read_page(space_id_t space_id, page_id_t page_id, 
                 lsn_t target_lsn, PageData* page);
  
  /**
   * Read multiple pages
   */
  bool read_pages(const std::vector<std::pair<space_id_t, page_id_t>>& page_ids,
                  lsn_t target_lsn, std::vector<PageData>* pages);
  
  /**
   * Get redo logs for reader synchronization
   */
  bool get_redo_logs(lsn_t from_lsn, lsn_t to_lsn,
                     std::vector<RedoRecord>* records, lsn_t* next_lsn);
  
  /**
   * Freeze writes (for failover)
   */
  bool freeze_writes(lsn_t* final_lsn);
  
  /**
   * Unfreeze writes
   */
  bool unfreeze_writes();
  
  /**
   * Get node statuses
   */
  std::vector<NodeStatus> get_node_statuses();
  
  /**
   * Check if client is healthy
   */
  bool is_healthy() const;

private:
  const AuroraConfig& config_;
  std::unique_ptr<ConnectionPool> pool_;
  std::vector<std::unique_ptr<StorageServiceClient>> storage_clients_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};
  
  bool write_to_node(const std::string& node, const RedoBatch& batch,
                     WriteResult* result);
  bool read_from_node(const std::string& node, space_id_t space_id,
                      page_id_t page_id, lsn_t target_lsn, PageData* page);
};

/**
 * Aurora Metadata Client
 * Manages connections to metadata service for VDL tracking.
 */
class AuroraMetadataClient {
public:
  explicit AuroraMetadataClient(const AuroraConfig& config);
  ~AuroraMetadataClient();
  
  // Disable copy
  AuroraMetadataClient(const AuroraMetadataClient&) = delete;
  AuroraMetadataClient& operator=(const AuroraMetadataClient&) = delete;
  
  /**
   * Initialize the client
   */
  bool init();
  
  /**
   * Shutdown the client
   */
  void shutdown();
  
  /**
   * Update VDL
   */
  bool update_vdl(lsn_t new_vdl);
  
  /**
   * Get current VDL
   */
  bool get_vdl(lsn_t* vdl);
  
  /**
   * Get VDL info with node LSNs
   */
  bool get_vdl_info(VDLInfo* info);
  
  /**
   * Register this instance
   */
  bool register_instance();
  
  /**
   * Unregister this instance
   */
  bool unregister_instance();
  
  /**
   * Get page location (which storage nodes)
   */
  bool get_page_location(space_id_t space_id, page_id_t page_id,
                         std::vector<std::string>* nodes);
  
  /**
   * Check if client is healthy
   */
  bool is_healthy() const;
  
  /**
   * Check if this node is the leader (for metadata)
   */
  bool is_leader() const;

private:
  const AuroraConfig& config_;
  std::unique_ptr<ConnectionPool> pool_;
  std::unique_ptr<MetadataServiceClient> metadata_client_;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutdown_{false};
  std::string current_leader_;
  mutable std::mutex leader_mutex_;
  
  bool find_leader();
};

/**
 * Global client instances
 */
extern std::unique_ptr<AuroraStorageClient> g_storage_client;
extern std::unique_ptr<AuroraMetadataClient> g_metadata_client;

/**
 * Initialize Aurora clients
 */
bool aurora_clients_init();

/**
 * Shutdown Aurora clients
 */
void aurora_clients_shutdown();

}  // namespace aurora

#endif  // AURORA_CLIENT_H
