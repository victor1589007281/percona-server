/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Recovery
Implements recovery from Aurora storage layer instead of local redo logs.

Reference: 01_compute_layer.md Section 4.2, 4.4

*****************************************************************************/

#ifndef AURORA_RECOVERY_H
#define AURORA_RECOVERY_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

namespace aurora {

// Forward declarations
class AuroraStorageClient;
class AuroraMetadataClient;

/**
 * Recovery Mode
 */
enum class RecoveryMode {
  WRITER_RESTART,    // Writer instance restarting
  READER_RESTART,    // Reader instance restarting
  FAILOVER,          // Writer failover recovery
  PITR               // Point-in-Time Recovery
};

/**
 * Recovery State
 */
enum class RecoveryState {
  NOT_STARTED,
  FETCHING_METADATA,
  VALIDATING_VDL,
  LOADING_CATALOG,
  WARMING_BUFFER_POOL,
  COMPLETED,
  FAILED
};

/**
 * Recovery Progress
 */
struct RecoveryProgress {
  RecoveryState state;
  uint64_t current_lsn;
  uint64_t target_lsn;
  uint64_t pages_loaded;
  uint64_t pages_total;
  double progress_percent;
  std::string status_message;
};

/**
 * Recovery Options
 */
struct RecoveryOptions {
  RecoveryMode mode;
  std::string volume_id;
  uint64_t target_lsn;           // For PITR
  bool warm_buffer_pool;         // Pre-load frequently accessed pages
  uint32_t buffer_pool_warm_pct; // Percentage of buffer pool to warm
  uint32_t timeout_ms;
};

/**
 * Aurora Recovery Manager
 * Handles recovery from Aurora storage layer
 */
class AuroraRecoveryManager {
public:
  AuroraRecoveryManager();
  ~AuroraRecoveryManager();

  /**
   * Initialize recovery manager
   */
  bool initialize(AuroraStorageClient* storage_client,
                  AuroraMetadataClient* metadata_client);

  /**
   * Start recovery process
   */
  bool start_recovery(const RecoveryOptions& options);

  /**
   * Get current recovery progress
   */
  RecoveryProgress get_progress() const;

  /**
   * Wait for recovery to complete
   */
  bool wait_complete(uint32_t timeout_ms);

  /**
   * Cancel ongoing recovery
   */
  void cancel();

  /**
   * Check if recovery is needed
   */
  bool is_recovery_needed() const;

  /**
   * Get recovered LSN
   */
  uint64_t get_recovered_lsn() const { return recovered_lsn_; }

  /**
   * Get recovered VDL
   */
  uint64_t get_recovered_vdl() const { return recovered_vdl_; }

private:
  // Recovery phases
  bool phase_fetch_metadata();
  bool phase_validate_vdl();
  bool phase_load_catalog();
  bool phase_warm_buffer_pool();

  // State
  AuroraStorageClient* storage_client_{nullptr};
  AuroraMetadataClient* metadata_client_{nullptr};
  RecoveryOptions options_;
  RecoveryProgress progress_;
  mutable std::mutex mutex_;
  std::atomic<bool> cancelled_{false};
  
  uint64_t recovered_lsn_{0};
  uint64_t recovered_vdl_{0};
};

/**
 * Perform Writer recovery
 * Called when Writer instance restarts
 */
bool aurora_writer_recovery(const std::string& volume_id,
                            uint64_t* recovered_lsn,
                            uint64_t* recovered_vdl);

/**
 * Perform Reader recovery
 * Called when Reader instance restarts
 */
bool aurora_reader_recovery(const std::string& volume_id,
                            uint64_t* read_point,
                            uint64_t* current_vdl);

/**
 * Perform Failover recovery
 * Called when promoting a Reader to Writer
 */
bool aurora_failover_recovery(const std::string& volume_id,
                              const std::string& old_writer_id,
                              uint64_t* failover_lsn);

/**
 * Global recovery manager instance
 */
extern std::unique_ptr<AuroraRecoveryManager> g_recovery_manager;

}  // namespace aurora

#endif  // AURORA_RECOVERY_H
