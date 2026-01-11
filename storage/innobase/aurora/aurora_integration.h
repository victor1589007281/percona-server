/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora MySQL Integration
Header to be included in MySQL source files that need Aurora hooks.

This file provides the integration points for MySQL source code.
Include this header and use the AURORA_HOOK_* macros.

Reference: 01_compute_layer.md Section 11.6

*****************************************************************************/

#ifndef AURORA_INTEGRATION_H
#define AURORA_INTEGRATION_H

#ifdef HAVE_AURORA

#include "aurora_hook.h"
#include "aurora_startup.h"
#include "aurora_sysvars.h"

//============================================================================
// Convenience macros for hook integration
//============================================================================

/**
 * Redo Write Hook Integration
 * Use in log/log0write.cc before writing to local file
 *
 * Usage:
 *   AURORA_HOOK_REDO_WRITE(log_block, len, start_lsn, end_lsn) {
 *     return;  // Skip local write if handled
 *   }
 */
#define AURORA_HOOK_REDO_WRITE(block, len, start, end) \
  if (AURORA_HOOKS.call_redo_write_hook(block, len, start, end))

/**
 * Redo Flush Hook Integration
 * Use in log/log0write.cc before fsync
 */
#define AURORA_HOOK_REDO_FLUSH(lsn) \
  if (AURORA_HOOKS.call_redo_flush_hook(lsn))

/**
 * Page Read Hook Integration
 * Use in buf/buf0buf.cc when page not in buffer pool
 *
 * Returns true if page was read from remote storage
 */
#define AURORA_HOOK_PAGE_READ(space, page, buf, lsn) \
  AURORA_HOOKS.call_page_read_hook(space, page, buf, lsn)

/**
 * Page Write Hook Integration
 * Use in buf/buf0flu.cc before writing dirty page
 *
 * Returns true if local write should be skipped
 */
#define AURORA_HOOK_PAGE_WRITE(space, page, buf) \
  if (AURORA_HOOKS.call_page_write_hook(space, page, buf))

/**
 * Checkpoint Hook Integration
 * Use in log/log0chkp.cc
 */
#define AURORA_HOOK_CHECKPOINT(lsn) \
  if (AURORA_HOOKS.call_checkpoint_hook(lsn))

/**
 * Recovery Hook Integration
 * Use in log/log0recv.cc
 */
#define AURORA_HOOK_RECOVERY() \
  if (AURORA_HOOKS.call_recovery_hook())

/**
 * Transaction Commit Hook Integration
 * Use in trx/trx0trx.cc after commit
 */
#define AURORA_HOOK_TRX_COMMIT(trx_id, lsn) \
  AURORA_HOOKS.call_trx_commit_hook(trx_id, lsn)

/**
 * Check if Aurora mode is enabled
 */
#define AURORA_IS_ENABLED() aurora::aurora_is_enabled()

/**
 * Check if this is a writer instance
 */
#define AURORA_IS_WRITER() aurora::aurora_is_writer()

/**
 * Check if this is a reader instance
 */
#define AURORA_IS_READER() aurora::aurora_is_reader()

#else  // !HAVE_AURORA

// No-op macros when Aurora is not compiled in
#define AURORA_HOOK_REDO_WRITE(block, len, start, end) if (false)
#define AURORA_HOOK_REDO_FLUSH(lsn) if (false)
#define AURORA_HOOK_PAGE_READ(space, page, buf, lsn) (false)
#define AURORA_HOOK_PAGE_WRITE(space, page, buf) if (false)
#define AURORA_HOOK_CHECKPOINT(lsn) if (false)
#define AURORA_HOOK_RECOVERY() if (false)
#define AURORA_HOOK_TRX_COMMIT(trx_id, lsn) ((void)0)
#define AURORA_IS_ENABLED() (false)
#define AURORA_IS_WRITER() (false)
#define AURORA_IS_READER() (false)

#endif  // HAVE_AURORA

#endif  // AURORA_INTEGRATION_H
