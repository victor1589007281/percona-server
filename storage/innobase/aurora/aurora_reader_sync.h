/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Reader Synchronization
Handles redo log synchronization for reader (replica) instances.

*****************************************************************************/

#ifndef AURORA_READER_SYNC_H
#define AURORA_READER_SYNC_H

#include "aurora_config.h"
#include "aurora_types.h"
#include "aurora_client.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>

namespace aurora {

/**
 * Reader sync statistics
 */
struct ReaderSyncStats {
  std::atomic<uint64_t> records_applied{0};
  std::atomic<uint64_t> bytes_applied{0};
  std::atomic<uint64_t> sync_rounds{0};
  std::atomic<uint64_t> pages_invalidated{0};
  std::atomic<int64_t> lag_ms{0};
  
  void reset() {
    records_applied = 0;
    bytes_applied = 0;
    sync_rounds = 0;
    pages_invalidated = 0;
    lag_ms = 0;
  }
};

/**
 * Callback for applying redo records locally
 */
using ApplyRedoCallback = std::function<bool(const RedoRecord& record)>;

/**
 * Aurora Reader Sync
 * 
 * This class handles redo log synchronization for reader instances.
 * It fetches redo logs from storage nodes and applies them locally
 * to keep the reader instance up-to-date.
 * 
 * Key features:
 * - Continuous sync from VDL
 * - Buffer pool invalidation for modified pages
 * - Lag tracking
 */
class AuroraReaderSync {
public:
  explicit AuroraReaderSync(const AuroraConfig& config);
  ~AuroraReaderSync();
  
  // Disable copy
  AuroraReaderSync(const AuroraReaderSync&) = delete;
  AuroraReaderSync& operator=(const AuroraReaderSync&) = delete;
  
  /**
   * Initialize the sync
   */
  bool init();
  
  /**
   * Shutdown the sync
   */
  void shutdown();
  
  /**
   * Start synchronization
   */
  bool start();
  
  /**
   * Stop synchronization
   */
  void stop();
  
  /**
   * Set callback for applying redo records
   */
  void set_apply_callback(ApplyRedoCallback callback);
  
  /**
   * Get current applied LSN
   */
  lsn_t get_applied_lsn() const;
  
  /**
   * Get current VDL from writer
   */
  lsn_t get_writer_vdl() const;
  
  /**
   * Get replication lag in milliseconds
   */
  int64_t get_lag_ms() const;
  
  /**
   * Wait for applied LSN to reach target
   */
  bool wait_for_lsn(lsn_t target_lsn, uint32_t timeout_ms);
  
  /**
   * Get statistics
   */
  const ReaderSyncStats& get_stats() const;
  
  /**
   * Check if sync is healthy
   */
  bool is_healthy() const;

private:
  const AuroraConfig& config_;
  AuroraStorageClient* storage_client_;
  AuroraMetadataClient* metadata_client_;
  
  // Sync state
  std::atomic<lsn_t> applied_lsn_{0};
  std::atomic<lsn_t> writer_vdl_{0};
  
  // Apply callback
  ApplyRedoCallback apply_callback_;
  std::mutex callback_mutex_;
  
  // Sync thread
  std::thread sync_thread_;
  std::atomic<bool> running_{false};
  std::condition_variable sync_cv_;
  std::mutex sync_mutex_;
  
  // Statistics
  ReaderSyncStats stats_;
  
  // Sync loop
  void sync_loop();
  bool fetch_and_apply();
  bool apply_records(const std::vector<RedoRecord>& records);
  void invalidate_pages(const std::vector<RedoRecord>& records);
  void update_lag();
};

/**
 * Global reader sync instance
 */
extern std::unique_ptr<AuroraReaderSync> g_reader_sync;

/**
 * Initialize reader sync
 */
bool aurora_reader_sync_init();

/**
 * Shutdown reader sync
 */
void aurora_reader_sync_shutdown();

/**
 * Start reader sync
 */
bool aurora_reader_sync_start();

/**
 * Stop reader sync
 */
void aurora_reader_sync_stop();

/**
 * Get reader replication lag
 */
int64_t aurora_get_replication_lag_ms();

}  // namespace aurora

#endif  // AURORA_READER_SYNC_H
