/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Redo Sender Implementation

*****************************************************************************/

#include "aurora_redo_sender.h"
#include <algorithm>

namespace aurora {

// Global redo sender instance
std::unique_ptr<AuroraRedoSender> g_redo_sender;

AuroraRedoSender::AuroraRedoSender(const AuroraConfig& config)
    : config_(config),
      storage_client_(nullptr),
      metadata_client_(nullptr) {}

AuroraRedoSender::~AuroraRedoSender() {
  shutdown();
}

bool AuroraRedoSender::init() {
  if (running_) return true;
  
  // Get global clients
  storage_client_ = g_storage_client.get();
  metadata_client_ = g_metadata_client.get();
  
  if (!storage_client_ || !metadata_client_) {
    return false;
  }
  
  // Initialize batch
  current_batch_.volume_id = config_.volume_id;
  current_batch_.clear();
  
  // Start background threads
  running_ = true;
  sender_thread_ = std::thread(&AuroraRedoSender::sender_loop, this);
  vdl_thread_ = std::thread(&AuroraRedoSender::vdl_updater_loop, this);
  
  return true;
}

void AuroraRedoSender::shutdown() {
  if (!running_) return;
  
  running_ = false;
  
  // Wake up threads
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_cv_.notify_all();
  }
  send_cv_.notify_all();
  
  // Wait for threads
  if (sender_thread_.joinable()) {
    sender_thread_.join();
  }
  if (vdl_thread_.joinable()) {
    vdl_thread_.join();
  }
  
  // Flush remaining batch
  {
    std::lock_guard<std::mutex> lock(batch_mutex_);
    if (!current_batch_.empty()) {
      send_batch(current_batch_);
      current_batch_.clear();
    }
  }
}

void AuroraRedoSender::add_record(const RedoRecord& record) {
  RedoRecord r = record;
  add_record(std::move(r));
}

void AuroraRedoSender::add_record(RedoRecord&& record) {
  std::lock_guard<std::mutex> lock(batch_mutex_);
  
  current_batch_.add(std::move(record));
  
  // Check if batch should be flushed
  if (current_batch_.size() >= config_.max_batch_size) {
    flush_batch();
  }
}

lsn_t AuroraRedoSender::flush_and_wait() {
  // Flush current batch
  {
    std::lock_guard<std::mutex> lock(batch_mutex_);
    if (!current_batch_.empty()) {
      flush_batch();
    }
  }
  
  // Wait for all pending batches
  std::unique_lock<std::mutex> lock(queue_mutex_);
  queue_cv_.wait(lock, [this] { return send_queue_.empty(); });
  
  return durable_lsn_.load();
}

bool AuroraRedoSender::wait_for_lsn(lsn_t lsn, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(lsn_mutex_);
  
  return lsn_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                          [this, lsn] { return durable_lsn_.load() >= lsn; });
}

lsn_t AuroraRedoSender::get_vdl() const {
  return vdl_.load();
}

lsn_t AuroraRedoSender::get_sent_lsn() const {
  return sent_lsn_.load();
}

const RedoSenderStats& AuroraRedoSender::get_stats() const {
  return stats_;
}

bool AuroraRedoSender::is_healthy() const {
  return running_ && storage_client_ && storage_client_->is_healthy();
}

void AuroraRedoSender::flush_batch() {
  // Called with batch_mutex_ held
  if (current_batch_.empty()) return;
  
  RedoBatch batch = std::move(current_batch_);
  current_batch_.volume_id = config_.volume_id;
  current_batch_.clear();
  
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    send_queue_.push(std::move(batch));
    queue_cv_.notify_one();
  }
}

void AuroraRedoSender::sender_loop() {
  while (running_) {
    RedoBatch batch;
    
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      
      // Wait for batch or timeout
      queue_cv_.wait_for(lock, std::chrono::milliseconds(config_.batch_timeout_ms),
                         [this] { return !send_queue_.empty() || !running_; });
      
      if (!running_ && send_queue_.empty()) break;
      
      if (send_queue_.empty()) {
        // Timeout - flush current batch
        std::lock_guard<std::mutex> bm(batch_mutex_);
        if (!current_batch_.empty()) {
          send_queue_.push(std::move(current_batch_));
          current_batch_.volume_id = config_.volume_id;
          current_batch_.clear();
        }
        continue;
      }
      
      batch = std::move(send_queue_.front());
      send_queue_.pop();
    }
    
    // Send batch
    if (!batch.empty()) {
      send_batch(batch);
    }
  }
}

bool AuroraRedoSender::send_batch(const RedoBatch& batch) {
  auto start = std::chrono::steady_clock::now();
  
  lsn_t persisted_lsn = 0;
  bool success = storage_client_->write_redo(batch, &persisted_lsn);
  
  auto end = std::chrono::steady_clock::now();
  auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  
  // Update stats
  stats_.batches_sent++;
  stats_.records_sent += batch.records.size();
  stats_.bytes_sent += batch.size();
  stats_.total_latency_us += latency.count();
  
  if (success) {
    stats_.quorum_achieved++;
    
    // Update LSN tracking
    sent_lsn_.store(batch.max_lsn);
    
    lsn_t expected = durable_lsn_.load();
    while (persisted_lsn > expected) {
      if (durable_lsn_.compare_exchange_weak(expected, persisted_lsn)) {
        break;
      }
    }
    
    // Notify waiters
    {
      std::lock_guard<std::mutex> lock(lsn_mutex_);
      lsn_cv_.notify_all();
    }
    
    // Update VDL in metadata service
    metadata_client_->update_vdl(persisted_lsn);
  } else {
    stats_.quorum_failed++;
  }
  
  return success;
}

void AuroraRedoSender::vdl_updater_loop() {
  while (running_) {
    update_vdl_from_metadata();
    
    std::this_thread::sleep_for(
        std::chrono::milliseconds(config_.vdl_update_interval_ms));
  }
}

void AuroraRedoSender::update_vdl_from_metadata() {
  lsn_t vdl = 0;
  if (metadata_client_->get_vdl(&vdl)) {
    vdl_.store(vdl);
  }
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_redo_sender_init() {
  if (!aurora_config.enabled) return true;
  if (!aurora_is_writer()) return true;  // Only writer needs redo sender
  
  g_redo_sender = std::make_unique<AuroraRedoSender>(aurora_config);
  return g_redo_sender->init();
}

void aurora_redo_sender_shutdown() {
  if (g_redo_sender) {
    g_redo_sender->shutdown();
    g_redo_sender.reset();
  }
}

void aurora_send_redo(lsn_t lsn, space_id_t space_id, page_id_t page_id,
                      trx_id_t trx_id, const uint8_t* data, size_t len,
                      RedoType type, uint16_t flags) {
  if (!g_redo_sender) return;
  
  RedoRecord record;
  record.lsn = lsn;
  record.space_id = space_id;
  record.page_id = page_id;
  record.trx_id = trx_id;
  record.mtr_id = 0;  // Set by caller if needed
  record.type = type;
  record.flags = flags;
  record.data.assign(data, data + len);
  record.created_at = std::chrono::steady_clock::now();
  
  g_redo_sender->add_record(std::move(record));
}

bool aurora_wait_for_redo(lsn_t lsn, uint32_t timeout_ms) {
  if (!g_redo_sender) return false;
  return g_redo_sender->wait_for_lsn(lsn, timeout_ms);
}

lsn_t aurora_get_vdl() {
  if (!g_redo_sender) return 0;
  return g_redo_sender->get_vdl();
}

}  // namespace aurora
