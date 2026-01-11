/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Hook Framework
Provides hooks for minimal invasive modification of MySQL/InnoDB.

Reference: 01_compute_layer.md Section 11.5

*****************************************************************************/

#ifndef AURORA_HOOK_H
#define AURORA_HOOK_H

#include <functional>
#include <memory>
#include <mutex>
#include <cstdint>

namespace aurora {

/**
 * Aurora Hook Interface
 * Uses function pointers to allow runtime registration
 */
class AuroraHooks {
public:
  static AuroraHooks& instance();

  // ==================== Redo Log Hooks ====================

  /**
   * Redo Write Hook
   * Called before log_writer writes to local file
   * Returns true if handled (skip local write)
   * Returns false to continue local write (non-Aurora mode)
   */
  using RedoWriteHook = std::function<bool(
      const unsigned char* log_block,
      size_t len,
      uint64_t start_lsn,
      uint64_t end_lsn
  )>;

  void register_redo_write_hook(RedoWriteHook hook);
  bool call_redo_write_hook(const unsigned char* log_block, size_t len,
                            uint64_t start_lsn, uint64_t end_lsn);

  /**
   * Redo Flush Hook
   * Called before fsync, used for Quorum wait
   */
  using RedoFlushHook = std::function<bool(uint64_t flush_to_lsn)>;

  void register_redo_flush_hook(RedoFlushHook hook);
  bool call_redo_flush_hook(uint64_t flush_to_lsn);

  // ==================== Page I/O Hooks ====================

  /**
   * Page Read Hook
   * Called before local file read
   * Returns true if read from remote
   */
  using PageReadHook = std::function<bool(
      uint32_t space_id,
      uint32_t page_no,
      unsigned char* buf,
      uint64_t target_lsn
  )>;

  void register_page_read_hook(PageReadHook hook);
  bool call_page_read_hook(uint32_t space_id, uint32_t page_no,
                           unsigned char* buf, uint64_t target_lsn);

  /**
   * Page Write Hook
   * In Aurora mode, disable local file writes
   */
  using PageWriteHook = std::function<bool(
      uint32_t space_id,
      uint32_t page_no,
      const unsigned char* buf
  )>;

  void register_page_write_hook(PageWriteHook hook);
  bool call_page_write_hook(uint32_t space_id, uint32_t page_no,
                            const unsigned char* buf);

  // ==================== Transaction Hooks ====================

  /**
   * Transaction Commit Hook
   * Used for GTID assignment and Quorum wait
   */
  using TrxCommitHook = std::function<void(
      uint64_t trx_id,
      uint64_t commit_lsn
  )>;

  void register_trx_commit_hook(TrxCommitHook hook);
  void call_trx_commit_hook(uint64_t trx_id, uint64_t commit_lsn);

  /**
   * Transaction Prepare Hook (for 2PC)
   */
  using TrxPrepareHook = std::function<void(
      uint64_t trx_id,
      uint64_t prepare_lsn
  )>;

  void register_trx_prepare_hook(TrxPrepareHook hook);
  void call_trx_prepare_hook(uint64_t trx_id, uint64_t prepare_lsn);

  // ==================== Checkpoint Hooks ====================

  /**
   * Checkpoint Hook
   * Aurora mode disables local checkpoint
   */
  using CheckpointHook = std::function<bool(uint64_t checkpoint_lsn)>;

  void register_checkpoint_hook(CheckpointHook hook);
  bool call_checkpoint_hook(uint64_t checkpoint_lsn);

  // ==================== Recovery Hooks ====================

  /**
   * Recovery Hook
   * Aurora mode skips local recovery
   */
  using RecoveryHook = std::function<bool()>;

  void register_recovery_hook(RecoveryHook hook);
  bool call_recovery_hook();

  // ==================== Startup/Shutdown Hooks ====================

  /**
   * Startup Hook
   * Called during InnoDB startup
   */
  using StartupHook = std::function<bool()>;

  void register_startup_hook(StartupHook hook);
  bool call_startup_hook();

  /**
   * Shutdown Hook
   * Called during InnoDB shutdown
   */
  using ShutdownHook = std::function<void()>;

  void register_shutdown_hook(ShutdownHook hook);
  void call_shutdown_hook();

  // ==================== Transport Selection Hook ====================

  /**
   * Transport Select Hook
   * Choose between TCP/RDMA based on operation
   */
  enum class TransportType { TCP, RDMA };
  
  using TransportSelectHook = std::function<TransportType(
      const char* operation,  // "redo_write", "page_read"
      size_t data_size
  )>;

  void register_transport_select_hook(TransportSelectHook hook);
  TransportType call_transport_select_hook(const char* op, size_t size);

  // ==================== State Management ====================

  bool is_aurora_mode() const { return aurora_mode_; }
  void set_aurora_mode(bool enabled) { aurora_mode_ = enabled; }

  bool is_writer_mode() const { return is_writer_; }
  void set_writer_mode(bool is_writer) { is_writer_ = is_writer; }

  // Clear all hooks (for testing)
  void clear_all_hooks();

private:
  AuroraHooks() = default;
  ~AuroraHooks() = default;

  // Disable copy
  AuroraHooks(const AuroraHooks&) = delete;
  AuroraHooks& operator=(const AuroraHooks&) = delete;

  bool aurora_mode_ = false;
  bool is_writer_ = false;

  // Hooks
  RedoWriteHook redo_write_hook_;
  RedoFlushHook redo_flush_hook_;
  PageReadHook page_read_hook_;
  PageWriteHook page_write_hook_;
  TrxCommitHook trx_commit_hook_;
  TrxPrepareHook trx_prepare_hook_;
  CheckpointHook checkpoint_hook_;
  RecoveryHook recovery_hook_;
  StartupHook startup_hook_;
  ShutdownHook shutdown_hook_;
  TransportSelectHook transport_select_hook_;

  mutable std::mutex mutex_;
};

// Convenience macro
#define AURORA_HOOKS aurora::AuroraHooks::instance()

// Check if Aurora mode is enabled
inline bool aurora_is_enabled() {
  return AURORA_HOOKS.is_aurora_mode();
}

// Check if this is a writer instance
inline bool aurora_is_writer() {
  return AURORA_HOOKS.is_aurora_mode() && AURORA_HOOKS.is_writer_mode();
}

// Check if this is a reader instance
inline bool aurora_is_reader() {
  return AURORA_HOOKS.is_aurora_mode() && !AURORA_HOOKS.is_writer_mode();
}

}  // namespace aurora

#endif  // AURORA_HOOK_H
