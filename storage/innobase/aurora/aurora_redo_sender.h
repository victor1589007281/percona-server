/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Redo Sender
Intercepts InnoDB redo log writes and sends to Aurora storage layer.

*****************************************************************************/

#ifndef AURORA_REDO_SENDER_H
#define AURORA_REDO_SENDER_H

#include "aurora_config.h"
#include "aurora_types.h"
#include "aurora_client.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace aurora {

/**
 * Redo log sender statistics
 */
struct RedoSenderStats {
  std::atomic<uint64_t> records_sent{0};
  std::atomic<uint64_t> bytes_sent{0};
  std::atomic<uint64_t> batches_sent{0};
  std::atomic<uint64_t> quorum_achieved{0};
  std::atomic<uint64_t> quorum_failed{0};
  std::atomic<uint64_t> retries{0};
  std::atomic<uint64_t> total_latency_us{0};
  
  void reset() {
    records_sent = 0;
    bytes_sent = 0;
    batches_sent = 0;
    quorum_achieved = 0;
    quorum_failed = 0;
    retries = 0;
    total_latency_us = 0;
  }
  
  double avg_latency_ms() const {
    uint64_t batches = batches_sent.load();
    if (batches == 0) return 0;
    return static_cast<double>(total_latency_us.load()) / batches / 1000.0;
  }
};

/**
 * Aurora Redo Sender
 * 
 * This class intercepts InnoDB redo log writes and sends them to
 * the Aurora storage layer using the Log-is-Database approach.
 * 
 * Key features:
 * - Batching for efficiency
 * - Parallel writes to all storage nodes
 * - Quorum-based durability (4/6)
 * - VDL tracking
 */
class AuroraRedoSender {
public:
  explicit AuroraRedoSender(const AuroraConfig& config);
  ~AuroraRedoSender();
  
  // Disable copy
  AuroraRedoSender(const AuroraRedoSender&) = delete;
  AuroraRedoSender& operator=(const AuroraRedoSender&) = delete;
  
  /**
   * Initialize the sender
   */
  bool init();
  
  /**
   * Shutdown the sender
   */
  void shutdown();
  
  /**
   * Add a redo record to be sent
   * This is called from InnoDB's log writer
   */
  void add_record(const RedoRecord& record);
  
  /**
   * Add a redo record (move semantics)
   */
  void add_record(RedoRecord&& record);
  
  /**
   * Flush pending records and wait for quorum
   * Returns the persisted LSN
   */
  lsn_t flush_and_wait();
  
  /**
   * Wait for a specific LSN to be persisted
   */
  bool wait_for_lsn(lsn_t lsn, uint32_t timeout_ms);
  
  /**
   * Get the current VDL (Volume Durable LSN)
   */
  lsn_t get_vdl() const;
  
  /**
   * Get the highest LSN sent (may not be durable yet)
   */
  lsn_t get_sent_lsn() const;
  
  /**
   * Get statistics
   */
  const RedoSenderStats& get_stats() const;
  
  /**
   * Check if sender is healthy
   */
  bool is_healthy() const;

private:
  const AuroraConfig& config_;
  AuroraStorageClient* storage_client_;
  AuroraMetadataClient* metadata_client_;
  
  // Current batch being assembled
  RedoBatch current_batch_;
  std::mutex batch_mutex_;
  
  // Background sender thread
  std::thread sender_thread_;
  std::atomic<bool> running_{false};
  std::condition_variable send_cv_;
  
  // Queue of batches to send
  std::queue<RedoBatch> send_queue_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  
  // LSN tracking
  std::atomic<lsn_t> sent_lsn_{0};
  std::atomic<lsn_t> durable_lsn_{0};
  std::mutex lsn_mutex_;
  std::condition_variable lsn_cv_;
  
  // VDL tracking
  std::atomic<lsn_t> vdl_{0};
  std::thread vdl_thread_;
  
  // Statistics
  RedoSenderStats stats_;
  
  // Background threads
  void sender_loop();
  void vdl_updater_loop();
  
  // Internal methods
  void flush_batch();
  bool send_batch(const RedoBatch& batch);
  void update_vdl_from_metadata();
};

/**
 * Global redo sender instance
 */
extern std::unique_ptr<AuroraRedoSender> g_redo_sender;

/**
 * Initialize redo sender
 */
bool aurora_redo_sender_init();

/**
 * Shutdown redo sender
 */
void aurora_redo_sender_shutdown();

/**
 * Hook called from InnoDB log writer
 * Converts InnoDB redo log format to Aurora format and queues for sending
 */
void aurora_send_redo(lsn_t lsn, space_id_t space_id, page_id_t page_id,
                      trx_id_t trx_id, const uint8_t* data, size_t len,
                      RedoType type, uint16_t flags);

/**
 * Wait for redo to be durable up to given LSN
 */
bool aurora_wait_for_redo(lsn_t lsn, uint32_t timeout_ms);

/**
 * Get current VDL
 */
lsn_t aurora_get_vdl();

}  // namespace aurora

#endif  // AURORA_REDO_SENDER_H
