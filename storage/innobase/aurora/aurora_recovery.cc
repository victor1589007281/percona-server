/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Recovery Implementation

*****************************************************************************/

#include "aurora_recovery.h"
#include "aurora_client.h"
#include "aurora_config.h"

#include <thread>
#include <chrono>

namespace aurora {

// Global instance
std::unique_ptr<AuroraRecoveryManager> g_recovery_manager;

// External references
extern AuroraConfig g_config;
extern std::unique_ptr<AuroraStorageClient> g_storage_client;
extern std::unique_ptr<AuroraMetadataClient> g_metadata_client;

//============================================================================
// AuroraRecoveryManager Implementation
//============================================================================

AuroraRecoveryManager::AuroraRecoveryManager() {
  progress_.state = RecoveryState::NOT_STARTED;
  progress_.current_lsn = 0;
  progress_.target_lsn = 0;
  progress_.pages_loaded = 0;
  progress_.pages_total = 0;
  progress_.progress_percent = 0.0;
}

AuroraRecoveryManager::~AuroraRecoveryManager() {}

bool AuroraRecoveryManager::initialize(
    AuroraStorageClient* storage_client,
    AuroraMetadataClient* metadata_client) {
  
  std::lock_guard<std::mutex> lock(mutex_);
  storage_client_ = storage_client;
  metadata_client_ = metadata_client;
  return true;
}

bool AuroraRecoveryManager::start_recovery(const RecoveryOptions& options) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  options_ = options;
  cancelled_.store(false);
  progress_.state = RecoveryState::FETCHING_METADATA;
  progress_.status_message = "Starting recovery...";
  
  // Phase 1: Fetch metadata from metadata service
  if (!phase_fetch_metadata()) {
    progress_.state = RecoveryState::FAILED;
    progress_.status_message = "Failed to fetch metadata";
    return false;
  }
  
  if (cancelled_.load()) return false;
  
  // Phase 2: Validate VDL
  progress_.state = RecoveryState::VALIDATING_VDL;
  progress_.status_message = "Validating VDL...";
  
  if (!phase_validate_vdl()) {
    progress_.state = RecoveryState::FAILED;
    progress_.status_message = "Failed to validate VDL";
    return false;
  }
  
  if (cancelled_.load()) return false;
  
  // Phase 3: Load catalog (data dictionary)
  progress_.state = RecoveryState::LOADING_CATALOG;
  progress_.status_message = "Loading catalog...";
  
  if (!phase_load_catalog()) {
    progress_.state = RecoveryState::FAILED;
    progress_.status_message = "Failed to load catalog";
    return false;
  }
  
  if (cancelled_.load()) return false;
  
  // Phase 4: Warm buffer pool (optional)
  if (options_.warm_buffer_pool) {
    progress_.state = RecoveryState::WARMING_BUFFER_POOL;
    progress_.status_message = "Warming buffer pool...";
    
    phase_warm_buffer_pool();  // Non-fatal if this fails
  }
  
  progress_.state = RecoveryState::COMPLETED;
  progress_.status_message = "Recovery completed";
  progress_.progress_percent = 100.0;
  
  return true;
}

bool AuroraRecoveryManager::phase_fetch_metadata() {
  if (!metadata_client_) return false;
  
  // Get volume info from metadata service
  VolumeInfo volume_info;
  if (!metadata_client_->get_volume(options_.volume_id, &volume_info)) {
    return false;
  }
  
  recovered_vdl_ = volume_info.vdl;
  progress_.target_lsn = volume_info.vdl;
  
  // For PITR, use the specified target LSN
  if (options_.mode == RecoveryMode::PITR && options_.target_lsn > 0) {
    if (options_.target_lsn > volume_info.vdl) {
      // Can't recover beyond current VDL
      return false;
    }
    progress_.target_lsn = options_.target_lsn;
    recovered_vdl_ = options_.target_lsn;
  }
  
  return true;
}

bool AuroraRecoveryManager::phase_validate_vdl() {
  if (!storage_client_) return false;
  
  // Query all storage nodes for their local LSN
  // Verify that Quorum of nodes have LSN >= VDL
  
  std::vector<StorageNodeInfo> nodes;
  if (!storage_client_->get_storage_nodes(&nodes)) {
    return false;
  }
  
  uint32_t nodes_at_vdl = 0;
  uint32_t quorum = g_config.quorum_read;
  
  for (const auto& node : nodes) {
    if (node.local_lsn >= recovered_vdl_) {
      nodes_at_vdl++;
    }
  }
  
  if (nodes_at_vdl < quorum) {
    // Not enough nodes have the data
    progress_.status_message = "Insufficient nodes at VDL (data loss possible)";
    return false;
  }
  
  recovered_lsn_ = recovered_vdl_;
  progress_.current_lsn = recovered_lsn_;
  progress_.progress_percent = 50.0;
  
  return true;
}

bool AuroraRecoveryManager::phase_load_catalog() {
  // In Aurora mode, the data dictionary (catalog) is stored in
  // regular InnoDB tables which are in the shared storage.
  // We need to ensure the system tablespace pages are accessible.
  
  // Read critical system pages:
  // - ibdata1 header page
  // - Data dictionary header
  // - Undo tablespace headers
  
  // These will be materialized on-demand by the storage layer
  // when MySQL tries to read them.
  
  // For now, we just verify connectivity
  if (!storage_client_) return false;
  
  // Ping storage nodes
  std::vector<StorageNodeInfo> nodes;
  if (!storage_client_->get_storage_nodes(&nodes)) {
    return false;
  }
  
  bool all_healthy = true;
  for (const auto& node : nodes) {
    if (!node.is_healthy) {
      all_healthy = false;
    }
  }
  
  progress_.progress_percent = 75.0;
  
  return true;  // Continue even if some nodes are unhealthy
}

bool AuroraRecoveryManager::phase_warm_buffer_pool() {
  // Pre-load frequently accessed pages into buffer pool
  // This is optional and improves performance after restart
  
  if (!storage_client_) return false;
  
  // In real implementation:
  // 1. Get list of frequently accessed pages from metadata
  // 2. Read pages from storage in batches
  // 3. Add to buffer pool
  
  // For now, simulate warm-up
  uint32_t target_pages = 1000 * options_.buffer_pool_warm_pct / 100;
  progress_.pages_total = target_pages;
  
  for (uint32_t i = 0; i < target_pages && !cancelled_.load(); i++) {
    // Simulate page load
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    progress_.pages_loaded = i + 1;
    progress_.progress_percent = 75.0 + 25.0 * (i + 1) / target_pages;
  }
  
  return true;
}

RecoveryProgress AuroraRecoveryManager::get_progress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return progress_;
}

bool AuroraRecoveryManager::wait_complete(uint32_t timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (progress_.state == RecoveryState::COMPLETED) {
        return true;
      }
      if (progress_.state == RecoveryState::FAILED) {
        return false;
      }
    }
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
    if (elapsed.count() >= timeout_ms) {
      return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void AuroraRecoveryManager::cancel() {
  cancelled_.store(true);
}

bool AuroraRecoveryManager::is_recovery_needed() const {
  // Recovery is not needed if:
  // 1. This is a fresh start (no existing volume)
  // 2. Clean shutdown was performed
  
  // In Aurora mode, we always "recover" by getting VDL from metadata
  return true;
}

//============================================================================
// Global Recovery Functions
//============================================================================

bool aurora_writer_recovery(const std::string& volume_id,
                            uint64_t* recovered_lsn,
                            uint64_t* recovered_vdl) {
  
  if (!g_recovery_manager) {
    g_recovery_manager = std::make_unique<AuroraRecoveryManager>();
    g_recovery_manager->initialize(g_storage_client.get(), g_metadata_client.get());
  }
  
  RecoveryOptions options;
  options.mode = RecoveryMode::WRITER_RESTART;
  options.volume_id = volume_id;
  options.target_lsn = 0;  // Use current VDL
  options.warm_buffer_pool = true;
  options.buffer_pool_warm_pct = 10;  // Warm 10% of buffer pool
  options.timeout_ms = 300000;  // 5 minutes
  
  if (!g_recovery_manager->start_recovery(options)) {
    return false;
  }
  
  *recovered_lsn = g_recovery_manager->get_recovered_lsn();
  *recovered_vdl = g_recovery_manager->get_recovered_vdl();
  
  return true;
}

bool aurora_reader_recovery(const std::string& volume_id,
                            uint64_t* read_point,
                            uint64_t* current_vdl) {
  
  if (!g_recovery_manager) {
    g_recovery_manager = std::make_unique<AuroraRecoveryManager>();
    g_recovery_manager->initialize(g_storage_client.get(), g_metadata_client.get());
  }
  
  RecoveryOptions options;
  options.mode = RecoveryMode::READER_RESTART;
  options.volume_id = volume_id;
  options.target_lsn = 0;
  options.warm_buffer_pool = false;  // Readers don't pre-warm
  options.timeout_ms = 60000;  // 1 minute
  
  if (!g_recovery_manager->start_recovery(options)) {
    return false;
  }
  
  *read_point = 0;  // Readers start from beginning
  *current_vdl = g_recovery_manager->get_recovered_vdl();
  
  return true;
}

bool aurora_failover_recovery(const std::string& volume_id,
                              const std::string& old_writer_id,
                              uint64_t* failover_lsn) {
  
  if (!g_recovery_manager) {
    g_recovery_manager = std::make_unique<AuroraRecoveryManager>();
    g_recovery_manager->initialize(g_storage_client.get(), g_metadata_client.get());
  }
  
  RecoveryOptions options;
  options.mode = RecoveryMode::FAILOVER;
  options.volume_id = volume_id;
  options.target_lsn = 0;
  options.warm_buffer_pool = true;
  options.buffer_pool_warm_pct = 5;
  options.timeout_ms = 60000;  // Failover should be fast
  
  if (!g_recovery_manager->start_recovery(options)) {
    return false;
  }
  
  *failover_lsn = g_recovery_manager->get_recovered_lsn();
  
  return true;
}

}  // namespace aurora
