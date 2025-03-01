/*****************************************************************************

Copyright (c) 1997, 2022, Oracle and/or its affiliates.
Copyright (c) 2012, Facebook Inc.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file log/log0recv.cc
 Recovery

 Created 9/20/1997 Heikki Tuuri
 *******************************************************/

#include "ha_prototypes.h"

#include <my_aes.h>
#include <sys/types.h>

#include <array>
#include <iomanip>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "arch0arch.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "clone0api.h"
#include "dict0dd.h"
#include "fil0fil.h"
#include "ha_prototypes.h"
#include "ibuf0ibuf.h"
#include "log0chkp.h"       /* log_next_checkpoint_header */
#include "log0encryption.h" /* log_encryption_read */
#include "log0files_io.h"
#include "log0pre_8_0_30.h"
#include "log0recv.h"
#include "log0test.h"
#include "mem0mem.h"
#include "mtr0log.h"
#include "mtr0mtr.h"
#include "os0thread-create.h"
#include "page0cur.h"
#include "page0zip.h"
#include "trx0rec.h"
#include "trx0undo.h"
#include "ut0new.h"

#include "my_dbug.h"

#ifndef UNIV_HOTBACKUP
#include "buf0rea.h"
#include "ddl0ddl.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0purge.h"
#else /* !UNIV_HOTBACKUP */
#include "../meb/mutex.h"
#endif /* !UNIV_HOTBACKUP */

std::list<space_id_t> recv_encr_ts_list;

/** Log records are stored in the hash table in chunks at most of this size;
this must be less than UNIV_PAGE_SIZE as it is stored in the buffer pool */
#define RECV_DATA_BLOCK_SIZE (MEM_MAX_ALLOC_IN_BUF - sizeof(recv_data_t))

/** Read-ahead area in applying log records to file pages */
static const size_t RECV_READ_AHEAD_AREA = 32;

/** The recovery system */
recv_sys_t *recv_sys = nullptr;

/** true when applying redo log records during crash recovery; false
otherwise.  Note that this is false while a background thread is
rolling back incomplete transactions. */
volatile bool recv_recovery_on;

#ifdef UNIV_HOTBACKUP
std::list<std::pair<space_id_t, lsn_t>> index_load_list;
volatile lsn_t backup_redo_log_flushed_lsn;

extern bool meb_is_space_loaded(const space_id_t space_id);

/* Re-define mutex macros to use the Mutex class defined by the MEB
source. MEB calls the routines in "fil0fil.cc" in parallel and,
therefore, the mutex protecting the critical sections of the tablespace
memory cache must be included also in the MEB compilation of this
module. (For other modules the mutex macros are defined as no ops in the
MEB compilation in "meb/src/include/bh_univ.i".) */

#undef mutex_enter
#undef mutex_exit
#undef mutex_own
#undef mutex_validate

#define mutex_enter(M) recv_mutex.lock()
#define mutex_exit(M) recv_mutex.unlock()
#define mutex_own(M) 1
#define mutex_validate(M) 1

/* Re-define the mutex macros for the mutex protecting the critical
sections of the log subsystem using an object of the meb::Mutex class. */

meb::Mutex recv_mutex;
extern meb::Mutex log_mutex;
meb::Mutex apply_log_mutex;

#undef log_mutex_enter
#undef log_mutex_exit
#define log_mutex_enter() log_mutex.lock()
#define log_mutex_exit() log_mutex.unlock()

/** Print important values from a page header.
@param[in]      page    page */
void meb_print_page_header(const page_t *page) {
  ib::trace_1() << "space_id " << mach_read_from_4(page + FIL_PAGE_SPACE_ID)
                << " page_nr " << mach_read_from_4(page + FIL_PAGE_OFFSET)
                << " lsn " << mach_read_from_8(page + FIL_PAGE_LSN) << " type "
                << mach_read_from_2(page + FIL_PAGE_TYPE);
}
#endif /* UNIV_HOTBACKUP */

//#ifndef UNIV_HOTBACKUP
PSI_memory_key mem_log_recv_page_hash_key;
PSI_memory_key mem_log_recv_space_hash_key;
//#endif /* !UNIV_HOTBACKUP */

/** true when recv_init_crash_recovery() has been called. */
bool recv_needed_recovery;

/** true if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially false, and set by
recv_recovery_from_checkpoint_start(). */
bool recv_lsn_checks_on;

/** If the following is true, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this becomes true if
the log record hash table becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

true means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
bool recv_no_ibuf_operations;

/** true When the redo log is being backed up */
bool recv_is_making_a_backup = false;

/** true when recovering from a backed up redo log file */
bool recv_is_from_backup = false;

/** The following counter is used to decide when to print info on
log scan */
static ulint recv_scan_print_counter;

/** The type of the previous parsed redo log record */
static mlog_id_t recv_previous_parsed_rec_type;

/** The offset of the previous parsed redo log record */
static ulint recv_previous_parsed_rec_offset;

/** The 'multi' flag of the previous parsed redo log record */
static ulint recv_previous_parsed_rec_is_multi;

/** This many frames must be left free in the buffer pool when we scan
the log and store the scanned log records in the buffer pool: we will
use these free frames to read in pages when we start applying the
log records to the database.
This is the default value. If the actual size of the buffer pool is
larger than 10 MB we'll set this value to 512. */
ulint recv_n_pool_free_frames;

/** The maximum lsn we see for a page during the recovery process. If this
is bigger than the lsn we are able to scan up to, that is an indication that
the recovery failed and the database may be corrupt. */
static lsn_t recv_max_page_lsn;

/* prototypes */

#ifndef UNIV_HOTBACKUP

/** Reads a specified log segment to a buffer.
@param[in,out]  log             redo log
@param[in,out]  buf             buffer where to read
@param[in]      start_lsn       read area start
@param[in]      end_lsn         read area end
@return lsn up to which data was available on disk (ideally end_lsn) */
static lsn_t recv_read_log_seg(log_t &log, byte *buf, lsn_t start_lsn,
                               lsn_t end_lsn);

/** Initialize crash recovery environment. Can be called iff
recv_needed_recovery == false. */
static dberr_t recv_init_crash_recovery();
#endif /* !UNIV_HOTBACKUP */

/** Calculates the new value for lsn when more data is added to the log.
@param[in]      lsn             Old LSN
@param[in]      len             This many bytes of data is added, log block
                                headers not included
@return LSN after data addition */
lsn_t recv_calc_lsn_on_data_add(lsn_t lsn, os_offset_t len) {
  os_offset_t frag_len;
  os_offset_t lsn_len;

  frag_len = (lsn % OS_FILE_LOG_BLOCK_SIZE) - LOG_BLOCK_HDR_SIZE;

  ut_ad(frag_len <
        OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_TRL_SIZE);

  lsn_len = len;

  lsn_len +=
      (lsn_len + frag_len) /
      (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_TRL_SIZE) *
      (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);

  return lsn + lsn_len;
}

/** Destructor */
MetadataRecover::~MetadataRecover() {
  for (auto &table : m_tables) {
    ut::delete_(table.second);
  }
}

/** Get the dynamic metadata of a specified table, create a new one
if not exist
@param[in]      id      table id
@return the metadata of the specified table */
PersistentTableMetadata *MetadataRecover::getMetadata(table_id_t id) {
  PersistentTableMetadata *metadata = nullptr;
  PersistentTables::iterator iter = m_tables.find(id);

  if (iter == m_tables.end()) {
    metadata = ut::new_withkey<PersistentTableMetadata>(
        UT_NEW_THIS_FILE_PSI_KEY, id, 0);

    m_tables.insert(std::make_pair(id, metadata));
  } else {
    metadata = iter->second;
    ut_ad(metadata->get_table_id() == id);
  }

  ut_ad(metadata != nullptr);
  return metadata;
}

/** Parse a dynamic metadata redo log of a table and store
the metadata locally
@param[in]      id      table id
@param[in]      version table dynamic metadata version
@param[in]      ptr     redo log start
@param[in]      end     end of redo log
@retval ptr to next redo log record, nullptr if this log record
was truncated */
byte *MetadataRecover::parseMetadataLog(table_id_t id, uint64_t version,
                                        byte *ptr, byte *end) {
  if (ptr + 2 > end) {
    /* At least we should get type byte and another one byte
    for data, if not, it's an incomplete log */
    return nullptr;
  }

  persistent_type_t type = static_cast<persistent_type_t>(ptr[0]);

  ut_ad(dict_persist->persisters != nullptr);

  Persister *persister = dict_persist->persisters->get(type);
  PersistentTableMetadata *metadata = getMetadata(id);

  bool corrupt;
  ulint consumed = persister->read(*metadata, ptr, end - ptr, &corrupt);

  if (corrupt) {
    recv_sys->found_corrupt_log = true;
  } else if (consumed != 0) {
    metadata->set_version(version);
  }

  if (consumed == 0) {
    return nullptr;
  } else {
    return ptr + consumed;
  }
}

/** Apply the collected persistent dynamic metadata to in-memory
table objects */
void MetadataRecover::apply() {
  PersistentTables::iterator iter;

  for (iter = m_tables.begin(); iter != m_tables.end(); ++iter) {
    table_id_t table_id = iter->first;
    PersistentTableMetadata *metadata = iter->second;
    dict_table_t *table;

    table = dd_table_open_on_id(table_id, nullptr, nullptr, false, true);

    /* If the table is nullptr, it might be already dropped */
    if (table == nullptr) {
      continue;
    }

    dict_sys_mutex_enter();

    /* At this time, the metadata in DDTableBuffer has
    already been applied to table object, we can apply
    the latest status of metadata read from redo logs to
    the table now. We can read the dirty_status directly
    since it's in recovery phase */

    /* The table should be either CLEAN or applied BUFFERED
    metadata from DDTableBuffer just now */
    ut_ad(table->dirty_status.load() == METADATA_CLEAN ||
          table->dirty_status.load() == METADATA_BUFFERED);

    bool buffered = (table->dirty_status.load() == METADATA_BUFFERED);

    mutex_enter(&dict_persist->mutex);

    uint64_t autoinc_persisted = table->autoinc_persisted;
    bool is_dirty = dict_table_apply_dynamic_metadata(table, metadata);

    if (is_dirty) {
      /* This table was not marked as METADATA_BUFFERED
      before the redo logs are applied, so it's not in
      the list */
      if (!buffered) {
        ut_ad(!table->in_dirty_dict_tables_list);
#ifndef UNIV_HOTBACKUP
        UT_LIST_ADD_LAST(dict_persist->dirty_dict_tables, table);
#endif
      }

      table->dirty_status.store(METADATA_DIRTY);
      ut_d(table->in_dirty_dict_tables_list = true);
      ++dict_persist->num_dirty_tables;

      /* For those tables which are not initialized by
      innobase_initialize_autoinc(), the next counter should be advanced to
      point to the next auto increment value.  This is simlilar to
      metadata_applier::operator(). */
      if (autoinc_persisted != table->autoinc_persisted &&
          table->autoinc != ~0ULL) {
        ++table->autoinc;
      }
    }

    mutex_exit(&dict_persist->mutex);
    dict_sys_mutex_exit();

    dd_table_close(table, nullptr, nullptr, false);
  }
}

/** Creates the recovery system. */
void recv_sys_create() {
  if (recv_sys != nullptr) {
    return;
  }

  recv_sys = static_cast<recv_sys_t *>(
      ut::zalloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(*recv_sys)));

  mutex_create(LATCH_ID_RECV_SYS, &recv_sys->mutex);

  recv_sys->spaces = nullptr;
}

/** Resize the recovery parsing buffer up to log_buffer_size */
static bool recv_sys_resize_buf() {
  ut_ad(recv_sys->buf_len <= srv_log_buffer_size);

#ifndef UNIV_HOTBACKUP
  /* If the buffer cannot be extended further, return false. */
  if (recv_sys->buf_len == srv_log_buffer_size) {
    ib::error(ER_IB_MSG_723, srv_log_buffer_size);
    return false;
  }
#else  /* !UNIV_HOTBACKUP */
  if ((recv_sys->buf_len >= srv_log_buffer_size) ||
      (recv_sys->len >= srv_log_buffer_size)) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_ERR_LOG_PARSING_BUFFER_OVERFLOW)
        << "Log parsing buffer overflow. Log parse failed. "
        << "Please increase --limit-memory above "
        << srv_log_buffer_size / 1024 / 1024 << " (MB)";
  }
#endif /* !UNIV_HOTBACKUP */

  /* Extend the buffer by double the current size with the resulting
  size not more than srv_log_buffer_size. */
  recv_sys->buf_len = ((recv_sys->buf_len * 2) >= srv_log_buffer_size)
                          ? srv_log_buffer_size
                          : recv_sys->buf_len * 2;

  /* Resize the buffer to the new size. */
  recv_sys->buf = static_cast<byte *>(ut::realloc_withkey(
      UT_NEW_THIS_FILE_PSI_KEY, recv_sys->buf, recv_sys->buf_len));

  ut_ad(recv_sys->buf != nullptr);

  /* Return error and fail the recovery if not enough memory available */
  if (recv_sys->buf == nullptr) {
    ib::error(ER_IB_MSG_740);
    return false;
  }

  ib::info(ER_IB_MSG_739, recv_sys->buf_len);
  return true;
}

/** Free up recovery data structures. */
static void recv_sys_finish() {
#ifndef UNIV_HOTBACKUP
  recv_sys->dblwr->recovered();
#endif /* !UNIV_HOTBACKUP */

  if (recv_sys->spaces != nullptr) {
    for (auto &space : *recv_sys->spaces) {
      if (space.second.m_heap != nullptr) {
        mem_heap_free(space.second.m_heap);
        space.second.m_heap = nullptr;
      }
    }

    ut::delete_(recv_sys->spaces);
  }

  ut::free(recv_sys->buf);
  ut::aligned_free(recv_sys->last_block);
  ut::delete_(recv_sys->metadata_recover);

  recv_sys->buf = nullptr;
  recv_sys->spaces = nullptr;
  recv_sys->metadata_recover = nullptr;
  recv_sys->last_block = nullptr;
}

/** Release recovery system mutexes. */
void recv_sys_close() {
  if (recv_sys == nullptr) {
    return;
  }

  recv_sys_finish();

#ifndef UNIV_HOTBACKUP
  if (recv_sys->flush_start != nullptr) {
    os_event_destroy(recv_sys->flush_start);
  }

  if (recv_sys->flush_end != nullptr) {
    os_event_destroy(recv_sys->flush_end);
  }
#endif /* !UNIV_HOTBACKUP */

  ut::delete_(recv_sys->dblwr);

  call_destructor(&recv_sys->deleted);
  call_destructor(&recv_sys->missing_ids);
  call_destructor(&recv_sys->saved_recs);

  mutex_free(&recv_sys->mutex);

  ut::free(recv_sys);
  recv_sys = nullptr;
}

#ifndef UNIV_HOTBACKUP
/** Reset the state of the recovery system variables. */
void recv_sys_var_init() {
  recv_recovery_on = false;
  recv_needed_recovery = false;
  recv_lsn_checks_on = false;
  recv_no_ibuf_operations = false;
  recv_scan_print_counter = 0;
  recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG;
  recv_previous_parsed_rec_offset = 0;
  recv_previous_parsed_rec_is_multi = 0;
  recv_n_pool_free_frames = 256;
  recv_max_page_lsn = 0;
}
#endif /* !UNIV_HOTBACKUP */

/** Get the number of bytes used by all the heaps
@return number of bytes used */
#ifndef UNIV_HOTBACKUP
static size_t recv_heap_used()
#else  /* !UNIV_HOTBACKUP */
size_t meb_heap_used()
#endif /* !UNIV_HOTBACKUP */
{
  size_t size = 0;

  for (auto &space : *recv_sys->spaces) {
    if (space.second.m_heap != nullptr) {
      size += mem_heap_get_size(space.second.m_heap);
    }
  }

  return size;
}

/** Prints diagnostic info of corrupt log.
@param[in]      ptr     pointer to corrupt log record
@param[in]      type    type of the log record (could be garbage)
@param[in]      space   tablespace ID (could be garbage)
@param[in]      page_no page number (could be garbage)
@return whether processing should continue */
static bool recv_report_corrupt_log(const byte *ptr, int type, space_id_t space,
                                    page_no_t page_no) {
  ib::error(ER_IB_MSG_694);

  ib::info(
      ER_IB_MSG_695, type, ulong{space}, ulong{page_no},
      ulonglong{recv_sys->recovered_lsn}, int{recv_previous_parsed_rec_type},
      ulonglong{recv_previous_parsed_rec_is_multi},
      ssize_t{ptr - recv_sys->buf}, ulonglong{recv_previous_parsed_rec_offset});

#ifdef UNIV_HOTBACKUP
  ut_ad(ptr >= recv_sys->buf);
#endif /* UNIV_HOTBACKUP */
  ut_ad(ptr <= recv_sys->buf + recv_sys->len);

  const ulint limit = 100;
  const ulint before = std::min(recv_previous_parsed_rec_offset, limit);
  const ulint after = std::min(recv_sys->len - (ptr - recv_sys->buf), limit);

  ib::info(ER_IB_MSG_696, ulonglong{before}, ulonglong{after});

  ut_print_buf(
      stderr, recv_sys->buf + recv_previous_parsed_rec_offset - before,
      ptr - recv_sys->buf + before + after - recv_previous_parsed_rec_offset);
  putc('\n', stderr);

#ifndef UNIV_HOTBACKUP
  if (srv_force_recovery == 0) {
    ib::info(ER_IB_MSG_697);

    return false;
  }

  ib::warn(ER_IB_MSG_LOG_CORRUPT, FORCE_RECOVERY_MSG);
#endif /* !UNIV_HOTBACKUP */

  return true;
}

void recv_sys_init() {
  if (recv_sys->spaces != nullptr) {
    return;
  }

  mutex_enter(&recv_sys->mutex);

#ifndef UNIV_HOTBACKUP
  if (!srv_read_only_mode) {
    recv_sys->flush_start = os_event_create();
    recv_sys->flush_end = os_event_create();
  }
#else  /* !UNIV_HOTBACKUP */
  recv_is_from_backup = true;
  recv_sys->apply_file_operations = false;
#endif /* !UNIV_HOTBACKUP */

  recv_sys->buf_len =
      std::min<unsigned long>(RECV_PARSING_BUF_SIZE, srv_log_buffer_size);
  recv_sys->buf = static_cast<byte *>(
      ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, recv_sys->buf_len));

  recv_sys->len = 0;
  recv_sys->recovered_offset = 0;

  using Spaces = recv_sys_t::Spaces;

  recv_sys->spaces = ut::new_withkey<Spaces>(
      ut::make_psi_memory_key(mem_log_recv_space_hash_key));

  recv_sys->n_addrs = 0;

  recv_sys->apply_log_recs = false;
  recv_sys->apply_batch_on = false;
  recv_sys->is_cloned_db = false;

  recv_sys->last_block = static_cast<byte *>(
      ut::aligned_alloc(OS_FILE_LOG_BLOCK_SIZE, OS_FILE_LOG_BLOCK_SIZE));

  recv_sys->found_corrupt_log = false;
  recv_sys->found_corrupt_fs = false;

  recv_max_page_lsn = 0;

  recv_sys->dblwr =
      ut::new_withkey<dblwr::recv::DBLWR>(UT_NEW_THIS_FILE_PSI_KEY);

  new (&recv_sys->deleted) recv_sys_t::Missing_Ids();

  new (&recv_sys->missing_ids) recv_sys_t::Missing_Ids();

  new (&recv_sys->saved_recs) recv_sys_t::Mlog_records();

  recv_sys->saved_recs.resize(recv_sys_t::MAX_SAVED_MLOG_RECS);

  recv_sys->metadata_recover =
      ut::new_withkey<MetadataRecover>(UT_NEW_THIS_FILE_PSI_KEY);

  mutex_exit(&recv_sys->mutex);
}

/** Empties the hash table when it has been fully processed. */
static void recv_sys_empty_hash() {
  ut_ad(mutex_own(&recv_sys->mutex));

  if (recv_sys->n_addrs != 0) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_699, ulonglong{recv_sys->n_addrs});
  }

  for (auto &space : *recv_sys->spaces) {
    if (space.second.m_heap != nullptr) {
      mem_heap_free(space.second.m_heap);
      space.second.m_heap = nullptr;
    }
  }

  ut::delete_(recv_sys->spaces);

  using Spaces = recv_sys_t::Spaces;

  recv_sys->spaces = ut::new_withkey<Spaces>(
      ut::make_psi_memory_key(mem_log_recv_space_hash_key));
}

/** Check the 4-byte checksum to the trailer checksum field of a log
block.
@param[in]      block   pointer to a log block
@return whether the checksum matches */
#ifndef UNIV_HOTBACKUP
static
#endif /* !UNIV_HOTBACKUP */
    bool
    log_block_checksum_is_ok(const byte *block) {
  return !srv_log_checksums ||
         log_block_get_checksum(block) == log_block_calc_checksum(block);
}

/** Get the page map for a tablespace. It will create one if one isn't found.
@param[in]      space_id        Tablespace ID for which page map required.
@param[in]      create          false if lookup only
@return the space data or null if not found */
static recv_sys_t::Space *recv_get_page_map(space_id_t space_id, bool create) {
  auto it = recv_sys->spaces->find(space_id);

  if (it != recv_sys->spaces->end()) {
    return &it->second;

  } else if (create) {
    mem_heap_t *heap;

    heap = mem_heap_create(256, UT_LOCATION_HERE, MEM_HEAP_FOR_RECV_SYS);

    using Space = recv_sys_t::Space;
    using Value = recv_sys_t::Spaces::value_type;

    auto where = recv_sys->spaces->insert(it, Value{space_id, Space(heap)});

    return &where->second;
  }

  return nullptr;
}

/** Gets the list of log records for a <space, page>.
@param[in]      space_id        Tablespace ID
@param[in]      page_no         Page number
@return the redo log entries or nullptr if not found */
static recv_addr_t *recv_get_rec(space_id_t space_id, page_no_t page_no) {
  recv_sys_t::Space *space;

  space = recv_get_page_map(space_id, false);

  if (space != nullptr) {
    auto it = space->m_pages.find(page_no);

    if (it != space->m_pages.end()) {
      return it->second;
    }
  }

  return nullptr;
}

/** Checks if a given log data block could be considered a next valid block,
with regards to the epoch_no it has stored in its header, during the recovery.
@param[in]  log_block_epoch_no  epoch_no of the log data block to check
@param[in]  last_epoch_no       epoch_no of the last data block scanned
@return true iff the provided log block has valid epoch_no */
static bool log_block_epoch_no_is_valid(uint32_t log_block_epoch_no,
                                        uint32_t last_epoch_no) {
  const auto expected_next_epoch_no = last_epoch_no + 1;

  return log_block_epoch_no == last_epoch_no ||
         log_block_epoch_no == expected_next_epoch_no;
}

bool is_mysql_ibd_page_0_in_redo() {
  return recv_get_rec(dict_sys_t::s_dict_space_id, 0) != nullptr;
}

#ifndef UNIV_HOTBACKUP
/** Store the collected persistent dynamic metadata to
mysql.innodb_dynamic_metadata */
void MetadataRecover::store() {
  ut_ad(dict_sys->dynamic_metadata != nullptr);
  ut_ad(dict_persist->table_buffer != nullptr);

  DDTableBuffer *table_buffer = dict_persist->table_buffer;

  if (empty()) {
    return;
  }

  mutex_enter(&dict_persist->mutex);

  for (auto meta : m_tables) {
    table_id_t table_id = meta.first;
    PersistentTableMetadata *metadata = meta.second;
    byte buffer[REC_MAX_DATA_SIZE];
    size_t size;

    size = dict_persist->persisters->write(*metadata, buffer);

    dberr_t error =
        table_buffer->replace(table_id, metadata->get_version(), buffer, size);
    if (error != DB_SUCCESS) {
      ut_d(ut_error);
    }
  }

  mutex_exit(&dict_persist->mutex);
}

#endif /* !UNIV_HOTBACKUP */

/** Frees the recovery system. */
void recv_sys_free() {
  if (!recv_sys) return;

  mutex_enter(&recv_sys->mutex);

  recv_sys_finish();

#ifndef UNIV_HOTBACKUP
  /* wake page cleaner up to progress */
  if (!srv_read_only_mode) {
    ut_ad(!recv_recovery_on);
    if (buf_flush_event != nullptr) {
      os_event_reset(buf_flush_event);
    }
    os_event_set(recv_sys->flush_start);
  }
#endif /* !UNIV_HOTBACKUP */

  /* Free encryption data structures. */
  if (recv_sys->keys != nullptr) {
    for (auto &key : *recv_sys->keys) {
      if (key.ptr != nullptr) {
        ut::free(key.ptr);
        key.ptr = nullptr;
      }

      if (key.iv != nullptr) {
        ut::free(key.iv);
        key.iv = nullptr;
      }
    }

    recv_sys->keys->swap(*recv_sys->keys);

    ut::delete_(recv_sys->keys);
    recv_sys->keys = nullptr;
  }

  mutex_exit(&recv_sys->mutex);
}

#ifndef UNIV_HOTBACKUP

/** Determine if a redo log from a version before MySQL 8.0.30 is clean.
@param[in,out]  log             redo log
@return error code
@retval DB_SUCCESS  if the redo log is clean
@retval DB_ERROR    if the redo log is corrupted or dirty */
static dberr_t recv_log_recover_pre_8_0_30(log_t &log) {
  const size_t n_files = log_files_number_of_existing_files(log.m_files);
  ut_a(n_files >= 2);

  ib::info(ER_IB_MSG_LOG_FORMAT_OLD, ulong{to_int(log.m_format)});

  using namespace log_pre_8_0_30;

  const auto logfile0 = log.m_files.file(0);
  ut_a(logfile0 != log.m_files.end());

  const os_offset_t file_size = logfile0->m_size_in_bytes;

  /* For unknown reasons, InnoDB before 8.0.30 was choosing the latest
  checkpoint by comparing checkpoints' numbers instead of checkpoints'
  LSN values. These should be ordered the same and there shouldn't be
  difference, but to preserve the full compatibility, we prefer to do
  it the same way as it was (after 8.0.30, checkpoints are compared by
  their LSN values because we no longer store checkpoint numbers). */
  byte header_buf[OS_FILE_LOG_BLOCK_SIZE] = {};

  Checkpoint_header chkp_header = {};
  bool checkpoint_found = false;
  for (auto hdr_no : {Log_checkpoint_header_no::HEADER_1,
                      Log_checkpoint_header_no::HEADER_2}) {
    auto file_handle = logfile0->open(Log_file_access_mode::READ_ONLY);
    if (!file_handle.is_open()) {
      return DB_CANNOT_OPEN_FILE;
    }
    const dberr_t err =
        log_checkpoint_header_read(file_handle, hdr_no, header_buf);
    if (err != DB_SUCCESS) {
      return DB_ERROR;
    }
    Checkpoint_header h;
    if (!checkpoint_header_deserialize(header_buf, h)) {
      continue;
    }
    if (!checkpoint_found || h.m_checkpoint_no > chkp_header.m_checkpoint_no) {
      chkp_header = h;
      checkpoint_found = true;
    }
  }
  if (!checkpoint_found) {
    ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_NOT_FOUND);
    return DB_ERROR;
  }

  if (log_encryption_read(log, *logfile0) != DB_SUCCESS) {
    return DB_ERROR;
  }

  os_offset_t source_offset =
      chkp_header.m_checkpoint_offset % (file_size * n_files);

  const Log_file_id file_id = source_offset / file_size;

  source_offset %= file_size;

  static const char *RTFM_LINK = REFMAN "upgrading.html";

  byte buf[OS_FILE_LOG_BLOCK_SIZE];

  auto file_handle =
      Log_file::open(log.m_files_ctx, file_id, Log_file_access_mode::READ_ONLY,
                     log.m_encryption_metadata);
  ut_a(file_handle.is_open());

  const dberr_t err = file_handle.read(
      ut_uint64_align_down(source_offset, OS_FILE_LOG_BLOCK_SIZE),
      OS_FILE_LOG_BLOCK_SIZE, buf);
  ut_a(err == DB_SUCCESS);

  file_handle.close();

  if (!log_block_checksum_is_ok(buf)) {
    ib::error(ER_IB_MSG_LOG_FORMAT_OLD_AND_LOG_CORRUPTED,
              log.m_creator_name.c_str(), RTFM_LINK);
    return DB_ERROR;
  }

  /* On a shutdown with innodb-fast-shutdown < 2, the redo log will be
  logically empty after the checkpoint LSN. */

  if (log_block_get_data_len(buf) !=
      (source_offset & (OS_FILE_LOG_BLOCK_SIZE - 1))) {
    ib::error(ER_IB_MSG_LOG_FORMAT_OLD_AND_NO_CLEAN_SHUTDOWN,
              log.m_creator_name.c_str(), RTFM_LINK);
    return DB_ERROR;
  }

  /* Start at the beginning of the next block to avoid a need to rewrite
  real data bytes for checkpoint_lsn-1, checkpoint_lsn-2, .. inside the
  same log block to which the checkpoint_lsn belongs to. */
  const lsn_t checkpoint_lsn =
      ut_uint64_align_up(chkp_header.m_checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE) +
      LOG_BLOCK_HDR_SIZE;

  recv_sys->parse_start_lsn = checkpoint_lsn;
  recv_sys->bytes_to_ignore_before_checkpoint = 0;
  recv_sys->recovered_lsn = checkpoint_lsn;
  recv_sys->previous_recovered_lsn = checkpoint_lsn;
  recv_sys->checkpoint_lsn = checkpoint_lsn;
  recv_sys->scanned_lsn = checkpoint_lsn;
  recv_sys->last_block_first_rec_group = 0;

  ut_d(log.first_block_is_correct_for_lsn = checkpoint_lsn);

  /* We are not going to rewrite the block, but just in case we prefer to
  have first_rec_group which points on checkpoint_lsn (instead of pointing
  on mini-transactions from earlier formats). This is extra safety if one
  day this block would become rewritten because of some new bug (using new
  format). */
  log_block_set_first_rec_group(buf, checkpoint_lsn % OS_FILE_LOG_BLOCK_SIZE);

  return log_start(log, checkpoint_lsn, checkpoint_lsn, nullptr);
}

/** Describes location of a single checkpoint. */
struct Log_checkpoint_location {
  /** File containing checkpoint header and checkpoint lsn. */
  Log_file_id m_checkpoint_file_id{0};

  /** Checkpoint header number. */
  Log_checkpoint_header_no m_checkpoint_header_no{};

  /** Checkpoint LSN. */
  lsn_t m_checkpoint_lsn{0};
};

/** Find the latest checkpoint in the given log file.
@param[in]      file_handle     handle for the opened redo log file
@param[out]     checkpoint      the latest checkpoint found (if any)
@return true iff any checkpoint has been found */
/** 在给定的日志文件中查找最新的检查点。
@param[in]      file_handle     已打开的重做日志文件句柄
@param[out]     checkpoint      找到的最新检查点（如果有）
@return 如果找到任何检查点则返回 true */
[[nodiscard]] static bool recv_find_max_checkpoint(
    log_t &, Log_file_handle &file_handle,
    Log_checkpoint_location &checkpoint) {
  bool found = false; // 是否找到检查点的标志
  checkpoint = {}; // 初始化检查点

  for (auto checkpoint_header_no : {Log_checkpoint_header_no::HEADER_1,
                                    Log_checkpoint_header_no::HEADER_2}) { // 遍历两个检查点头部
    Log_checkpoint_header checkpoint_header; // 检查点头部
    const dberr_t err = log_checkpoint_header_read(
        file_handle, checkpoint_header_no, checkpoint_header); // 读取检查点头部
    if (err != DB_SUCCESS) { // 如果读取失败
      /* Crash if IO error on read */
      /* 如果读取时发生 IO 错误，则崩溃 */
      ut_a(err == DB_CORRUPTION); // 断言错误为 DB_CORRUPTION
      continue; // 继续
    }

    const lsn_t checkpoint_lsn = checkpoint_header.m_checkpoint_lsn; // 获取检查点 LSN
    if (checkpoint_lsn == 0) { // 如果检查点 LSN 为 0
      continue; // 继续
    }

    DBUG_PRINT("ib_log", ("checkpoint at " LSN_PF, checkpoint_lsn)); // 打印调试信息

    if (!found || checkpoint_lsn > checkpoint.m_checkpoint_lsn) { // 如果未找到检查点或检查点 LSN 大于当前检查点 LSN
      ut_a(checkpoint_lsn >= LOG_START_LSN); // 断言检查点 LSN 大于等于日志起始 LSN
      found = true; // 设置找到检查点的标志
      checkpoint.m_checkpoint_file_id = file_handle.file_id(); // 设置检查点文件 ID
      checkpoint.m_checkpoint_header_no = checkpoint_header_no; // 设置检查点头部编号
      checkpoint.m_checkpoint_lsn = checkpoint_lsn; // 设置检查点 LSN
    }
  }

  return found; // 返回是否找到检查点
}

/** Find the latest checkpoint (check all existing redo log files).
@param[in,out]  log             redo log
@param[out]     checkpoint      the latest checkpoint found (if any)
@return true iff any checkpoint has been found */
/** 查找最新的检查点（检查所有现有的重做日志文件）。
@param[in,out]  log             重做日志
@param[out]     checkpoint      找到的最新检查点（如果有）
@return 如果找到任何检查点则返回 true */
static bool recv_find_max_checkpoint(log_t &log,
                                     Log_checkpoint_location &checkpoint) {
  bool found = false; // 是否找到检查点的标志
  checkpoint = {}; // 初始化检查点

  log_files_for_each(log.m_files, [&](const Log_file &file) { // 遍历所有重做日志文件
    auto file_handle = file.open(Log_file_access_mode::READ_ONLY); // 以只读方式打开文件
    ut_a(file_handle.is_open()); // 断言文件已打开

    Log_checkpoint_location checkpoint_in_file; // 文件中的检查点位置

    if (!recv_find_max_checkpoint(log, file_handle, checkpoint_in_file)) { // 如果未找到检查点
      return; // 返回
    }

    if (!file.contains(checkpoint_in_file.m_checkpoint_lsn)) { // 如果文件不包含检查点 LSN
      const auto file_path = file_handle.file_path(); // 获取文件路径
      ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_OUTSIDE_LOG_FILE,
                ulonglong{checkpoint_in_file.m_checkpoint_lsn},
                file_path.c_str(), ulonglong{file.m_start_lsn},
                ulonglong{file.m_end_lsn}); // 打印错误信息
      return; // 返回
    }

    if (!found ||
        checkpoint_in_file.m_checkpoint_lsn > checkpoint.m_checkpoint_lsn) { // 如果未找到检查点或文件中的检查点 LSN 大于当前检查点 LSN
      found = true; // 设置找到检查点的标志
      checkpoint = checkpoint_in_file; // 更新检查点
    }
  });

  return found; // 返回是否找到检查点
}

/** Reads in pages which have hashed log records, from an area around a given
page number.
读取具有哈希日志记录的页面，从给定页面编号周围的区域读取。
@param[in]      page_id         Read the pages around this page number
                                读取此页面编号周围的页面
@return number of pages found
        找到的页面数量 */
static ulint recv_read_in_area(const page_id_t &page_id) {
  page_no_t low_limit; // 低限制

  low_limit = page_id.page_no() - (page_id.page_no() % RECV_READ_AHEAD_AREA); // 计算低限制

  ulint n = 0; // 找到的页面数量

  std::array<page_no_t, RECV_READ_AHEAD_AREA> page_nos; // 页面编号数组

  for (page_no_t page_no = low_limit;
       page_no < low_limit + RECV_READ_AHEAD_AREA; ++page_no) { // 遍历页面编号
    recv_addr_t *recv_addr; // 接收地址

    recv_addr = recv_get_rec(page_id.space(), page_no); // 获取接收记录

    const page_id_t cur_page_id(page_id.space(), page_no); // 当前页面 ID

    if (recv_addr != nullptr && !buf_page_peek(cur_page_id)) { // 如果接收地址不为空且页面未被查看
      mutex_enter(&recv_sys->mutex); // 进入互斥锁

      if (recv_addr->state == RECV_NOT_PROCESSED) { // 如果接收地址状态为未处理
        recv_addr->state = RECV_BEING_READ; // 设置接收地址状态为正在读取

        page_nos[n] = page_no; // 保存页面编号

        ++n; // 增加找到的页面数量
      }

      mutex_exit(&recv_sys->mutex); // 退出互斥锁
    }
  }

  if (n > 0) {
    /* There are pages that need to be read. Go ahead and read them
    for recovery. */
    /* 有需要读取的页面。继续读取它们以进行恢复。 */
    buf_read_recv_pages(false, page_id.space(), &page_nos[0], n); // 读取接收页面
  }

  return n; // 返回找到的页面数量
}

/** Apply the log records to a page
@param[in,out]  recv_addr       Redo log records to apply */
/** 将日志记录应用到页面
@param[in,out]  recv_addr       要应用的重做日志记录 */
static void recv_apply_log_rec(recv_addr_t *recv_addr) {
  if (recv_addr->state == RECV_DISCARDED) {
    ut_a(recv_sys->n_addrs > 0);
    --recv_sys->n_addrs;
    return;
  }

  bool found;
  const page_id_t page_id(recv_addr->space, recv_addr->page_no); // 获取页面 ID

  const page_size_t page_size =
      fil_space_get_page_size(recv_addr->space, &found); // 获取页面大小

  if (!found || recv_sys->missing_ids.find(recv_addr->space) !=
                    recv_sys->missing_ids.end()) {
    /* Tablespace was discarded or dropped after changes were
    made to it. Or, we have ignored redo log for this tablespace
    earlier and somehow it has been found now. We can't apply
    this redo log out of order. */
    /* 表空间在更改后被丢弃或删除。或者，我们之前忽略了该表空间的重做日志，
    现在以某种方式找到了它。我们不能无序地应用这个重做日志。 */

    recv_addr->state = RECV_PROCESSED; // 设置状态为已处理

    ut_a(recv_sys->n_addrs > 0);
    --recv_sys->n_addrs;

    /* If the tablespace has been explicitly deleted, we
    can safely ignore it. */
    /* 如果表空间已被显式删除，我们可以安全地忽略它。 */

    if (recv_sys->deleted.find(recv_addr->space) == recv_sys->deleted.end()) {
      recv_sys->missing_ids.insert(recv_addr->space); // 插入缺失的表空间 ID
    }

  } else if (recv_addr->state == RECV_NOT_PROCESSED) {
    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    if (buf_page_peek(page_id)) {
      mtr_t mtr;

      mtr_start(&mtr); // 开始 mini-transaction

      buf_block_t *block;

      block =
          buf_page_get(page_id, page_size, RW_X_LATCH, UT_LOCATION_HERE, &mtr); // 获取页面块

      buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);

      recv_recover_page(false, block); // 恢复页面

      mtr_commit(&mtr); // 提交 mini-transaction

    } else {
      recv_read_in_area(page_id); // 读取页面区域(预读)
    }

    mutex_enter(&recv_sys->mutex); // 进入互斥锁
  }
}

dberr_t recv_apply_hashed_log_recs(log_t &log, bool allow_ibuf) {
  for (;;) {
    mutex_enter(&recv_sys->mutex); // 进入互斥锁

    if (!recv_sys->apply_batch_on) {
      break; // 如果不需要应用批处理，则退出循环
    }

    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 休眠 500 毫秒
  }

  if (!allow_ibuf) {
    recv_no_ibuf_operations = true; // 禁止插入缓冲操作
  }

  recv_sys->apply_log_recs = true; // 设置应用日志记录标志
  recv_sys->apply_batch_on = true; // 设置应用批处理标志

  auto batch_size = recv_sys->n_addrs; // 获取批处理大小

  ib::info(ER_IB_MSG_707, ulonglong{batch_size}); // 打印批处理大小信息

  static const size_t PCT = 10;

  size_t pct = PCT;
  size_t applied = 0;
  auto unit = batch_size / PCT;

  if (unit <= PCT) {
    pct = 100;
    unit = batch_size;
  }

  auto start_time = std::chrono::steady_clock::now(); // 获取当前时间

  for (const auto &space : *recv_sys->spaces) { // 遍历所有表空间
    bool dropped;

    if (space.first == TRX_SYS_SPACE) {
      dropped = false;
    } else {
      dberr_t err = fil_tablespace_open_for_recovery(space.first); // 打开表空间进行恢复
      if (err == DB_SUCCESS) {
        dropped = false;
      } else if (err == DB_CORRUPTION) {
        /* Page couldn't be recovered from doublewrite, we cannot proceed
        with recovery. Skip applying redos and abort the startup. */
        /* 无法从双写缓冲区恢复页面，无法继续恢复。跳过应用重做日志并中止启动。 */
        mutex_exit(&recv_sys->mutex); // 退出互斥锁
        return err; // 返回错误
      } else {
        /* Tablespace was dropped. It should not have been scanned unless it
        is an undo space that was under construction. */
        /* 表空间已删除。除非是正在构建的撤销表空间，否则不应扫描它。 */

        if (fil_tablespace_lookup_for_recovery(space.first)) {
          ut_ad(fsp_is_undo_tablespace(space.first));
        }
        dropped = true;
      }
    }

    for (auto pages : space.second.m_pages) { // 遍历表空间中的所有页面
      ut_ad(pages.second->space == space.first);

      if (dropped) {
        pages.second->state = RECV_DISCARDED; // 如果表空间已删除，将页面状态设置为丢弃
      }

      recv_apply_log_rec(pages.second); // 应用日志记录到页面

      ++applied;

      if (unit == 0 || (applied % unit) == 0) {
        ib::info(ER_IB_MSG_708) << pct << "%"; // 打印进度信息

        pct += PCT;

        start_time = std::chrono::steady_clock::now(); // 更新开始时间

      } else if (std::chrono::steady_clock::now() - start_time >=
                 PRINT_INTERVAL) {
        start_time = std::chrono::steady_clock::now(); // 更新开始时间

        ib::info(ER_IB_MSG_709)
            << std::setprecision(2)
            << ((double)applied * 100) / (double)batch_size << "%"; // 打印进度百分比
      }
    }
  }

  /* Wait until all the pages have been processed */
  /* 等待所有页面处理完成 */

  while (recv_sys->n_addrs != 0) {
    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 休眠 500 毫秒

    mutex_enter(&recv_sys->mutex); // 进入互斥锁
  }

  if (!allow_ibuf) {
    /* Flush all the file pages to disk and invalidate them in
    the buffer pool */
    /* 将所有文件页面刷新到磁盘，并在缓冲池中使其失效 */
    ut_d(log.disable_redo_writes = true);
    ut_a(recv_sys->flush_end != nullptr);

    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    os_event_reset(recv_sys->flush_end); // 重置事件

    os_event_set(recv_sys->flush_start); // 设置事件

    os_event_wait(recv_sys->flush_end); // 等待事件

    /* Wait for any currently running batch to end. */
    /* 等待任何当前正在运行的批处理结束。 */
    buf_flush_wait_LRU_batch_end();

    buf_pool_invalidate(); // 使缓冲池失效

    ut_d(log.disable_redo_writes = false);

    mutex_enter(&recv_sys->mutex); // 进入互斥锁

    recv_no_ibuf_operations = false; // 允许插入缓冲操作
  }

  recv_sys->apply_log_recs = false; // 清除应用日志记录标志
  recv_sys->apply_batch_on = false; // 清除应用批处理标志

  recv_sys_empty_hash(); // 清空哈希表

  mutex_exit(&recv_sys->mutex); // 退出互斥锁

  ib::info(ER_IB_MSG_710); // 打印完成信息
  return DB_SUCCESS; // 返回成功
}

#else /* !UNIV_HOTBACKUP */
/** Scans the log segment and n_bytes_scanned is set to the length of valid
log scanned.
@param[in]      buf                     buffer containing log data
@param[in]      buf_len                 data length in that buffer
@param[in,out]  scanned_lsn             LSN of buffer start, we return scanned
lsn
@param[in,out]  scanned_epoch_no        the highest scanned epoch number so far
@param[out]     block_no        highest block no in scanned buffer.
@param[out]     n_bytes_scanned         how much we were able to scan, smaller
than buf_len if log data ended here
+@param[out]    has_encrypted_log       set true, if buffer contains encrypted
+redo log, set false otherwise */
void meb_scan_log_seg(byte *buf, size_t buf_len, lsn_t *scanned_lsn,
                      uint32_t *scanned_epoch_no, uint32_t *block_no,
                      size_t *n_bytes_scanned, bool *has_encrypted_log) {
  *n_bytes_scanned = 0;
  *has_encrypted_log = false;

  for (auto log_block = buf; log_block < buf + buf_len;
       log_block += OS_FILE_LOG_BLOCK_SIZE) {
    Log_data_block_header block_header;
    log_data_block_header_deserialize(log_block, block_header);
    uint32_t no = block_header.m_hdr_no;
    bool is_encrypted = log_block_get_encrypt_bit(log_block);

    if (is_encrypted) {
      *has_encrypted_log = true;
      return;
    }

    if (no != log_block_convert_lsn_to_hdr_no(*scanned_lsn) ||
        !log_block_checksum_is_ok(log_block)) {
      ib::trace_2() << "Scanned lsn: " << *scanned_lsn << " header no: " << no
                    << " converted no: "
                    << log_block_convert_lsn_to_hdr_no(*scanned_lsn)
                    << " checksum: " << log_block_checksum_is_ok(log_block)
                    << " block epoch no: " << block_header.m_epoch_no;

      /* Garbage or an incompletely written log block */

      log_block += OS_FILE_LOG_BLOCK_SIZE;
      break;
    }

    if (*scanned_epoch_no > 0 &&
        !log_block_epoch_no_is_valid(block_header.m_epoch_no,
                                     *scanned_epoch_no)) {
      /* Garbage from a log buffer flush which was made
      before the most recent database recovery */

      ib::trace_2() << "Scanned ep no: " << *scanned_epoch_no << " block ep no "
                    << block_header.m_epoch_no;

      break;
    }

    const auto data_len = block_header.m_data_len;

    *scanned_epoch_no = block_header.m_epoch_no;
    *scanned_lsn += data_len;

    *n_bytes_scanned += data_len;

    if (data_len < OS_FILE_LOG_BLOCK_SIZE) {
      /* Log data ends here */

      break;
    }
    *block_no = no;
  }
}

/** Apply a single log record stored in the hash table.
@param[in,out]  recv_addr       a parsed log record
@param[in,out]  block           a buffer pool frame for applying the record */
void meb_apply_log_record(recv_addr_t *recv_addr, buf_block_t *block) {
  bool found;
  const page_id_t page_id(recv_addr->space, recv_addr->page_no);

  const page_size_t &page_size =
      fil_space_get_page_size(recv_addr->space, &found);

  ib::trace_3() << "meb_apply_log_record: recv state " << recv_addr->state
                << " space_id " << recv_addr->space << " page_nr "
                << recv_addr->page_no << " page size " << page_size << " found "
                << found;

  if (!found) {
    recv_addr->state = RECV_DISCARDED;

    mutex_enter(&recv_sys->mutex);

    ut_a(recv_sys->n_addrs);
    --recv_sys->n_addrs;

    mutex_exit(&recv_sys->mutex);

    return;
  }

  mutex_enter(&recv_sys->mutex);

  /* We simulate a page read made by the buffer pool, to
  make sure the recovery apparatus works ok. We must init
  the block. */

  meb_page_init(page_id, page_size, block);

  /* Extend the tablespace's last file if the page_no
  does not fall inside its bounds; we assume the last
  file is auto-extending, and mysqlbackup copied the file
  when it still was smaller */

  fil_space_t *space = fil_space_get(recv_addr->space);

  bool success;

  success = fil_space_extend(space, recv_addr->page_no + 1);

  if (!success) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_711)
        << "Cannot extend tablespace " << recv_addr->space << " to hold "
        << recv_addr->page_no << " pages";
  }

  mutex_exit(&recv_sys->mutex);

  /* Read the page from the tablespace file. */

  dberr_t err;

  if (page_size.is_compressed()) {
    err = fil_io(IORequestRead, true, page_id, page_size, 0,
                 page_size.physical(), block->page.zip.data, nullptr);

    if (err == DB_SUCCESS && !buf_zip_decompress(block, true)) {
      ut_error;
    }
  } else {
    err = fil_io(IORequestRead, true, page_id, page_size, 0,
                 page_size.logical(), block->frame, nullptr);
  }

  if (err != DB_SUCCESS) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_712)
        << "Cannot read from tablespace " << recv_addr->space << " page number "
        << recv_addr->page_no;
  }

  apply_log_mutex.lock();

  /* Apply the log records to this page */
  recv_recover_page(false, block);

  apply_log_mutex.unlock();

  mutex_enter(&recv_sys->mutex);

  /* Write the page back to the tablespace file using the
  fil0fil.cc routines */

  buf_flush_init_for_writing(block, block->frame, buf_block_get_page_zip(block),
                             mach_read_from_8(block->frame + FIL_PAGE_LSN),
                             fsp_is_checksum_disabled(block->page.id.space()),
                             true /* skip_lsn_check */);

  mutex_exit(&recv_sys->mutex);

  if (page_size.is_compressed()) {
    err = fil_io(IORequestWrite, true, page_id, page_size, 0,
                 page_size.physical(), block->page.zip.data, nullptr);
  } else {
    err = fil_io(IORequestWrite, true, page_id, page_size, 0,
                 page_size.logical(), block->frame, nullptr);
  }

  if (err != DB_SUCCESS) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_713)
        << "Cannot write to tablespace " << recv_addr->space << " page number "
        << recv_addr->page_no;
  }
}

/** Apply a single log record stored in the hash table using default block.
@param[in,out]  recv_addr       a parsed log record */
void meb_apply_log_rec_func(recv_addr_t *recv_addr) {
  meb_apply_log_record(recv_addr, back_block1);
}

/** Dummy wait function for meb_apply_log_recs_via_callback(). */
void meb_nowait_func() { return; }

/** Applies log records in the hash table to a backup. */
void meb_apply_log_recs() {
  meb_apply_log_recs_via_callback(meb_apply_log_rec_func, meb_nowait_func);
}

/** Apply all log records in the hash table to a backup using callback
functions. This function employes two callback functions that allow redo
log records to be applied in parallel. The apply_log_record_function
assigns a parsed redo log record for application. The
apply_log_record_function is called repeatedly until all log records in
the hash table are assigned for application. After that the
wait_till_done_function is called once. The wait_till_done_function
function blocks until the application of all the redo log records
previously assigned with apply_log_record_function calls is complete.
Even though this function assigns the log records in the hash table
sequentially, the application of the log records may be done in parallel
if the apply_log_record_function delegates the actual application work
to multiple worker threads running in parallel.
@param[in]  apply_log_record_function   a function that assigns one redo log
record for application
@param[in]  wait_till_done_function     a function that blocks until all
assigned redo log records have been applied */
void meb_apply_log_recs_via_callback(
    void (*apply_log_record_function)(recv_addr_t *),
    void (*wait_till_done_function)()) {
  ulint n_hash_cells = recv_sys->n_addrs;
  ulint i = 0;

  recv_sys->apply_log_recs = true;
  recv_sys->apply_batch_on = true;

  ib::info(ER_IB_MSG_714) << "Starting to apply a batch of log records to the"
                          << " database...";

  fputs("InnoDB: Progress in percent: ", stderr);

  for (const auto &space : *recv_sys->spaces) {
    for (auto pages : space.second.m_pages) {
      ut_ad(pages.second->space == space.first);

      (*apply_log_record_function)(pages.second);
    }

    ++i;
    if ((100 * i) / n_hash_cells != (100 * (i + 1)) / n_hash_cells) {
      fprintf(stderr, "%lu ", (ulong)((100 * i) / n_hash_cells));
      fflush(stderr);
    }
  }

  /* wait till all the redo log records have been applied */
  (*wait_till_done_function)();

  /* write logs in next line */
  fprintf(stderr, "\n");
  recv_sys->apply_log_recs = false;
  recv_sys->apply_batch_on = false;
  recv_sys_empty_hash();
}

#endif /* !UNIV_HOTBACKUP */

/** Check if redo log is for encryption information.
@param[in]      page_no         Page number
@param[in]      space_id        Tablespace identifier
@param[in]      start           Redo log record body
@param[in]      end             End of buffer
@return true if encryption information. */
static inline bool check_encryption(page_no_t page_no, space_id_t space_id,
                                    const byte *start, const byte *end) {
  /* Only page zero contains encryption metadata. */
  if (page_no != 0 || fsp_is_system_or_temp_tablespace(space_id) ||
      end < start + 4) {
    return false;
  }

  bool found = false;

  const page_size_t &page_size = fil_space_get_page_size(space_id, &found);

  if (!found) {
    return false;
  }

  auto encryption_offset = fsp_header_get_encryption_offset(page_size);
  auto offset = mach_read_from_2(start);

  /* Encryption offset at page 0 is the only way we can identify encryption
  information as of today. Ideally we should have a separate redo type. */
  if (offset == encryption_offset) {
    auto len = mach_read_from_2(start + 2);
    ut_ad(len == Encryption::INFO_SIZE);

    if (len != Encryption::INFO_SIZE) {
      /* purecov: begin inspected */
      ib::warn(ER_IB_WRN_ENCRYPTION_INFO_SIZE_MISMATCH, size_t{len},
               Encryption::INFO_SIZE);
      return false;
      /* purecov: end */
    }
    return true;
  }

  return false;
}

/** Try to parse a single log record body and also applies it if specified.
    尝试解析单个日志记录主体，如果指定则应用它。
@param[in]      type            Redo log entry type
                                重做日志条目类型
@param[in]      ptr             Redo log record body
                                重做日志记录主体
@param[in]      end_ptr         End of buffer
                                缓冲区结束位置
@param[in]      space_id        Tablespace identifier
                                表空间标识符
@param[in]      page_no         Page number
                                页面编号
@param[in,out]  block           Buffer block, or nullptr if
                                a page log record should not be applied
                                or if it is a MLOG_FILE_ operation
                                缓冲块，如果不应用页面日志记录或是 MLOG_FILE_ 操作，则为 nullptr
@param[in,out]  mtr             Mini-transaction, or nullptr if
                                a page log record should not be applied
                                mini-transaction，如果不应用页面日志记录，则为 nullptr
@param[in]      parsed_bytes    Number of bytes parsed so far
                                已解析的字节数
@param[in]      start_lsn       lsn for REDO record
                                重做记录的 LSN
@return log record end, nullptr if not a complete record
        日志记录结束位置，如果不是完整记录则为 nullptr */
static byte *recv_parse_or_apply_log_rec_body(
    mlog_id_t type, byte *ptr, byte *end_ptr, space_id_t space_id,
    page_no_t page_no, buf_block_t *block, mtr_t *mtr, ulint parsed_bytes,
    lsn_t start_lsn) {
  bool applying_redo = (block != nullptr); // 判断是否正在应用重做日志

  switch (type) { // 根据日志类型进行处理
#ifndef UNIV_HOTBACKUP
    case MLOG_FILE_DELETE: // 删除表空间文件的重做操作

      return fil_tablespace_redo_delete(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_CREATE: // 创建表空间文件的重做操作

      return fil_tablespace_redo_create(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_RENAME: // 重命名表空间文件的重做操作

      return fil_tablespace_redo_rename(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);

    case MLOG_FILE_EXTEND: // 扩展表空间文件的重做操作

      return fil_tablespace_redo_extend(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          recv_sys->bytes_to_ignore_before_checkpoint != 0);
#else  /* !UNIV_HOTBACKUP */
      // Mysqlbackup does not execute file operations. It cares for all
      // files to be at their final places when it applies the redo log.
      // The exception is the restore of an incremental_with_redo_log_only
      // backup.
      // Mysqlbackup 不执行文件操作。它关心所有文件在应用重做日志时都在最终位置。
      // 例外情况是仅恢复增量_with_redo_log_only 备份。
    case MLOG_FILE_DELETE: // 删除表空间文件的重做操作

      return fil_tablespace_redo_delete(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_CREATE: // 创建表空间文件的重做操作

      return fil_tablespace_redo_create(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_RENAME: // 重命名表空间文件的重做操作

      return fil_tablespace_redo_rename(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);

    case MLOG_FILE_EXTEND: // 扩展表空间文件的重做操作

      return fil_tablespace_redo_extend(
          ptr, end_ptr, page_id_t(space_id, page_no), parsed_bytes,
          !recv_sys->apply_file_operations);
#endif /* !UNIV_HOTBACKUP */

    case MLOG_INDEX_LOAD: // 加载索引的重做操作
#ifdef UNIV_HOTBACKUP
      // While scanning redo logs during a backup operation a
      // MLOG_INDEX_LOAD type redo log record indicates, that a DDL
      // (create index, alter table...) is performed with
      // 'algorithm=inplace'. The affected tablespace must be re-copied
      // in the backup lock phase. Record it in the index_load_list.
      // 在备份操作期间扫描重做日志时，MLOG_INDEX_LOAD 类型的重做日志记录表示执行了 DDL（创建索引，修改表...），
      // 使用 'algorithm=inplace'。受影响的表空间必须在备份锁定阶段重新复制。在 index_load_list 中记录它。
      if (!recv_recovery_on) {
        index_load_list.emplace_back(
            std::pair<space_id_t, lsn_t>(space_id, recv_sys->recovered_lsn));
      }
#endif /* UNIV_HOTBACKUP */
      if (end_ptr < ptr + 8) { // 检查缓冲区是否足够大
        return nullptr;
      }

      return ptr + 8; // 返回日志记录结束位置

case MLOG_WRITE_STRING: // 写入字符串的重做操作

#ifdef UNIV_HOTBACKUP
      if (recv_recovery_on && meb_is_space_loaded(space_id)) { // 如果在恢复过程中并且表空间已加载
#endif /* UNIV_HOTBACKUP */
        /* For encrypted tablespace, we need to get the encryption key
        information before the page 0 is recovered. Otherwise, redo will not
        find the key to decrypt the data pages. */
        /* 对于加密的表空间，我们需要在恢复页面 0 之前获取加密密钥信息。
        否则，重做日志将无法找到解密数据页面的密钥。 */
        if (page_no == 0 && !applying_redo &&
            !fsp_is_system_or_temp_tablespace(space_id) &&
            /* For cloned db header page has the encryption information. */
            /* 对于克隆的数据库头页面具有加密信息。 */
            !recv_sys->is_cloned_db) {
          ut_ad(LSN_MAX != start_lsn);
          return fil_tablespace_redo_encryption(ptr, end_ptr, space_id,
                                                start_lsn); // 返回加密信息
        }
#ifdef UNIV_HOTBACKUP
      }
#endif /* UNIV_HOTBACKUP */

      break;

    default:
      break;
  }

  page_t *page; // 页面指针
  page_zip_des_t *page_zip; // 压缩页面描述符
  dict_index_t *index = nullptr; // 索引指针

#ifdef UNIV_DEBUG
  ulint page_type; // 页面类型
#endif /* UNIV_DEBUG */

#if defined(UNIV_HOTBACKUP) && defined(UNIV_DEBUG)
  ib::trace_3() << "recv_parse_or_apply_log_rec_body: type "
                << get_mlog_string(type) << " space_id " << space_id
                << " page_nr " << page_no << " ptr "
                << static_cast<const void *>(ptr) << " end_ptr "
                << static_cast<const void *>(end_ptr) << " block "
                << static_cast<const void *>(block) << " mtr "
                << static_cast<const void *>(mtr);
#endif /* UNIV_HOTBACKUP && UNIV_DEBUG */

  if (applying_redo) { // 如果正在应用重做日志
    /* Applying a page log record. */
    /* 应用页面日志记录。 */
    ut_ad(mtr != nullptr);

    page = block->frame; // 获取页面帧
    page_zip = buf_block_get_page_zip(block); // 获取压缩页面

    ut_d(page_type = fil_page_get_type(page)); // 获取页面类型
#if defined(UNIV_HOTBACKUP) && defined(UNIV_DEBUG)
    if (page_type == 0) {
      meb_print_page_header(page); // 打印页面头信息
    }
#endif /* UNIV_HOTBACKUP && UNIV_DEBUG */

  } else {
    /* Parsing a page log record. */
    /* 解析页面日志记录。 */
    ut_ad(mtr == nullptr);
    page = nullptr;
    page_zip = nullptr;

    ut_d(page_type = FIL_PAGE_TYPE_ALLOCATED); // 设置页面类型为已分配
  }

  const byte *old_ptr = ptr; // 保存旧指针

  switch (type) { // 根据日志类型进行处理
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
      /* The LSN is checked in recv_parse_log_rec(). */
      /* LSN 在 recv_parse_log_rec() 中检查。 */
      break;
#endif /* UNIV_LOG_LSN_DEBUG */
    case MLOG_4BYTES:

      ut_ad(page == nullptr || end_ptr > ptr + 2);

      /* Most FSP flags can only be changed by CREATE or ALTER with
      ALGORITHM=COPY, so they do not change once the file
      is created. The SDI flag is the only one that can be
      changed by a recoverable transaction. So if there is
      change in FSP flags, update the in-memory space structure
      (fil_space_t) */
      /* 大多数 FSP 标志只能通过 CREATE 或 ALTER 使用 ALGORITHM=COPY 更改，
      因此一旦文件创建，它们就不会更改。SDI 标志是唯一可以通过可恢复事务更改的标志。
      因此，如果 FSP 标志发生变化，请更新内存中的空间结构 (fil_space_t) */

      if (page != nullptr && page_no == 0 &&
          mach_read_from_2(ptr) == FSP_HEADER_OFFSET + FSP_SPACE_FLAGS) {
        ptr = mlog_parse_nbytes(MLOG_4BYTES, ptr, end_ptr, page, page_zip); // 解析 4 字节日志记录

        /* When applying log, we have complete records.
        They can be incomplete (ptr=nullptr) only during
        scanning (page==nullptr) */
        /* 在应用日志时，我们有完整的记录。
        它们只能在扫描期间（page==nullptr）不完整（ptr=nullptr） */

        ut_ad(ptr != nullptr);

        fil_space_t *space = fil_space_acquire(space_id); // 获取表空间

        ut_ad(space != nullptr);

        fil_space_set_flags(space, mach_read_from_4(FSP_HEADER_OFFSET +
                                                    FSP_SPACE_FLAGS + page)); // 设置表空间标志
        fil_space_release(space); // 释放表空间

        break;
      }

      [[fallthrough]]; // 继续执行下一个 case

case MLOG_1BYTE:
      /* If 'ALTER TABLESPACE ... ENCRYPTION' was in progress and page 0 has
      REDO entry for this, now while applying this entry, set
      encryption_op_in_progress flag now so that any other page of this
      tablespace in redo log is written accordingly. */
      /* 如果 'ALTER TABLESPACE ... ENCRYPTION' 正在进行，并且页面 0 有重做条目，
      那么现在在应用此条目时，设置 encryption_op_in_progress 标志，
      以便在重做日志中相应地写入此表空间的任何其他页面。 */
      if (page_no == 0 && page != nullptr && end_ptr >= ptr + 2) { // 如果页面编号为 0 并且页面不为空且缓冲区足够大
        ulint offs = mach_read_from_2(ptr); // 从指针读取 2 字节偏移量

        fil_space_t *space = fil_space_acquire(space_id); // 获取表空间
        ut_ad(space != nullptr);
        ulint offset = fsp_header_get_encryption_progress_offset(
            page_size_t(space->flags)); // 获取加密进度偏移量

        if (offs == offset) { // 如果偏移量匹配
          ptr = mlog_parse_nbytes(MLOG_1BYTE, ptr, end_ptr, page, page_zip); // 解析 1 字节日志记录
          byte op = mach_read_from_1(page + offset); // 从页面读取 1 字节操作
          switch (op) { // 根据操作类型设置加密进度
            case Encryption::ENCRYPT_IN_PROGRESS:
              space->encryption_op_in_progress =
                  Encryption::Progress::ENCRYPTION;
              break;
            case Encryption::DECRYPT_IN_PROGRESS:
              space->encryption_op_in_progress =
                  Encryption::Progress::DECRYPTION;
              break;
            default:
              space->encryption_op_in_progress = Encryption::Progress::NONE;
              break;
          }
        }
        fil_space_release(space); // 释放表空间
      }

      [[fallthrough]]; // 继续执行下一个 case

    case MLOG_2BYTES:
    case MLOG_8BYTES:
#ifdef UNIV_DEBUG
      if (page && page_type == FIL_PAGE_TYPE_ALLOCATED && end_ptr >= ptr + 2) {
        /* It is OK to set FIL_PAGE_TYPE and certain
        list node fields on an empty page.  Any other
        write is not OK. */
        /* 可以在空页面上设置 FIL_PAGE_TYPE 和某些列表节点字段。任何其他写入都是不可以的。 */

        /* NOTE: There may be bogus assertion failures for
        dict_hdr_create(), trx_rseg_header_create(),
        trx_sys_create_doublewrite_buf(), and
        trx_sysf_create().
        These are only called during database creation. */
        /* 注意：对于 dict_hdr_create()、trx_rseg_header_create()、
        trx_sys_create_doublewrite_buf() 和 trx_sysf_create() 可能会有虚假的断言失败。
        这些仅在数据库创建期间调用。 */

        ulint offs = mach_read_from_2(ptr); // 从指针读取 2 字节偏移量

        switch (type) { // 根据日志类型进行处理
          default:
            ut_error;
          case MLOG_2BYTES:
            /* Note that this can fail when the
            redo log been written with something
            older than InnoDB Plugin 1.0.4. */
            /* 请注意，当重做日志是用比 InnoDB 插件 1.0.4 更旧的版本写入时，这可能会失败。 */
            ut_ad(
                offs == FIL_PAGE_TYPE ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_BYTE ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_BYTE +
                            FIL_ADDR_SIZE ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_OFFSET ||
                offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                            FIL_ADDR_BYTE + 0 /*FLST_PREV*/
                || offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                               FIL_ADDR_BYTE + FIL_ADDR_SIZE /*FLST_NEXT*/);
            break;
          case MLOG_4BYTES:
            /* Note that this can fail when the
            redo log been written with something
            older than InnoDB Plugin 1.0.4. */
            /* 请注意，当重做日志是用比 InnoDB 插件 1.0.4 更旧的版本写入时，这可能会失败。 */
            ut_ad(
                0 ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_SPACE ||
                offs == IBUF_TREE_SEG_HEADER + IBUF_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER /* flst_init */
                ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_PAGE ||
                offs == PAGE_BTR_IBUF_FREE_LIST + PAGE_HEADER + FIL_ADDR_PAGE +
                            FIL_ADDR_SIZE ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_SEG_LEAF + PAGE_HEADER + FSEG_HDR_SPACE ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_PAGE_NO ||
                offs == PAGE_BTR_SEG_TOP + PAGE_HEADER + FSEG_HDR_SPACE ||
                offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                            FIL_ADDR_PAGE + 0 /*FLST_PREV*/
                || offs == PAGE_BTR_IBUF_FREE_LIST_NODE + PAGE_HEADER +
                               FIL_ADDR_PAGE + FIL_ADDR_SIZE /*FLST_NEXT*/);
            break;
        }
      }
#endif /* UNIV_DEBUG */

      ptr = mlog_parse_nbytes(type, ptr, end_ptr, page, page_zip); // 解析指定字节数的日志记录

      if (ptr != nullptr && page != nullptr && page_no == 0 &&
          type == MLOG_4BYTES) { // 如果指针不为空且页面不为空且页面编号为 0 且日志类型为 MLOG_4BYTES
        ulint offs = mach_read_from_2(old_ptr); // 从旧指针读取 2 字节偏移量

        switch (offs) { // 根据偏移量进行处理
          fil_space_t *space;
          uint32_t val;
          default:
            break;

          case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
          case FSP_HEADER_OFFSET + FSP_SIZE:
          case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
          case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:

            space = fil_space_get(space_id); // 获取表空间

            ut_a(space != nullptr);

            val = mach_read_from_4(page + offs); // 从页面读取 4 字节值

            switch (offs) { // 根据偏移量设置表空间属性
              case FSP_HEADER_OFFSET + FSP_SPACE_FLAGS:
                space->flags = val;
                break;

              case FSP_HEADER_OFFSET + FSP_SIZE:

                space->size_in_header = val;

                if (space->size >= val) {
                  break;
                }

                ib::info(ER_IB_MSG_718, ulong{space->id}, space->name,
                         ulong{val});

                if (fil_space_extend(space, val)) {
                  break;
                }

                ib::error(ER_IB_MSG_719, ulong{space->id}, space->name,
                          ulong{val});
                break;

              case FSP_HEADER_OFFSET + FSP_FREE_LIMIT:
                space->free_limit = val;
                break;

              case FSP_HEADER_OFFSET + FSP_FREE + FLST_LEN:
                space->free_len = val;
                ut_ad(val == flst_get_len(page + offs));
                break;
            }
        }
      }
      break;

    case MLOG_REC_INSERT:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_cur_parse_insert_rec(false, ptr, end_ptr, block, index, mtr); // 解析并插入记录
      }
      break;

    case MLOG_REC_INSERT_8027:
    case MLOG_COMP_REC_INSERT_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_INSERT_8027, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_cur_parse_insert_rec(false, ptr, end_ptr, block, index, mtr); // 解析并插入记录
      }
      break;

    case MLOG_REC_CLUST_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = btr_cur_parse_del_mark_set_clust_rec(ptr, end_ptr, page, page_zip,
                                                   index); // 解析并设置聚簇记录删除标记
      }

      break;

case MLOG_REC_CLUST_DELETE_MARK_8027:
    case MLOG_COMP_REC_CLUST_DELETE_MARK_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_CLUST_DELETE_MARK_8027,
               &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = btr_cur_parse_del_mark_set_clust_rec(ptr, end_ptr, page, page_zip,
                                                   index); // 解析并设置聚簇记录删除标记
      }

      break;

    case MLOG_COMP_REC_SEC_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      /* This log record type is obsolete, but we process it for
      backward compatibility with MySQL 5.0.3 and 5.0.4. */
      /* 这种日志记录类型已过时，但我们处理它是为了与 MySQL 5.0.3 和 5.0.4 向后兼容。 */

      ut_a(!page || page_is_comp(page)); // 断言页面为空或页面为压缩页面
      ut_a(!page_zip); // 断言页面压缩描述符为空

      ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index); // 解析索引日志记录

      if (ptr == nullptr) { // 如果指针为空
        break;
      }

      [[fallthrough]]; // 继续执行下一个 case

    case MLOG_REC_SEC_DELETE_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      ptr = btr_cur_parse_del_mark_set_sec_rec(ptr, end_ptr, page, page_zip); // 解析并设置次级记录删除标记
      break;

    case MLOG_REC_UPDATE_IN_PLACE:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr =
            btr_cur_parse_update_in_place(ptr, end_ptr, page, page_zip, index); // 解析并更新记录
      }

      break;

    case MLOG_REC_UPDATE_IN_PLACE_8027:
    case MLOG_COMP_REC_UPDATE_IN_PLACE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_UPDATE_IN_PLACE_8027,
               &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr =
            btr_cur_parse_update_in_place(ptr, end_ptr, page, page_zip, index); // 解析并更新记录
      }

      break;

    case MLOG_LIST_END_DELETE:
    case MLOG_LIST_START_DELETE:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_parse_delete_rec_list(type, ptr, end_ptr, block, index, mtr); // 解析并删除记录列表
      }

      break;

    case MLOG_LIST_END_DELETE_8027:
    case MLOG_COMP_LIST_END_DELETE_8027:
    case MLOG_LIST_START_DELETE_8027:
    case MLOG_COMP_LIST_START_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index_8027(
                          ptr, end_ptr,
                          type == MLOG_COMP_LIST_END_DELETE_8027 ||
                              type == MLOG_COMP_LIST_START_DELETE_8027,
                          &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_parse_delete_rec_list(type, ptr, end_ptr, block, index, mtr); // 解析并删除记录列表
      }

      break;

    case MLOG_LIST_END_COPY_CREATED:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_parse_copy_rec_list_to_created_page(ptr, end_ptr, block,
                                                       index, mtr); // 解析并复制记录列表到新创建的页面
      }

      break;

    case MLOG_LIST_END_COPY_CREATED_8027:
    case MLOG_COMP_LIST_END_COPY_CREATED_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_LIST_END_COPY_CREATED_8027,
               &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_parse_copy_rec_list_to_created_page(ptr, end_ptr, block,
                                                       index, mtr); // 解析并复制记录列表到新创建的页面
      }

      break;

    case MLOG_PAGE_REORGANIZE:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index,
                                        type == MLOG_ZIP_PAGE_REORGANIZE_8027,
                                        block, mtr); // 解析并重新组织页面
      }

      break;

    case MLOG_PAGE_REORGANIZE_8027:
      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引
      /* Uncompressed pages don't have any payload in the
      MTR so ptr and end_ptr can be, and are nullptr */
      /* 未压缩的页面在 MTR 中没有任何有效负载，因此 ptr 和 end_ptr 可以为空，并且确实为空 */
      mlog_parse_index_8027(ptr, end_ptr, false, &index); // 解析索引日志记录
      ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

      ptr = btr_parse_page_reorganize(ptr, end_ptr, index, false, block, mtr); // 解析并重新组织页面

      break;

    case MLOG_ZIP_PAGE_REORGANIZE:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index, true, block, mtr); // 解析并重新组织页面
      }

      break;

case MLOG_COMP_PAGE_REORGANIZE_8027:
    case MLOG_ZIP_PAGE_REORGANIZE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = btr_parse_page_reorganize(ptr, end_ptr, index,
                                        type == MLOG_ZIP_PAGE_REORGANIZE_8027,
                                        block, mtr); // 解析并重新组织页面
      }

      break;

    case MLOG_PAGE_CREATE:
    case MLOG_COMP_PAGE_CREATE:

      /* Allow anything in page_type when creating a page. */
      /* 创建页面时允许任何页面类型。 */
      ut_a(!page_zip); // 断言页面压缩描述符为空

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE, FIL_PAGE_INDEX); // 解析并创建页面

      break;

    case MLOG_PAGE_CREATE_RTREE:
    case MLOG_COMP_PAGE_CREATE_RTREE:

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_RTREE,
                        FIL_PAGE_RTREE); // 解析并创建 RTREE 页面

      break;

    case MLOG_PAGE_CREATE_SDI:
    case MLOG_COMP_PAGE_CREATE_SDI:

      page_parse_create(block, type == MLOG_COMP_PAGE_CREATE_SDI, FIL_PAGE_SDI); // 解析并创建 SDI 页面

      break;

    case MLOG_UNDO_INSERT:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG); // 断言页面为空或页面类型为 UNDO 日志

      ptr = trx_undo_parse_add_undo_rec(ptr, end_ptr, page); // 解析并添加 UNDO 记录

      break;

    case MLOG_UNDO_ERASE_END:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG); // 断言页面为空或页面类型为 UNDO 日志

      ptr = trx_undo_parse_erase_page_end(ptr, end_ptr, page, mtr); // 解析并擦除页面末尾

      break;

    case MLOG_UNDO_INIT:

      /* Allow anything in page_type when creating a page. */
      /* 创建页面时允许任何页面类型。 */

      ptr = trx_undo_parse_page_init(ptr, end_ptr, page, mtr); // 解析并初始化页面

      break;
    case MLOG_UNDO_HDR_CREATE:
    case MLOG_UNDO_HDR_REUSE:

      ut_ad(!page || page_type == FIL_PAGE_UNDO_LOG); // 断言页面为空或页面类型为 UNDO 日志

      ptr = trx_undo_parse_page_header(type, ptr, end_ptr, page, mtr); // 解析并处理页面头

      break;

    case MLOG_REC_MIN_MARK:
    case MLOG_COMP_REC_MIN_MARK:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      /* On a compressed page, MLOG_COMP_REC_MIN_MARK
      will be followed by MLOG_COMP_REC_DELETE
      or MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_nullptr)
      in the same mini-transaction. */
      /* 在压缩页面上，MLOG_COMP_REC_MIN_MARK 后面会跟着 MLOG_COMP_REC_DELETE 或 MLOG_ZIP_WRITE_HEADER(FIL_PAGE_PREV, FIL_nullptr) 在同一个 mini-transaction 中。 */

      ut_a(type == MLOG_COMP_REC_MIN_MARK || !page_zip); // 断言类型为 MLOG_COMP_REC_MIN_MARK 或页面压缩描述符为空

      ptr = btr_parse_set_min_rec_mark(
          ptr, end_ptr, type == MLOG_COMP_REC_MIN_MARK, page, mtr); // 解析并设置最小记录标记

      break;

    case MLOG_REC_DELETE:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_cur_parse_delete_rec(ptr, end_ptr, block, index, mtr); // 解析并删除记录
      }

      break;

    case MLOG_REC_DELETE_8027:
    case MLOG_COMP_REC_DELETE_8027:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      if (nullptr !=
          (ptr = mlog_parse_index_8027(
               ptr, end_ptr, type == MLOG_COMP_REC_DELETE_8027, &index))) { // 解析索引日志记录
        ut_a(!page || page_is_comp(page) == dict_table_is_comp(index->table)); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_cur_parse_delete_rec(ptr, end_ptr, block, index, mtr); // 解析并删除记录
      }

      break;

    case MLOG_IBUF_BITMAP_INIT:

      /* Allow anything in page_type when creating a page. */
      /* 创建页面时允许任何页面类型。 */

      ptr = ibuf_parse_bitmap_init(ptr, end_ptr, block, mtr); // 解析并初始化位图

      break;

    case MLOG_INIT_FILE_PAGE:
    case MLOG_INIT_FILE_PAGE2: {
      /* For clone, avoid initializing page-0. Page-0 should already have been
      initialized. This is to avoid erasing encryption information. We cannot
      update encryption information later with redo logged information for
      clone. Please check comments in MLOG_WRITE_STRING. */
      /* 对于克隆，避免初始化页面 0。页面 0 应该已经初始化。这是为了避免擦除加密信息。我们不能在以后使用重做日志信息更新加密信息。请查看 MLOG_WRITE_STRING 中的注释。 */
      bool skip_init = (recv_sys->is_cloned_db && page_no == 0); // 判断是否跳过初始化

      if (!skip_init) {
        /* Allow anything in page_type when creating a page. */
        /* 创建页面时允许任何页面类型。 */
        ptr = fsp_parse_init_file_page(ptr, end_ptr, block); // 解析并初始化文件页面
      }
      break;
    }

    case MLOG_WRITE_STRING: {
      ut_ad(!page || page_type != FIL_PAGE_TYPE_ALLOCATED || page_no == 0); // 断言页面为空或页面类型不为已分配或页面编号为 0
      bool is_encryption = check_encryption(page_no, space_id, ptr, end_ptr); // 检查是否为加密

#ifndef UNIV_HOTBACKUP
      /* Reset in-mem encryption information for the tablespace here if this
      is "resetting encryprion info" log. */
      /* 如果这是“重置加密信息”日志，则在此处重置表空间的内存加密信息。 */
      if (is_encryption && !recv_sys->is_cloned_db) {
        byte buf[Encryption::INFO_SIZE] = {0};

        if (memcmp(ptr + 4, buf, Encryption::INFO_SIZE - 4) == 0) {
          ut_a(DB_SUCCESS == fil_reset_encryption(space_id)); // 断言重置加密成功
        }
      }

#endif
      auto apply_page = page; // 应用页面

      /* For clone recovery, skip applying encryption information from
      redo log. It is already updated in page 0. Redo log encryption
      information is encrypted with donor master key and must be ignored. */
      /* 对于克隆恢复，跳过应用重做日志中的加密信息。它已经在页面 0 中更新。重做日志加密信息使用捐赠者主密钥加密，必须忽略。 */
      if (recv_sys->is_cloned_db && is_encryption) {
        apply_page = nullptr;
      }

      ptr = mlog_parse_string(ptr, end_ptr, apply_page, page_zip); // 解析字符串日志记录
      break;
    }

    case MLOG_ZIP_WRITE_NODE_PTR:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      ptr = page_zip_parse_write_node_ptr(ptr, end_ptr, page, page_zip); // 解析并写入节点指针

      break;

    case MLOG_ZIP_WRITE_BLOB_PTR:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      ptr = page_zip_parse_write_blob_ptr(ptr, end_ptr, page, page_zip); // 解析并写入 BLOB 指针

      break;

    case MLOG_ZIP_WRITE_HEADER:

      ut_ad(!page || fil_page_type_is_index(page_type)); // 断言页面为空或页面类型为索引

      ptr = page_zip_parse_write_header(ptr, end_ptr, page, page_zip); // 解析并写入头部

      break;

    case MLOG_ZIP_PAGE_COMPRESS:

      /* Allow anything in page_type when creating a page. */
      /* 创建页面时允许任何页面类型。 */
      ptr = page_zip_parse_compress(ptr, end_ptr, page, page_zip); // 解析并压缩页面
      break;

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:

      if (nullptr != (ptr = mlog_parse_index(ptr, end_ptr, &index))) { // 解析索引日志记录
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table))); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_zip_parse_compress_no_data(ptr, end_ptr, page, page_zip,
                                              index); // 解析并压缩页面（无数据）
      }

      break;

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027:

      if (nullptr !=
          (ptr = mlog_parse_index_8027(ptr, end_ptr, true, &index))) { // 解析索引日志记录
        ut_a(!page || (page_is_comp(page) == dict_table_is_comp(index->table))); // 断言页面为空或页面压缩状态与索引表一致

        ptr = page_zip_parse_compress_no_data(ptr, end_ptr, page, page_zip,
                                              index); // 解析并压缩页面（无数据）
      }

      break;

    case MLOG_TEST:
#ifndef UNIV_HOTBACKUP
      if (log_test != nullptr) {
        ptr = log_test->parse_mlog_rec(ptr, end_ptr); // 解析测试日志记录
      } else {
        /* Just parse and ignore record to pass it and go forward. Note that
        this record is also used in the innodb.log_first_rec_group mtr test.
        The record is written in the buf0flu.cc when flushing page in that
        case. */
        /* 仅解析并忽略记录以通过并继续。请注意，此记录也用于 innodb.log_first_rec_group mtr 测试。在这种情况下，记录在刷新页面时写入 buf0flu.cc。 */
        Log_test::Key key;
        Log_test::Value value;
        lsn_t start_lsn, end_lsn;

        ptr = Log_test::parse_mlog_rec(ptr, end_ptr, key, value, start_lsn,
                                       end_lsn); // 解析测试日志记录
      }
      break;
#endif /* !UNIV_HOTBACKUP */
      /* Fall through. */
      /* 继续执行。 */

    default:
      ptr = nullptr; // 设置指针为空
      recv_sys->found_corrupt_log = true; // 设置发现损坏日志标志
  }

  if (index != nullptr) { // 如果索引不为空
    dict_table_t *table = index->table; // 获取索引表

    dict_mem_index_free(index); // 释放索引内存
    dict_mem_table_free(table); // 释放表内存
  }

  return ptr; // 返回指针
}

/** Adds a new log record to the hash table of log records.
@param[in]      type            log record type
@param[in]      space_id        Tablespace id
@param[in]      page_no         page number
@param[in]      body            log record body
@param[in]      rec_end         log record end
@param[in]      start_lsn       start lsn of the mtr
@param[in]      end_lsn         end lsn of the mtr */
/** 将新的日志记录添加到日志记录的哈希表中。
@param[in]      type            日志记录类型
@param[in]      space_id        表空间 ID
@param[in]      page_no         页面编号
@param[in]      body            日志记录主体
@param[in]      rec_end         日志记录结束位置
@param[in]      start_lsn       mtr 的起始 LSN
@param[in]      end_lsn         mtr 的结束 LSN */
static void recv_add_to_hash_table(mlog_id_t type, space_id_t space_id,
                                   page_no_t page_no, byte *body, byte *rec_end,
                                   lsn_t start_lsn, lsn_t end_lsn) {
  ut_ad(type != MLOG_FILE_DELETE);
  ut_ad(type != MLOG_FILE_CREATE);
  ut_ad(type != MLOG_FILE_RENAME);
  ut_ad(type != MLOG_FILE_EXTEND);
  ut_ad(type != MLOG_DUMMY_RECORD);
  ut_ad(type != MLOG_INDEX_LOAD);

  recv_sys_t::Space *space;

  space = recv_get_page_map(space_id, true); // 获取表空间的页面映射

  recv_t *recv;

  recv = static_cast<recv_t *>(mem_heap_alloc(space->m_heap, sizeof(*recv))); // 分配内存

  recv->type = type;
  recv->end_lsn = end_lsn;
  recv->len = rec_end - body;
  recv->start_lsn = start_lsn;

  auto it = space->m_pages.find(page_no); // 查找页面编号

  recv_addr_t *recv_addr;

  if (it != space->m_pages.end()) {
    recv_addr = it->second; // 如果找到页面编号，获取对应的 recv_addr

  } else {
    recv_addr = static_cast<recv_addr_t *>(
        mem_heap_alloc(space->m_heap, sizeof(*recv_addr))); // 分配内存

    recv_addr->space = space_id;
    recv_addr->page_no = page_no;
    recv_addr->state = RECV_NOT_PROCESSED;

    UT_LIST_INIT(recv_addr->rec_list); // 初始化记录列表

    using Value = recv_sys_t::Pages::value_type;

    space->m_pages.insert(it, Value{page_no, recv_addr}); // 插入新的页面编号和 recv_addr

    ++recv_sys->n_addrs; // 增加地址计数
  }

  UT_LIST_ADD_LAST(recv_addr->rec_list, recv); // 将新的日志记录添加到记录列表的末尾

  recv_data_t **prev_field;

  prev_field = &recv->data;

  /* Store the log record body in chunks of less than UNIV_PAGE_SIZE:
  the heap grows into the buffer pool, and bigger chunks could not
  be allocated */
  /* 将日志记录主体存储为小于 UNIV_PAGE_SIZE 的块：
  堆增长到缓冲池中，无法分配更大的块 */

  while (rec_end > body) {
    ulint len = rec_end - body;

    if (len > RECV_DATA_BLOCK_SIZE) {
      len = RECV_DATA_BLOCK_SIZE;
    }

    recv_data_t *recv_data;

    recv_data = static_cast<recv_data_t *>(
        mem_heap_alloc(space->m_heap, sizeof(*recv_data) + len)); // 分配内存

    *prev_field = recv_data;

    memcpy(recv_data + 1, body, len); // 复制日志记录主体数据

    prev_field = &recv_data->next;

    body += len;
  }

  *prev_field = nullptr; // 设置最后一个字段为 nullptr
}

/** Copies the log record body from recv to buf.
@param[in]      buf             Buffer of length at least recv->len
@param[in]      recv            Log record */
static void recv_data_copy_to_buf(byte *buf, recv_t *recv) {
  ulint len = recv->len;
  recv_data_t *recv_data = recv->data;

  while (len > 0) {
    ulint part_len;

    if (len > RECV_DATA_BLOCK_SIZE) {
      part_len = RECV_DATA_BLOCK_SIZE;
    } else {
      part_len = len;
    }

    memcpy(buf, ((byte *)recv_data) + sizeof(*recv_data), part_len);

    buf += part_len;
    len -= part_len;

    recv_data = recv_data->next;
  }
}

bool recv_page_is_brand_new(buf_block_t *block) {
  mutex_enter(&recv_sys->mutex);

  recv_addr_t *recv_addr;
  recv_addr = recv_get_rec(block->page.id.space(), block->page.id.page_no());
  if (recv_addr == nullptr) {
    /* no redo log treated as brand new */
    mutex_exit(&recv_sys->mutex);
    return true;
  }

  auto recv = UT_LIST_GET_FIRST(recv_addr->rec_list);
  if (recv == nullptr) {
    /* no redo log treated as brand new */
    mutex_exit(&recv_sys->mutex);
    return true;
  }
  if (recv->type == MLOG_INIT_FILE_PAGE2 || recv->type == MLOG_INIT_FILE_PAGE) {
    mutex_exit(&recv_sys->mutex);
    return true;
  }

  mutex_exit(&recv_sys->mutex);
  return false;
}

/** Applies the hashed log records to the page, if the page lsn is less than
the lsn of a log record. This can be called when a buffer page has just been
read in, or also for a page already in the buffer pool.

@param[in]      just_read_in    true if the IO handler calls this for a freshly
                                read page
@param[in,out]  block           buffer block */
/** 如果页面的 LSN 小于日志记录的 LSN，则将哈希日志记录应用到页面。
这可以在缓冲页面刚刚读入时调用，也可以在缓冲池中已有页面时调用。

@param[in]      just_read_in    如果 I/O 处理程序为新读取的页面调用此函数，则为 true
@param[in,out]  block           缓冲块 */
void recv_recover_page_func(
#ifndef UNIV_HOTBACKUP
    bool just_read_in,
#endif /* !UNIV_HOTBACKUP */
    buf_block_t *block) {
  mutex_enter(&recv_sys->mutex); // 进入互斥锁

  if (recv_sys->apply_log_recs == false) {
    /* Log records should not be applied now */
    /* 现在不应该应用日志记录 */

    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    return;
  }

  recv_addr_t *recv_addr;

  recv_addr = recv_get_rec(block->page.id.space(), block->page.id.page_no()); // 获取日志记录地址

  if (recv_addr == nullptr || recv_addr->state == RECV_BEING_PROCESSED ||
      recv_addr->state == RECV_PROCESSED) {
#ifndef UNIV_HOTBACKUP
    ut_ad(recv_addr == nullptr || recv_needed_recovery ||
          recv_sys->scanned_lsn < recv_sys->checkpoint_lsn);
#endif /* !UNIV_HOTBACKUP */

    mutex_exit(&recv_sys->mutex); // 退出互斥锁

    return;
  }

#ifndef UNIV_HOTBACKUP
  /* The following block is the scope of usage of the following bpage object
  reference.*/
  /* 以下块是使用以下 bpage 对象引用的范围。 */
  {
    buf_page_t &bpage = block->page;

    if (!fsp_is_system_temporary(bpage.id.space()) &&
        (arch_page_sys != nullptr && arch_page_sys->is_active())) {
      page_t *frame;
      lsn_t frame_lsn;

      frame = bpage.zip.data;

      if (!frame) {
        frame = block->frame;
      }
      frame_lsn = mach_read_from_8(frame + FIL_PAGE_LSN);

      arch_page_sys->track_page(&bpage, LSN_MAX, frame_lsn, true);
    }
  }
#endif /* !UNIV_HOTBACKUP */

#ifndef UNIV_HOTBACKUP
  /* this is explicitly false in case of meb, skip the assert */
  ut_ad(recv_needed_recovery ||
        recv_sys->scanned_lsn < recv_sys->checkpoint_lsn);

  DBUG_PRINT("ib_log", ("Applying log to page %u:%u", recv_addr->space,
                        recv_addr->page_no));

#ifdef UNIV_DEBUG
  lsn_t max_lsn;

  ut_d(max_lsn = log_sys->m_scanned_lsn);
#endif /* UNIV_DEBUG */
#else  /* !UNIV_HOTBACKUP */
  ib::trace_2() << "Applying log to space_id " << recv_addr->space
                << " page_nr " << recv_addr->page_no;
#endif /* !UNIV_HOTBACKUP */

  recv_addr->state = RECV_BEING_PROCESSED; // 设置状态为正在处理

  mutex_exit(&recv_sys->mutex); // 退出互斥锁

  mtr_t mtr;

  mtr_start(&mtr); // 开始 mini-transaction

  mtr_set_log_mode(&mtr, MTR_LOG_NONE); // 设置日志模式为不记录

  page_t *page = block->frame;

  page_zip_des_t *page_zip = buf_block_get_page_zip(block);

#ifndef UNIV_HOTBACKUP
  if (just_read_in) {
    /* Move the ownership of the x-latch on the page to
    this OS thread, so that we can acquire a second
    x-latch on it.  This is needed for the operations to
    the page to pass the debug checks. */
    /* 将页面上的 x 锁的所有权移动到此 OS 线程，以便我们可以获取第二个 x 锁。
    这对于通过调试检查的页面操作是必需的。 */

    rw_lock_x_lock_move_ownership(&block->lock);
  }

  bool success = buf_page_get_known_nowait(
      RW_X_LATCH, block, Cache_hint::KEEP_OLD, __FILE__, __LINE__, &mtr); // 获取页面块
  ut_a(success);

  buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
#endif /* !UNIV_HOTBACKUP */

  /* Read the newest modification lsn from the page */
  /* 从页面读取最新的修改 lsn */
  lsn_t page_lsn = mach_read_from_8(page + FIL_PAGE_LSN);

#ifndef UNIV_HOTBACKUP

  /* It may be that the page has been modified in the buffer
  pool: read the newest modification LSN there */
  /* 可能页面在缓冲池中已被修改：读取最新的修改 LSN */

  lsn_t page_newest_lsn;

  page_newest_lsn = buf_page_get_newest_modification(&block->page);

  if (page_newest_lsn) {
    page_lsn = page_newest_lsn;
  }
#else  /* !UNIV_HOTBACKUP */
  /* In recovery from a backup we do not really use the buffer pool */
  /* 在从备份恢复时，我们实际上不使用缓冲池 */
  lsn_t page_newest_lsn = 0;
  /* Count applied and skipped log records */
  /* 计算已应用和跳过的日志记录 */
  size_t applied_recs = 0;
  size_t skipped_recs = 0;
#endif /* !UNIV_HOTBACKUP */

#ifndef UNIV_HOTBACKUP
  lsn_t end_lsn = 0;
#endif /* !UNIV_HOTBACKUP */
  lsn_t start_lsn = 0;
  bool modification_to_page = false;

  for (auto recv : recv_addr->rec_list) {
#ifndef UNIV_HOTBACKUP
    end_lsn = recv->end_lsn;

    ut_ad(end_lsn <= max_lsn);
#endif /* !UNIV_HOTBACKUP */

    byte *buf = nullptr;

    if (recv->len > RECV_DATA_BLOCK_SIZE) {
      /* We have to copy the record body to a separate
      buffer */
      /* 我们必须将记录主体复制到单独的缓冲区 */

      buf = static_cast<byte *>(
          ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, recv->len));

      recv_data_copy_to_buf(buf, recv);
    } else if (recv->data != nullptr) {
      buf = ((byte *)(recv->data)) + sizeof(recv_data_t);
    } else {
      /* Redo record that does not have a payload, such as
       MLOG_UNDO_ERASE_END, MLOG_COMP_PAGE_CREATE, MLOG_INIT_FILE_PAGE2 etc.
     */
      /* 没有有效负载的重做记录，例如 MLOG_UNDO_ERASE_END、MLOG_COMP_PAGE_CREATE、MLOG_INIT_FILE_PAGE2 等。 */
      ut_ad(recv->data == nullptr);
      ut_ad(recv->len == 0);
    }

    if (recv->type == MLOG_INIT_FILE_PAGE) {
      page_lsn = page_newest_lsn;

      memset(FIL_PAGE_LSN + page, 0, 8);
      memset(UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + page, 0, 8);

      if (page_zip) {
        memset(FIL_PAGE_LSN + page_zip->data, 0, 8);
      }
    }

    /* Ignore applying the redo logs for tablespace that is
    truncated. Truncated tablespaces are handled explicitly
    post-recovery, where we will restore the tablespace back
    to a normal state.
    
    Applying redo at this stage will cause problems because the
    redo will have action recorded on page before tablespace
    was re-inited and that would lead to a problem later. */
    /* 忽略对截断的表空间应用重做日志。截断的表空间在恢复后会被显式处理，
    我们将在恢复后将表空间恢复到正常状态。
    在此阶段应用重做日志会导致问题，因为重做日志会在表空间重新初始化之前记录页面上的操作，这会导致以后出现问题。 */

    if (recv->start_lsn >= page_lsn
#ifndef UNIV_HOTBACKUP
        && undo::is_active(recv_addr->space)
#endif /* !UNIV_HOTBACKUP */
    ) {

      lsn_t end_lsn;

      if (!modification_to_page) {
#ifndef UNIV_HOTBACKUP
        ut_a(recv_needed_recovery);
#endif /* !UNIV_HOTBACKUP */
        modification_to_page = true;
        start_lsn = recv->start_lsn;
      }

      DBUG_PRINT("ib_log", ("apply " LSN_PF ":"
                            " %s len " ULINTPF " page %u:%u",
                            recv->start_lsn, get_mlog_string(recv->type),
                            recv->len, recv_addr->space, recv_addr->page_no));
      /* Since buf can be a nullptr for record types without a payload we can
      end up with nullptr + 0 if we calc buf + recv->len. This is undefined
      behaviour. Avoid this by only calculating the end_ptr when there's
      actual data to work with, otherwise set it to nullptr. */
      /* 由于 buf 对于没有有效负载的记录类型可以是 nullptr，如果我们计算 buf + recv->len，我们可能会得到 nullptr + 0。
      这是未定义的行为。通过仅在有实际数据时计算 end_ptr 来避免这种情况，否则将其设置为 nullptr。 */
      unsigned char *buf_end = nullptr;
      if (buf != nullptr) {
        buf_end = buf + recv->len;
      }
      recv_parse_or_apply_log_rec_body(recv->type, buf, buf_end,
                                       recv_addr->space, recv_addr->page_no,
                                       block, &mtr, ULINT_UNDEFINED, LSN_MAX);

      end_lsn = recv->start_lsn + recv->len;

      mach_write_to_8(FIL_PAGE_LSN + page, end_lsn);

      mach_write_to_8(UNIV_PAGE_SIZE - FIL_PAGE_END_LSN_OLD_CHKSUM + page,
                      end_lsn);

      if (page_zip) {
        mach_write_to_8(FIL_PAGE_LSN + page_zip->data, end_lsn);
      }
#ifdef UNIV_HOTBACKUP
      ++applied_recs;
    } else {
      ++skipped_recs;
#endif /* UNIV_HOTBACKUP */
    }

    if (recv->len > RECV_DATA_BLOCK_SIZE) {
      ut::free(buf);
    }
  }

#ifdef UNIV_ZIP_DEBUG
  if (fil_page_index_page_check(page)) {
    page_zip_des_t *page_zip = buf_block_get_page_zip(block);

    ut_a(!page_zip || page_zip_validate_low(page_zip, page, nullptr, false));
  }
#endif /* UNIV_ZIP_DEBUG */

#ifndef UNIV_HOTBACKUP
  if (modification_to_page) {
    buf_flush_recv_note_modification(block, start_lsn, end_lsn);
  }
#else  /* !UNIV_HOTBACKUP */
  UT_NOT_USED(start_lsn);
#endif /* !UNIV_HOTBACKUP */

  /* Make sure that committing mtr does not change the modification
  LSN values of page */
  /* 确保提交 mtr 不会更改页面的修改 LSN 值 */

  mtr.discard_modifications();

  mtr_commit(&mtr); // 提交 mini-transaction

  mutex_enter(&recv_sys->mutex); // 进入互斥锁

  if (recv_max_page_lsn < page_lsn) {
    recv_max_page_lsn = page_lsn;
  }

  recv_addr->state = RECV_PROCESSED; // 设置状态为已处理

  ut_a(recv_sys->n_addrs > 0);
  --recv_sys->n_addrs;

  mutex_exit(&recv_sys->mutex); // 退出互斥锁

#ifdef UNIV_HOTBACKUP
  ib::trace_2() << "Applied " << applied_recs << " Skipped " << skipped_recs;
#endif /* UNIV_HOTBACKUP */
}

/** Tries to parse a single log record.
@param[out]     type            log record type
@param[in]      ptr             pointer to a buffer
@param[in]      end_ptr         end of the buffer
@param[out]     space_id        tablespace identifier
@param[out]     page_no         page number
@param[in]      online_log      do we process DDL online log
@param[out]     body            start of log record body
@return length of the record, or 0 if the record was not complete */
ulint recv_parse_log_rec(mlog_id_t *type, byte *ptr, byte *end_ptr,
                         space_id_t *space_id, page_no_t *page_no, bool apply,
                         byte **body) {
  byte *new_ptr;

  *body = nullptr;

  UNIV_MEM_INVALID(type, sizeof *type);
  UNIV_MEM_INVALID(space_id, sizeof *space_id);
  UNIV_MEM_INVALID(page_no, sizeof *page_no);
  UNIV_MEM_INVALID(body, sizeof *body);

  if (ptr == end_ptr) {
    return 0;
  }

  switch (*ptr) {
#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN | MLOG_SINGLE_REC_FLAG:
    case MLOG_LSN:

      new_ptr =
          mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no);

      if (new_ptr != nullptr) {
        const lsn_t lsn = static_cast<lsn_t>(*space_id) << 32 | *page_no;

        ut_a(lsn == recv_sys->recovered_lsn);
      }

      *type = MLOG_LSN;
      return new_ptr == nullptr ? 0 : new_ptr - ptr;
#endif /* UNIV_LOG_LSN_DEBUG */

    case MLOG_MULTI_REC_END:
    case MLOG_DUMMY_RECORD:
      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;
      *type = static_cast<mlog_id_t>(*ptr);
      return 1;

    case MLOG_MULTI_REC_END | MLOG_SINGLE_REC_FLAG:
    case MLOG_DUMMY_RECORD | MLOG_SINGLE_REC_FLAG:
      recv_sys->found_corrupt_log = true;
      return 0;

    case MLOG_TABLE_DYNAMIC_META:
    case MLOG_TABLE_DYNAMIC_META | MLOG_SINGLE_REC_FLAG:

      table_id_t id;
      uint64_t version;

      *page_no = FIL_NULL;
      *space_id = SPACE_UNKNOWN;

      new_ptr =
          mlog_parse_initial_dict_log_record(ptr, end_ptr, type, &id, &version);

      if (new_ptr != nullptr) {
        new_ptr = recv_sys->metadata_recover->parseMetadataLog(
            id, version, new_ptr, end_ptr);
      }

      return new_ptr == nullptr ? 0 : new_ptr - ptr;
  }

  new_ptr =
      mlog_parse_initial_log_record(ptr, end_ptr, type, space_id, page_no);

  *body = new_ptr;

  if (new_ptr == nullptr) {
    return 0;
  }

  new_ptr = recv_parse_or_apply_log_rec_body(
      *type, new_ptr, end_ptr, *space_id, *page_no, nullptr, nullptr,
      new_ptr - ptr, recv_sys->recovered_lsn);

  if (new_ptr == nullptr) {
    return 0;
  }

  return new_ptr - ptr;
}

/** Subtracts next number of bytes to ignore before we reach the checkpoint
or returns information that there was nothing more to skip.
@param[in]      next_parsed_bytes       number of next bytes that were parsed,
which are supposed to be subtracted from bytes to ignore before checkpoint
@retval true    there were still bytes to ignore
@retval false   there was already 0 bytes to ignore, nothing changed. */
static bool recv_update_bytes_to_ignore_before_checkpoint(
    size_t next_parsed_bytes) {
  auto &to_ignore = recv_sys->bytes_to_ignore_before_checkpoint;

  if (to_ignore != 0) {
    if (to_ignore >= next_parsed_bytes) {
      to_ignore -= next_parsed_bytes;
    } else {
      to_ignore = 0;
    }
    return true;
  }

  return false;
}

/** Tracks changes of recovered_lsn and tracks proper values for what
first_rec_group should be for consecutive blocks. Must be called when
recv_sys->recovered_lsn is changed to next lsn pointing at boundary
between consecutive parsed mini-transactions. */
static void recv_track_changes_of_recovered_lsn() {
  if (recv_sys->parse_start_lsn == 0) {
    return;
  }
  /* If we have already found the first block with mtr beginning there,
  we started to track boundaries between blocks. Since then we track
  all proper values of first_rec_group for consecutive blocks.
  The reason for that is to ensure that the first_rec_group of the last
  block is correct. Even though we do not depend during this recovery
  on that value, it would become important if we crashed later, because
  the last recovered block would become the first used block in redo and
  since then we would depend on a proper value of first_rec_group there.
  The checksums of log blocks should detect if it was incorrect, but the
  checksums might be disabled in the configuration. */
  const auto old_block =
      recv_sys->previous_recovered_lsn / OS_FILE_LOG_BLOCK_SIZE;

  const auto new_block = recv_sys->recovered_lsn / OS_FILE_LOG_BLOCK_SIZE;

  if (old_block != new_block) {
    ut_a(new_block > old_block);

    recv_sys->last_block_first_rec_group =
        recv_sys->recovered_lsn % OS_FILE_LOG_BLOCK_SIZE;
  }

  recv_sys->previous_recovered_lsn = recv_sys->recovered_lsn;
}

/** Parse and store a single log record entry.
@param[in]      ptr             start of buffer
@param[in]      end_ptr         end of buffer
@return true if end of processing */
static bool recv_single_rec(byte *ptr, byte *end_ptr) {
  /* The mtr did not modify multiple pages */

  lsn_t old_lsn = recv_sys->recovered_lsn;

  /* Try to parse a log record, fetching its type, space id,
  page no, and a pointer to the body of the log record */

  byte *body;
  mlog_id_t type;
  page_no_t page_no;
  space_id_t space_id;

  ulint len =
      recv_parse_log_rec(&type, ptr, end_ptr, &space_id, &page_no, true, &body);

  if (recv_sys->found_corrupt_log) {
    recv_report_corrupt_log(ptr, type, space_id, page_no);

#ifdef UNIV_HOTBACKUP
    return true;
#endif /* UNIV_HOTBACKUP */

  } else if (len == 0 || recv_sys->found_corrupt_fs) {
    return true;
  }

  lsn_t new_recovered_lsn;

  new_recovered_lsn = recv_calc_lsn_on_data_add(old_lsn, len);

  if (new_recovered_lsn > recv_sys->scanned_lsn) {
    /* The log record filled a log block, and we
    require that also the next log block should
    have been scanned in */

    return true;
  }

  recv_previous_parsed_rec_type = type;
  recv_previous_parsed_rec_is_multi = 0;
  recv_previous_parsed_rec_offset = recv_sys->recovered_offset;

  recv_sys->recovered_offset += len;
  recv_sys->recovered_lsn = new_recovered_lsn;

  recv_track_changes_of_recovered_lsn();

  if (recv_update_bytes_to_ignore_before_checkpoint(len)) {
    return false;
  }

  switch (type) {
    case MLOG_DUMMY_RECORD:
      /* Do nothing */
      break;

#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
      /* Do not add these records to the hash table.
      The page number and space id fields are misused
      for something else. */
      break;
#endif /* UNIV_LOG_LSN_DEBUG */

    default:

      if (recv_recovery_on) {
#ifndef UNIV_HOTBACKUP
        if (space_id == TRX_SYS_SPACE ||
            fil_tablespace_lookup_for_recovery(space_id)) {
#endif /* !UNIV_HOTBACKUP */

          recv_add_to_hash_table(type, space_id, page_no, body, ptr + len,
                                 old_lsn, recv_sys->recovered_lsn);

#ifndef UNIV_HOTBACKUP
        } else {
          recv_sys->missing_ids.insert(space_id);
        }
#endif /* !UNIV_HOTBACKUP */
      }

      [[fallthrough]];

    case MLOG_INDEX_LOAD:
    case MLOG_FILE_DELETE:
    case MLOG_FILE_RENAME:
    case MLOG_FILE_CREATE:
    case MLOG_FILE_EXTEND:
    case MLOG_TABLE_DYNAMIC_META:

      /* These were already handled by
      recv_parse_log_rec() and
      recv_parse_or_apply_log_rec_body(). */

      DBUG_PRINT("ib_log",
                 ("scan " LSN_PF ": log rec %s"
                  " len " ULINTPF " " PAGE_ID_PF,
                  old_lsn, get_mlog_string(type), len, space_id, page_no));
      break;
  }

  return false;
}

/** Parse and store a multiple record log entry.
@param[in]      ptr             start of buffer
@param[in]      end_ptr         end of buffer
@return true if end of processing */
/** 解析并存储多条记录的日志条目。
@param[in]      ptr             缓冲区起始位置
@param[in]      end_ptr         缓冲区结束位置
@return 如果处理结束则返回 true */
static bool recv_multi_rec(byte *ptr, byte *end_ptr) {
  /* Check that all the records associated with the single mtr
  are included within the buffer */
  /* 检查与单个 mtr 关联的所有记录是否包含在缓冲区内 */

  ulint n_recs = 0; // 记录数量
  ulint total_len = 0; // 总长度

  for (;;) {
    mlog_id_t type = MLOG_BIGGEST_TYPE; // 日志记录类型
    byte *body; // 日志记录主体
    page_no_t page_no = 0; // 页面编号
    space_id_t space_id = 0; // 表空间 ID

    ulint len = recv_parse_log_rec(&type, ptr, end_ptr, &space_id, &page_no,
                                   true, &body); // 解析日志记录

    if (recv_sys->found_corrupt_log) {
      recv_report_corrupt_log(ptr, type, space_id, page_no); // 报告损坏的日志记录

      return true;

    } else if (len == 0) {
      return true;

    } else if ((*ptr & MLOG_SINGLE_REC_FLAG)) {
      recv_sys->found_corrupt_log = true;

      recv_report_corrupt_log(ptr, type, space_id, page_no); // 报告损坏的日志记录

      return true;

    } else if (recv_sys->found_corrupt_fs) {
      return true;
    }

    recv_sys->save_rec(n_recs, space_id, page_no, type, body, len); // 保存日志记录

    recv_previous_parsed_rec_type = type; // 更新上一个解析的记录类型

    recv_previous_parsed_rec_offset = recv_sys->recovered_offset + total_len; // 更新上一个解析的记录偏移量

    recv_previous_parsed_rec_is_multi = 1; // 标记为多条记录

    total_len += len; // 更新总长度
    ++n_recs; // 更新记录数量

    ptr += len; // 更新指针位置

    if (type == MLOG_MULTI_REC_END) {
      DBUG_PRINT("ib_log", ("scan " LSN_PF ": multi-log end total_len " ULINTPF
                            " n=" ULINTPF,
                            recv_sys->recovered_lsn, total_len, n_recs)); // 打印调试信息

      break;
    }

    DBUG_PRINT("ib_log",
               ("scan " LSN_PF ": multi-log rec %s len " ULINTPF " " PAGE_ID_PF,
                recv_sys->recovered_lsn, get_mlog_string(type), len, space_id,
                page_no)); // 打印调试信息
  }

  lsn_t new_recovered_lsn =
      recv_calc_lsn_on_data_add(recv_sys->recovered_lsn, total_len); // 计算新的恢复 LSN

  if (new_recovered_lsn > recv_sys->scanned_lsn) {
    /* The log record filled a log block, and we require
    that also the next log block should have been scanned in */
    /* 日志记录填满了一个日志块，我们要求下一个日志块也应该被扫描 */

    return true;
  }

  /* Add all the records to the hash table */
  /* 将所有记录添加到哈希表 */

  ptr = recv_sys->buf + recv_sys->recovered_offset; // 更新指针位置

  for (ulint i = 0; i < n_recs; i++) {
    lsn_t old_lsn = recv_sys->recovered_lsn; // 保存旧的恢复 LSN

    /* This will apply MLOG_FILE_ records. */
    /* 这将应用 MLOG_FILE_ 记录。 */
    space_id_t space_id = 0; // 表空间 ID
    page_no_t page_no = 0; // 页面编号

    mlog_id_t type = MLOG_BIGGEST_TYPE; // 日志记录类型

    byte *body = nullptr; // 日志记录主体
    size_t len = 0; // 日志记录长度

    /* Avoid parsing if we have the record saved already. */
    /* 如果已经保存了记录，则避免解析。 */
    if (!recv_sys->get_saved_rec(i, space_id, page_no, type, body, len)) {
      len = recv_parse_log_rec(&type, ptr, end_ptr, &space_id, &page_no, false,
                               &body); // 解析日志记录
    }

    if (recv_sys->found_corrupt_log &&
        !recv_report_corrupt_log(ptr, type, space_id, page_no)) {
      return true;

    } else if (recv_sys->found_corrupt_fs) {
      return true;
    }

    ut_a(len != 0);
    ut_a(!(*ptr & MLOG_SINGLE_REC_FLAG));

    recv_sys->recovered_offset += len; // 更新恢复偏移量

    recv_sys->recovered_lsn = recv_calc_lsn_on_data_add(old_lsn, len); // 更新恢复 LSN

    const bool apply = !recv_update_bytes_to_ignore_before_checkpoint(len); // 检查是否应用日志记录

    switch (type) {
      case MLOG_MULTI_REC_END:
        recv_track_changes_of_recovered_lsn(); // 跟踪恢复 LSN 的变化
        /* Found the end mark for the records */
        /* 找到记录的结束标记 */
        return false;

#ifdef UNIV_LOG_LSN_DEBUG
      case MLOG_LSN:
        /* Do not add these records to the hash table.
        The page number and space id fields are misused
        for something else. */
        /* 不要将这些记录添加到哈希表。
        页面编号和表空间 ID 字段被误用于其他用途。 */
        break;
#endif /* UNIV_LOG_LSN_DEBUG */

      case MLOG_FILE_DELETE:
      case MLOG_FILE_CREATE:
      case MLOG_FILE_RENAME:
      case MLOG_FILE_EXTEND:
      case MLOG_TABLE_DYNAMIC_META:
        /* case MLOG_TRUNCATE: Disabled for WL6378 */
        /* These were already handled by
        recv_parse_or_apply_log_rec_body(). */
        /* 这些已经由 recv_parse_or_apply_log_rec_body() 处理。 */
        break;

      default:

        if (!apply) {
          break;
        }

        if (recv_recovery_on) {
#ifndef UNIV_HOTBACKUP
          if (space_id == TRX_SYS_SPACE ||
              fil_tablespace_lookup_for_recovery(space_id)) {
#endif /* !UNIV_HOTBACKUP */

            recv_add_to_hash_table(type, space_id, page_no, body, ptr + len,
                                   old_lsn, new_recovered_lsn); // 将记录添加到哈希表

#ifndef UNIV_HOTBACKUP
          } else {
            recv_sys->missing_ids.insert(space_id); // 插入缺失的表空间 ID
          }
#endif /* !UNIV_HOTBACKUP */
        }
    }

    ptr += len; // 更新指针位置
  }

  return false;
}

/** Parse log records from a buffer and optionally store them to a
hash table to wait merging to file pages. */
/** 从缓冲区解析日志记录，并可选择将它们存储到哈希表中以等待合并到文件页面。 */
static void recv_parse_log_recs() {
  ut_ad(recv_sys->parse_start_lsn != 0); // 断言解析起始 LSN 不为 0

  for (;;) {
    byte *ptr = recv_sys->buf + recv_sys->recovered_offset; // 获取当前解析位置指针

    byte *end_ptr = recv_sys->buf + recv_sys->len; // 获取缓冲区结束位置指针

    if (ptr == end_ptr) {
      return; // 如果解析位置指针等于缓冲区结束位置指针，则返回
    }

    bool single_rec; // 是否为单条记录

    switch (*ptr) {
#ifdef UNIV_LOG_LSN_DEBUG
      case MLOG_LSN:
#endif /* UNIV_LOG_LSN_DEBUG */
      case MLOG_DUMMY_RECORD:
        single_rec = true; // 如果是 MLOG_LSN 或 MLOG_DUMMY_RECORD，则为单条记录
        break;
      default:
        single_rec = !!(*ptr & MLOG_SINGLE_REC_FLAG); // 根据标志位判断是否为单条记录
    }

    if (single_rec) {
      if (recv_single_rec(ptr, end_ptr)) {
        return; // 解析单条记录，如果解析完成则返回
      }

    } else if (recv_multi_rec(ptr, end_ptr)) {
      return; // 解析多条记录，如果解析完成则返回
    }
  }
}

/** Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys->parse_start_lsn is non-zero.
@param[in]      log_block               log block
@param[in]      scanned_lsn             lsn of how far we were able
                                        to find data in this log block
@return true if more data added */
/** 如果 recv_sys->parse_start_lsn 非零，则将新日志块中的数据添加到 recv_sys 的解析缓冲区。
@param[in]      log_block               日志块
@param[in]      scanned_lsn             在此日志块中找到数据的 LSN
@return 如果添加了更多数据，则返回 true */
static bool recv_sys_add_to_parsing_buf(const byte *log_block,
                                        lsn_t scanned_lsn) {
  ut_ad(scanned_lsn >= recv_sys->scanned_lsn); // 断言扫描的 LSN 大于等于 recv_sys 的扫描 LSN

  if (!recv_sys->parse_start_lsn) {
    /* Cannot start parsing yet because no start point for
    it found */
    /* 由于未找到起始点，无法开始解析 */

    return false;
  }

  ulint more_len; // 需要添加的长度
  ulint data_len = log_block_get_data_len(log_block); // 获取日志块的数据长度

  if (recv_sys->parse_start_lsn >= scanned_lsn) {
    return false;

  } else if (recv_sys->scanned_lsn >= scanned_lsn) {
    return false;

  } else if (recv_sys->parse_start_lsn > recv_sys->scanned_lsn) {
    more_len = (ulint)(scanned_lsn - recv_sys->parse_start_lsn); // 计算需要添加的长度

  } else {
    more_len = (ulint)(scanned_lsn - recv_sys->scanned_lsn); // 计算需要添加的长度
  }

  if (more_len == 0) {
    return false;
  }

  ut_ad(data_len >= more_len); // 断言数据长度大于等于需要添加的长度

  ulint start_offset = data_len - more_len; // 计算起始偏移量

  if (start_offset < LOG_BLOCK_HDR_SIZE) {
    start_offset = LOG_BLOCK_HDR_SIZE; // 确保起始偏移量不小于日志块头部大小
  }

  ulint end_offset = data_len; // 结束偏移量为数据长度

  if (end_offset > OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) {
    end_offset = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE; // 确保结束偏移量不大于日志块尾部大小
  }

  ut_ad(start_offset <= end_offset); // 断言起始偏移量小于等于结束偏移量

  if (start_offset < end_offset) {
    memcpy(recv_sys->buf + recv_sys->len, log_block + start_offset,
           end_offset - start_offset); // 将日志块的数据复制到解析缓冲区

    recv_sys->len += end_offset - start_offset; // 更新解析缓冲区的长度

    ut_a(recv_sys->len <= recv_sys->buf_len); // 断言解析缓冲区的长度不超过缓冲区大小
  }

  return true; // 返回 true 表示添加了更多数据
}

/** Moves the parsing buffer data left to the buffer start. */
static void recv_reset_buffer() {
  ut_memmove(recv_sys->buf, recv_sys->buf + recv_sys->recovered_offset,
             recv_sys->len - recv_sys->recovered_offset);

  recv_sys->len -= recv_sys->recovered_offset;

  recv_sys->recovered_offset = 0;
}

/** Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.  Unless
UNIV_HOTBACKUP is defined, this function will apply log records
automatically when the hash table becomes full.
@param[in,out]  log             redo log
@param[in]      max_memory      we let the hash table of recs to grow to
                                this size, at the maximum
@param[in]      buf             buffer containing a log segment or garbage
@param[in]      len             buffer length
@param[in]      start_lsn       buffer start lsn
@param[out]  read_upto_lsn  scanning succeeded up to this lsn
@param[out]  err             DB_SUCCESS when no dblwr corruptions.
@return true if not able to scan any more in this log */
/** 从缓冲区扫描日志并将新日志数据存储到解析缓冲区。
如果找到新数据，则解析并哈希日志记录。除非定义了 UNIV_HOTBACKUP，否则当哈希表变满时，此函数将自动应用日志记录。
@param[in,out]  log             重做日志
@param[in]      max_memory      我们让记录的哈希表最大增长到这个大小
@param[in]      buf             包含日志段或垃圾的缓冲区
@param[in]      len             缓冲区长度
@param[in]      start_lsn       缓冲区起始 lsn
@param[out]  read_upto_lsn  扫描成功到达的 lsn
@param[out]  err             当没有 dblwr 损坏时为 DB_SUCCESS。
@return 如果无法在此日志中扫描更多内容，则返回 true */
#ifndef UNIV_HOTBACKUP
static bool recv_scan_log_recs(log_t &log,
#else  /* !UNIV_HOTBACKUP */
bool meb_scan_log_recs(
#endif /* !UNIV_HOTBACKUP */
                               size_t max_memory, const byte *buf, size_t len,
                               lsn_t start_lsn, lsn_t *read_upto_lsn,
                               dberr_t &err) {
  const byte *log_block = buf; // 日志块指针
  lsn_t scanned_lsn = start_lsn; // 已扫描的 lsn
  bool finished = false; // 是否完成标志
  bool more_data = false; // 是否有更多数据标志
  err = DB_SUCCESS; // 错误码初始化为 DB_SUCCESS

  ut_ad(start_lsn % OS_FILE_LOG_BLOCK_SIZE == 0); // 断言起始 lsn 是日志块大小的倍数
  ut_ad(len % OS_FILE_LOG_BLOCK_SIZE == 0); // 断言缓冲区长度是日志块大小的倍数
  ut_ad(len >= OS_FILE_LOG_BLOCK_SIZE); // 断言缓冲区长度大于等于日志块大小

  do {
    ut_ad(!finished); // 断言未完成

    Log_data_block_header block_header; // 日志数据块头部
    log_data_block_header_deserialize(log_block, block_header); // 反序列化日志数据块头部

    const uint32_t expected_hdr_no =
        log_block_convert_lsn_to_hdr_no(scanned_lsn); // 计算预期的头部编号

    if (block_header.m_hdr_no != expected_hdr_no) { // 如果头部编号不匹配
      /* Garbage or an incompletely written log block.
      We will not report any error, because this can
      happen when InnoDB was killed while it was
      writing redo log. We simply treat this as an
      abrupt end of the redo log. */
      /* 垃圾或未完全写入的日志块。
      我们不会报告任何错误，因为当 InnoDB 在写入重做日志时被杀死时，这可能会发生。
      我们只是将其视为重做日志的突然结束。 */

      finished = true; // 设置完成标志

      break; // 退出循环
    }

    if (!log_block_checksum_is_ok(log_block)) { // 如果日志块校验和不正确
      uint32_t checksum1 = log_block_get_checksum(log_block); // 获取日志块校验和
      uint32_t checksum2 = log_block_calc_checksum(log_block); // 计算日志块校验和
      ib::error(ER_IB_MSG_720, ulong{block_header.m_hdr_no},
                ulonglong{scanned_lsn}, ulong{checksum1}, ulong{checksum2}); // 打印错误信息

      /* Garbage or an incompletely written log block.
      This could be the result of killing the server
      while it was writing this log block. We treat
      this as an abrupt end of the redo log. */
      /* 垃圾或未完全写入的日志块。
      这可能是由于在写入此日志块时杀死服务器的结果。
      我们将其视为重做日志的突然结束。 */

      finished = true; // 设置完成标志

      break; // 退出循环
    }

    const auto data_len = block_header.m_data_len; // 获取数据长度

    if (scanned_lsn + data_len > recv_sys->scanned_lsn &&
        recv_sys->scanned_epoch_no > 0 &&
        !log_block_epoch_no_is_valid(block_header.m_epoch_no,
                                     recv_sys->scanned_epoch_no)) {
      /* Garbage from a log buffer flush which was made
      before the most recent database recovery */
      /* 最近一次数据库恢复之前进行的日志缓冲区刷新产生的垃圾 */

      finished = true; // 设置完成标志

      break; // 退出循环
    }

    if (!recv_sys->parse_start_lsn && block_header.m_first_rec_group > 0) { // 如果未找到解析起始 lsn 且第一个记录组大于 0
      /* We found a point from which to start the parsing of log records */
      /* 我们找到了一个可以开始解析日志记录的点 */

      recv_sys->parse_start_lsn = scanned_lsn + block_header.m_first_rec_group; // 设置解析起始 lsn

      ib::info(ER_IB_MSG_1261)
          << "Starting to parse redo log at lsn = " << recv_sys->parse_start_lsn
          << ", whereas checkpoint_lsn = " << recv_sys->checkpoint_lsn
          << " and start_lsn = " << start_lsn; // 打印解析起始 lsn 信息

      if (recv_sys->parse_start_lsn < recv_sys->checkpoint_lsn) { // 如果解析起始 lsn 小于检查点 lsn
        /* We start to parse log records even before
        checkpoint_lsn, from the beginning of the log
        block which contains the checkpoint_lsn.
        That's because the first group of log records
        in the log block, starts before checkpoint_lsn,
        and checkpoint_lsn could potentially point to
        the middle of some log record. We need to find
        the first group of log records that starts at
        or after checkpoint_lsn. This could be only
        achieved by traversing all groups of log records
        that start within the log block since the first
        one (to discover their beginnings we need to
        parse them). However, we don't want to report
        missing tablespaces for space_id in log records
        before checkpoint_lsn. Hence we need to ignore
        those records and that's why we need a counter
        of bytes to ignore. */
        /* 我们甚至在 checkpoint_lsn 之前就开始解析日志记录，从包含 checkpoint_lsn 的日志块的开头开始。
        这是因为日志块中的第一个日志记录组在 checkpoint_lsn 之前开始，并且 checkpoint_lsn 可能指向某些日志记录的中间。
        我们需要找到在 checkpoint_lsn 处或之后开始的第一个日志记录组。
        这只能通过遍历自第一个日志记录组以来的所有日志记录组来实现（要发现它们的开头，我们需要解析它们）。
        但是，我们不想报告 checkpoint_lsn 之前日志记录中 space_id 的丢失表空间。
        因此，我们需要忽略这些记录，这就是为什么我们需要一个要忽略的字节计数器。 */

        recv_sys->bytes_to_ignore_before_checkpoint =
            recv_sys->checkpoint_lsn - recv_sys->parse_start_lsn; // 设置在检查点之前要忽略的字节数

        ut_a(recv_sys->bytes_to_ignore_before_checkpoint <=
             OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE); // 断言要忽略的字节数小于等于日志块大小减去头部大小

        ut_a(recv_sys->checkpoint_lsn % OS_FILE_LOG_BLOCK_SIZE +
                 LOG_BLOCK_TRL_SIZE <
             OS_FILE_LOG_BLOCK_SIZE); // 断言检查点 lsn 加上尾部大小小于日志块大小

        ut_a(recv_sys->parse_start_lsn % OS_FILE_LOG_BLOCK_SIZE >=
             LOG_BLOCK_HDR_SIZE); // 断言解析起始 lsn 大于等于头部大小
      }

      recv_sys->scanned_lsn = recv_sys->parse_start_lsn; // 设置扫描 lsn
      recv_sys->recovered_lsn = recv_sys->parse_start_lsn; // 设置恢复 lsn

      recv_track_changes_of_recovered_lsn(); // 跟踪恢复 lsn 的变化
    }

    scanned_lsn += data_len; // 更新已扫描的 lsn

    if (scanned_lsn > recv_sys->scanned_lsn) { // 如果已扫描的 lsn 大于扫描 lsn
#ifndef UNIV_HOTBACKUP
      if (!recv_needed_recovery && scanned_lsn > recv_sys->checkpoint_lsn) { // 如果不需要恢复且已扫描的 lsn 大于检查点 lsn
        if (srv_read_only_mode) { // 如果是只读模式
          ut_a(srv_force_recovery < SRV_FORCE_NO_LOG_REDO); // 断言强制恢复级别小于 SRV_FORCE_NO_LOG_REDO
          ib::warn(ER_IB_MSG_RECOVERY_SKIPPED_IN_READ_ONLY_MODE); // 打印警告信息
          *read_upto_lsn = scanned_lsn; // 设置读取到的 lsn
          return true; // 返回 true
        }

        ib::info(ER_IB_MSG_722, ulonglong{recv_sys->scanned_lsn}); // 打印信息

        err = recv_init_crash_recovery(); // 初始化崩溃恢复
        if (err != DB_SUCCESS) { // 如果初始化失败
          return true; // 返回 true
        }
      }
#endif /* !UNIV_HOTBACKUP */

      /* We were able to find more log data: add it to the
      parsing buffer if parse_start_lsn is already
      non-zero */
      /* 我们能够找到更多的日志数据：如果 parse_start_lsn 已经非零，则将其添加到解析缓冲区 */

      DBUG_EXECUTE_IF("simulate_3mb_mtr_recovery", {
        uint saved_len = recv_sys->len; // 保存当前长度
        recv_sys->len = 3 * 1024 * 1024; // 设置长度为 3MB
        recv_sys_resize_buf(); // 调整缓冲区大小
        recv_sys->len = saved_len; // 恢复长度
      });

      if (recv_sys->len + 4 * OS_FILE_LOG_BLOCK_SIZE >= recv_sys->buf_len) { // 如果缓冲区长度加上 4 个日志块大小大于等于缓冲区大小
        if (!recv_sys_resize_buf()) { // 如果调整缓冲区大小失败
          recv_sys->found_corrupt_log = true; // 设置找到损坏日志标志

#ifndef UNIV_HOTBACKUP
          if (srv_force_recovery == 0) { // 如果强制恢复级别为 0
            ib::error(ER_IB_MSG_724); // 打印错误信息
            return true; // 返回 true
          }
#else  /* !UNIV_HOTBACKUP */
          ib::fatal(UT_LOCATION_HERE,
                    ER_IB_ERR_NOT_ENOUGH_MEMORY_FOR_PARSE_BUFFER)
              << "Insufficient memory for InnoDB parse buffer; want "
              << recv_sys->buf_len; // 打印致命错误信息
#endif /* !UNIV_HOTBACKUP */
        }
      }

      if (!recv_sys->found_corrupt_log) { // 如果未找到损坏日志
        more_data = recv_sys_add_to_parsing_buf(log_block, scanned_lsn); // 将日志块添加到解析缓冲区
      }

      recv_sys->scanned_lsn = scanned_lsn; // 更新扫描 lsn

      recv_sys->scanned_epoch_no = block_header.m_epoch_no; // 更新扫描纪元号
    }

    if (data_len < OS_FILE_LOG_BLOCK_SIZE) { // 如果数据长度小于日志块大小
      /* Log data for this group ends here */
      /* 此组的日志数据在此结束 */
      finished = true; // 设置完成标志

      break; // 退出循环

    } else {
      log_block += OS_FILE_LOG_BLOCK_SIZE; // 更新日志块指针
    }

  } while (log_block < buf + len); // 循环直到日志块指针大于等于缓冲区末尾

  *read_upto_lsn = scanned_lsn; // 设置读取到的 lsn

  if (recv_needed_recovery ||
      (recv_is_from_backup && !recv_is_making_a_backup)) { // 如果需要恢复或来自备份且未进行备份
    ++recv_scan_print_counter; // 增加扫描打印计数器

    if (finished || (recv_scan_print_counter % 80) == 0) { // 如果完成或扫描打印计数器是 80 的倍数
      ib::info(ER_IB_MSG_725, ulonglong{scanned_lsn}); // 打印信息
    }
  }

  if (more_data && !recv_sys->found_corrupt_log) { // 如果有更多数据且未找到损坏日志
    /* Try to parse more log records */
    /* 尝试解析更多日志记录 */

    recv_parse_log_recs(); // 解析日志记录

#ifndef UNIV_HOTBACKUP
    if (recv_heap_used() > max_memory) { // 如果堆使用量大于最大内存
      recv_apply_hashed_log_recs(log, false); // 应用哈希日志记录
    }
#endif /* !UNIV_HOTBACKUP */

    if (recv_sys->recovered_offset > recv_sys->buf_len / 4) { // 如果恢复偏移量大于缓冲区大小的四分之一
      /* Move parsing buffer data to the buffer start */
      /* 将解析缓冲区数据移动到缓冲区开头 */

      recv_reset_buffer(); // 重置缓冲区
    }
  }

  return finished; // 返回是否完成
}

#ifndef UNIV_HOTBACKUP
static lsn_t recv_read_log_seg(log_t &log, byte *buf, lsn_t start_lsn,
                               const lsn_t end_lsn) {
  log_background_threads_inactive_validate(); // 验证后台线程是否处于非活动状态

  ut_a(start_lsn < end_lsn); // 断言起始 LSN 小于结束 LSN

  auto file = log.m_files.find(start_lsn); // 查找包含起始 LSN 的日志文件

  if (file == log.m_files.end()) { // 如果未找到日志文件
    /* Missing valid file ! */
    /* 缺少有效文件！ */
    return start_lsn; // 返回起始 LSN
  }

  auto file_handle = file->open(Log_file_access_mode::READ_ONLY); // 以只读方式打开日志文件
  ut_a(file_handle.is_open()); // 断言文件已打开

  do {
    os_offset_t source_offset; // 源偏移量

    source_offset = file->offset(start_lsn); // 获取起始 LSN 的偏移量

    ut_a(end_lsn - start_lsn <= ULINT_MAX); // 断言结束 LSN 减去起始 LSN 小于等于 ULINT_MAX

    os_offset_t len = end_lsn - start_lsn; // 计算读取长度

    ut_ad(len != 0); // 断言读取长度不为 0

    bool switch_to_next_file = false; // 是否切换到下一个文件的标志

    if (source_offset + len > file->m_size_in_bytes) { // 如果读取长度超出文件大小
      /* If the above condition is true then len
      (which is unsigned) is > the expression below,
      so the typecast is ok */
      /* 如果上述条件为真，则 len（无符号）大于下面的表达式，因此类型转换是可以的 */
      ut_a(file->m_size_in_bytes > source_offset); // 断言文件大小大于源偏移量
      len = file->m_size_in_bytes - source_offset; // 调整读取长度
      switch_to_next_file = true; // 设置切换到下一个文件的标志
    }

    ++log.n_log_ios; // 增加日志 IO 次数

    const dberr_t err =
        log_data_blocks_read(file_handle, source_offset, len, buf); // 从文件中读取日志数据块
    ut_a(err == DB_SUCCESS); // 断言读取成功

    start_lsn += len; // 更新起始 LSN
    buf += len; // 更新缓冲区指针

    if (switch_to_next_file) { // 如果需要切换到下一个文件
      auto next_id = file->next_id(); // 获取下一个文件的 ID

      const auto next_file = log.m_files.file(next_id); // 查找下一个文件

      if (next_file == log.m_files.end() || !next_file->contains(start_lsn)) { // 如果未找到下一个文件或下一个文件不包含起始 LSN
        return start_lsn; // 返回起始 LSN
      }

      file_handle.close(); // 关闭当前文件

      file = next_file; // 切换到下一个文件

      file_handle = file->open(Log_file_access_mode::READ_ONLY); // 以只读方式打开下一个文件
      ut_a(file_handle.is_open()); // 断言文件已打开
    }

  } while (start_lsn != end_lsn); // 循环直到起始 LSN 等于结束 LSN

  ut_a(start_lsn == end_lsn); // 断言起始 LSN 等于结束 LSN

  return end_lsn; // 返回结束 LSN
}

/** Scans log from a buffer and stores new log data to the parsing buffer.
Parses and hashes the log records if new data found.
@param[in,out]  log                     redo log
@param[in,out]  checkpoint_lsn          log sequence number found in checkpoint
                                        header. May be inexact (in a middle of
                                        an mtr which we can ignore, as it is
                                        already applied to tablespace files)
                                        until which all redo log has been
                                        scanned */
/** 从缓冲区扫描日志并将新日志数据存储到解析缓冲区。
如果找到新数据，则解析并哈希日志记录。
@param[in,out]  log                     重做日志
@param[in,out]  checkpoint_lsn          在检查点头部找到的日志序列号。可能不准确（在我们可以忽略的 mtr 中间，因为它已经应用于表空间文件）
                                        直到所有重做日志都已扫描 */
static dberr_t recv_recovery_begin(log_t &log, const lsn_t checkpoint_lsn) {
  mutex_enter(&recv_sys->mutex); // 进入互斥锁

  recv_sys->len = 0; // 重置长度
  recv_sys->recovered_offset = 0; // 重置恢复偏移量
  recv_sys->n_addrs = 0; // 重置地址数量
  recv_sys_empty_hash(); // 清空哈希表

  /* Since 8.0, we can start recovery at checkpoint_lsn which points
  to the middle of log record. In such case we first to need to find
  the beginning of the first group of log records, which is at lsn
  greater than the checkpoint_lsn. */
  /* 自 8.0 以来，我们可以从指向日志记录中间的 checkpoint_lsn 开始恢复。
  在这种情况下，我们首先需要找到第一个日志记录组的开头，该组位于大于 checkpoint_lsn 的 lsn 处。 */
  recv_sys->parse_start_lsn = 0; // 重置解析起始 LSN

  /* This is updated when we find value for parse_start_lsn. */
  /* 当我们找到解析起始 LSN 的值时更新此值。 */
  recv_sys->bytes_to_ignore_before_checkpoint = 0; // 重置在检查点之前要忽略的字节数

  recv_sys->checkpoint_lsn = checkpoint_lsn; // 设置检查点 LSN
  recv_sys->scanned_lsn = checkpoint_lsn; // 设置扫描 LSN
  recv_sys->recovered_lsn = checkpoint_lsn; // 设置恢复 LSN

  /* We have to trust that the first_rec_group in the first block is
  correct as we can't start parsing earlier to check it ourselves. */
  /* 我们必须相信第一个块中的 first_rec_group 是正确的，因为我们无法提前开始解析来自己检查它。 */
  recv_sys->previous_recovered_lsn = checkpoint_lsn; // 设置之前恢复的 LSN
  recv_sys->last_block_first_rec_group = 0; // 重置最后一个块的第一个记录组

  recv_sys->scanned_epoch_no = 0; // 重置扫描纪元号
  recv_previous_parsed_rec_type = MLOG_SINGLE_REC_FLAG; // 设置之前解析的记录类型
  recv_previous_parsed_rec_offset = 0; // 重置之前解析的记录偏移量
  recv_previous_parsed_rec_is_multi = 0; // 重置之前解析的记录是否为多条记录
  ut_ad(recv_max_page_lsn == 0); // 断言最大页面 LSN 为 0

  mutex_exit(&recv_sys->mutex); // 退出互斥锁

  ulint max_mem =
      UNIV_PAGE_SIZE * (buf_pool_get_n_pages() -
                        (recv_n_pool_free_frames * srv_buf_pool_instances)); // 计算最大内存

  lsn_t start_lsn =
      ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE); // 对齐起始 LSN

  bool finished = false; // 是否完成标志

  while (!finished) { // 循环直到完成
    const lsn_t end_lsn =
        recv_read_log_seg(log, log.buf, start_lsn, start_lsn + RECV_SCAN_SIZE); // 读取日志段

    if (end_lsn == start_lsn) { // 如果结束 LSN 等于起始 LSN
      /* This could happen if we crashed just after completing file,
      and before next file has been successfully created. */
      /* 如果我们在完成文件后崩溃，并且在下一个文件成功创建之前，这可能会发生。 */
      break; // 退出循环
    }

    dberr_t err; // 错误码

    finished = recv_scan_log_recs(log, max_mem, log.buf, end_lsn - start_lsn,
                                  start_lsn, &log.m_scanned_lsn, err); // 扫描日志记录

    if (err != DB_SUCCESS) { // 如果扫描失败
      return err; // 返回错误
    }

    start_lsn = end_lsn; // 更新起始 LSN
  }

  DBUG_PRINT("ib_log", ("scan " LSN_PF " completed", log.m_scanned_lsn)); // 打印调试信息
  return DB_SUCCESS; // 返回成功
}

/** Initialize crash recovery environment. Can be called iff
recv_needed_recovery == false. */
/** 初始化崩溃恢复环境。仅当 recv_needed_recovery == false 时可以调用。 */
static dberr_t recv_init_crash_recovery() {
  ut_ad(!srv_read_only_mode); // 断言不是只读模式
  ut_a(!recv_needed_recovery); // 断言不需要恢复

  recv_needed_recovery = true; // 设置需要恢复标志为 true

  ib::info(ER_IB_MSG_726); // 打印信息
  ib::info(ER_IB_MSG_727); // 打印信息

  return recv_sys->dblwr->recover(); // 调用双写缓冲区的恢复函数
}
#endif /* !UNIV_HOTBACKUP */

#ifndef UNIV_HOTBACKUP

dberr_t recv_recovery_from_checkpoint_start(log_t &log, lsn_t flush_lsn) {
  /* Initialize red-black tree for fast insertions into the
  flush_list during recovery process. */
  /* 初始化红黑树，以便在恢复过程中快速插入到 flush_list 中。 */
  buf_flush_init_flush_rbt();

  if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) { // 如果强制恢复级别大于等于 SRV_FORCE_NO_LOG_REDO
    ib::info(ER_IB_MSG_728); // 打印信息

    /* We leave redo log not started and this is read-only mode. */
    /* 我们不启动重做日志，这是只读模式。 */
    ut_a(log.sn == 0); // 断言日志序列号为 0
    ut_a(srv_read_only_mode); // 断言是只读模式

    return DB_SUCCESS; // 返回成功
  }

  recv_recovery_on = true; // 设置恢复标志为 true

  switch (log.m_format) { // 根据日志格式进行处理
    case Log_format::CURRENT: // 当前格式
      break;

    case Log_format::VERSION_5_7_9:
    case Log_format::VERSION_8_0_1:
    case Log_format::VERSION_8_0_3:
    case Log_format::VERSION_8_0_19:
    case Log_format::VERSION_8_0_28:

      /* Check if the redo log from an older known redo log
      version is from a clean shutdown. */
      /* 检查旧版本的重做日志是否来自干净的关闭。 */
      return recv_log_recover_pre_8_0_30(log); // 恢复旧版本日志

    default: // 不支持的格式
      ib::error(ER_IB_MSG_LOG_FORMAT_NOT_SUPPORTED, ulong{to_int(log.m_format)},
                ulong{to_int(Log_format::CURRENT)}); // 打印错误信息

      ut_ad(0); // 断言失败
      recv_sys->found_corrupt_log = true; // 设置找到损坏日志标志
      return DB_ERROR; // 返回错误
  }

  ut_a(log.m_format == Log_format::CURRENT); // 断言日志格式为当前格式

  /* Look for the latest checkpoint */
  /* 查找最新的检查点 */
  Log_checkpoint_location checkpoint; // 检查点位置
  if (!recv_find_max_checkpoint(log, checkpoint)) { // 如果未找到最大检查点
    ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_NOT_FOUND); // 打印错误信息
    return DB_ERROR; // 返回错误
  }

  const auto checkpoint_file = log.m_files.find(checkpoint.m_checkpoint_lsn); // 查找检查点文件

  /* When reading checkpoints from redo log files, error would be reported
  if checkpoint_lsn was outside the redo log file from which it was read,
  and such file would be skipped. If no checkpoint was found because of that,
  then recv_find_max_checkpoint would return false. Therefore here we know
  that InnoDB found a valid checkpoint (for which there is a redo log file
  which contains the checkpoint_lsn). */
  /* 从重做日志文件读取检查点时，如果检查点 LSN 超出读取的重做日志文件范围，则会报告错误，并跳过该文件。
  如果因此未找到检查点，则 recv_find_max_checkpoint 会返回 false。因此在这里我们知道 InnoDB 找到了一个有效的检查点（有一个包含检查点 LSN 的重做日志文件）。 */
  if (checkpoint_file == log.m_files.end()) { // 如果未找到检查点文件
    ut_d(ut_error); // 调试断言错误
    ut_o(return DB_ERROR); // 返回错误
  }

  log.last_checkpoint_lsn.store(checkpoint.m_checkpoint_lsn); // 存储最后一个检查点 LSN

  const auto file_path = log_file_path(log.m_files_ctx, checkpoint_file->m_id); // 获取检查点文件路径
  ib::info(ER_IB_MSG_LOG_CHECKPOINT_FOUND,
           ulonglong{checkpoint.m_checkpoint_lsn}, file_path.c_str()); // 打印检查点信息

  Log_checkpoint_header checkpoint_header; // 检查点头部

  auto checkpoint_file_handle =
      checkpoint_file->open(Log_file_access_mode::READ_ONLY); // 以只读方式打开检查点文件

  if (!checkpoint_file_handle.is_open()) { // 如果文件未打开
    return DB_CANNOT_OPEN_FILE; // 返回无法打开文件错误
  }

  dberr_t err = log_checkpoint_header_read(checkpoint_file_handle,
                                           checkpoint.m_checkpoint_header_no,
                                           checkpoint_header); // 读取检查点头部
  if (err != DB_SUCCESS) { // 如果读取失败
    return err; // 返回错误
  }

  checkpoint_file_handle.close(); // 关闭检查点文件

  const lsn_t checkpoint_lsn = checkpoint.m_checkpoint_lsn; // 获取检查点 LSN

  ut_a(checkpoint_lsn == checkpoint_header.m_checkpoint_lsn); // 断言检查点 LSN 与头部 LSN 相同

  /* Read the encryption header to get the encryption information. */
  /* 读取加密头部以获取加密信息。 */
  err = log_encryption_read(log); // 读取加密信息
  if (err != DB_SUCCESS) { // 如果读取失败
    return DB_ERROR; // 返回错误
  }

  /* Start reading the log from the checkpoint LSN up. */
  /* 从检查点 LSN 开始读取日志。 */

  ut_ad(RECV_SCAN_SIZE <= log.buf_size); // 断言扫描大小小于等于日志缓冲区大小

  ut_ad(recv_sys->n_addrs == 0); // 断言地址数量为 0

  /* NOTE: we always do a 'recovery' at startup, but only if
  there is something wrong we will print a message to the
  user about recovery: */
  /* 注意：我们在启动时总是进行“恢复”，但只有在出现问题时才会向用户打印恢复消息： */

  if (checkpoint_lsn != flush_lsn) { // 如果检查点 LSN 不等于刷新 LSN
    if (checkpoint_lsn < flush_lsn) { // 如果检查点 LSN 小于刷新 LSN
      ib::warn(ER_IB_MSG_RECOVERY_CHECKPOINT_FROM_BEFORE_CLEAN_SHUTDOWN,
               ulonglong{checkpoint_lsn}, ulonglong{flush_lsn}); // 打印警告信息
    }

    if (!recv_needed_recovery) { // 如果不需要恢复
      ib::info(ER_IB_MSG_RECOVERY_IS_NEEDED, ulonglong{flush_lsn},
               ulonglong{checkpoint_lsn}); // 打印恢复信息

      if (srv_read_only_mode) { // 如果是只读模式
        ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY); // 打印错误信息

        return DB_ERROR; // 返回错误
      }

      err = recv_init_crash_recovery(); // 初始化崩溃恢复
      if (err != DB_SUCCESS) { // 如果初始化失败
        return err; // 返回错误
      }
    }
  }

  err = recv_recovery_begin(log, checkpoint_lsn); // 开始恢复
  if (err != DB_SUCCESS) { // 如果恢复失败
    return err; // 返回错误
  }

  if (srv_read_only_mode && log.m_scanned_lsn > checkpoint_lsn) { // 如果是只读模式且扫描的 LSN 大于检查点 LSN
    ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY); // 打印错误信息
    return DB_ERROR; // 返回错误
  }

  lsn_t recovered_lsn; // 恢复的 LSN

  recovered_lsn = recv_sys->recovered_lsn; // 获取恢复的 LSN

  ut_a(recv_needed_recovery || checkpoint_lsn == recovered_lsn); // 断言需要恢复或检查点 LSN 等于恢复的 LSN

  ut_a(!srv_read_only_mode || !recv_needed_recovery); // 断言不是只读模式或不需要恢复
  ut_a(!srv_read_only_mode || checkpoint_lsn == recovered_lsn); // 断言不是只读模式或检查点 LSN 等于恢复的 LSN

  log.recovered_lsn = recovered_lsn; // 设置恢复的 LSN

  ut_a(log.m_files.find(recovered_lsn) != log.m_files.end()); // 断言恢复的 LSN 在日志文件中

  /* If it is at block boundary, add header size. */
  /* 如果在块边界上，则添加头部大小。 */
  auto check_scanned_lsn = log.m_scanned_lsn; // 获取扫描的 LSN
  if (check_scanned_lsn % OS_FILE_LOG_BLOCK_SIZE == 0) { // 如果扫描的 LSN 是块大小的倍数
    check_scanned_lsn += LOG_BLOCK_HDR_SIZE; // 添加头部大小
  }

  if (check_scanned_lsn < checkpoint_lsn ||
      check_scanned_lsn < recv_max_page_lsn) { // 如果扫描的 LSN 小于检查点 LSN 或最大页面 LSN
    ib::error(ER_IB_MSG_737, ulonglong{log.m_scanned_lsn},
              ulonglong{checkpoint_lsn}, ulonglong{recv_max_page_lsn}); // 打印错误信息
  }

  if (recovered_lsn < checkpoint_lsn) { // 如果恢复的 LSN 小于检查点 LSN
    /* No harm in trying to do RO access. */
    /* 尝试进行只读访问没有害处。 */
    if (!srv_read_only_mode) { // 如果不是只读模式
      ut_error; // 断言错误
    }

    return DB_ERROR; // 返回错误
  }

  if ((recv_sys->found_corrupt_log && srv_force_recovery == 0) ||
      recv_sys->found_corrupt_fs) { // 如果找到损坏的日志且强制恢复级别为 0 或找到损坏的文件系统
    return DB_ERROR; // 返回错误
  }

  /* Read the last recovered log block. */
  /* 读取最后一个恢复的日志块。 */
  lsn_t start_lsn; // 起始 LSN
  lsn_t end_lsn; // 结束 LSN

  start_lsn = ut_uint64_align_down(recovered_lsn, OS_FILE_LOG_BLOCK_SIZE); // 对齐起始 LSN
  end_lsn = ut_uint64_align_up(recovered_lsn, OS_FILE_LOG_BLOCK_SIZE); // 对齐结束 LSN

  ut_a(start_lsn < end_lsn); // 断言起始 LSN 小于结束 LSN
  ut_a(start_lsn % log.buf_size + OS_FILE_LOG_BLOCK_SIZE <= log.buf_size); // 断言起始 LSN 加块大小小于等于日志缓冲区大小

  const lsn_t recv_read_log_seg_ended_at_lsn =
      recv_read_log_seg(log, recv_sys->last_block, start_lsn, end_lsn); // 读取日志段

  ut_a(recv_read_log_seg_ended_at_lsn == end_lsn); // 断言读取的日志段结束 LSN 等于结束 LSN

  if (recv_sys->last_block_first_rec_group != 0 &&
      log_block_get_first_rec_group(recv_sys->last_block) !=
          recv_sys->last_block_first_rec_group) { // 如果最后一个块的第一个记录组不为 0 且第一个记录组不等于最后一个块的第一个记录组
    /* We must not start with invalid first_rec_group in the first block,
    because if we crashed, we could be unable to recover. We do NOT have
    guarantee that the first_rec_group was correct because recovery did
    not report error. The first_rec_group was used only to locate the
    beginning of the log for recovery. For later blocks it was not used.
    It might be corrupted on disk and stay unnoticed if checksums for
    log blocks are disabled. In such case it would be better to repair
    it now instead of relying on the broken value and risking data loss.
    We emit warning to notice user about the situation. We repair that
    only in the log buffer. */
    /* 我们不能在第一个块中以无效的第一个记录组开始，因为如果我们崩溃了，我们可能无法恢复。
    我们不能保证第一个记录组是正确的，因为恢复没有报告错误。第一个记录组仅用于定位恢复的日志的开头。
    对于后续块，它没有使用。如果日志块的校验和被禁用，它可能在磁盘上损坏并保持未被注意到。
    在这种情况下，最好现在修复它，而不是依赖损坏的值并冒数据丢失的风险。我们发出警告以通知用户这种情况。
    我们只在日志缓冲区中修复它。 */

    ib::warn(ER_IB_RECV_FIRST_REC_GROUP_INVALID,
             uint(log_block_get_first_rec_group(recv_sys->last_block)),
             uint(recv_sys->last_block_first_rec_group)); // 打印警告信息

    log_block_set_first_rec_group(recv_sys->last_block,
                                  recv_sys->last_block_first_rec_group); // 设置第一个记录组

  } else if (log_block_get_first_rec_group(recv_sys->last_block) == 0) { // 如果第一个记录组为 0
    /* Again, if it was zero, for any reason, we prefer to fix it
    before starting (we emit warning). */
    /* 同样，如果它为 0，出于任何原因，我们更愿意在开始之前修复它（我们发出警告）。 */

    ib::warn(ER_IB_RECV_FIRST_REC_GROUP_INVALID, uint(0),
             uint(recovered_lsn % OS_FILE_LOG_BLOCK_SIZE)); // 打印警告信息

    log_block_set_first_rec_group(recv_sys->last_block,
                                  recovered_lsn % OS_FILE_LOG_BLOCK_SIZE); // 设置第一个记录组
  }

  ut_d(log.first_block_is_correct_for_lsn = recovered_lsn); // 调试断言第一个块正确

  /* Disallow checkpoints until recovery is finished, and changes gathered
  in recv_sys->recovered_metadata (srv_dict_metadata) are transferred to
  dict_table_t objects (happens in srv0start.cc). */
  /* 在恢复完成之前不允许检查点，并且将 recv_sys->recovered_metadata（srv_dict_metadata）中收集的更改传输到 dict_table_t 对象（在 srv0start.cc 中发生）。 */

  err = log_start(log, checkpoint_lsn, recovered_lsn, recv_sys->last_block,
                  false); // 启动日志
  if (err != DB_SUCCESS) { // 如果启动失败
    return err; // 返回错误
  }

  /* Make the preservation of max checkpoint info on disk certain by writing
  the checkpoint also to the other checkpoint header. After that both headers
  will have the same checkpoint_lsn. This is an extra protection in case next
  checkpoint write will become corrupted because of crash during the write. */
  /* 通过将检查点写入另一个检查点头部来确保磁盘上最大检查点信息的保存。之后，两个头部将具有相同的检查点 LSN。
  这是额外的保护，以防下一个检查点写入在写入期间崩溃而损坏。 */

  if (!srv_read_only_mode) { // 如果不是只读模式
    log.next_checkpoint_header_no =
        log_next_checkpoint_header(checkpoint.m_checkpoint_header_no); // 获取下一个检查点头部编号

      err = log_files_next_checkpoint(log, checkpoint_lsn); // 写入下一个检查点
    if (err != DB_SUCCESS) { // 如果写入失败
      return err; // 返回错误
    }
  }

  mutex_enter(&recv_sys->mutex); // 进入互斥锁
    recv_sys->apply_log_recs = true; // 设置应用日志记录标志
  mutex_exit(&recv_sys->mutex); // 退出互斥锁

  /* The database is now ready to start almost normal processing of user
  transactions: transaction rollbacks and the application of the log
  records in the hash table can be run in background. */
  /* 数据库现在准备开始几乎正常的用户事务处理：事务回滚和哈希表中日志记录的应用可以在后台运行。 */

  return DB_SUCCESS; // 返回成功
}

/** Check the page type, if there is a mismtach then throw
fatal error. It may so happen that data file before 5.7 GA version
may contain uninitialized bytes in the FIL_PAGE_TYPE field.
@param[in]  page_id         Page id to verify
@param[in]  type            Expected page type
*/
static void verify_page_type(page_id_t page_id, page_type_t type) {
  mtr_t mtr;
  mtr_start(&mtr);
  /* We should not write to redo log before checkpointing is enabled as it risks
  running out of space, and we don't expect to write anything in this mtr.
  It should be read only */
  mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

  const auto *block =
      buf_page_get(page_id, univ_page_size, RW_S_LATCH, UT_LOCATION_HERE, &mtr);

  const auto page_type = fil_page_get_type(block->frame);
  if (page_type != type) {
    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_INVALID_PAGE_TYPE, unsigned{type},
              unsigned{page_type}, ulong{page_id.space()},
              ulong{page_id.page_no()});
  }
  mtr_commit(&mtr);
}

MetadataRecover *recv_recovery_from_checkpoint_finish(bool aborting) {
  /* Restore state. */
  /* 恢复状态。 */
  if (recv_sys->is_meb_db) dblwr::g_mode = recv_sys->dblwr_state;

  /* Free the resources of the recovery system */
  /* 释放恢复系统的资源 */
  recv_recovery_on = false;

  /* Now wait for currently in progress batches to finish. */
  /* 现在等待正在进行的批处理完成。 */
  buf_flush_wait_LRU_batch_end();

  MetadataRecover *metadata;

  if (!aborting) {
    metadata = recv_sys->metadata_recover;

    recv_sys->metadata_recover = nullptr;
  } else {
    metadata = nullptr;
  }

  recv_sys_free();

  if (!aborting) {
    /* Validate a few system page types that were left uninitialized
    by older versions of MySQL. */
    /* 验证一些旧版本 MySQL 未初始化的系统页面类型。 */
    verify_page_type({IBUF_SPACE_ID, FSP_IBUF_HEADER_PAGE_NO},
                     FIL_PAGE_TYPE_SYS);
    verify_page_type({TRX_SYS_SPACE, FSP_FIRST_RSEG_PAGE_NO},
                     FIL_PAGE_TYPE_SYS);
    verify_page_type({TRX_SYS_SPACE, TRX_SYS_PAGE_NO}, FIL_PAGE_TYPE_TRX_SYS);
    verify_page_type({TRX_SYS_SPACE, FSP_DICT_HDR_PAGE_NO}, FIL_PAGE_TYPE_SYS);
  }

  /* Free up the flush_rbt. */
  /* 释放 flush_rbt。 */
  buf_flush_free_flush_rbt();

  return metadata;
}

#endif /* !UNIV_HOTBACKUP */

#if defined(UNIV_DEBUG) || defined(UNIV_HOTBACKUP)
/** Return string name of the redo log record type.
@param[in]      type    record log record enum
@return string name of record log record */
const char *get_mlog_string(mlog_id_t type) {
  switch (type) {
    case MLOG_SINGLE_REC_FLAG:
      return "MLOG_SINGLE_REC_FLAG";

    case MLOG_1BYTE:
      return "MLOG_1BYTE";

    case MLOG_2BYTES:
      return "MLOG_2BYTES";

    case MLOG_4BYTES:
      return "MLOG_4BYTES";

    case MLOG_8BYTES:
      return "MLOG_8BYTES";

    case MLOG_REC_INSERT_8027:
      return "MLOG_REC_INSERT_8027";

    case MLOG_REC_CLUST_DELETE_MARK_8027:
      return "MLOG_REC_CLUST_DELETE_MARK_8027";

    case MLOG_REC_SEC_DELETE_MARK:
      return "MLOG_REC_SEC_DELETE_MARK";

    case MLOG_REC_UPDATE_IN_PLACE_8027:
      return "MLOG_REC_UPDATE_IN_PLACE_8027";

    case MLOG_REC_DELETE_8027:
      return "MLOG_REC_DELETE_8027";

    case MLOG_LIST_END_DELETE_8027:
      return "MLOG_LIST_END_DELETE_8027";

    case MLOG_LIST_START_DELETE_8027:
      return "MLOG_LIST_START_DELETE_8027";

    case MLOG_LIST_END_COPY_CREATED_8027:
      return "MLOG_LIST_END_COPY_CREATED_8027";

    case MLOG_PAGE_REORGANIZE_8027:
      return "MLOG_PAGE_REORGANIZE_8027";

    case MLOG_PAGE_CREATE:
      return "MLOG_PAGE_CREATE";

    case MLOG_UNDO_INSERT:
      return "MLOG_UNDO_INSERT";

    case MLOG_UNDO_ERASE_END:
      return "MLOG_UNDO_ERASE_END";

    case MLOG_UNDO_INIT:
      return "MLOG_UNDO_INIT";

    case MLOG_UNDO_HDR_REUSE:
      return "MLOG_UNDO_HDR_REUSE";

    case MLOG_UNDO_HDR_CREATE:
      return "MLOG_UNDO_HDR_CREATE";

    case MLOG_REC_MIN_MARK:
      return "MLOG_REC_MIN_MARK";

    case MLOG_IBUF_BITMAP_INIT:
      return "MLOG_IBUF_BITMAP_INIT";

#ifdef UNIV_LOG_LSN_DEBUG
    case MLOG_LSN:
      return "MLOG_LSN";
#endif /* UNIV_LOG_LSN_DEBUG */

    case MLOG_INIT_FILE_PAGE:
      return "MLOG_INIT_FILE_PAGE";

    case MLOG_WRITE_STRING:
      return "MLOG_WRITE_STRING";

    case MLOG_MULTI_REC_END:
      return "MLOG_MULTI_REC_END";

    case MLOG_DUMMY_RECORD:
      return "MLOG_DUMMY_RECORD";

    case MLOG_FILE_DELETE:
      return "MLOG_FILE_DELETE";

    case MLOG_COMP_REC_MIN_MARK:
      return "MLOG_COMP_REC_MIN_MARK";

    case MLOG_COMP_PAGE_CREATE:
      return "MLOG_COMP_PAGE_CREATE";

    case MLOG_COMP_REC_INSERT_8027:
      return "MLOG_COMP_REC_INSERT_8027";

    case MLOG_COMP_REC_CLUST_DELETE_MARK_8027:
      return "MLOG_COMP_REC_CLUST_DELETE_MARK_8027";

    case MLOG_COMP_REC_SEC_DELETE_MARK:
      return "MLOG_COMP_REC_SEC_DELETE_MARK";

    case MLOG_COMP_REC_UPDATE_IN_PLACE_8027:
      return "MLOG_COMP_REC_UPDATE_IN_PLACE_8027";

    case MLOG_COMP_REC_DELETE_8027:
      return "MLOG_COMP_REC_DELETE_8027";

    case MLOG_COMP_LIST_END_DELETE_8027:
      return "MLOG_COMP_LIST_END_DELETE_8027";

    case MLOG_COMP_LIST_START_DELETE_8027:
      return "MLOG_COMP_LIST_START_DELETE_8027";

    case MLOG_COMP_LIST_END_COPY_CREATED_8027:
      return "MLOG_COMP_LIST_END_COPY_CREATED_8027";

    case MLOG_COMP_PAGE_REORGANIZE_8027:
      return "MLOG_COMP_PAGE_REORGANIZE_8027";

    case MLOG_FILE_CREATE:
      return "MLOG_FILE_CREATE";

    case MLOG_ZIP_WRITE_NODE_PTR:
      return "MLOG_ZIP_WRITE_NODE_PTR";

    case MLOG_ZIP_WRITE_BLOB_PTR:
      return "MLOG_ZIP_WRITE_BLOB_PTR";

    case MLOG_ZIP_WRITE_HEADER:
      return "MLOG_ZIP_WRITE_HEADER";

    case MLOG_ZIP_PAGE_COMPRESS:
      return "MLOG_ZIP_PAGE_COMPRESS";

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027:
      return "MLOG_ZIP_PAGE_COMPRESS_NO_DATA_8027";

    case MLOG_ZIP_PAGE_REORGANIZE_8027:
      return "MLOG_ZIP_PAGE_REORGANIZE_8027";

    case MLOG_FILE_RENAME:
      return "MLOG_FILE_RENAME";

    case MLOG_FILE_EXTEND:
      return "MLOG_FILE_EXTEND";

    case MLOG_PAGE_CREATE_RTREE:
      return "MLOG_PAGE_CREATE_RTREE";

    case MLOG_COMP_PAGE_CREATE_RTREE:
      return "MLOG_COMP_PAGE_CREATE_RTREE";

    case MLOG_INIT_FILE_PAGE2:
      return "MLOG_INIT_FILE_PAGE2";

    case MLOG_INDEX_LOAD:
      return "MLOG_INDEX_LOAD";

      /* Disabled for WL6378
      case MLOG_TRUNCATE:
              return "MLOG_TRUNCATE";
      */

    case MLOG_TABLE_DYNAMIC_META:
      return "MLOG_TABLE_DYNAMIC_META";

    case MLOG_PAGE_CREATE_SDI:
      return "MLOG_PAGE_CREATE_SDI";

    case MLOG_COMP_PAGE_CREATE_SDI:
      return "MLOG_COMP_PAGE_CREATE_SDI";

    case MLOG_REC_INSERT:
      return "MLOG_REC_INSERT";

    case MLOG_REC_CLUST_DELETE_MARK:
      return "MLOG_REC_CLUST_DELETE_MARK";

    case MLOG_REC_DELETE:
      return "MLOG_REC_DELETE";

    case MLOG_REC_UPDATE_IN_PLACE:
      return "MLOG_REC_UPDATE_IN_PLACE";

    case MLOG_LIST_END_COPY_CREATED:
      return "MLOG_LIST_END_COPY_CREATED";

    case MLOG_PAGE_REORGANIZE:
      return "MLOG_PAGE_REORGANIZE";

    case MLOG_ZIP_PAGE_REORGANIZE:
      return "MLOG_ZIP_PAGE_REORGANIZE";

    case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
      return "MLOG_ZIP_PAGE_COMPRESS_NO_DATA";

    case MLOG_LIST_END_DELETE:
      return "MLOG_LIST_END_DELETE";

    case MLOG_LIST_START_DELETE:
      return "MLOG_LIST_START_DELETE";

    case MLOG_TEST:
      return "MLOG_TEST";
  }

  assert(0);

  return nullptr;
}
#endif /* UNIV_DEBUG || UNIV_HOTBACKUP */
