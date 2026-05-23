/*****************************************************************************

Copyright (c) 1997, 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/row0vers.h
 Row versions

 Created 2/6/1997 Heikki Tuuri
 *******************************************************/

#ifndef row0vers_h
#define row0vers_h

#include "data0data.h"
#include "dict0mem.h"
#include "dict0types.h"
#include "lob0undo.h"
#include "mtr0mtr.h"
#include "que0types.h"
#include "rem0types.h"
#include "trx0types.h"
#include "univ.i"

// Forward declaration
class ReadView;

/** Finds out if an active transaction has inserted or modified a secondary
 index record.
 @param[in]   rec       record in a secondary index
 @param[in]   index     the secondary index
 @param[in]   offsets   rec_get_offsets(rec, index)
 @return 0 if committed, else the active transaction id;
 NOTE that this function can return false positives but never false
 negatives. The caller must confirm all positive results by checking if the trx
 is still active.
*/
trx_t *row_vers_impl_x_locked(const rec_t *rec, const dict_index_t *index,
                              const ulint *offsets);

/** Finds out if we must preserve a delete marked earlier version of a clustered
 index record, because it is >= the purge view.
 @param[in]     trx_id          Transaction id in the version
 @param[in]     name            Table name
 @param[in,out] mtr             Mini-transaction  holding the latch on the
                                 clustered index record; it will also hold
                                  the latch on purge_view
 @return true if earlier version should be preserved */
bool row_vers_must_preserve_del_marked(trx_id_t trx_id,
                                       const table_name_t &name, mtr_t *mtr);

/** Finds out if a version of the record, where the version >= the current
 purge view, should have ientry as its secondary index entry. We check
 if there is any not delete marked version of the record where the trx
 id >= purge view, and the secondary index entry == ientry; exactly in
 this case we return true.
 @return true if earlier version should have */
bool row_vers_old_has_index_entry(
    bool also_curr,            /*!< in: true if also rec is included in the
                              versions to search; otherwise only versions
                              prior to it are searched */
    const rec_t *rec,          /*!< in: record in the clustered index; the
                               caller must have a latch on the page */
    mtr_t *mtr,                /*!< in: mtr holding the latch on rec; it will
                               also hold the latch on purge_view */
    dict_index_t *index,       /*!< in: the secondary index */
    const dtuple_t *ientry,    /*!< in: the secondary index entry */
    roll_ptr_t roll_ptr,       /*!< in: roll_ptr for the purge record */
    trx_id_t trx_id,           /*!< in: transaction ID on the purging record */
    row_prebuilt_t *prebuilt); /*!< in: compress_heap must be taken from
                               here */

/** Constructs the version of a clustered index record which a consistent
 read should see. We assume that the trx id stored in rec is such that
 the consistent read should not see rec in its present version.
 @param[in]   rec   record in a clustered index; the caller must have a latch
                    on the page; this latch locks the top of the stack of
                    versions of this records
 @param[in]   mtr   mtr holding the latch on rec; it will also hold the latch
                    on purge_view
 @param[in]   index   the clustered index
 @param[in]   offsets   offsets returned by rec_get_offsets(rec, index)
 @param[in]   view   the consistent read view
 @param[in,out]   offset_heap   memory heap from which the offsets are
                                allocated
 @param[in]   in_heap   memory heap from which the memory for *old_vers is
                        allocated; memory for possible intermediate versions
                        is allocated and freed locally within the function
 @param[out]   old_vers   old version, or NULL if the history is missing or
                          the record does not exist in the view, that is, it
                          was freshly inserted afterwards.
 @param[out]   vrow   reports virtual column info if any
 @param[in]   lob_undo   undo log to be applied to blobs.
 @return DB_SUCCESS or DB_MISSING_HISTORY */
dberr_t row_vers_build_for_consistent_read(
    const rec_t *rec, mtr_t *mtr, dict_index_t *index, ulint **offsets,
    ReadView *view, mem_heap_t **offset_heap, mem_heap_t *in_heap,
    rec_t **old_vers, const dtuple_t **vrow, lob::undo_vers_t *lob_undo);

/** Constructs the last committed version of a clustered index record,
 which should be seen by a semi-consistent read.
@param[in] rec Record in a clustered index; the caller must have a latch on the
page; this latch locks the top of the stack of versions of this records
@param[in] mtr Mini-transaction holding the latch on rec
@param[in] index The clustered index
@param[in,out] offsets Offsets returned by rec_get_offsets(rec, index)
@param[in,out] offset_heap Memory heap from which the offsets are allocated
@param[in] in_heap Memory heap from which the memory for *old_vers is allocated;
memory for possible intermediate versions is allocated and freed locally within
the function
@param[out] old_vers Rec, old version, or null if the record does not exist in
the view, that is, it was freshly inserted afterwards
@param[out] vrow Virtual row, old version, or null if it is not updated in the
view */
void row_vers_build_for_semi_consistent_read(
    const rec_t *rec, mtr_t *mtr, dict_index_t *index, ulint **offsets,
    mem_heap_t **offset_heap, mem_heap_t *in_heap, const rec_t **old_vers,
    const dtuple_t **vrow);

/** Builds a historical version of a clustered index record as of a given
transaction ID. Used for flashback queries (SELECT ... AS OF TRX_ID).

This function reuses row_vers_build_for_consistent_read() internally by
constructing a ReadView that treats all transactions >= target_trx_id as
"not yet committed".

@param[in]  rec             Current clustered index record. Caller must hold
                            a page latch on rec.
@param[in]  mtr             Mini-transaction holding the latch on rec.
@param[in]  index           Clustered index descriptor.
@param[in]  offsets         Offsets for rec, computed via rec_get_offsets().
@param[in]  target_trx_id   Target transaction ID. Transactions committed
                            strictly before this ID are visible; transactions
                            with ID >= target_trx_id are treated as not yet
                            committed.
@param[in,out] offset_heap  Memory heap for offset allocations.
@param[in]  in_heap         Memory heap for the output record.
@param[out] old_vers        Output: the historical version, or nullptr if
                            the record was freshly inserted after the target
                            point (or undo history has been purged).
@param[out] vrow            Output: virtual column data, if any.
@param[out] lob_undo        Output: LOB undo info for flashback LOB reads.
@return DB_SUCCESS on success, DB_MISSING_HISTORY if undo log has been
        purged before the target transaction, DB_INTERRUPTED if the query
        was cancelled. */
[[nodiscard]] dberr_t row_build_flashback_version(
    const rec_t *rec, mtr_t *mtr, dict_index_t *index, ulint **offsets,
    trx_id_t target_trx_id, mem_heap_t **offset_heap, mem_heap_t *in_heap,
    rec_t **old_vers, const dtuple_t **vrow, lob::undo_vers_t *lob_undo);

/** Builds a historical version of a clustered index record using a caller-
provided ReadView. Useful when the caller has already constructed a ReadView
and wants to reuse it across multiple rows.

@param[in]  rec             Current clustered index record.
@param[in]  mtr             Mini-transaction holding the latch on rec.
@param[in]  index           Clustered index descriptor.
@param[in]  offsets         Offsets for rec.
@param[in]  view            Pre-constructed ReadView defining visibility.
@param[in,out] offset_heap  Memory heap for offset allocations.
@param[in]  in_heap         Memory heap for the output record.
@param[out] old_vers        Output: the historical version, or nullptr.
@param[out] vrow            Output: virtual column data, if any.
@param[out] lob_undo        Output: LOB undo info.
@return DB_SUCCESS on success, DB_MISSING_HISTORY or DB_INTERRUPTED. */
[[nodiscard]] dberr_t row_build_flashback_version_with_view(
    const rec_t *rec, mtr_t *mtr, dict_index_t *index, ulint **offsets,
    ReadView *view, mem_heap_t **offset_heap, mem_heap_t *in_heap,
    rec_t **old_vers, const dtuple_t **vrow, lob::undo_vers_t *lob_undo);

#include "row0vers.ic"

#endif
