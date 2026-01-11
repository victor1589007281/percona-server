/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Reader Synchronization Implementation

*****************************************************************************/

#include "aurora_reader_sync.h"
#include <set>

namespace aurora {

// Global reader sync instance
std::unique_ptr<AuroraReaderSync> g_reader_sync;

AuroraReaderSync::AuroraReaderSync(const AuroraConfig& config)
    : config_(config),
      storage_client_(nullptr),
      metadata_client_(nullptr) {}

AuroraReaderSync::~AuroraReaderSync() {
  shutdown();
}

bool AuroraReaderSync::init() {
  storage_client_ = g_storage_client.get();
  metadata_client_ = g_metadata_client.get();
  
  if (!storage_client_ || !metadata_client_) {
    return false;
  }
  
  return true;
}

void AuroraReaderSync::shutdown() {
  stop();
  storage_client_ = nullptr;
  metadata_client_ = nullptr;
}

bool AuroraReaderSync::start() {
  if (running_) return true;
  
  running_ = true;
  sync_thread_ = std::thread(&AuroraReaderSync::sync_loop, this);
  
  return true;
}

void AuroraReaderSync::stop() {
  if (!running_) return;
  
  running_ = false;
  
  {
    std::lock_guard<std::mutex> lock(sync_mutex_);
    sync_cv_.notify_all();
  }
  
  if (sync_thread_.joinable()) {
    sync_thread_.join();
  }
}

void AuroraReaderSync::set_apply_callback(ApplyRedoCallback callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  apply_callback_ = std::move(callback);
}

lsn_t AuroraReaderSync::get_applied_lsn() const {
  return applied_lsn_.load();
}

lsn_t AuroraReaderSync::get_writer_vdl() const {
  return writer_vdl_.load();
}

int64_t AuroraReaderSync::get_lag_ms() const {
  return stats_.lag_ms.load();
}

bool AuroraReaderSync::wait_for_lsn(lsn_t target_lsn, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(sync_mutex_);
  
  return sync_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [this, target_lsn] { 
                             return applied_lsn_.load() >= target_lsn; 
                           });
}

const ReaderSyncStats& AuroraReaderSync::get_stats() const {
  return stats_;
}

bool AuroraReaderSync::is_healthy() const {
  return running_ && storage_client_ && storage_client_->is_healthy();
}

void AuroraReaderSync::sync_loop() {
  while (running_) {
    // Get current VDL from metadata
    lsn_t vdl = 0;
    if (metadata_client_->get_vdl(&vdl)) {
      writer_vdl_.store(vdl);
    }
    
    // Fetch and apply if behind
    lsn_t current = applied_lsn_.load();
    if (current < vdl) {
      if (fetch_and_apply()) {
        stats_.sync_rounds++;
      }
    }
    
    // Update lag
    update_lag();
    
    // Wait before next round
    std::unique_lock<std::mutex> lock(sync_mutex_);
    sync_cv_.wait_for(lock, std::chrono::milliseconds(10),
                      [this] { return !running_; });
  }
}

bool AuroraReaderSync::fetch_and_apply() {
  lsn_t from_lsn = applied_lsn_.load();
  lsn_t to_lsn = writer_vdl_.load();
  
  if (from_lsn >= to_lsn) {
    return true;  // Already caught up
  }
  
  std::vector<RedoRecord> records;
  lsn_t next_lsn = 0;
  
  if (!storage_client_->get_redo_logs(from_lsn, to_lsn, &records, &next_lsn)) {
    return false;
  }
  
  if (records.empty()) {
    return true;
  }
  
  // Apply records
  if (!apply_records(records)) {
    return false;
  }
  
  // Invalidate affected pages in buffer pool
  invalidate_pages(records);
  
  // Update applied LSN
  lsn_t max_lsn = records.back().lsn;
  applied_lsn_.store(max_lsn);
  
  // Update stats
  stats_.records_applied += records.size();
  for (const auto& r : records) {
    stats_.bytes_applied += r.size();
  }
  
  // Notify waiters
  sync_cv_.notify_all();
  
  return true;
}

bool AuroraReaderSync::apply_records(const std::vector<RedoRecord>& records) {
  ApplyRedoCallback callback;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callback = apply_callback_;
  }
  
  if (!callback) {
    // No callback set - just track LSN, pages will be read from storage
    return true;
  }
  
  for (const auto& record : records) {
    if (!callback(record)) {
      return false;
    }
  }
  
  return true;
}

void AuroraReaderSync::invalidate_pages(const std::vector<RedoRecord>& records) {
  // Collect unique page IDs
  std::set<std::pair<space_id_t, page_id_t>> pages;
  
  for (const auto& record : records) {
    pages.insert({record.space_id, record.page_id});
  }
  
  // Invalidate each page
  for (const auto& [space_id, page_id] : pages) {
    // Call InnoDB buffer pool invalidation
    // aurora_invalidate_buffer_pool(space_id, page_id);
    
    // Also invalidate page reader cache
    if (g_page_reader) {
      g_page_reader->invalidate_page(space_id, page_id);
    }
  }
  
  stats_.pages_invalidated += pages.size();
}

void AuroraReaderSync::update_lag() {
  lsn_t applied = applied_lsn_.load();
  lsn_t vdl = writer_vdl_.load();
  
  if (vdl <= applied) {
    stats_.lag_ms.store(0);
    return;
  }
  
  // Estimate lag based on LSN difference
  // Assuming ~10MB/s write throughput
  int64_t lsn_diff = vdl - applied;
  int64_t lag = lsn_diff / 10000;  // Rough estimate in ms
  
  stats_.lag_ms.store(lag);
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_reader_sync_init() {
  if (!aurora_config.enabled) return true;
  if (!aurora_is_reader()) return true;  // Only reader needs sync
  
  g_reader_sync = std::make_unique<AuroraReaderSync>(aurora_config);
  return g_reader_sync->init();
}

void aurora_reader_sync_shutdown() {
  if (g_reader_sync) {
    g_reader_sync->shutdown();
    g_reader_sync.reset();
  }
}

bool aurora_reader_sync_start() {
  if (!g_reader_sync) return false;
  return g_reader_sync->start();
}

void aurora_reader_sync_stop() {
  if (g_reader_sync) {
    g_reader_sync->stop();
  }
}

int64_t aurora_get_replication_lag_ms() {
  if (!g_reader_sync) return -1;
  return g_reader_sync->get_lag_ms();
}

}  // namespace aurora
