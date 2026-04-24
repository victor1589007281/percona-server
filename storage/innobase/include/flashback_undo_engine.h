/*****************************************************************************

Copyright (c) 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/flashback_undo_engine.h
 UndoFlashbackEngine — Undo-based flashback engine

 Provides three core operations:
 1. execute_query()    — Flashback query: read a record's historical version
    as of a given timestamp (SELECT ... AS OF).
 2. execute_table()    — Flashback table: walk all rows in a table and restore
    them to the state at a given timestamp.
 3. execute_dry_run()  — Dry-run mode: estimate how many rows would be
    restored by execute_table() without modifying any data.

 Design principles:
 - Reuses existing InnoDB infrastructure: row_build_flashback_version(),
   trx_sys_find_trx_id_by_timestamp(), FlashbackPurgeGuard.
 - Does NOT panic; all errors are returned via dberr_t.
 - Supports dry_run mode and max_rows limit for safety.
 - Thread-safe: each call operates on its own prebuilt/mtr context.

 Usage example:
   UndoFlashbackEngine engine(table, target_trx_id, max_rows);
   auto result = engine.execute_table(false);  // false = not dry run
   if (result.m_error == DB_SUCCESS) {
     printf("Restored %lu rows\n", result.m_rows_processed);
   }
 */

#ifndef flashback_undo_engine_h
#define flashback_undo_engine_h

#include "data0types.h"
#include "db0err.h"
#include "dict0dict.h"
#include "dict0types.h"
#include "mtr0mtr.h"
#include "rem0types.h"
#include "trx0types.h"
#include "univ.i"

/* Forward declarations from other InnoDB headers */
struct trx_t;
struct row_prebuilt_t;

/** Result of a flashback operation */
struct FlashbackResult {
  /** Error code: DB_SUCCESS on success, otherwise the failure reason */
  dberr_t m_error{DB_SUCCESS};

  /** Number of rows examined during the operation */
  ulint m_rows_scanned{0};

  /** Number of rows that were (or would be) restored to a previous version */
  ulint m_rows_restored{0};

  /** Number of rows skipped because undo history was purged */
  ulint m_rows_skipped{0};

  /** Number of rows that were already at the target version (no change needed) */
  ulint m_rows_unchanged{0};

  /** Human-readable error message (empty if m_error == DB_SUCCESS) */
  const char *m_error_msg{nullptr};
};

/** Undo-based flashback engine.
 *
 * WHY: This engine encapsulates the logic for performing flashback operations
 * using InnoDB's undo log version chain. It provides a clean interface that
 * higher layers (SQL layer, handler) can use without needing to understand
 * the internals of MVCC, undo records, or persistent cursors.
 *
 * Thread safety: Not thread-safe per instance. Create one instance per
 * flashback operation. Multiple instances can run concurrently on different
 * tables.
 */
class UndoFlashbackEngine {
 public:
  /** Constructor
   * @param[in] table        Target InnoDB table (must be open and valid).
   * @param[in] target_trx_id  Target transaction ID. All transactions with
   *                          ID >= target_trx_id are treated as not yet
   *                          committed. Must be > 0 and within the undo window.
   * @param[in] max_rows     Maximum number of rows to process. 0 means no limit.
   *                         Used as a safety brake for large tables.
   */
  UndoFlashbackEngine(dict_table_t *table, trx_id_t target_trx_id,
                      ulint max_rows = 0);

  /** Destructor: cleans up internal resources */
  ~UndoFlashbackEngine();

  /** Disable copy/move */
  UndoFlashbackEngine(const UndoFlashbackEngine &) = delete;
  UndoFlashbackEngine &operator=(const UndoFlashbackEngine &) = delete;
  UndoFlashbackEngine(UndoFlashbackEngine &&) = delete;
  UndoFlashbackEngine &operator=(UndoFlashbackEngine &&) = delete;

  /** Execute a flashback query: build the historical version of a single
   * record as of the target transaction ID.
   *
   * @param[in] rec        Current clustered index record. Caller must hold
   *                       a page latch on rec.
   * @param[in] offsets    Offsets for rec (from rec_get_offsets).
   * @param[out] old_vers  Output: the historical version record, or nullptr
   *                       if the record was inserted after target_trx_id.
   * @param[in] heap       Memory heap for allocating the output record.
   * @return DB_SUCCESS on success, DB_MISSING_HISTORY if undo was purged,
   *         or other error codes on failure.
   */
  [[nodiscard]] dberr_t execute_query(const rec_t *rec, const ulint *offsets,
                                      const rec_t **old_vers,
                                      mem_heap_t *heap);

  /** Execute a full table flashback: scan all rows in the clustered index,
   * build each row's historical version, and if different from the current
   * version, restore it to the historical state.
   *
   * WHY: This is the core FLASHBACK TABLE implementation. It performs a
   * forward scan of the clustered index, calling row_build_flashback_version()
   * for each row, and then applying the inverse operation (INSERT→DELETE,
   * DELETE→INSERT, UPDATE→reverse UPDATE).
   *
   * @param[in] dry_run  If true, only count rows that would be restored
   *                     without actually modifying any data.
   * @return FlashbackResult with row counts and error status.
   */
  [[nodiscard]] FlashbackResult execute_table(bool dry_run);

  /** Execute a dry-run flashback table operation.
   *
   * Convenience wrapper around execute_table(true). Estimates how many rows
   * would be restored and how much undo history is missing, without making
   * any changes to the table.
   *
   * @return FlashbackResult with estimated counts and error status.
   */
  [[nodiscard]] FlashbackResult execute_dry_run() {
    return execute_table(true);
  }

  /** Get the target transaction ID */
  trx_id_t target_trx_id() const { return m_target_trx_id; }

  /** Get the target table */
  dict_table_t *table() const { return m_table; }

  /** Get max_rows limit (0 = no limit) */
  ulint max_rows() const { return m_max_rows; }

  /** Check if the operation has hit the max_rows limit */
  bool is_limited() const { return m_limited; }

 private:
  /** Scan and flashback a single row during execute_table.
   *
   * @param[in] rec        Current clustered index record.
   * @param[in,out] result Accumulated result counters.
   * @param[in] dry_run    Whether to skip actual row restoration.
   * @param[in,out] mtr    Mini-transaction for page latching.
   * @return true if the engine should stop (max_rows reached or error),
   *         false to continue scanning.
   */
  bool process_single_row(const rec_t *rec, FlashbackResult &result,
                          bool dry_run, mtr_t *mtr);

  /** Restore a single row to its historical version.
   *
   * Depending on what happened between the target point and now:
   * - Row existed then, exists now but changed → UPDATE to old values
   * - Row existed then, was deleted since → INSERT the old row
   * - Row didn't exist then, was inserted since → DELETE the row
   * - Row didn't exist then, still doesn't exist → no-op
   *
   * @param[in] rec        Current clustered index record.
   * @param[in] old_vers   Historical version (nullptr if row was inserted
   *                       after target_trx_id).
   * @param[in] offsets    Offsets for rec.
   * @param[in] dry_run    If true, only count, don't modify.
   * @param[in,out] result Result counter to update.
   * @return DB_SUCCESS or error code.
   */
  [[nodiscard]] dberr_t restore_row(const rec_t *rec, const rec_t *old_vers,
                                    const ulint *offsets, bool dry_run,
                                    FlashbackResult &result);

  /** Target InnoDB table */
  dict_table_t *m_table;

  /** Target transaction ID boundary */
  trx_id_t m_target_trx_id;

  /** Maximum rows to process (0 = unlimited) */
  ulint m_max_rows;

  /** Whether the operation was stopped due to max_rows limit */
  bool m_limited;

  /** Pre-built structure for MySQL-InnoDB interface (lazy-initialized) */
  row_prebuilt_t *m_prebuilt;

  /** Whether m_prebuilt was allocated by this engine */
  bool m_own_prebuilt;
};

#endif /* flashback_undo_engine_h */
