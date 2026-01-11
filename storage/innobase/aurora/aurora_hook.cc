/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Hook Framework Implementation

*****************************************************************************/

#include "aurora_hook.h"

namespace aurora {

AuroraHooks& AuroraHooks::instance() {
  static AuroraHooks inst;
  return inst;
}

// ==================== Redo Write Hook ====================

void AuroraHooks::register_redo_write_hook(RedoWriteHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  redo_write_hook_ = std::move(hook);
}

bool AuroraHooks::call_redo_write_hook(
    const unsigned char* log_block,
    size_t len,
    uint64_t start_lsn,
    uint64_t end_lsn) {
  if (!aurora_mode_ || !redo_write_hook_) {
    return false;  // Continue local processing
  }
  return redo_write_hook_(log_block, len, start_lsn, end_lsn);
}

// ==================== Redo Flush Hook ====================

void AuroraHooks::register_redo_flush_hook(RedoFlushHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  redo_flush_hook_ = std::move(hook);
}

bool AuroraHooks::call_redo_flush_hook(uint64_t flush_to_lsn) {
  if (!aurora_mode_ || !redo_flush_hook_) {
    return false;
  }
  return redo_flush_hook_(flush_to_lsn);
}

// ==================== Page Read Hook ====================

void AuroraHooks::register_page_read_hook(PageReadHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  page_read_hook_ = std::move(hook);
}

bool AuroraHooks::call_page_read_hook(
    uint32_t space_id,
    uint32_t page_no,
    unsigned char* buf,
    uint64_t target_lsn) {
  if (!aurora_mode_ || !page_read_hook_) {
    return false;
  }
  return page_read_hook_(space_id, page_no, buf, target_lsn);
}

// ==================== Page Write Hook ====================

void AuroraHooks::register_page_write_hook(PageWriteHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  page_write_hook_ = std::move(hook);
}

bool AuroraHooks::call_page_write_hook(
    uint32_t space_id,
    uint32_t page_no,
    const unsigned char* buf) {
  if (!aurora_mode_ || !page_write_hook_) {
    return false;
  }
  return page_write_hook_(space_id, page_no, buf);
}

// ==================== Transaction Commit Hook ====================

void AuroraHooks::register_trx_commit_hook(TrxCommitHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  trx_commit_hook_ = std::move(hook);
}

void AuroraHooks::call_trx_commit_hook(uint64_t trx_id, uint64_t commit_lsn) {
  if (!aurora_mode_ || !trx_commit_hook_) {
    return;
  }
  trx_commit_hook_(trx_id, commit_lsn);
}

// ==================== Transaction Prepare Hook ====================

void AuroraHooks::register_trx_prepare_hook(TrxPrepareHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  trx_prepare_hook_ = std::move(hook);
}

void AuroraHooks::call_trx_prepare_hook(uint64_t trx_id, uint64_t prepare_lsn) {
  if (!aurora_mode_ || !trx_prepare_hook_) {
    return;
  }
  trx_prepare_hook_(trx_id, prepare_lsn);
}

// ==================== Checkpoint Hook ====================

void AuroraHooks::register_checkpoint_hook(CheckpointHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  checkpoint_hook_ = std::move(hook);
}

bool AuroraHooks::call_checkpoint_hook(uint64_t checkpoint_lsn) {
  if (!aurora_mode_ || !checkpoint_hook_) {
    return false;
  }
  return checkpoint_hook_(checkpoint_lsn);
}

// ==================== Recovery Hook ====================

void AuroraHooks::register_recovery_hook(RecoveryHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  recovery_hook_ = std::move(hook);
}

bool AuroraHooks::call_recovery_hook() {
  if (!aurora_mode_ || !recovery_hook_) {
    return false;
  }
  return recovery_hook_();
}

// ==================== Startup Hook ====================

void AuroraHooks::register_startup_hook(StartupHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  startup_hook_ = std::move(hook);
}

bool AuroraHooks::call_startup_hook() {
  if (!startup_hook_) {
    return false;
  }
  return startup_hook_();
}

// ==================== Shutdown Hook ====================

void AuroraHooks::register_shutdown_hook(ShutdownHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  shutdown_hook_ = std::move(hook);
}

void AuroraHooks::call_shutdown_hook() {
  if (!shutdown_hook_) {
    return;
  }
  shutdown_hook_();
}

// ==================== Transport Select Hook ====================

void AuroraHooks::register_transport_select_hook(TransportSelectHook hook) {
  std::lock_guard<std::mutex> lock(mutex_);
  transport_select_hook_ = std::move(hook);
}

AuroraHooks::TransportType AuroraHooks::call_transport_select_hook(
    const char* op, size_t size) {
  if (!transport_select_hook_) {
    return TransportType::TCP;  // Default to TCP
  }
  return transport_select_hook_(op, size);
}

// ==================== Utility ====================

void AuroraHooks::clear_all_hooks() {
  std::lock_guard<std::mutex> lock(mutex_);
  redo_write_hook_ = nullptr;
  redo_flush_hook_ = nullptr;
  page_read_hook_ = nullptr;
  page_write_hook_ = nullptr;
  trx_commit_hook_ = nullptr;
  trx_prepare_hook_ = nullptr;
  checkpoint_hook_ = nullptr;
  recovery_hook_ = nullptr;
  startup_hook_ = nullptr;
  shutdown_hook_ = nullptr;
  transport_select_hook_ = nullptr;
}

}  // namespace aurora
