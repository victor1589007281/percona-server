/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Redo Log Generator
Generates Aurora-specific redo records and integrates with MySQL 2PC.

Reference: 01_compute_layer.md Section 2, 3

*****************************************************************************/

#ifndef AURORA_REDO_GENERATOR_H
#define AURORA_REDO_GENERATOR_H

#include "aurora_redo_types.h"
#include <cstdint>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>

namespace aurora {

// Forward declarations
class AuroraRedoSender;

/**
 * MTR (Mini-Transaction) State
 */
struct MtrState {
  uint64_t mtr_id;
  uint64_t start_lsn;
  uint64_t trx_id;
  std::vector<RedoRecord> records;
  bool is_ddl;
  bool is_2pc;
  
  void reset() {
    mtr_id = 0;
    start_lsn = 0;
    trx_id = 0;
    records.clear();
    is_ddl = false;
    is_2pc = false;
  }
};

/**
 * 2PC Transaction State
 */
enum class TwoPhaseState {
  NONE,
  PREPARING,
  PREPARED,
  COMMITTING,
  COMMITTED,
  ROLLING_BACK,
  ABORTED
};

/**
 * XA Transaction Info
 */
struct XATransactionInfo {
  uint64_t trx_id;
  std::string xid;           // XA transaction ID
  TwoPhaseState state;
  uint64_t prepare_lsn;
  uint64_t commit_lsn;
  bool is_external;          // External XA (not internal 2PC)
};

/**
 * Aurora Redo Log Generator
 * Generates Aurora-specific redo records from MySQL operations
 */
class AuroraRedoGenerator {
public:
  AuroraRedoGenerator();
  ~AuroraRedoGenerator();

  /**
   * Initialize the generator
   */
  bool initialize(uint64_t start_lsn, AuroraRedoSender* sender);

  /**
   * Shutdown
   */
  void shutdown();

  //============================================================================
  // MTR Operations (Mini-Transaction)
  //============================================================================

  /**
   * Begin a new MTR
   */
  void mtr_begin(uint64_t trx_id = 0);

  /**
   * Add a redo record to current MTR
   */
  void mtr_add_record(RedoType type,
                      uint64_t space_id,
                      uint64_t page_id,
                      const uint8_t* data,
                      size_t data_len,
                      uint16_t flags = 0);

  /**
   * Commit current MTR
   * Returns the commit LSN
   */
  uint64_t mtr_commit();

  /**
   * Abort current MTR
   */
  void mtr_abort();

  //============================================================================
  // Record Operations
  //============================================================================

  /**
   * Generate INSERT redo record
   */
  void log_insert(uint64_t space_id, uint64_t page_id,
                  uint16_t slot_no, const uint8_t* rec, size_t rec_len);

  /**
   * Generate UPDATE_IN_PLACE redo record
   */
  void log_update_in_place(uint64_t space_id, uint64_t page_id,
                           uint16_t slot_no, uint16_t offset,
                           const uint8_t* old_data, size_t old_len,
                           const uint8_t* new_data, size_t new_len);

  /**
   * Generate DELETE redo record
   */
  void log_delete(uint64_t space_id, uint64_t page_id,
                  uint16_t slot_no, const uint8_t* rec, size_t rec_len);

  /**
   * Generate PAGE_INIT redo record
   */
  void log_page_init(uint64_t space_id, uint64_t page_id,
                     uint32_t page_type, uint32_t flags);

  //============================================================================
  // 2PC (Two-Phase Commit) Operations
  //============================================================================

  /**
   * Begin 2PC for a transaction
   */
  void begin_2pc(uint64_t trx_id, const std::string& xid = "");

  /**
   * Phase 1: Prepare
   * Writes MLOG_TRX_PREPARE record
   * Returns prepare LSN
   */
  uint64_t prepare(uint64_t trx_id);

  /**
   * Phase 2: Commit
   * Writes MLOG_TRX_COMMIT record
   * Returns commit LSN
   */
  uint64_t commit_2pc(uint64_t trx_id);

  /**
   * Rollback (after prepare failed or explicit rollback)
   * Writes MLOG_TRX_ROLLBACK record
   */
  void rollback_2pc(uint64_t trx_id);

  /**
   * Get transaction 2PC state
   */
  TwoPhaseState get_2pc_state(uint64_t trx_id) const;

  /**
   * Get XA transaction info
   */
  bool get_xa_info(uint64_t trx_id, XATransactionInfo* info) const;

  //============================================================================
  // DDL Operations
  //============================================================================

  /**
   * Log CREATE TABLE
   */
  void log_create_table(uint64_t table_id,
                        const std::string& schema,
                        const std::string& table);

  /**
   * Log DROP TABLE
   */
  void log_drop_table(uint64_t table_id,
                      const std::string& schema,
                      const std::string& table);

  /**
   * Log CREATE INDEX
   */
  void log_create_index(uint64_t table_id,
                        uint64_t index_id,
                        const std::string& index_name);

  /**
   * Log DROP INDEX
   */
  void log_drop_index(uint64_t table_id, uint64_t index_id);

  /**
   * Log DDL barrier
   * Ensures all previous redo is durable before DDL
   */
  uint64_t log_ddl_barrier();

  //============================================================================
  // Aurora-Specific Operations
  //============================================================================

  /**
   * Log VDL update (called by Writer after receiving Quorum ACK)
   */
  void log_vdl_update(uint64_t new_vdl, uint64_t old_vdl, uint32_t quorum_count);

  /**
   * Log Reader sync marker
   */
  void log_reader_sync(const std::string& reader_id,
                       uint64_t read_point,
                       uint64_t target_vdl);

  /**
   * Log freeze/unfreeze writes (for failover)
   */
  void log_freeze_writes();
  void log_unfreeze_writes();

  /**
   * Log failover event
   */
  void log_failover(const std::string& old_writer,
                    const std::string& new_writer,
                    uint64_t failover_lsn);

  //============================================================================
  // LSN Management
  //============================================================================

  /**
   * Get current LSN
   */
  uint64_t get_current_lsn() const { return current_lsn_.load(); }

  /**
   * Allocate LSN for a record
   */
  uint64_t allocate_lsn(size_t record_size);

  /**
   * Wait until LSN is durable
   */
  bool wait_durable(uint64_t lsn, uint32_t timeout_ms);

  /**
   * Get last durable LSN
   */
  uint64_t get_durable_lsn() const { return durable_lsn_.load(); }

  /**
   * Update durable LSN (called after Quorum ACK)
   */
  void set_durable_lsn(uint64_t lsn) { durable_lsn_.store(lsn); }

  //============================================================================
  // Statistics
  //============================================================================

  struct Stats {
    uint64_t records_generated;
    uint64_t bytes_generated;
    uint64_t mtr_commits;
    uint64_t trx_prepares;
    uint64_t trx_commits;
    uint64_t trx_rollbacks;
    uint64_t ddl_barriers;
  };

  Stats get_stats() const;

private:
  // Thread-local MTR state
  static thread_local MtrState tls_mtr_;
  
  // Current and durable LSN
  std::atomic<uint64_t> current_lsn_{0};
  std::atomic<uint64_t> durable_lsn_{0};
  
  // MTR ID counter
  std::atomic<uint64_t> mtr_id_counter_{0};
  
  // 2PC transaction tracking
  mutable std::mutex xa_mutex_;
  std::unordered_map<uint64_t, XATransactionInfo> xa_transactions_;
  
  // Redo sender
  AuroraRedoSender* sender_{nullptr};
  
  // Statistics
  mutable std::mutex stats_mutex_;
  Stats stats_{};
  
  // Helper methods
  void send_mtr_records(const MtrState& mtr, uint64_t end_lsn);
  void serialize_record(const RedoRecord& rec, std::vector<uint8_t>& out);
  uint32_t calculate_checksum(const uint8_t* data, size_t len);
};

/**
 * Global redo generator instance
 */
extern std::unique_ptr<AuroraRedoGenerator> g_redo_generator;

/**
 * Initialize global redo generator
 */
bool aurora_redo_generator_init(uint64_t start_lsn, AuroraRedoSender* sender);

/**
 * Shutdown global redo generator
 */
void aurora_redo_generator_shutdown();

}  // namespace aurora

#endif  // AURORA_REDO_GENERATOR_H
