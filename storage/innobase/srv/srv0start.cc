/*****************************************************************************

Copyright (c) 1996, 2022, Oracle and/or its affiliates.
Copyright (c) 2008, Google Inc.
Copyright (c) 2009, Percona Inc.

Portions of this file contain modifications contributed and copyrighted by
Google, Inc. Those modifications are gratefully acknowledged and are described
briefly in the InnoDB documentation. The contributions by Google are
incorporated with their permission, and subject to the conditions contained in
the file COPYING.Google.

Portions of this file contain modifications contributed and copyrighted
by Percona Inc.. Those modifications are
gratefully acknowledged and are described briefly in the InnoDB
documentation. The contributions by Percona Inc. are incorporated with
their permission, and subject to the conditions contained in the file
COPYING.Percona.

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

/** @file srv/srv0start.cc
 Starts the InnoDB database server

 Created 2/16/1996 Heikki Tuuri
 *************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <zlib.h>

#include "my_dbug.h"

#include "btr0btr.h"
#include "btr0cur.h"
#include "buf0buf.h"
#include "buf0dump.h"
#include "current_thd.h"
#include "data0data.h"
#include "data0type.h"
#include "dict0dd.h"
#include "dict0dict.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fsp0sysspace.h"
#include "ha_prototypes.h"
#include "ibuf0ibuf.h"
#include "log0buf.h"
#include "log0chkp.h"
#include "log0recv.h"
#include "log0write.h"
#include "mem0mem.h"
#include "mtr0mtr.h"

#include "my_dbug.h"
#include "my_psi_config.h"
#include "mysql/psi/mysql_stage.h"
#include "mysqld.h"

#include "ddl0fts.h"
#include "os0file.h"
#include "os0thread-create.h"
#include "os0thread.h"
#include "page0cur.h"
#include "page0page.h"
#include "rem0rec.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "ut0mem.h"

#include <zlib.h>

#include "arch0arch.h"
#include "arch0recv.h"
#include "btr0pcur.h"
#include "btr0sea.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "clone0api.h"
#include "clone0clone.h"
#include "dict0boot.h"
#include "dict0crea.h"
#include "dict0load.h"
#include "dict0stats_bg.h"
#include "lock0lock.h"
#include "log0meb.h"
#include "os0event.h"
#include "os0proc.h"
#include "pars0pars.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "row0row.h"
#include "row0sel.h"
#include "row0upd.h"
#include "srv0tmp.h"
#include "trx0purge.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "usr0sess.h"
#include "ut0crc32.h"
#include "ut0new.h"

/** fil_space_t::flags for hard-coded tablespaces */
extern uint32_t predefined_flags;

/** Recovered persistent metadata */
static MetadataRecover *srv_dict_metadata;

/** true if a raw partition is in use */
bool srv_start_raw_disk_in_use = false;

/** Number of IO threads to use */
ulint srv_n_file_io_threads = 0;

/** true if the server is being started */
bool srv_is_being_started = false;
/** true if SYS_TABLESPACES is available for lookups */
bool srv_sys_tablespaces_open = false;
/** true if the server is being started, before rolling back any
incomplete transactions */
bool srv_startup_is_before_trx_rollback_phase = false;
/** true if srv_start() has been called */
static bool srv_start_has_been_called = false;

/** Bit flags for tracking background thread creation. They are used to
determine which threads need to be stopped if we need to abort during
the initialisation step. */
enum srv_start_state_t {
  /** No thread started */
  SRV_START_STATE_NONE = 0,
  /** Started IO threads */
  SRV_START_STATE_IO = 1,
  /** Started purge thread(s) */
  SRV_START_STATE_PURGE = 2,
  /** Started bufdump + dict stat and FTS optimize thread. */
  SRV_START_STATE_STAT = 4
};

/** Track server thrd starting phases */
static uint64_t srv_start_state = SRV_START_STATE_NONE;

std::atomic<enum srv_shutdown_t> srv_shutdown_state{SRV_SHUTDOWN_NONE};

/** Name of srv_monitor_file */
static char *srv_monitor_file_name;

/** */
#define SRV_MAX_N_PENDING_SYNC_IOS 100

/* Keys to register InnoDB threads with performance schema */
#ifdef UNIV_PFS_THREAD
mysql_pfs_key_t log_archiver_thread_key;
mysql_pfs_key_t page_archiver_thread_key;
mysql_pfs_key_t buf_dump_thread_key;
mysql_pfs_key_t buf_resize_thread_key;
mysql_pfs_key_t clone_ddl_thread_key;
mysql_pfs_key_t clone_gtid_thread_key;
mysql_pfs_key_t ddl_thread_key;
mysql_pfs_key_t dict_stats_thread_key;
mysql_pfs_key_t fts_optimize_thread_key;
mysql_pfs_key_t fts_parallel_merge_thread_key;
mysql_pfs_key_t fts_parallel_tokenization_thread_key;
mysql_pfs_key_t io_handler_thread_key;
mysql_pfs_key_t io_ibuf_thread_key;
mysql_pfs_key_t io_log_thread_key;
mysql_pfs_key_t io_read_thread_key;
mysql_pfs_key_t io_write_thread_key;
mysql_pfs_key_t srv_error_monitor_thread_key;
mysql_pfs_key_t srv_lock_timeout_thread_key;
mysql_pfs_key_t srv_master_thread_key;
mysql_pfs_key_t srv_monitor_thread_key;
mysql_pfs_key_t srv_purge_thread_key;
mysql_pfs_key_t srv_worker_thread_key;
mysql_pfs_key_t trx_recovery_rollback_thread_key;
mysql_pfs_key_t srv_ts_alter_encrypt_thread_key;
mysql_pfs_key_t parallel_rseg_init_thread_key;
#endif /* UNIV_PFS_THREAD */

#ifdef HAVE_PSI_STAGE_INTERFACE
/** Array of all InnoDB stage events for monitoring activities via
performance schema. */
static PSI_stage_info *srv_stages[] = {
    &srv_stage_alter_table_end,
    &srv_stage_alter_table_flush,
    &srv_stage_alter_table_insert,
    &srv_stage_alter_table_log_index,
    &srv_stage_alter_table_log_table,
    &srv_stage_alter_table_merge_sort,
    &srv_stage_alter_table_read_pk_internal_sort,
    &srv_stage_alter_tablespace_encryption,
    &srv_stage_buffer_pool_load,
    &srv_stage_clone_file_copy,
    &srv_stage_clone_redo_copy,
    &srv_stage_clone_page_copy,
};
#endif /* HAVE_PSI_STAGE_INTERFACE */

/** Sleep time in loops which wait for pending tasks during shutdown. */
static constexpr uint32_t SHUTDOWN_SLEEP_TIME_US = 100;

/** Number of wait rounds during shutdown, after which error is produced,
or other policy for timed out wait is applied. */
static constexpr uint32_t SHUTDOWN_SLEEP_ROUNDS =
    60 * 1000 * 1000 / SHUTDOWN_SLEEP_TIME_US;

static std::atomic<ulint> io_tid_i(0);

/** I/o-handler thread function.
@param[in]      segment         The AIO segment the thread will work on */
static void io_handler_thread(ulint segment) {
  const auto tid_i = io_tid_i.fetch_add(1, std::memory_order_relaxed);
  ut_ad(tid_i < srv_n_file_io_threads);
  srv_io_tids[tid_i] = os_thread_get_tid();
  const auto actual_priority =
      os_thread_set_priority(srv_io_tids[tid_i], srv_sched_priority_io);
  if (UNIV_UNLIKELY(actual_priority != srv_sched_priority_purge))
    ib::warn() << "Failed to set I/O thread priority to "
               << srv_sched_priority_master << " the current priority is "
               << actual_priority;

  while (srv_shutdown_state.load() != SRV_SHUTDOWN_EXIT_THREADS ||
         buf_flush_page_cleaner_is_active() || !os_aio_all_slots_free() ||
         buf_flush_active_lru_managers() > 0) {
    fil_aio_wait(segment);
  }
}

/** Create undo tablespace.
@param[in]  undo_space  Undo Tablespace
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespace_create(undo::Tablespace &undo_space) {
  pfs_os_file_t fh;
  bool ret;
  dberr_t err = DB_SUCCESS;
  char *file_name = undo_space.file_name();
  space_id_t space_id = undo_space.id();

  ut_a(!srv_read_only_mode);
  ut_a(!srv_force_recovery);

  os_file_create_subdirs_if_needed(file_name);

  /* Until this undo tablespace can become active, keep a truncate log
  file around so that if a crash happens it can be rebuilt at startup. */
  err = undo::start_logging(&undo_space);
  if (err != DB_SUCCESS) {
    ib::error(ER_IB_MSG_1070, undo_space.log_file_name(),
              undo_space.space_name());
  }
  ut_ad(err == DB_SUCCESS);

  fh = os_file_create(innodb_data_file_key, file_name,
                      (srv_read_only_mode ? OS_FILE_OPEN : OS_FILE_CREATE) |
                          OS_FILE_ON_ERROR_NO_EXIT,
                      OS_FILE_NORMAL, OS_DATA_FILE, srv_read_only_mode, &ret);

  if (ret == false) {
    std::ostringstream stmt;

    if (os_file_get_last_error(false) == OS_FILE_ALREADY_EXISTS) {
      stmt << " since '" << file_name << "' already exists.";
    } else {
      stmt << ". os_file_create() returned " << ret << ".";
    }

    ib::error(ER_IB_MSG_1214, undo_space.space_name(), stmt.str().c_str());

    err = DB_ERROR;
  } else {
    ut_a(!srv_read_only_mode);

    /* We created the data file and now write it full of zeros */
    undo_space.set_new();

    ib::info(ER_IB_MSG_1071, file_name);

    ulint size_mb = UNDO_INITIAL_SIZE >> 20;

    ib::info(ER_IB_MSG_1072, file_name, ulonglong{size_mb});

    ib::info(ER_IB_MSG_1073);

    ret = os_file_set_size(file_name, fh, 0, UNDO_INITIAL_SIZE, true);

    DBUG_EXECUTE_IF("ib_undo_tablespace_create_fail", ret = false;);

    if (!ret) {
      ib::info(ER_IB_MSG_1074, file_name);
      err = DB_OUT_OF_FILE_SPACE;
    }

    os_file_close(fh);

    /* Add this space to the list of undo tablespaces to
    construct by creating header pages. If an old undo
    tablespace needed fixup before it is upgraded,
    there is no need to construct it.*/
    if (undo::is_reserved(space_id)) {
      undo::add_space_to_construction_list(space_id);
    }
  }

  return (err);
}

/** Try to enable encryption of an undo log tablespace.
@param[in]      space_id        undo tablespace id
@return DB_SUCCESS if success */
static dberr_t srv_undo_tablespace_enable_encryption(space_id_t space_id) {
  dberr_t err;

  ut_ad(Encryption::check_keyring());

  /* Set the space flag.  The encryption metadata
  will be generated in fsp_header_init later. */
  fil_space_t *space = fil_space_get(space_id);
  if (!FSP_FLAGS_GET_ENCRYPTION(space->flags)) {
    fsp_flags_set_encryption(space->flags);
    err = fil_set_encryption(space_id, Encryption::AES, nullptr, nullptr);
    if (err != DB_SUCCESS) {
      ib::error(ER_IB_MSG_1075, space->name);
      return (err);
    }
  }

  return (DB_SUCCESS);
}

/** Try to read encryption metadata from an undo tablespace.
@param[in]      fh              file handle of undo log file
@param[in]      file_name       file name
@param[in]      space           undo tablespace
@return DB_SUCCESS if success */
static dberr_t srv_undo_tablespace_read_encryption(pfs_os_file_t fh,
                                                   const char *file_name,
                                                   fil_space_t *space) {
  IORequest request;
  ulint n_read = 0;
  size_t page_size = UNIV_PAGE_SIZE_MAX;
  dberr_t err = DB_ERROR;

  /* Align the memory for a possible read from a raw device */
  byte *first_page = static_cast<byte *>(
      ut::aligned_alloc(UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE));

  /* Don't want unnecessary complaints about partial reads. */
  request.disable_partial_io_warnings();

  err = os_file_read_no_error_handling(request, file_name, fh, first_page, 0,
                                       page_size, &n_read);

  if (err != DB_SUCCESS) {
    ib::info(ER_IB_MSG_1076, space->name, ut_strerr(err));
    ut::aligned_free(first_page);
    return (err);
  }

  ulint offset;
  const page_size_t space_page_size(space->flags);

  offset = fsp_header_get_encryption_offset(space_page_size);
  ut_ad(offset);

  /* Return if the encryption metadata is empty. */
  if (!Encryption::is_encrypted_with_v3(first_page + offset) &&
      !(srv_is_upgrade_mode &&
        memcmp(first_page + offset, Encryption::KEY_MAGIC_V2,
               Encryption::MAGIC_SIZE) == 0)) {
    ut::aligned_free(first_page);
    return (DB_SUCCESS);
  }

  byte key[Encryption::KEY_LEN];
  byte iv[Encryption::KEY_LEN];
  Encryption_key e_key{key, iv};
  if (fsp_header_get_encryption_key(space->flags, e_key, first_page)) {
    fsp_flags_set_encryption(space->flags);
    err = fil_set_encryption(space->id, Encryption::AES, key, iv);
    ut_ad(err == DB_SUCCESS);
  } else {
    ut::aligned_free(first_page);
    return (DB_FAIL);
  }

  ut::aligned_free(first_page);
  ib::info(ER_IB_MSG_UNDO_ENCRYPTION_INFO_LOADED, space->name);

  return (DB_SUCCESS);
}

/** Fix up a v5.7 type undo tablespace that was being truncated.
The space_id is not a reserved undo space_id. We will just delete
the file since it will be replaced.
@param[in]  space_id  Tablespace ID
@return error code */
static dberr_t srv_undo_tablespace_fixup_57(space_id_t space_id) {
  space_id_t space_num = undo::id2num(space_id);
  ut_ad(space_num == space_id);
  if (undo::is_active_truncate_log_present(space_num)) {
    ib::info(ER_IB_MSG_1077, ulong{space_num});

    if (srv_read_only_mode) {
      ib::error(ER_IB_MSG_1078);
      return (DB_READ_ONLY);
    }

    undo::Tablespace undo_space(space_id);

    /* Flush any changes recovered in REDO */
    fil_flush(space_id);
    fil_space_close(space_id);

    os_file_delete_if_exists(innodb_data_file_key, undo_space.file_name(),
                             nullptr);

    return (DB_TABLESPACE_DELETED);
  }

  return (DB_SUCCESS);
}

/** Start the fix-up process on an undo tablespace if it was in the process
of being truncated when the server crashed. At this point, just delete the
old file if it exists.
We could do the whole reconstruction here for implicit undo spaces since we
know the space_id, space_name, and file_name implicitly.  But for explicit
undo spaces, we must wait for the DD to be scanned in boot_tablespaces()
in order to know the space_id, space_name, and file_name.
@param[in]  space_num  undo tablespace number
@return error code */
static dberr_t srv_undo_tablespace_fixup_num(space_id_t space_num) {
  if (!undo::is_active_truncate_log_present(space_num)) {
    return (DB_SUCCESS);
  }

  ib::info(ER_IB_MSG_1077, ulong{space_num});

  if (srv_read_only_mode) {
    ib::error(ER_IB_MSG_1078);
    return (DB_READ_ONLY);
  }

  /*
    Search for a file that is using any of the space IDs assigned to this
    undo number. The directory scan assured that there are no duplicate files
    with the same space_id or with the same undo space number.
   */
  space_id_t space_id = SPACE_UNKNOWN;
  std::string scanned_name;
  fil_system_get_file_by_space_num(space_num, space_id, scanned_name);

  /* If the previous file still exists, delete it. */
  if (scanned_name.length() > 0) {
    /* Flush any changes recovered in REDO */
    fil_flush(space_id);
    fil_space_close(space_id);
    os_file_delete_if_exists(innodb_data_file_key, scanned_name.c_str(),
                             nullptr);

  } else if (space_num < FSP_IMPLICIT_UNDO_TABLESPACES) {
    /* If there is any file with the implicit file name, delete it. */
    undo::Tablespace undo_space(undo::num2id(space_num, 0));
    os_file_delete_if_exists(innodb_data_file_key, undo_space.file_name(),
                             nullptr);
  }

  return (DB_SUCCESS);
}

/** Fix up an undo tablespace if it was in the process of being truncated
when the server crashed. This is the second call and is done after the DD
is available so now we know the space_name, file_name and previous space_id.
@param[in]  space_name  undo tablespace name
@param[in]  file_name   undo tablespace file name
@param[in]  space_id    undo tablespace ID
@return error code */
dberr_t srv_undo_tablespace_fixup(const char *space_name, const char *file_name,
                                  space_id_t space_id) {
  ut_ad(fsp_is_undo_tablespace(space_id));

  space_id_t space_num = undo::id2num(space_id);
  if (!undo::is_active_truncate_log_present(space_num)) {
    return (DB_SUCCESS);
  }

  if (srv_read_only_mode) {
    return (DB_READ_ONLY);
  }

  ib::info(ER_IB_MSG_1079, ulong{space_num});

  /* It is possible for an explicit undo tablespace to have been truncated and
  recreated but not yet written with a header page when a crash occurred.  In
  this case, the empty file would not have been scanned at startup and the
  first call to fixup did not know the filename.  Now that we know it, just
  delete any file with that name if it exists.  The dictionary claims it is
  an undo tablespace and there is a truncate log file present. */
  os_file_delete_if_exists(innodb_data_file_key, file_name, nullptr);

  /* Mark the space_id for this undo tablespace number as in-use. */
  undo::spaces->x_lock();
  undo::unuse_space_id(space_id);
  space_id_t new_space_id = undo::next_space_id(space_id);
  undo::use_space_id(new_space_id);
  undo::spaces->x_unlock();

  dberr_t err = srv_undo_tablespace_create(space_name, file_name, new_space_id);
  if (err != DB_SUCCESS) {
    return (err);
  }

  /* Update the DD with the new space ID and state. */
  undo::spaces->s_lock();
  undo::Tablespace *undo_space = undo::spaces->find(space_num);
  dd_space_states to_state;
  if (undo_space->is_inactive_explicit()) {
    to_state = DD_SPACE_STATE_EMPTY;
    undo_space->set_empty();
  } else {
    to_state = DD_SPACE_STATE_ACTIVE;
    undo_space->set_active();
  }
  undo::spaces->s_unlock();

  bool dd_result = dd_tablespace_get_mdl(space_name);
  if (dd_result == DD_SUCCESS) {
    dd_result =
        dd_tablespace_set_id_and_state(space_name, new_space_id, to_state);
  }
  if (dd_result != DD_SUCCESS) {
    err = DB_ERROR;
  }

  return (err);
}

/** Open an undo tablespace.
@param[in]  undo_space  Undo tablespace
@return DB_SUCCESS or error code */
dberr_t srv_undo_tablespace_open(undo::Tablespace &undo_space) {
  DBUG_EXECUTE_IF("ib_undo_tablespace_open_fail",
                  return (DB_CANNOT_OPEN_FILE););

  pfs_os_file_t fh;
  bool success;
  uint32_t flags;
  bool atomic_write;
  dberr_t err = DB_ERROR;
  space_id_t space_id = undo_space.id();
  char *undo_name = undo_space.space_name();
  char *file_name = undo_space.file_name();

  /* Check if it was already opened during redo recovery. */
  fil_space_t *space = fil_space_get(space_id);

  /* Flush and close any current file handle so we can open
  a local one below. */
  if (space != nullptr) {
    fil_flush(space_id);
    fil_space_close(space_id);
  }

  if (!os_file_check_mode(file_name, srv_read_only_mode)) {
    ib::error(ER_IB_MSG_1081, file_name,
              srv_read_only_mode ? "readable!" : "writable!");

    return (DB_READ_ONLY);
  }

  /* Open a local handle. */
  fh = os_file_create(
      innodb_data_file_key, file_name,
      OS_FILE_OPEN_RETRY | OS_FILE_ON_ERROR_NO_EXIT | OS_FILE_ON_ERROR_SILENT,
      OS_FILE_NORMAL, OS_DATA_FILE, srv_read_only_mode, &success);
  if (!success) {
    return (DB_CANNOT_OPEN_FILE);
  }

  /* Check if this file supports atomic write. */
#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
  if (!dblwr::is_enabled()) {
    atomic_write = fil_fusionio_enable_atomic_write(fh);
  } else {
    atomic_write = false;
  }
#else
  atomic_write = false;
#endif /* !NO_FALLOCATE && UNIV_LINUX */

  if (space == nullptr) {
    /* Load the tablespace into InnoDB's internal data structures.
    Set the compressed page size to 0 (non-compressed) */
    flags = fsp_flags_init(univ_page_size, false, false, false, false);
    space = fil_space_create(undo_name, space_id, flags, FIL_TYPE_TABLESPACE);
    ut_a(space != nullptr);
    ut_ad(fil_validate());

    os_offset_t size = os_file_get_size(fh);
    ut_a(size != (os_offset_t)-1);
    page_no_t n_pages = static_cast<page_no_t>(size / UNIV_PAGE_SIZE);

    if (fil_node_create(file_name, n_pages, space, false, atomic_write) ==
        nullptr) {
      os_file_close(fh);

      ib::error(ER_IB_MSG_1082, undo_name);

      return (DB_ERROR);
    }

  } else {
    auto &file = space->files.front();

    file.atomic_write = atomic_write;
  }

  /* Read the encryption metadata in this undo tablespace.
  If the encryption info in the first page cannot be decrypted
  by the master key, this table cannot be opened. */
  err = srv_undo_tablespace_read_encryption(fh, file_name, space);

  /* The file handle will no longer be needed. */
  success = os_file_close(fh);
  ut_ad(success);

  if (err != DB_SUCCESS) {
    ib::error(ER_IB_MSG_1083, undo_name);
    return (err);
  }

  /* Now that space and node exist, make sure this undo tablespace
  is open so that it stays open until shutdown.
  But if it is under construction, we cannot open it until the
  header page has been written. */
  if (!undo::is_under_construction(space_id)) {
    bool success = fil_space_open(space_id);
    ut_a(success);
  }

  if (undo::is_reserved(space_id)) {
    undo::spaces->add(undo_space);
  }

  return (DB_SUCCESS);
}

/** Open an undo tablespace with a specified space_id.
@param[in]      space_id        tablespace ID
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespace_open_by_id(space_id_t space_id) {
  undo::Tablespace undo_space(space_id);
  std::string scanned_name;

  /* If an undo tablespace with this space_id already exists,
  check if the name found in the file map for this undo space_id
  is the standard name.  The directory scan assured that there are
  no duplicates.  The filename found must match the standard name
  if this is an implicit undo tablespace. In other words, implicit
  undo tablespaces must be found in srv_undo_dir. */

  bool found = fil_system_get_file_by_space_id(space_id, scanned_name);

  if (found &&
      !Fil_path::is_same_as(undo_space.file_name(), scanned_name.c_str())) {
    ib::error(ER_IB_MSG_FOUND_WRONG_UNDO_SPACE, undo_space.file_name(),
              ulong{space_id}, scanned_name.c_str());
    return (DB_WRONG_FILE_NAME);
  }

  dberr_t err = srv_undo_tablespace_open(undo_space);

  if (err == DB_SUCCESS) {
    fil_space_set_undo_size(space_id, false);
  }

  return (err);
}

/** Open an undo tablespace with a specified undo number.
@param[in]  space_num  undo tablespace number
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespace_open_by_num(space_id_t space_num) {
  space_id_t space_id = SPACE_UNKNOWN;
  std::string scanned_name;

  /* Search for a file that is using any of the space IDs assigned to this
  undo number. The directory scan assured that there are no duplicate files
  with the same space_id or with the same undo space number. */
  if (!fil_system_get_file_by_space_num(space_num, space_id, scanned_name)) {
    return (DB_CANNOT_OPEN_FILE);
  }

  /* The first 2 undo space numbers must be implicit. */
  bool is_default = (space_num <= FSP_IMPLICIT_UNDO_TABLESPACES);

  /* v8.0.12 used innodb_undo_tablespaces to implicitly create undo
  spaces so there may be more than 2 implicit undo tablespaces.  They
  must match the default undo filename and must be found in
  srv_undo_directory. */
  undo::Tablespace undo_space(space_id);
  if (!Fil_path::is_same_as(undo_space.file_name(), scanned_name.c_str())) {
    if (is_default) {
      ib::info(ER_IB_MSG_1080, undo_space.file_name(), scanned_name.c_str(),
               ulong{space_id});

      return (DB_WRONG_FILE_NAME);
    }

    /* Explicit undo tablespaces must end with the suffix '.ibu'. */
    if (!Fil_path::has_suffix(IBU, scanned_name)) {
      ib::info(ER_IB_MSG_NOT_END_WITH_IBU, scanned_name.c_str());

      return (DB_WRONG_FILE_NAME);
    }

    /* Use the file name found in the scan. */
    undo_space.set_file_name(scanned_name.c_str());
  }

  /* Mark the space_id for this undo tablespace number as in-use. */
  undo::use_space_id(space_id);

  ib::info(ER_IB_MSG_USING_UNDO_SPACE, scanned_name.c_str());

  dberr_t err = srv_undo_tablespace_open(undo_space);

  if (err == DB_SUCCESS) {
    fil_space_set_undo_size(space_id, false);
  }

  return (err);
}

/* Open existing undo tablespaces up to the number in target_undo_tablespace.
If we are making a new database, these have been created.
If doing recovery, these should exist and may be needed for recovery.
If we fail to open any of these it is a fatal error.
@return DB_SUCCESS or error code */
/* 打开现有的 undo 表空间，数量达到 target_undo_tablespace。
如果我们正在创建一个新数据库，这些表空间已经被创建。
如果正在进行恢复，这些表空间应该存在并且可能需要用于恢复。
如果我们无法打开其中任何一个，这是一个致命错误。
@return DB_SUCCESS 或错误代码 */
static dberr_t srv_undo_tablespaces_open() {
  dberr_t err; // 错误码

  /* If upgrading from 5.7, build a list of existing undo tablespaces
  from the references in the TRX_SYS page. (not including the system
  tablespace) */
  /* 如果从 5.7 版本升级，从 TRX_SYS 页面中的引用构建现有 undo 表空间的列表。（不包括系统表空间） */
  trx_rseg_get_n_undo_tablespaces(trx_sys_undo_spaces); // 获取 TRX_SYS 页面中的 undo 表空间列表

  /* If undo tablespaces are being tracked in trx_sys then these
  will need to be replaced by independent undo tablespaces with
  reserved space_ids and RSEG_ARRAY pages. */
  /* 如果 undo 表空间在 trx_sys 中被跟踪，那么这些表空间需要被具有保留 space_id 和 RSEG_ARRAY 页的独立 undo 表空间替换。 */
  if (trx_sys_undo_spaces->size() > 0) { // 如果 TRX_SYS 页面中有 undo 表空间
    /* Open each undo tablespace tracked in TRX_SYS. */
    /* 打开 TRX_SYS 中跟踪的每个 undo 表空间。 */
    for (const auto space_id : *trx_sys_undo_spaces) { // 遍历每个 undo 表空间的 space_id
      fil_set_max_space_id_if_bigger(space_id); // 如果 space_id 更大，则设置为最大 space_id

      /* Check if this undo tablespace was in the process of being truncated.
      If so, just delete the file since it will be replaced. */
      /* 检查此 undo 表空间是否正在被截断。如果是这样，只需删除该文件，因为它将被替换。 */
      if (DB_TABLESPACE_DELETED == srv_undo_tablespace_fixup_57(space_id)) { // 如果表空间正在被截断
        continue; // 跳过此表空间
      }

      err = srv_undo_tablespace_open_by_id(space_id); // 打开指定 space_id 的 undo 表空间
      if (err != DB_SUCCESS) { // 如果打开失败
        ib::error(ER_IB_MSG_CANNOT_OPEN_57_UNDO, ulong{space_id}); // 打印错误信息
        return (err); // 返回错误码
      }
    }
  }

  /* Open all existing implicit and explicit undo tablespaces.
  The tablespace scan has completed and the undo::space_id_bank has been
  filled with the space Ids that were found. */
  /* 打开所有现有的隐式和显式 undo 表空间。表空间扫描已完成，undo::space_id_bank 已填充找到的空间 ID。 */
  undo::spaces->x_lock(); // 加锁
  ut_ad(undo::spaces->size() == 0); // 断言 undo 表空间列表为空

  for (space_id_t num = 1; num <= FSP_MAX_UNDO_TABLESPACES; ++num) { // 遍历所有可能的 undo 表空间编号
    /* Check if this undo tablespace was in the process of being truncated.
    If so, recreate it and add it to the construction list. */
    /* 检查此 undo 表空间是否正在被截断。如果是这样，重新创建它并将其添加到构建列表中。 */
    dberr_t err = srv_undo_tablespace_fixup_num(num); // 修复指定编号的 undo 表空间
    if (err != DB_SUCCESS) { // 如果修复失败
      undo::spaces->x_unlock(); // 解锁
      return (err); // 返回错误码
    }

    err = srv_undo_tablespace_open_by_num(num); // 打开指定编号的 undo 表空间
    switch (err) { // 根据错误码进行处理
      case DB_WRONG_FILE_NAME:
        /* An Undo tablespace was found where the mapping
        file said it was.  Now we have a different filename
        for it. The undo directory must have changed and
        the the files were not moved. Cannot startup. */
        /* 在映射文件所说的位置找到了一个 Undo 表空间。现在我们有一个不同的文件名。undo 目录必须已更改，并且文件未移动。无法启动。 */
      case DB_READ_ONLY:
        /* The undo tablespace was found where it should be
        but it cannot be opened in read/write mode. */
        /* 在应该在的位置找到了 undo 表空间，但无法以读/写模式打开它。 */
      default:
        /* The undo tablespace was found where it should be
        but it cannot be used. */
        /* 在应该在的位置找到了 undo 表空间，但无法使用它。 */
        undo::spaces->x_unlock(); // 解锁
        return (err); // 返回错误码

      case DB_SUCCESS:
        // 成功打开 undo 表空间
        break;

      case DB_CANNOT_OPEN_FILE:
        /* Doesn't exist, keep looking */
        /* 不存在，继续寻找 */
        break;
    }
  }

  ulint n_found_new = undo::spaces->size(); // 获取找到的新 undo 表空间数量
  ulint n_found_old = trx_sys_undo_spaces->size(); // 获取找到的旧 undo 表空间数量
  undo::spaces->x_unlock(); // 解锁

  if (n_found_old != 0 || n_found_new < FSP_IMPLICIT_UNDO_TABLESPACES) { // 如果找到的旧 undo 表空间数量不为 0 或找到的新 undo 表空间数量小于隐式 undo 表空间数量
    std::ostringstream msg; // 创建消息流

    if (n_found_old != 0) { // 如果找到的旧 undo 表空间数量不为 0
      msg << "Found " << n_found_old << " undo tablespaces that"
          << " need to be upgraded. "; // 添加需要升级的 undo 表空间数量信息
    }

    if (n_found_new < FSP_IMPLICIT_UNDO_TABLESPACES) { // 如果找到的新 undo 表空间数量小于隐式 undo 表空间数量
      msg << "Will create " << (FSP_IMPLICIT_UNDO_TABLESPACES - n_found_new)
          << " new undo tablespaces."; // 添加需要创建的新 undo 表空间数量信息
    }

    ib::info(ER_IB_MSG_1215) << msg.str(); // 打印信息
  }

  if (n_found_new + n_found_old) { // 如果找到的 undo 表空间数量不为 0
    ib::info(ER_IB_MSG_1085, ulonglong{n_found_new + n_found_old}); // 打印找到的 undo 表空间数量信息
  }

  return (DB_SUCCESS); // 返回成功
}

/** Create the implicit undo tablespaces if we are creating a new instance
or if there was not enough implicit undo tablespaces previously existing.
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespaces_create() {
  dberr_t err = DB_SUCCESS;

  undo::spaces->x_lock();

  ulint initial_implicit_undo_spaces = 0;
  for (auto undo_space : undo::spaces->m_spaces) {
    if (undo_space->num() <= FSP_IMPLICIT_UNDO_TABLESPACES) {
      initial_implicit_undo_spaces++;
    }
  }

  if (initial_implicit_undo_spaces >= FSP_IMPLICIT_UNDO_TABLESPACES) {
    undo::spaces->x_unlock();
    return (DB_SUCCESS);
  }

  if (srv_read_only_mode || srv_force_recovery > 0) {
    const char *mode;

    mode = srv_read_only_mode ? "read_only" : "force_recovery",

    ib::warn(ER_IB_MSG_1086, mode, ulonglong{initial_implicit_undo_spaces});

    if (initial_implicit_undo_spaces == 0) {
      ib::error(ER_IB_MSG_1087, mode);

      undo::spaces->x_unlock();
      return (DB_ERROR);
    }

    undo::spaces->x_unlock();
    return (DB_SUCCESS);
  }

  /* Create all implicit undo tablespaces that are needed. */
  for (space_id_t num = 1; num <= FSP_IMPLICIT_UNDO_TABLESPACES; ++num) {
    /* If the trunc log file is present, the fixup process will be
    finished later. */
    if (undo::is_active_truncate_log_present(num)) {
      continue;
    }

    /* Check if an independent undo space for this space_id
    has already been found. */
    if (undo::spaces->contains(num)) {
      continue;
    }

    /* Mark this implicit undo space number as used and return the next
    available space_id. */
    space_id_t space_id = undo::use_next_space_id(num);

    /* Since it is not found, create it. */
    undo::Tablespace undo_space(space_id);
    undo_space.set_new();
    err = srv_undo_tablespace_create(undo_space);
    if (err != DB_SUCCESS) {
      ib::info(ER_IB_MSG_1088, undo_space.space_name());
      break;
    }

    /* Open this new undo tablespace. */
    err = srv_undo_tablespace_open(undo_space);
    if (err != DB_SUCCESS) {
      ib::info(ER_IB_MSG_1089, int{err}, ut_strerr(err),
               undo_space.space_name());

      break;
    }
  }

  undo::spaces->x_unlock();

  ulint new_spaces =
      FSP_IMPLICIT_UNDO_TABLESPACES - initial_implicit_undo_spaces;

  ib::info(ER_IB_MSG_1090, ulonglong{new_spaces});

  return (err);
}

/** Finish building an undo tablespace. So far these tablespace files in
the construction list should be created and filled with zeros.
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespaces_construct() {
  mtr_t mtr;

  if (undo::s_under_construction.empty()) {
    return (DB_SUCCESS);
  }

  ut_a(!srv_read_only_mode);
  ut_a(!srv_force_recovery);

  if (srv_undo_log_encrypt && Encryption::check_keyring() == false) {
    my_error(ER_CANNOT_FIND_KEY_IN_KEYRING, MYF(0));
    return (DB_ERROR);
  }

  for (auto space_id : undo::s_under_construction) {
    /* Enable undo log encryption if it's ON. */
    if (srv_undo_log_encrypt) {
      dberr_t err = srv_undo_tablespace_enable_encryption(space_id);

      if (err != DB_SUCCESS) {
        ib::error(ER_IB_MSG_1091, ulong{undo::id2num(space_id)});

        return (err);
      }
    }

    log_free_check();

    mtr_start(&mtr);

    mtr_x_lock(fil_space_get_latch(space_id), &mtr, UT_LOCATION_HERE);

    if (!fsp_header_init(space_id, UNDO_INITIAL_SIZE_IN_PAGES, &mtr)) {
      ib::error(ER_IB_MSG_1093, ulong{undo::id2num(space_id)});

      mtr_commit(&mtr);
      return (DB_ERROR);
    }

    /* Add the RSEG_ARRAY page. */
    trx_rseg_array_create(space_id, &mtr);

    mtr_commit(&mtr);

    /* The rollback segments will get created later in
    trx_rseg_add_rollback_segments(). */
  }

  if (srv_undo_log_encrypt) {
    ut_d(bool ret =) srv_enable_undo_encryption(nullptr);
    ut_ad(!ret);
  }

  return (DB_SUCCESS);
}

/** Mark the point in which the undo tablespaces in the construction list
are fully constructed and ready to use. */
static void srv_undo_tablespaces_mark_construction_done() {
  /* Remove the truncate log files if they exist. */
  for (auto space_id : undo::s_under_construction) {
    /* Flush these pages to disk since they were not redo logged. */
    auto flush_observer = ut::new_withkey<Flush_observer>(
        UT_NEW_THIS_FILE_PSI_KEY, space_id, nullptr, nullptr);

    flush_observer->flush();
    ut::delete_(flush_observer);

    space_id_t space_num = undo::id2num(space_id);
    if (undo::is_active_truncate_log_present(space_num)) {
      undo::done_logging(space_num);
    }
  }

  undo::clear_construction_list();
}

/** Upgrade undo tablespaces by deleting the old undo tablespaces
referenced by the TRX_SYS page.
@return error code */
dberr_t srv_undo_tablespaces_upgrade() {
  if (trx_sys_undo_spaces->empty()) {
    goto cleanup;
  }

  /* Recovered transactions in the prepared state prevent the old
  rsegs and undo tablespaces they are in from being deleted.
  These transactions must be either committed or rolled back by
  the mysql server.*/
  if (trx_sys->n_prepared_trx > 0) {
    ib::warn(ER_IB_MSG_1094);
    return (DB_SUCCESS);
  }

  ib::info(ER_IB_MSG_1095, trx_sys_undo_spaces->size(),
           ulong{FSP_IMPLICIT_UNDO_TABLESPACES});

  /* All Undo Tablespaces found in the TRX_SYS page need to be
  deleted. The new independent undo tablespaces were created in
  in srv_undo_tablespaces_create() */
  for (const auto space_id : *trx_sys_undo_spaces) {
    undo::Tablespace undo_space(space_id);

    fil_space_close(undo_space.id());

    auto err = fil_delete_tablespace(undo_space.id(), BUF_REMOVE_ALL_NO_WRITE);

    if (err != DB_SUCCESS) {
      ib::warn(ER_IB_MSG_57_UNDO_SPACE_DELETE_FAIL, undo_space.space_name());
    }
  }

  /* All pages should be removed from the spaces we deleted. We just collect
  them now, so that the space_id -> shard mapping is correct - it will be
  changed the second the trx_sys_undo_spaces is cleared.*/
  fil_purge();

  /* Remove the tracking of these undo tablespaces from TRX_SYS page and
  trx_sys->rsegs. */
  trx_rseg_upgrade_undo_tablespaces();

  /* Since we now have new format undo tablespaces, we will no longer
  look for undo tablespaces or rollback segments in the TRX_SYS page
  or the trx_sys->rsegs vector. */
  trx_sys_undo_spaces->clear();

cleanup:
  /* Post 5.7 undo tablespaces track their own rsegs.
  Clear the list of rsegs in old undo tablespaces. */
  trx_sys->rsegs.clear();

  return (DB_SUCCESS);
}

/** Downgrade undo tablespaces by deleting the new undo tablespaces which
are not referenced by the TRX_SYS page. */
static void srv_undo_tablespaces_downgrade() {
  ut_ad(srv_downgrade_logs);

  ib::info(ER_IB_MSG_1096, ulonglong{undo::spaces->size()});

  /* All the new independent undo tablespaces that were created in
  in srv_undo_tablespaces_create() need to be deleted. */
  for (const auto undo_space : undo::spaces->m_spaces) {
    fil_space_close(undo_space->id());

    os_file_delete(innodb_data_file_key, undo_space->file_name());
  }
}

/** Create an undo tablespace with an explicit file name
This is called during CREATE UNDO TABLESPACE.
@param[in]  space_name  tablespace name
@param[in]  file_name   file name
@param[in]  space_id    Tablespace ID
@return DB_SUCCESS or error code */
dberr_t srv_undo_tablespace_create(const char *space_name,
                                   const char *file_name, space_id_t space_id) {
  if (srv_undo_log_encrypt && Encryption::check_keyring() == false) {
    my_error(ER_CANNOT_FIND_KEY_IN_KEYRING, MYF(0));
    return (DB_ERROR);
  }

  /* We need to x_lock the undo::spaces list until after this
  is created and added to it. */
  undo::spaces->x_lock();

  ut_ad(undo::spaces->find(undo::id2num(space_id)) == nullptr);

  undo::Tablespace undo_space(space_id);
  undo_space.set_space_name(space_name);
  undo_space.set_file_name(file_name);

  dberr_t err = srv_undo_tablespace_create(undo_space);
  if (err != DB_SUCCESS) {
    undo::spaces->x_unlock();
    goto cleanup_and_exit;
  }

  /* Open this new undo tablespace. */
  err = srv_undo_tablespace_open(undo_space);
  if (err != DB_SUCCESS) {
    ib::error(ER_IB_MSG_ERROR_OPENING_NEW_UNDO_SPACE, int{err}, space_name);
    undo::spaces->x_unlock();
    goto cleanup_and_exit;
  }

  /* Unlock the undo::spaces list now that we are no longer changing it.
  This new undo space will not be used by new transactions until it
  becomes active. */
  undo::spaces->x_unlock();

  /* Write header and RSEG_ARRAY pages to this undo tablespace. */
  err = srv_undo_tablespaces_construct();
  if (err != DB_SUCCESS) {
    goto cleanup_and_exit;
  }

  /* Create the rollback segments in this tablespace and add an Rseg object
  for each one to the Rsegs list. */
  if (!trx_rseg_init_rollback_segments(space_id, srv_rollback_segments)) {
    err = DB_ERROR;
    goto cleanup_and_exit;
  }

cleanup_and_exit:
  /* If UNDO tablespace couldn't initialize completely, remove it from
  undo tablespace list */
  if (err != DB_SUCCESS) {
    undo::spaces->x_lock();
    undo::spaces->drop(undo_space);
    undo::spaces->x_unlock();

    /* Remove undo tablespace file (if created) */
    os_file_delete_if_exists(innodb_data_file_key, undo_space.file_name(),
                             nullptr);
  }

  srv_undo_tablespaces_mark_construction_done();
  return (err);
}

/** Initialize undo::spaces and trx_sys_undo_spaces,
called once during srv_start(). */
void undo_spaces_init() {
  ut_ad(undo::spaces == nullptr);

  undo::spaces = ut::new_withkey<undo::Tablespaces>(
      ut::make_psi_memory_key(mem_key_undo_spaces));

  trx_sys_undo_spaces_init();

  undo::init_space_id_bank();
}

/** Free the resources occupied by undo::spaces and trx_sys_undo_spaces,
called once during thread de-initialization. */
void undo_spaces_deinit() {
  if (srv_downgrade_logs) {
    srv_undo_tablespaces_downgrade();
  }

  if (undo::spaces != nullptr) {
    /* There can't be any active transactions. */
    undo::spaces->clear();

    ut::delete_(undo::spaces);
    undo::spaces = nullptr;
  }

  trx_sys_undo_spaces_deinit();

  if (undo::space_id_bank != nullptr) {
    ut::delete_arr(undo::space_id_bank);
    undo::space_id_bank = nullptr;
  }
}

/** Open the configured number of implicit undo tablespaces.
@param[in]      create_new_db   true if new db being created
@return DB_SUCCESS or error code */
static dberr_t srv_undo_tablespaces_init(bool create_new_db) {
  dberr_t err = DB_SUCCESS;

  /* Open any existing implicit undo tablespaces. */
  /* 打开所有已存在的隐式 undo 表空间。 */
  if (!create_new_db) {
    err = srv_undo_tablespaces_open();
    if (err != DB_SUCCESS) {
      return (err);
    }
  }

  /* If this is opening an existing database, create and open any
  undo tablespaces that are still needed. For a new DB, create
  them all. */
  /* 如果这是在打开一个已存在的数据库，创建并打开所有仍然需要的 undo 表空间。
  对于一个新数据库，创建所有的 undo 表空间。 */
  mutex_enter(&undo::ddl_mutex);
  err = srv_undo_tablespaces_create();
  if (err != DB_SUCCESS) {
    mutex_exit(&undo::ddl_mutex);
    return (err);
  }

  /* Finish building any undo tablespaces just created by adding
  header pages, rseg_array pages, and rollback segments. Then delete
  any undo truncation log files and clear the construction list.
  This list includes any tablespace newly created or fixed-up. */
  /* 通过添加头页、rseg_array 页和回滚段来完成任何刚刚创建的 undo 表空间的构建。
  然后删除任何 undo 截断日志文件并清除构建列表。
  该列表包括任何新创建或修复的表空间。 */
  err = srv_undo_tablespaces_construct();
  if (err != DB_SUCCESS) {
    mutex_exit(&undo::ddl_mutex);
    return (err);
  }

  mutex_exit(&undo::ddl_mutex);
  return (DB_SUCCESS);
}

/********************************************************************
Wait for the purge thread(s) to start up. */
static void srv_start_wait_for_purge_to_start() {
  /* Wait for the purge coordinator and master thread to startup. */

  purge_state_t state = trx_purge_state();

  ut_a(state != PURGE_STATE_DISABLED);

  while (srv_shutdown_state.load() < SRV_SHUTDOWN_PURGE &&
         srv_force_recovery < SRV_FORCE_NO_BACKGROUND &&
         state == PURGE_STATE_INIT) {
    switch (state = trx_purge_state()) {
      case PURGE_STATE_RUN:
      case PURGE_STATE_STOP:
        break;

      case PURGE_STATE_INIT:
        ib::info(ER_IB_MSG_1097);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        break;

      case PURGE_STATE_EXIT:
      case PURGE_STATE_DISABLED:
        ut_error;
    }
  }
}

/** Create the temporary file tablespace.
@param[in]      create_new_db   whether we are creating a new database
@param[in,out]  tmp_space       Shared Temporary SysTablespace
@return DB_SUCCESS or error code. */
static dberr_t srv_open_tmp_tablespace(bool create_new_db,
                                       SysTablespace *tmp_space) {
  page_no_t sum_of_new_sizes;

  /* Will try to remove if there is existing file left-over by last
  unclean shutdown */
  tmp_space->set_sanity_check_status(true);
  tmp_space->delete_files();
  tmp_space->set_ignore_read_only(true);

  ib::info(ER_IB_MSG_1098);

  bool create_new_temp_space = true;

  tmp_space->set_space_id(dict_sys_t::s_temp_space_id);

  RECOVERY_CRASH(100);

  dberr_t err =
      tmp_space->check_file_spec(create_new_temp_space, 12 * 1024 * 1024);

  if (err == DB_FAIL) {
    ib::error(ER_IB_MSG_1099, tmp_space->name());

    err = DB_ERROR;

  } else if (err != DB_SUCCESS) {
    ib::error(ER_IB_MSG_1100, tmp_space->name());

  } else if ((err = tmp_space->open_or_create(true, create_new_db,
                                              &sum_of_new_sizes, nullptr)) !=
             DB_SUCCESS) {
    ib::error(ER_IB_MSG_1101, tmp_space->name());

  } else {
    mtr_t mtr;
    page_no_t size = tmp_space->get_sum_of_sizes();

    /* Open this shared temp tablespace in the fil_system so that
    it stays open until shutdown. */
    if (fil_space_open(tmp_space->space_id())) {
      if (srv_tmp_tablespace_encrypt) {
        /* Make sure the keyring is loaded. */
        if (!Encryption::check_keyring()) {
          srv_tmp_tablespace_encrypt = false;
          ib::error() << "Can't set temporary"
                      << " tablespace to be encrypted"
                      << " because keyring plugin is"
                      << " not available.";
          return (DB_ERROR);
        }
        fil_space_t *const space = fil_space_get(dict_sys_t::s_temp_space_id);
        err = fil_set_encryption(space->id, Encryption::AES, nullptr, nullptr);
        tmp_space->set_flags(space->flags);
        ut_a(err == DB_SUCCESS);
      }

      /* Initialize the header page */
      mtr_start(&mtr);
      mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

      fsp_header_init(tmp_space->space_id(), size, &mtr);

      mtr_commit(&mtr);
    } else {
      /* This file was just opened in the code above! */
      ib::error(ER_IB_MSG_1102, tmp_space->name());

      err = DB_ERROR;
    }
  }

  return (err);
}

/** Create SDI Indexes in system tablespace. */
static void srv_create_sdi_indexes() {
  btr_sdi_create_index(SYSTEM_TABLE_SPACE, false);
}

/** Set state to indicate start of particular group of threads in InnoDB. */
static inline void srv_start_state_set(
    srv_start_state_t state) /*!< in: indicate current
                             state of thread startup */
{
  srv_start_state |= state;
}

/** Check if following group of threads is started.
 @return true if started */
static inline bool srv_start_state_is_set(
    srv_start_state_t state) /*!< in: state to check for */
{
  return (srv_start_state & state);
}

struct Thread_to_stop {
  /** Name of the thread, printed to the error log if we waited too
  long (after 60 seconds and then every 60 seconds). */
  const char *m_name;

  /** Future which allows to check if given task is completed. */
  const IB_thread &m_thread;

  /** Function which can be called any number of times to wake
  the possibly waiting thread, so it could exit. */
  std::function<void()> m_notify;

  /** Shutdown state in which we are waiting until thread is exited
  (earlier we keep notifying but we don't require it to exit before
  we may switch to the next state). */
  srv_shutdown_t m_wait_on_state;
};

/* 定义需要停止的线程数组,每个元素包含线程名称、线程句柄、通知函数和停止状态 */
static const Thread_to_stop threads_to_stop[]{
    /* 锁等待超时监控线程 */
    {"lock_wait_timeout", srv_threads.m_lock_wait_timeout,
     lock_set_timeout_event, SRV_SHUTDOWN_CLEANUP},

    /* 错误监控线程 */
    {"error_monitor", srv_threads.m_error_monitor,
     []() { os_event_set(srv_error_event); }, SRV_SHUTDOWN_CLEANUP},

    /* 监控线程,用于打印InnoDB监控信息 */
    {"monitor", srv_threads.m_monitor,
     []() { os_event_set(srv_monitor_event); }, SRV_SHUTDOWN_CLEANUP},

    /* 缓冲池dump线程 */
    {"buf_dump", srv_threads.m_buf_dump,
     []() { os_event_set(srv_buf_dump_event); }, SRV_SHUTDOWN_CLEANUP},

    /* 缓冲池大小调整线程 */
    {"buf_resize", srv_threads.m_buf_resize,
     []() { os_event_set(srv_buf_resize_event); }, SRV_SHUTDOWN_CLEANUP},

    /* 主线程,负责协调其他线程的工作 */
    {"master", srv_threads.m_master, srv_wake_master_thread,
     SRV_SHUTDOWN_MASTER_STOP}};

void srv_shutdown_exit_threads() {
  srv_shutdown_state.store(SRV_SHUTDOWN_EXIT_THREADS);

  if (srv_start_state == SRV_START_STATE_NONE) {
    return;
  }

  uint32_t i;

  /* All threads end up waiting for certain events. Put those events
  to the signaled state. Then the threads will exit themselves after
  os_event_wait(). */
  for (i = 0; i < SHUTDOWN_SLEEP_ROUNDS; i++) {
    /* NOTE: IF YOU CREATE THREADS IN INNODB, YOU MUST EXIT THEM
    HERE OR EARLIER */

    /* These threads normally finish when reaching SRV_SHUTDOWN_CLEANUP or
    SRV_SHUTDOWN_MASTER_STOP state, which we might have jumped over. */
    for (const auto &thread_info : threads_to_stop) {
      if (srv_thread_is_active(thread_info.m_thread)) {
        thread_info.m_notify();
      }
    }

    if (!srv_read_only_mode) {
      if (srv_start_state_is_set(SRV_START_STATE_PURGE)) {
        /* Wakeup purge threads. */
        srv_purge_wakeup();
      }
    }

    if (srv_start_state_is_set(SRV_START_STATE_IO)) {
      /* Exit the i/o threads */
      if (!srv_read_only_mode) {
        if (recv_sys->flush_start != nullptr) {
          os_event_set(recv_sys->flush_start);
        }
        if (recv_sys->flush_end != nullptr) {
          os_event_set(recv_sys->flush_end);
        }
      }

      os_event_set(buf_flush_event);

      if (!buf_flush_page_cleaner_is_active() && os_aio_all_slots_free()) {
        os_aio_wake_all_threads_at_shutdown();
      }
    }

    if (srv_thread_is_active(srv_threads.m_dict_stats)) {
      os_event_set(dict_stats_event);
    }

    /* Try to stop archiver threads. */
    arch_wake_threads();

    if (log_sys != nullptr) {
      /* Preserve the log threads for the 75% of the total
      time we are waiting here until all threads are stopped.
      This is because log threads are normally shut down at
      the very end and we might need their help to stop other
      threads. */
      if (!buf_flush_page_cleaner_is_active() ||
          i >= SHUTDOWN_SLEEP_ROUNDS * 0.75) {
        log_stop_background_threads_nowait(*log_sys);

      } else {
        /* Ensure log threads are working. The redo log is
        like a blood, we need it for a lot of other systems
        to work. Ensure the blood flows. */
        log_wake_threads(*log_sys);
      }
    }

    bool active = os_thread_any_active();

    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));

    if (!active) {
      break;
    }
  }

  if (i == SHUTDOWN_SLEEP_ROUNDS) {
    ib::warn(ER_IB_MSG_1103, os_thread_count.load());

#ifdef UNIV_DEBUG
    os_aio_print_pending_io(stderr);
    ut_d(ut_error);
#endif /* UNIV_DEBUG */
  } else {
    /* Reset the start state. */
    srv_start_state = SRV_START_STATE_NONE;
  }
}

#ifdef UNIV_DEBUG
#define srv_init_abort(_db_err) \
  srv_init_abort_low(create_new_db, __FILE__, __LINE__, _db_err)
#else
#define srv_init_abort(_db_err) srv_init_abort_low(create_new_db, _db_err)
#endif /* UNIV_DEBUG */

/** Innobase start-up aborted. Perform cleanup actions.
@param[in]      create_new_db   true if new db is  being created
@param[in]      file            File name
@param[in]      line            Line number
@param[in]      err             Reason for aborting InnoDB startup
@return DB_SUCCESS or error code. */
static dberr_t srv_init_abort_low(bool create_new_db,
                                  IF_DEBUG(const char *file, ulint line, )
                                      dberr_t err) {
  std::ostringstream msg;

#ifdef UNIV_DEBUG
  msg << "at " << innobase_basename(file) << "[" << line << "] ";
#endif /* UNIV_DEBUG */

  if (create_new_db) {
    ib::error(ER_IB_MSG_1104, msg.str().c_str(), ut_strerr(err));
  } else {
    ib::error(ER_IB_MSG_1105, msg.str().c_str(), ut_strerr(err));
  }

  clone_files_error();
  srv_shutdown_exit_threads();

  return (err);
}

/** Enable encryption of system tablespace if requested. At
startup load the encryption information from first datafile
to tablespace object
@return DB_SUCCESS on succes, others on failure */
static dberr_t srv_sys_enable_encryption(bool create_new_db) {
  fil_space_t *space = fil_space_get(TRX_SYS_SPACE);
  dberr_t err = DB_SUCCESS;

  if (create_new_db && srv_sys_tablespace_encrypt) {
    fsp_flags_set_encryption(space->flags);
    srv_sys_space.set_flags(space->flags);

    err = fil_set_encryption(space->id, Encryption::AES, nullptr, nullptr);
    ut_ad(err == DB_SUCCESS);
  } else {
    const auto fsp_flags = srv_sys_space.m_files.begin()->flags();
    const bool is_encrypted = FSP_FLAGS_GET_ENCRYPTION(fsp_flags);

    if (is_encrypted && !srv_sys_tablespace_encrypt) {
      ib::error() << "The system tablespace is encrypted but"
                  << " --innodb_sys_tablespace_encrypt is"
                  << " OFF. Enable the option and start server";
      return (DB_ERROR);
    }

    if (!is_encrypted && srv_sys_tablespace_encrypt) {
      ib::error() << "The system tablespace is not encrypted but"
                  << " --innodb_sys_tablespace_encrypt is"
                  << " ON. This instance was not bootstrapped"
                  << " with --innodb_sys_tablespace_encrypt=ON."
                  << " Disable this option and start server";
      return (DB_ERROR);
    }

    if (is_encrypted) {
      fsp_flags_set_encryption(space->flags);
      srv_sys_space.set_flags(space->flags);

      err = fil_set_encryption(space->id, Encryption::AES,
                               srv_sys_space.m_files.begin()->m_encryption_key,
                               srv_sys_space.m_files.begin()->m_encryption_iv);
      ut_ad(err == DB_SUCCESS);
    }
  }

  return (err);
}

dberr_t srv_start(bool create_new_db) {
  lsn_t flushed_lsn; // 定义变量flushed_lsn，用于存储已刷新到磁盘的日志序列号

  page_no_t sum_of_data_file_sizes; // 定义变量sum_of_data_file_sizes，用于存储数据文件大小的总和
  page_no_t tablespace_size_in_header; // 定义变量tablespace_size_in_header，用于存储表空间头中的大小
  dberr_t err; // 定义变量err，用于存储错误码
  mtr_t mtr; // 定义变量mtr，用于存储mini-transaction对象
  purge_pq_t *purge_queue; // 定义指针变量purge_queue，用于存储清除队列

  assert(srv_dict_metadata == nullptr); // 断言srv_dict_metadata为空指针，确保数据字典元数据未初始化
  /* Reset the start state. */
  srv_start_state = SRV_START_STATE_NONE; // 重置启动状态为SRV_START_STATE_NONE

#ifdef UNIV_LINUX
#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
  ib::info(ER_IB_MSG_1107); // 如果支持FALLOC_PUNCH_HOLE_AND_KEEP_SIZE，输出相关信息
#else
  ib::info(ER_IB_MSG_1108); // 如果不支持FALLOC_PUNCH_HOLE_AND_KEEP_SIZE，输出相关信息
#endif /* HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */
#endif /* UNIV_LINUX */

  static_assert(sizeof(ulint) == sizeof(void *),
                "Size of InnoDB's ulint is not the same as size of void*. The "
                "sizes should be the same so that on a 64-bit platforms you "
                "can allocate more than 4 GB of memory."); // 静态断言，确保ulint和void*的大小相同，以便在64位平台上分配超过4GB的内存

  if (srv_is_upgrade_mode) { // 如果处于升级模式
    if (srv_force_recovery != 0) { // 如果强制恢复模式不为0
      ib::error(ER_IB_MSG_1111); // 输出错误信息
      return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
    }
    if (srv_read_only_mode) { // 如果处于只读模式
      ib::error(ER_IB_MSG_1110); // 输出错误信息
      return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
    }
  }

#ifdef UNIV_DEBUG
  ib::info(ER_IB_MSG_1112) << "!!!!!!!! UNIV_DEBUG switched on !!!!!!!!!"; // 如果启用UNIV_DEBUG，输出调试信息
#endif

#ifdef UNIV_IBUF_DEBUG
  ib::info(ER_IB_MSG_1113) << "!!!!!!!! UNIV_IBUF_DEBUG switched on !!!!!!!!!"; // 如果启用UNIV_IBUF_DEBUG，输出调试信息
#ifdef UNIV_IBUF_COUNT_DEBUG
  ib::info(ER_IB_MSG_1114)
      << "!!!!!!!! UNIV_IBUF_COUNT_DEBUG switched on !!!!!!!!!"; // 如果启用UNIV_IBUF_COUNT_DEBUG，输出调试信息
  ib::error(ER_IB_MSG_1115)
      << "Crash recovery will fail with UNIV_IBUF_COUNT_DEBUG"; // 输出错误信息，表示启用UNIV_IBUF_COUNT_DEBUG会导致崩溃恢复失败
#endif
#endif

#ifdef UNIV_LOG_LSN_DEBUG
  ib::info(ER_IB_MSG_1116)
      << "!!!!!!!! UNIV_LOG_LSN_DEBUG switched on !!!!!!!!!"; // 如果启用UNIV_LOG_LSN_DEBUG，输出调试信息
#endif /* UNIV_LOG_LSN_DEBUG */

#if defined(COMPILER_HINTS_ENABLED)
  ib::info(ER_IB_MSG_1117) << "Compiler hints enabled."; // 如果启用编译器提示，输出相关信息
#endif /* defined(COMPILER_HINTS_ENABLED) */

  ib::info(ER_IB_MSG_1119) << MUTEX_TYPE; // 输出互斥锁类型信息
  ib::info(ER_IB_MSG_1120) << IB_MEMORY_BARRIER_STARTUP_MSG; // 输出内存屏障启动信息

  if (srv_force_recovery > 0) { // 如果强制恢复模式大于0
    ib::info(ER_IB_MSG_1121) << "!!! innodb_force_recovery is set to "
                             << srv_force_recovery << " !!!"; // 输出强制恢复模式设置信息
  }

#ifndef HAVE_MEMORY_BARRIER
#if defined __i386__ || defined __x86_64__ || defined _M_IX86 || \
    defined _M_X64 || defined _WIN32
#else
  ib::warn(ER_IB_MSG_1122); // 如果没有内存屏障，且不在特定平台上，输出警告信息
#endif /* IA32 or AMD64 */
#endif /* HAVE_MEMORY_BARRIER */

#ifdef UNIV_ZIP_DEBUG
  ib::info(ER_IB_MSG_1123, ZLIB_VERSION) << " with validation"; // 如果启用UNIV_ZIP_DEBUG，输出ZLIB版本信息和验证信息
#else
  ib::info(ER_IB_MSG_1123, ZLIB_VERSION); // 输出ZLIB版本信息
#endif /* UNIV_ZIP_DEBUG */

#ifdef UNIV_ZIP_COPY
  ib::info(ER_IB_MSG_1124) << "and extra copying"; // 如果启用UNIV_ZIP_COPY，输出额外复制信息
#endif /* UNIV_ZIP_COPY */

  /* Since InnoDB does not currently clean up all its internal data
  structures in MySQL Embedded Server Library server_end(), we
  print an error message if someone tries to start up InnoDB a
  second time during the process lifetime. */
  // 由于InnoDB当前不会在MySQL嵌入式服务器库的server_end()中清理其所有内部数据结构，如果在进程生命周期内第二次启动InnoDB，我们会打印错误信息

  if (srv_start_has_been_called) { // 如果srv_start已经被调用过
    ib::error(ER_IB_MSG_1125); // 输出错误信息
  }

  srv_start_has_been_called = true; // 设置srv_start_has_been_called为true

  srv_is_being_started = true; // 设置srv_is_being_started为true

#ifdef HAVE_PSI_STAGE_INTERFACE
  /* Register performance schema stages before any real work has been
  started which may need to be instrumented. */
  mysql_stage_register("innodb", srv_stages, UT_ARR_SIZE(srv_stages)); // 注册性能模式阶段，在任何实际工作开始之前
#endif /* HAVE_PSI_STAGE_INTERFACE */

  /* Switch latching order checks on in sync0debug.cc, if
  --innodb-sync-debug=false (default) */
  ut_d(sync_check_enable()); // 启用同步检查

  srv_boot(); // 启动InnoDB服务器

  ib::info(ER_IB_MSG_1126)
      << "Using "
      << (ut_crc32_cpu_enabled ? (ut_poly_mul_cpu_enabled
                                      ? "hardware accelerated crc32 and "
                                        "polynomial multiplication."
                                      : "hardware accelerated crc32 and "
                                        "software polynomial multiplication.")
                               : "software crc32."); // 输出CRC32和多项式乘法的使用信息

  os_create_block_cache(); // 创建块缓存

  fil_init(innobase_get_open_files_limit()); // 初始化文件系统

  /* This is the default directory for IBD and IBU files. Put it first
  in the list of known directories. */
  fil_set_scan_dir(MySQL_datadir_path.path()); // 设置IBD和IBU文件的默认目录，并将其放在已知目录列表的首位

  /* Add --innodb-data-home-dir as a known location for IBD and IBU files
  if it is not already there. */
  ut_ad(srv_data_home != nullptr && *srv_data_home != '\0'); // 断言srv_data_home不为空且不为空字符串
  fil_set_scan_dir(Fil_path::remove_quotes(srv_data_home)); // 添加--innodb-data-home-dir作为IBD和IBU文件的已知位置

  /* Add --innodb-directories as known locations for IBD and IBU files. */
  if (srv_innodb_directories != nullptr && *srv_innodb_directories != 0) { // 如果srv_innodb_directories不为空且不为空字符串
    fil_set_scan_dirs(Fil_path::remove_quotes(srv_innodb_directories)); // 添加--innodb-directories作为IBD和IBU文件的已知位置
  }

  /* Note whether the undo path is different (not the same or under)
  from all other known directories. If so, this will allow us to keep
  IBD files out of this unique undo location.*/
  MySQL_undo_path_is_unique = !fil_path_is_known(MySQL_undo_path.path()); // 检查undo路径是否与所有其他已知目录不同，如果是，则允许我们将IBD文件保留在这个唯一的undo位置之外

  /* For the purpose of file discovery at startup, we need to scan
  --innodb-undo-directory also if it is different from the locations above. */
  if (MySQL_undo_path_is_unique) { // 如果undo路径是唯一的
    fil_set_scan_dir(Fil_path::remove_quotes(MySQL_undo_path)); // 在启动时扫描--innodb-undo-directory
  }

  ib::info(ER_IB_MSG_378) << "Directories to scan '" << fil_get_dirs() << "'"; // 输出要扫描的目录信息

  /* Must replace clone files before scanning directories. When
  clone replaces current database, cloned files are moved to data files
  at this stage. */
  err = clone_init(); // 在扫描目录之前必须替换克隆文件，当克隆替换当前数据库时，克隆文件会在此阶段移动到数据文件

  if (err != DB_SUCCESS) { // 如果克隆初始化失败
    return (srv_init_abort(err)); // 返回错误码，初始化中止
  }

  err = fil_scan_for_tablespaces(); // 扫描表空间

  if (err != DB_SUCCESS) { // 如果扫描表空间失败
    return (srv_init_abort(err)); // 返回错误码，初始化中止
  }

  if (!srv_read_only_mode) { // 如果不是只读模式
    mutex_create(LATCH_ID_SRV_MONITOR_FILE, &srv_monitor_file_mutex); // 创建监控文件互斥锁

    if (srv_innodb_status) { // 如果启用InnoDB状态监控
      srv_monitor_file_name = static_cast<char *>(ut::malloc_withkey(
          UT_NEW_THIS_FILE_PSI_KEY,
          MySQL_datadir_path.len() + 20 + sizeof "/innodb_status.")); // 分配监控文件名内存

      sprintf(srv_monitor_file_name, "%s/innodb_status." ULINTPF,
              static_cast<const char *>(MySQL_datadir_path),
              os_proc_get_number()); // 格式化监控文件名

      srv_monitor_file = fopen(srv_monitor_file_name, "w+"); // 打开监控文件

      if (!srv_monitor_file) { // 如果打开监控文件失败
        ib::error(ER_IB_MSG_1127, srv_monitor_file_name, strerror(errno)); // 输出错误信息

        return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
      }
    } else {
      srv_monitor_file_name = nullptr; // 设置监控文件名为空指针
      srv_monitor_file = os_file_create_tmpfile(); // 创建临时监控文件

      if (!srv_monitor_file) { // 如果创建临时监控文件失败
        return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
      }
    }

    mutex_create(LATCH_ID_SRV_MISC_TMPFILE, &srv_misc_tmpfile_mutex); // 创建杂项临时文件互斥锁

    srv_misc_tmpfile = os_file_create_tmpfile(); // 创建杂项临时文件

    if (!srv_misc_tmpfile) { // 如果创建杂项临时文件失败
      return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
    }
  }

  srv_n_file_io_threads = srv_n_read_io_threads; // 设置文件IO线程数为读取IO线程数

  srv_n_file_io_threads += srv_n_write_io_threads; // 增加写入IO线程数

  if (!srv_read_only_mode) { // 如果不是只读模式
    /* Add the log and ibuf IO threads. */
    srv_n_file_io_threads += 2; // 增加日志和ibuf IO线程数
  } else {
    ib::info(ER_IB_MSG_1128); // 输出只读模式信息
  }

  ut_a(srv_n_file_io_threads <= SRV_MAX_N_IO_THREADS); // 断言文件IO线程数不超过最大IO线程数

  if (!os_aio_init(srv_n_read_io_threads, srv_n_write_io_threads)) { // 初始化异步IO
    ib::error(ER_IB_MSG_1129); // 输出错误信息

    return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
  }

  double size; // 定义变量size，用于存储缓冲池大小
  char unit; // 定义变量unit，用于存储缓冲池大小单位

  if (srv_buf_pool_size >= 1024 * 1024 * 1024) { // 如果缓冲池大小大于等于1GB
    size = ((double)srv_buf_pool_size) / (1024 * 1024 * 1024); // 将缓冲池大小转换为GB
    unit = 'G'; // 设置单位为GB
  } else {
    size = ((double)srv_buf_pool_size) / (1024 * 1024); // 将缓冲池大小转换为MB
    unit = 'M'; // 设置单位为MB
  }

  double chunk_size; // 定义变量chunk_size，用于存储缓冲池块大小
  char chunk_unit; // 定义变量chunk_unit，用于存储缓冲池块大小单位

  if (srv_buf_pool_chunk_unit >= 1024 * 1024 * 1024) { // 如果缓冲池块大小大于等于1GB
    chunk_size = srv_buf_pool_chunk_unit / 1024.0 / 1024 / 1024; // 将缓冲池块大小转换为GB
    chunk_unit = 'G'; // 设置单位为GB
  } else {
    chunk_size = srv_buf_pool_chunk_unit / 1024.0 / 1024; // 将缓冲池块大小转换为MB
    chunk_unit = 'M'; // 设置单位为MB
  }

  ib::info(ER_IB_MSG_1130, size, unit, srv_buf_pool_instances, chunk_size,
           chunk_unit); // 输出缓冲池大小、单位、实例数、块大小和块单位信息

  err = buf_pool_init(srv_buf_pool_size, static_cast<bool>(srv_numa_interleave),
                      srv_buf_pool_instances); // 初始化缓冲池

  if (err != DB_SUCCESS) { // 如果缓冲池初始化失败
    ib::error(ER_IB_MSG_1131); // 输出错误信息

    return (srv_init_abort(DB_ERROR)); // 返回错误码，初始化中止
  }

  ib::info(ER_IB_MSG_1132); // 输出缓冲池初始化成功信息

#ifdef UNIV_DEBUG
  /* We have observed deadlocks with a 5MB buffer pool but
  the actual lower limit could very well be a little higher. */
  // 我们观察到5MB缓冲池会导致死锁，但实际的下限可能会更高

  if (srv_buf_pool_size <= 5 * 1024 * 1024) { // 如果缓冲池大小小于等于5MB
    ib::info(ER_IB_MSG_1133, ulonglong{srv_buf_pool_size / 1024 / 1024}); // 输出缓冲池大小信息
  }
#endif /* UNIV_DEBUG */

  fsp_init(); // 初始化文件空间管理
  pars_init(); // 初始化解析器

  recv_sys_create(); // 创建恢复系统
  recv_sys_init(); // 初始化恢复系统
  trx_sys_create(); // 创建事务系统
  lock_sys_create(srv_lock_table_size); // 创建锁系统

  /* Create i/o-handler threads: */
  // 创建IO处理线程

  /* For read only mode, we don't need ibuf and log I/O thread.
  Please see innobase_start_or_create_for_mysql() */
  // 对于只读模式，我们不需要ibuf和日志IO线程，请参见innobase_start_or_create_for_mysql()
  ulint start = (srv_read_only_mode) ? 0 : 2; // 如果是只读模式，start为0，否则为2

  /* Sequence number displayed in the thread os name. */
  // 线程操作系统名称中显示的序列号
  PSI_thread_seqnum pfs_seqnum [[maybe_unused]]; // 定义PSI线程序列号

  for (ulint t = 0; t < srv_n_file_io_threads; ++t) { // 遍历所有文件IO线程
    IB_thread thread; // 定义IB线程对象
    if (t < start) { // 如果t小于start
      if (t == 0) { // 如果t等于0
        thread = os_thread_create(io_ibuf_thread_key, 0, io_handler_thread, t); // 创建ibuf IO线程
      } else {
        ut_ad(t == 1); // 断言t等于1
        thread = os_thread_create(io_log_thread_key, 0, io_handler_thread, t); // 创建日志IO线程
      }
    } else if (t >= start && t < (start + srv_n_read_io_threads)) { // 如果t大于等于start且小于读取IO线程数
      /* Numbering for ib_io_rd-NN starts with N=1. */
      // ib_io_rd-NN的编号从N=1开始
      pfs_seqnum = t + 1 - start; // 设置PSI线程序列号
      thread = os_thread_create(io_read_thread_key, pfs_seqnum,
                                io_handler_thread, t); // 创建读取IO线程

} else if (t >= (start + srv_n_read_io_threads) &&
               t < (start + srv_n_read_io_threads + srv_n_write_io_threads)) {
      /* Numbering for ib_io_wr-NN starts with N=1. */
      /* ib_io_wr-NN 的编号从 N=1 开始。 */
      pfs_seqnum = t + 1 - start - srv_n_read_io_threads; // 计算 pfs_seqnum
      thread = os_thread_create(io_write_thread_key, pfs_seqnum,
                                io_handler_thread, t); // 创建写 IO 线程
    } else {
      /* Dead code ? */
      /* 死代码？ */
      thread = os_thread_create(io_handler_thread_key, t, io_handler_thread, t); // 创建 IO 处理线程
    }
    thread.start(); // 启动线程
  }

  /* Even in read-only mode there could be flush job generated by
  intrinsic table operations. */
  /* 即使在只读模式下，也可能会有由内在表操作生成的刷新作业。 */
  buf_flush_page_cleaner_init(); // 初始化页面清理器

  srv_start_state_set(SRV_START_STATE_IO); // 设置启动状态为 IO

  srv_startup_is_before_trx_rollback_phase = !create_new_db; // 设置是否在事务回滚阶段之前启动

  if (create_new_db) { // 如果创建新数据库
    recv_sys_free(); // 释放恢复系统
  }

  /* Open or create the data files. */
  /* 打开或创建数据文件。 */
  page_no_t sum_of_new_sizes; // 新文件大小总和

  err = srv_sys_space.open_or_create(false, create_new_db, &sum_of_new_sizes,
                                     &flushed_lsn); // 打开或创建系统表空间

  if (flushed_lsn < LOG_START_LSN) { // 如果 flushed_lsn 小于 LOG_START_LSN
    ut_ad(!create_new_db); // 断言不是创建新数据库
    /* Data directory hasn't been initialized yet. */
    /* 数据目录尚未初始化。 */
    ib::error(ER_IB_MSG_DATA_DIRECTORY_NOT_INITIALIZED_OR_CORRUPTED); // 记录错误信息
    return DB_ERROR; // 返回 DB_ERROR
  }

  /* FIXME: This can be done earlier, but we now have to wait for
  checking of system tablespace. */
  /* FIXME：这可以更早完成，但我们现在必须等待检查系统表空间。 */
  dict_persist_init(); // 初始化字典持久化

  switch (err) { // 根据错误码进行处理
    case DB_SUCCESS: // 如果成功
      err = srv_sys_enable_encryption(create_new_db); // 启用加密
      if (err != DB_SUCCESS) return (srv_init_abort(err)); // 如果失败，中止初始化
      break;
    case DB_CANNOT_OPEN_FILE: // 如果无法打开文件
      ib::error(ER_IB_MSG_1134); // 记录错误信息
      [[fallthrough]];
    default: // 其他错误

      /* Other errors might come from
      Datafile::validate_first_page() */
      /* 其他错误可能来自 Datafile::validate_first_page() */

      return (srv_init_abort(err)); // 中止初始化
  }

  mtr_t::s_logging.init(); // 初始化日志记录

  if (dblwr::is_enabled() && ((err = dblwr::open()) != DB_SUCCESS)) { // 如果启用了双写缓冲区且打开失败
    return srv_init_abort(err); // 中止初始化
  }

  lsn_t new_files_lsn; // 新文件的 LSN

  err = log_sys_init(create_new_db, flushed_lsn, new_files_lsn); // 初始化日志系统

  if (err != DB_SUCCESS) { // 如果失败
    return srv_init_abort(err); // 中止初始化
  }

  ut_a(log_sys != nullptr); // 断言日志系统不为空

  arch_init(); // 初始化归档

  bool srv_monitor_thread_created = false; // 监视器线程是否已创建

  if (create_new_db) { // 如果创建新数据库
    ut_a(buf_are_flush_lists_empty_validate()); // 断言缓冲区刷新列表为空

    ut_a(!srv_read_only_mode); // 断言不是只读模式

    ut_a(log_sys->last_checkpoint_lsn.load() ==
         LOG_START_LSN + LOG_BLOCK_HDR_SIZE); // 断言最后一个检查点 LSN 等于 LOG_START_LSN + LOG_BLOCK_HDR_SIZE

    ut_a(new_files_lsn == LOG_START_LSN + LOG_BLOCK_HDR_SIZE); // 断言新文件的 LSN 等于 LOG_START_LSN + LOG_BLOCK_HDR_SIZE

    err = log_start(*log_sys, new_files_lsn, new_files_lsn, nullptr); // 启动日志系统

    if (err != DB_SUCCESS) { // 如果失败
      return srv_init_abort(err); // 中止初始化
    }

    log_start_background_threads(*log_sys); // 启动后台线程

    err = srv_undo_tablespaces_init(true); // 初始化撤销表空间

    if (err != DB_SUCCESS) { // 如果失败
      return (srv_init_abort(err)); // 中止初始化
    }

    mtr_start(&mtr); // 启动 mtr

    bool ret = fsp_header_init(0, sum_of_new_sizes, &mtr); // 初始化表空间头

    mtr_commit(&mtr); // 提交 mtr

    if (!ret) { // 如果失败
      return (srv_init_abort(DB_ERROR)); // 中止初始化
    }

    /* To maintain backward compatibility we create only
    the first rollback segment before the double write buffer.
    All the remaining rollback segments will be created later,
    after the double write buffers haves been created. */
    /* 为了保持向后兼容性，我们只在双写缓冲区之前创建第一个回滚段。所有剩余的回滚段将在创建双写缓冲区后创建。 */
    trx_sys_create_sys_pages(); // 创建系统页面

    trx_purge_sys_mem_create(); // 创建清除系统内存

    purge_queue = trx_sys_init_at_db_start(); // 在数据库启动时初始化事务系统

    /* The purge system needs to create the purge view and
    therefore requires that the trx_sys is inited. */
    /* 清除系统需要创建清除视图，因此需要初始化 trx_sys。 */

    trx_purge_sys_initialize(srv_threads.m_purge_workers_n, purge_queue); // 初始化清除系统

    err = dict_create(); // 创建字典

    if (err != DB_SUCCESS) { // 如果失败
      return (srv_init_abort(err)); // 中止初始化
    }

    srv_create_sdi_indexes(); // 创建 SDI 索引

    /* We always create the legacy double write buffer to preserve the
    expected page ordering of the system tablespace.
    FIXME: Try and remove this requirement. */
    /* 我们总是创建传统的双写缓冲区，以保持系统表空间的预期页面顺序。FIXME：尝试删除此要求。 */
    err = dblwr::v1::create(); // 创建双写缓冲区

    if (err != DB_SUCCESS) { // 如果失败
      return srv_init_abort(err); // 中止初始化
    }

  } else { // 如果不是创建新数据库
    bool log_upgrade = log_sys->m_format < Log_format::CURRENT; // 检查日志格式是否需要升级
    DBUG_EXECUTE_IF("log_force_upgrade", log_upgrade = true;); // 强制日志升级

    if (log_upgrade && srv_read_only_mode) { // 如果需要升级且是只读模式
      ib::error(ER_IB_MSG_LOG_UPGRADE_IN_READ_ONLY_MODE,
                ulong{to_int(log_sys->m_format)}); // 记录错误信息
      return srv_init_abort(DB_ERROR); // 中止初始化
    }

    /* Load the reserved boundaries of the legacy dblwr buffer, this is
    required to check for stray reads and writes trying to access this
    reserved region in the sys tablespace.
    FIXME: Try and remove this requirement. */
    /* 加载传统双写缓冲区的保留边界，这是检查试图访问系统表空间中此保留区域的意外读取和写入所必需的。FIXME：尝试删除此要求。 */
    err = dblwr::v1::init(); // 初始化双写缓冲区

    if (err != DB_SUCCESS) { // 如果失败
      return srv_init_abort(err); // 中止初始化
    }

    /* Invalidate the buffer pool to ensure that we reread
    the page that we read above, during recovery.
    Note that this is not as heavy weight as it seems. At
    this point there will be only ONE page in the buf_LRU
    and there must be no page in the buf_flush list. */
    /* 使缓冲池无效，以确保在恢复期间重新读取上面读取的页面。请注意，这并不像看起来那么重。在这一点上，buf_LRU 中只有一个页面，并且 buf_flush 列表中没有页面。 */
    buf_pool_invalidate(); // 使缓冲池无效

    /* Start monitor thread early enough so that e.g. crash recovery failing to
    find free pages in the buffer pool is diagnosed. */
    /* 尽早启动监视器线程，以便诊断崩溃恢复未能在缓冲池中找到空闲页面的情况。 */
    if (!srv_read_only_mode) { // 如果不是只读模式
      /* Create the thread which prints InnoDB monitor info */
      /* 创建打印 InnoDB 监视器信息的线程 */
      srv_threads.m_monitor =
          os_thread_create(srv_monitor_thread_key, 0, srv_monitor_thread); // 创建监视器线程
      srv_threads.m_monitor.start(); // 启动监视器线程
      srv_monitor_thread_created = true; // 设置监视器线程已创建
    }

    /* Open all data files in the system tablespace:
    we keep them open until database shutdown. */
    /* 打开系统表空间中的所有数据文件：我们将它们保持打开状态，直到数据库关闭。 */
    fil_open_system_tablespace_files(); // 打开系统表空间文件

    /* We always try to do a recovery, even if the database had
    been shut down normally: this is the normal startup path */
    /* 我们总是尝试进行恢复，即使数据库已正常关闭：这是正常的启动路径 */
    RECOVERY_CRASH(1); // 恢复崩溃点 1

    if (new_files_lsn != 0) { // 如果 new_files_lsn 不为 0
      /* This means that either no log files have been found
      or the existing log files were marked as uninitialized. */
      /* 这意味着要么没有找到日志文件，要么现有日志文件被标记为未初始化。 */
      flushed_lsn = new_files_lsn; // 设置 flushed_lsn 为 new_files_lsn
    }

    err = recv_recovery_from_checkpoint_start(*log_sys, flushed_lsn); // 从检查点开始恢复

    if (err == DB_SUCCESS) { // 如果成功
      arch_page_sys->post_recovery_init(); // 恢复后初始化归档页面系统

      /* Initialize the change buffer. */
      /* 初始化更改缓冲区。 */
      err = dict_boot(); // 引导字典
      DBUG_EXECUTE_IF("ib_dic_boot_error", err = DB_ERROR;); // 强制引导错误
    }

    if (err != DB_SUCCESS) { // 如果失败
      /* Set the abort flag to true. */
      /* 将中止标志设置为 true。 */
      auto p = recv_recovery_from_checkpoint_finish(true); // 完成从检查点恢复

      ut_a(p == nullptr); // 断言 p 为空

      return (srv_init_abort(err)); // 中止初始化
    }

    ut_ad(clone_check_recovery_crashpoint(recv_sys->is_cloned_db)); // 检查克隆恢复崩溃点

    const bool redo_writes_allowed = !srv_read_only_mode && !log_upgrade; // 检查是否允许重做写入

    ut_a(srv_force_recovery < SRV_FORCE_NO_LOG_REDO || !redo_writes_allowed); // 断言 srv_force_recovery 小于 SRV_FORCE_NO_LOG_REDO 或不允许重做写入

    if (redo_writes_allowed) { // 如果允许重做写入
      /* We need to start log threads now, because recovery
      could result in execution of ibuf merges. These merges
      could result in new redo records. In the read-only mode
      we do not need log threads, because we disallow new redo
      records in such mode. If upgrade was forced, or the data
      directory was cloned, we will start redo threads later. */
      /* 我们现在需要启动日志线程，因为恢复可能会导致执行 ibuf 合并。这些合并可能会产生新的重做记录。在只读模式下，我们不需要日志线程，因为我们在这种模式下不允许新的重做记录。如果强制升级或克隆了数据目录，我们将在稍后启动重做线程。 */
      log_start_background_threads(*log_sys); // 启动后台线程
    }

    if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) { // 如果 srv_force_recovery 小于 SRV_FORCE_NO_LOG_REDO
      /* Apply the hashed log records to the
      respective file pages, for the last batch of
      recv_group_scan_log_recs(). */
      /* 将哈希日志记录应用于相应的文件页面，用于最后一批 recv_group_scan_log_recs()。 */

      RECOVERY_CRASH(2); // 恢复崩溃点 2

      /* Don't allow IBUF operations for cloned database
      recovery as it would add extra redo log and we may
      not have enough margin.

      Don't allow IBUF operations when redo is written
      in the older format than the current, because we
      would write new redo records in the current fmt,
      and end up with file in both formats = invalid. */
      /* 不允许克隆数据库恢复的 IBUF 操作，因为它会增加额外的重做日志，我们可能没有足够的余量。

      当重做以旧格式而不是当前格式写入时，不允许 IBUF 操作，因为我们会以当前格式写入新的重做记录，最终导致文件在两种格式下都无效。 */

      err = recv_apply_hashed_log_recs(*log_sys,
                                       !recv_sys->is_cloned_db && !log_upgrade); // 应用哈希日志记录

      if (recv_sys->found_corrupt_log || err != DB_SUCCESS) { // 如果发现损坏的日志或失败
        err = DB_ERROR; // 设置错误码为 DB_ERROR
        /* Set the abort flag to true. */
        /* 将中止标志设置为 true。 */
        auto p = recv_recovery_from_checkpoint_finish(true); // 完成从检查点恢复

        ut_a(p == nullptr); // 断言 p 为空
        return (srv_init_abort(err)); // 中止初始化
      }

      DBUG_PRINT("ib_log", ("apply completed")); // 打印调试信息

      /* Check and print if there were any tablespaces
      which had redo log records but we couldn't apply
      them because the filenames were missing. */
      /* 检查并打印是否有任何表空间有重做日志记录，但由于缺少文件名，我们无法应用它们。 */

      /* Recovery complete, start verifying the
      page LSN on read. */
      /* 恢复完成，开始验证读取时的页面 LSN。 */
      recv_lsn_checks_on = true; // 启用 LSN 检查
    }

    /* We have gone through the redo log, now check if all the
    tablespaces were found and recovered. */
    /* 我们已经完成了重做日志，现在检查是否找到了所有表空间并进行了恢复。 */

    if (srv_force_recovery == 0 && fil_check_missing_tablespaces()) { // 如果 srv_force_recovery 为 0 且检查到缺少表空间
      ib::error(ER_IB_MSG_1139); // 记录错误信息
      RECOVERY_CRASH(3); // 恢复崩溃点 3

      /* Set the abort flag to true. */
      /* 将中止标志设置为 true。 */
      auto p = recv_recovery_from_checkpoint_finish(true); // 完成从检查点恢复

      ut_a(p == nullptr); // 断言 p 为空

      return (srv_init_abort(DB_ERROR)); // 中止初始化
    }

    /* We have successfully recovered from the redo log. The
     data dictionary should now be readable. */
    /* 我们已经成功地从重做日志中恢复。数据字典现在应该是可读的。 */

    DBUG_EXECUTE_IF(
        "ib_recovery_print_mysql_binlog_offset",
        if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO &&
            recv_needed_recovery) { trx_sys_print_mysql_binlog_offset(); });
    // 如果 srv_force_recovery 小于 SRV_FORCE_NO_LOG_REDO 并且需要恢复，则打印 MySQL binlog 偏移量

    if (recv_sys->found_corrupt_log) {
      ib::warn(ER_IB_MSG_RECOVERY_CORRUPT);
    }
    // 如果发现损坏的日志，发出警告

    if (!srv_force_recovery && !srv_read_only_mode) {
      buf_flush_sync_all_buf_pools();
    }
    // 如果没有强制恢复且不是只读模式，则同步刷新所有缓冲池

    RECOVERY_CRASH(3);
    // 恢复崩溃点 3

    srv_dict_metadata = recv_recovery_from_checkpoint_finish(false);
    // 从检查点完成恢复

    if (recv_sys->is_cloned_db && srv_dict_metadata != nullptr) {
      ut::delete_(srv_dict_metadata);
      srv_dict_metadata = nullptr;
    }
    // 如果是克隆数据库且 srv_dict_metadata 不为空，则删除 srv_dict_metadata 并将其置为空

    /* We need to save the dynamic metadata collected from redo log to DD
    buffer table here. This is to make sure that the dynamic metadata is not
    lost by any future checkpoint. Since DD and data dictionary in memory
    objects are not fully initialized at this point, the usual mechanism to
    persist dynamic metadata at checkpoint wouldn't work. */
    /* 我们需要将从重做日志中收集的动态元数据保存到 DD 缓冲表中。这样做是为了确保动态元数据不会因未来的检查点而丢失。由于此时 DD 和内存中的数据字典对象尚未完全初始化，通常的在检查点持久化动态元数据的机制将不起作用。 */

    DBUG_EXECUTE_IF("log_first_rec_group_test", {
      const lsn_t end_lsn = mtr_commit_mlog_test();
      log_write_up_to(*log_sys, end_lsn, true);
      DBUG_SUICIDE();
    });
    // 测试日志的第一个记录组

    if (srv_dict_metadata != nullptr && !srv_dict_metadata->empty()) {
      ut_a(redo_writes_allowed);
      // 确保允许重做写入

      /* Open this table in case srv_dict_metadata should be applied to this
      table before checkpoint. And because DD is not fully up yet, the table
      can be opened by internal APIs. */
      /* 打开此表，以防 srv_dict_metadata 应在检查点之前应用于此表。由于 DD 尚未完全启动，因此可以通过内部 API 打开该表。 */

      fil_space_t *space =
          fil_space_acquire_silent(dict_sys_t::s_dict_space_id);
      // 获取表空间
      if (space == nullptr) {
        dberr_t error =
            fil_ibd_open(true, FIL_TYPE_TABLESPACE, dict_sys_t::s_dict_space_id,
                         predefined_flags, dict_sys_t::s_dd_space_name,
                         dict_sys_t::s_dd_space_file_name, true, false);
        // 打开表空间文件
        if (error != DB_SUCCESS) {
          ib::error(ER_IB_MSG_1142);
          return (srv_init_abort(DB_ERROR));
        }
      } else {
        fil_space_release(space);
      }
      // 释放表空间

      dict_persist->table_buffer =
          ut::new_withkey<DDTableBuffer>(UT_NEW_THIS_FILE_PSI_KEY);
      // 创建新的 DDTableBuffer
      /* We write redo log here. We assume that there should be enough room in
      log files, supposing log_free_check() works fine before crash. */
      /* 我们在这里写入重做日志。我们假设日志文件中应该有足够的空间，假设 log_free_check() 在崩溃前工作正常。 */
      srv_dict_metadata->store();
      // 存储 srv_dict_metadata

      /* Flush logs to persist the changes. */
      /* 刷新日志以持久化更改。 */
      log_buffer_flush_to_disk(*log_sys);
      // 刷新日志缓冲区到磁盘
    }

    RECOVERY_CRASH(4);
    // 恢复崩溃点 4

    log_sys->m_allow_checkpoints.store(true, std::memory_order_release);
    // 允许检查点

    bool log_downsize_requested =
        log_sys->m_capacity.target_physical_capacity() <
        log_sys->m_capacity.current_physical_capacity();
    // 检查是否需要缩小日志文件

    DBUG_EXECUTE_IF("log_force_resize", log_downsize_requested = true;);
    // 强制调整日志文件大小

    const bool need_to_recreate_log_files =
        log_upgrade || (log_downsize_requested && !srv_force_recovery &&
                        !recv_sys->found_corrupt_log);
    // 检查是否需要重新创建日志文件

    if (need_to_recreate_log_files) {
      /* Prepare to replace the redo log files. */
      /* 准备替换重做日志文件。 */

      if (log_upgrade) {
        ut_a(!srv_read_only_mode);
        // 确保不是只读模式

        if (recv_sys->is_cloned_db) {
          ib::error(ER_IB_MSG_LOG_UPGRADE_CLONED_DB,
                    ulong{to_int(log_sys->m_format)});
          return srv_init_abort(DB_ERROR);
        }
        // 如果是克隆数据库，发出错误并中止初始化

        /* For non-empty redo log, upgrade is rejected, so there is even
        no attempt to parse it, so no way to discover it's corrupted. */
        /* 对于非空的重做日志，升级被拒绝，因此甚至没有尝试解析它，因此无法发现它已损坏。 */
        if (recv_sys->found_corrupt_log) {
          ib::error(ER_IB_MSG_LOG_UPGRADE_CORRUPTION__UNEXPECTED,
                    ulong{to_int(log_sys->m_format)});
          ut_d(ut_error);
          ut_o(return srv_init_abort(DB_ERROR));
        }
        // 如果发现损坏的日志，发出错误并中止初始化

        /* For non-empty redo log, upgrade is rejected, so there is even
        no way to reconstruct non empty srv_dict_metadata. */
        /* 对于非空的重做日志，升级被拒绝，因此甚至无法重建非空的 srv_dict_metadata。 */
        if (srv_dict_metadata != nullptr && !srv_dict_metadata->empty()) {
          ib::error(ER_IB_MSG_LOG_UPGRADE_NON_PERSISTED_DD_METADATA__UNEXPECTED,
                    ulong{to_int(log_sys->m_format)});
          ut_d(ut_error);
          ut_o(return srv_init_abort(DB_ERROR));
        }
        // 如果 srv_dict_metadata 不为空且不为空，发出错误并中止初始化

      } else {
        ut_a(srv_force_recovery == 0);
        // 确保 srv_force_recovery 为 0
        if (srv_read_only_mode) {
          const os_offset_t min_capacity_in_M =
              log_sys->m_capacity.current_physical_capacity() / (1024 * 1024UL);
          ib::error(ER_IB_MSG_LOG_FILES_RESIZE_ON_START_IN_READ_ONLY_MODE,
                    ulonglong{min_capacity_in_M});
          return srv_init_abort(DB_ERROR);
        }
        // 如果是只读模式，发出错误并中止初始化
      }

      buf_pool_wait_for_no_pending_io();
      // 等待缓冲池没有挂起的 IO

      if (redo_writes_allowed) {
        /* Create checkpoint to ensure that the checkpoint header is flushed
        to the newest redo log file, before log_files_remove() is called,
        because otherwise crash after the first file removal could lead to
        the state without a checkpoint. */
        /* 创建检查点以确保检查点头被刷新到最新的重做日志文件中，然后再调用 log_files_remove()，因为否则在删除第一个文件后崩溃可能会导致没有检查点的状态。 */
        log_make_empty_and_stop_background_threads(*log_sys);
        // 创建空的检查点并停止后台线程
      }

      flushed_lsn = log_sys->flushed_to_disk_lsn.load();
      // 获取已刷新到磁盘的 LSN

      ut_ad(buf_pool_pending_io_reads_count() == 0);
      // 确保缓冲池没有挂起的 IO 读取

      ut_d(log_sys->disable_redo_writes = true);
      // 禁用重做写入

      {
        /* Emit a message to the error log. */
        /* 向错误日志发出消息。 */
        const auto target_size = log_sys->m_capacity.target_physical_capacity();
        const auto target_size_in_M = target_size / (1024 * 1024UL);
        if (log_upgrade) {
          ib::info(ER_IB_MSG_LOG_FILES_UPGRADE, ulonglong{target_size_in_M},
                   ulonglong{flushed_lsn});
          // 记录日志文件升级的信息

        } else {
          const auto current_size =
              log_files_size_of_existing_files(log_sys->m_files);
          const auto current_size_in_M = current_size / (1024 * 1024UL);
          ib::info(ER_IB_MSG_LOG_FILES_RESIZE_ON_START,
                   ulonglong{current_size_in_M}, ulonglong{target_size_in_M},
                   ulonglong{flushed_lsn});
          // 记录日志文件调整大小的信息
        }
      }

      /* Prepare to delete the old redo log files. */
      /* 准备删除旧的重做日志文件。 */
      buf_flush_sync_all_buf_pools();
      // 同步刷新所有缓冲池

      RECOVERY_CRASH(5);
      // 恢复崩溃点 5

      if (flushed_lsn < log_get_lsn(*log_sys) ||
          buf_pool_get_oldest_modification_lwm() != 0) {
        if (log_upgrade) {
          ib::error(ER_IB_MSG_LOG_UPGRADE_FLUSH_FAILED__UNEXPECTED,
                    ulong{to_int(log_sys->m_format)});
        } else {
          ib::error(ER_IB_MSG_LOG_FILES_RESIZE_ON_START_FAILED__UNEXPECTED,
                    ulong{to_int(log_sys->m_format)});
        }
        ut_d(ut_error);
        ut_o(return srv_init_abort(DB_ERROR));
      }
      // 如果刷新 LSN 小于日志系统的 LSN 或缓冲池的最旧修改 LWM 不为 0，则发出错误并中止初始化

      /* Stamp the LSN to the data files. */
      /* 将 LSN 标记到数据文件中。 */
      err = fil_write_flushed_lsn(flushed_lsn);
      ut_a(err == DB_SUCCESS);
      // 将已刷新 LSN 写入数据文件

      RECOVERY_CRASH(6);
      // 恢复崩溃点 6

      ib::info(ER_IB_MSG_LOG_FILES_REWRITING);
      // 记录日志文件重写的信息

      /* Remove all existing log files. */
      /* 删除所有现有的日志文件。 */
      log_files_remove(*log_sys);
      // 删除日志文件

      log_sys_close();
      ut_a(log_sys == nullptr);
      // 关闭日志系统并确保日志系统为空

      /* Finish clone file recovery before creating new log files. We
      roll forward to remove any intermediate files here. */
      /* 在创建新日志文件之前完成克隆文件恢复。我们在这里前滚以删除任何中间文件。 */
      clone_files_recovery(true);
      // 完成克隆文件恢复

      /* This is to provide the property that data byte at given lsn never
      changes and avoid the need to rewrite the block with flushed_lsn. */
      /* 这是为了提供在给定 lsn 处的数据字节永不改变的属性，并避免需要用 flushed_lsn 重写块。 */
      flushed_lsn = ut_uint64_align_up(flushed_lsn, OS_FILE_LOG_BLOCK_SIZE) +
                    LOG_BLOCK_HDR_SIZE;
      // 对齐 flushed_lsn

      err = log_sys_init(true, flushed_lsn, flushed_lsn);
      // 初始化日志系统

      if (err != DB_SUCCESS) {
        return srv_init_abort(err);
      }
      // 如果初始化失败，中止初始化

      ut_d(log_sys->disable_redo_writes = false);
      // 启用重做写入

      fil_open_system_tablespace_files();
      // 打开系统表空间文件

      err = log_start(*log_sys, flushed_lsn, flushed_lsn, nullptr);
      // 启动日志系统

      if (err != DB_SUCCESS) {
        return srv_init_abort(err);
      }
      // 如果启动失败，中止初始化

      log_start_background_threads(*log_sys);
      // 启动后台线程

    } else if (recv_sys->is_cloned_db || recv_sys->is_meb_db) {
      buf_pool_wait_for_no_pending_io();
      // 等待缓冲池没有挂起的 IO

      /* Reset creator for log */
      /* 重置日志的创建者 */

      if (redo_writes_allowed) {
        log_stop_background_threads(*log_sys);
        // 停止后台线程
      }

      ut_ad(buf_pool_pending_io_reads_count() == 0);
      // 确保缓冲池没有挂起的 IO 读取

      err = log_files_reset_creator_and_set_full(*log_sys);
      // 重置日志文件的创建者并设置为满

      if (err != DB_SUCCESS) {
        return srv_init_abort(err);
      }
      // 如果重置失败，中止初始化

      log_start_background_threads(*log_sys);
      // 启动后台线程

    } else {
      ut_a(redo_writes_allowed || srv_read_only_mode);
      // 确保允许重做写入或处于只读模式
    }

    if (sum_of_new_sizes > 0) {
      ut_a(!srv_read_only_mode);
      // 确保不是只读模式

      /* New data file(s) were added */
      /* 添加了新的数据文件 */
      mtr_start(&mtr);
      // 启动 mtr

      fsp_header_inc_size(0, sum_of_new_sizes, &mtr);
      // 增加表空间头的大小

      mtr_commit(&mtr);
      // 提交 mtr

      /* Immediately write the log record about
      increased tablespace size to disk, so that it
      is durable even if mysqld would crash
      quickly */
      /* 立即将关于增加表空间大小的日志记录写入磁盘，以便即使 mysqld 快速崩溃也能持久化。 */

      log_buffer_flush_to_disk(*log_sys);
      // 刷新日志缓冲区到磁盘
    }

    err = srv_undo_tablespaces_init(false);
    // 初始化撤销表空间

    if (err != DB_SUCCESS && srv_force_recovery < SRV_FORCE_NO_UNDO_LOG_SCAN) {
      return (srv_init_abort(err));
    }
    // 如果初始化失败且 srv_force_recovery 小于 SRV_FORCE_NO_UNDO_LOG_SCAN，则中止初始化

    trx_purge_sys_mem_create();
    // 创建事务清除系统内存

    /* The purge system needs to create the purge view and
    therefore requires that the trx_sys is inited. */
    /* 清除系统需要创建清除视图，因此需要初始化 trx_sys。 */
    purge_queue = trx_sys_init_at_db_start();
    // 在数据库启动时初始化事务系统

    if (srv_is_upgrade_mode) {
      if (!purge_queue->empty()) {
        ib::info(ER_IB_MSG_1144);
        srv_upgrade_old_undo_found = true;
      }
      // 如果清除队列不为空，记录信息并设置 srv_upgrade_old_undo_found 为 true
      /* Either the old or new undo tablespaces will
      be deleted later depending on the value of
      'failed_upgrade' in dd_upgrade_finish(). */
      /* 旧的或新的撤销表空间将根据 dd_upgrade_finish() 中的 'failed_upgrade' 值在稍后删除。 */
    } else {
      /* New undo tablespaces have been created.
      Delete the old undo tablespaces and the references
      to them in the TRX_SYS page. */
      /* 已创建新的撤销表空间。删除旧的撤销表空间及其在 TRX_SYS 页面中的引用。 */
      srv_undo_tablespaces_upgrade();
      // 升级撤销表空间
    }

    DBUG_EXECUTE_IF("check_no_undo", ut_ad(purge_queue->empty()););
    // 检查清除队列是否为空

    /* The purge system needs to create the purge view and
    therefore requires that the trx_sys and trx lists were
    initialized in trx_sys_init_at_db_start(). */
    /* 清除系统需要创建清除视图，因此需要在 trx_sys_init_at_db_start() 中初始化 trx_sys 和 trx 列表。 */
    trx_purge_sys_initialize(srv_threads.m_purge_workers_n, purge_queue);
  }

  /* Open temp-tablespace and keep it open until shutdown. */
  /* 打开临时表空间并保持打开状态直到关闭。 */
  err = srv_open_tmp_tablespace(create_new_db, &srv_tmp_space);
  // 打开临时表空间
  if (err != DB_SUCCESS) {
    return (srv_init_abort(err));
  }
  // 如果打开失败，中止初始化

  err = ibt::open_or_create(create_new_db);
  // 打开或创建 ibt
  if (err != DB_SUCCESS) {
    return (srv_init_abort(err));
  }
  // 如果打开或创建失败，中止初始化

  /* Here the double write buffer has already been created and so
  any new rollback segments will be allocated after the double
  write buffer. The default segment should already exist.
  We create the new segments only if it's a new database or
  the database was shutdown cleanly. */
  /* 此时双写缓冲区已经创建，因此任何新的回滚段将在双写缓冲区之后分配。默认段应该已经存在。我们仅在是新数据库或数据库干净关闭时创建新段。 */
/* Note: When creating the extra rollback segments during an upgrade
  we violate the latching order, even if the change buffer is empty.
  We make an exception in sync0sync.cc and check srv_is_being_started
  for that violation. It cannot create a deadlock because we are still
  running in single threaded mode essentially. Only the IO threads
  should be running at this stage. */
/* 注意：在升级期间创建额外的回滚段时，即使更改缓冲区为空，我们也会违反锁存顺序。我们在 sync0sync.cc 中做了一个例外，并检查 srv_is_being_started 以应对这种违规行为。它不会创建死锁，因为我们本质上仍在单线程模式下运行。在此阶段，只有 IO 线程应该在运行。 */

  ut_a(srv_rollback_segments > 0); // 确保 srv_rollback_segments 大于 0
  ut_a(srv_rollback_segments <= TRX_SYS_N_RSEGS); // 确保 srv_rollback_segments 小于等于 TRX_SYS_N_RSEGS

  /* Make sure there are enough rollback segments in each tablespace
  and that each rollback segment has an associated memory object.
  If any of these rollback segments contain undo logs, load them into
  the purge queue */
  /* 确保每个表空间中有足够的回滚段，并且每个回滚段都有一个关联的内存对象。如果这些回滚段中有任何包含撤销日志，则将它们加载到清除队列中 */
  if (!trx_rseg_adjust_rollback_segments(srv_rollback_segments)) {
    return (srv_init_abort(DB_ERROR)); // 如果调整回滚段失败，则中止初始化
  }

  /* Any undo tablespaces under construction are now fully built
  with all needed rsegs. Delete the trunc.log files and clear the
  construction list. */
  /* 任何正在建设中的撤销表空间现在都已完全构建，所有需要的 rsegs 都已完成。删除 trunc.log 文件并清除建设列表。 */
  srv_undo_tablespaces_mark_construction_done(); // 标记撤销表空间的建设完成

  /* Now that all rsegs are ready for use, make them active. */
  /* 现在所有的 rsegs 都已准备好使用，使它们处于活动状态。 */
  undo::spaces->s_lock(); // 锁定撤销表空间
  for (auto undo_space : undo::spaces->m_spaces) {
    if (!undo_space->is_empty()) {
      undo_space->set_active(); // 如果撤销表空间不为空，则将其设置为活动状态
    }
  }
  undo::spaces->s_unlock(); // 解锁撤销表空间

  /* Undo Tablespaces and Rollback Segments are ready. */
  /* 撤销表空间和回滚段已准备就绪。 */
  srv_startup_is_before_trx_rollback_phase = false; // 设置 srv_startup_is_before_trx_rollback_phase 为 false

  if (!srv_read_only_mode) { // 如果不是只读模式
    if (create_new_db) { // 如果创建新数据库
      srv_buffer_pool_load_at_startup = false; // 设置 srv_buffer_pool_load_at_startup 为 false
    }

    /* Create the thread which watches the timeouts
    for lock waits */
    /* 创建监视锁等待超时的线程 */
    srv_threads.m_lock_wait_timeout = os_thread_create(
        srv_lock_timeout_thread_key, 0, lock_wait_timeout_thread); // 创建锁等待超时线程

    srv_threads.m_lock_wait_timeout.start(); // 启动锁等待超时线程

    /* Create the thread which warns of long semaphore waits */
    /* 创建警告长时间信号量等待的线程 */
    srv_threads.m_error_monitor = os_thread_create(srv_error_monitor_thread_key,
                                                   0, srv_error_monitor_thread); // 创建错误监视线程

    srv_threads.m_error_monitor.start(); // 启动错误监视线程

    /* Create the thread which prints InnoDB monitor info */
    /* 创建打印 InnoDB 监视器信息的线程 */
    if (!srv_monitor_thread_created) { // 如果监视器线程未创建
      srv_threads.m_monitor =
          os_thread_create(srv_monitor_thread_key, 0, srv_monitor_thread); // 创建监视器线程

      srv_threads.m_monitor.start(); // 启动监视器线程
      srv_monitor_thread_created = true; // 设置 srv_monitor_thread_created 为 true
    }
  }

  /* wake main loop of page cleaner up */
  /* 唤醒页面清理器的主循环 */
  os_event_set(buf_flush_event); // 设置 buf_flush_event 事件

  srv_sys_tablespaces_open = true; // 设置 srv_sys_tablespaces_open 为 true

  /* Rotate the encryption key for recovery. It's because
  server could crash in middle of key rotation. Some tablespace
  didn't complete key rotation. Here, we will resume the
  rotation. */
  /* 旋转加密密钥以进行恢复。这是因为服务器可能在密钥旋转中途崩溃。一些表空间未完成密钥旋转。在这里，我们将恢复旋转。 */
  if (!srv_read_only_mode && !create_new_db &&
      srv_force_recovery < SRV_FORCE_NO_LOG_REDO) { // 如果不是只读模式且未创建新数据库且 srv_force_recovery 小于 SRV_FORCE_NO_LOG_REDO
    size_t fail_count = fil_encryption_rotate(); // 旋转加密密钥
    if (fail_count > 0) { // 如果失败计数大于 0
      ib::info(ER_IB_MSG_1146)
          << "During recovery, fil_encryption_rotate() failed for "
          << fail_count << " tablespace(s)."; // 记录加密密钥旋转失败的信息
    }
  }

  srv_is_being_started = false; // 设置 srv_is_being_started 为 false

  ut_a(trx_purge_state() == PURGE_STATE_INIT); // 确保事务清除状态为 PURGE_STATE_INIT

  sum_of_data_file_sizes = srv_sys_space.get_sum_of_sizes(); // 获取数据文件大小总和
  ut_a(sum_of_new_sizes != FIL_NULL); // 确保 sum_of_new_sizes 不为 FIL_NULL

  tablespace_size_in_header = fsp_header_get_tablespace_size(); // 获取表空间头的大小

  if (!srv_read_only_mode && !srv_sys_space.can_auto_extend_last_file() &&
      sum_of_data_file_sizes != tablespace_size_in_header) { // 如果不是只读模式且不能自动扩展最后一个文件且数据文件大小总和不等于表空间头的大小
    ib::error(ER_IB_MSG_1147, ulong{tablespace_size_in_header},
              ulong{sum_of_data_file_sizes}); // 记录错误信息

    if (srv_force_recovery == 0 &&
        sum_of_data_file_sizes < tablespace_size_in_header) { // 如果 srv_force_recovery 为 0 且数据文件大小总和小于表空间头的大小
      /* This is a fatal error, the tail of a tablespace is
      missing */
      /* 这是一个致命错误，表空间的尾部丢失 */

      ib::error(ER_IB_MSG_1148); // 记录错误信息

      return (srv_init_abort(DB_ERROR)); // 中止初始化
    }
  }

  if (!srv_read_only_mode && srv_sys_space.can_auto_extend_last_file() &&
      sum_of_data_file_sizes < tablespace_size_in_header) { // 如果不是只读模式且可以自动扩展最后一个文件且数据文件大小总和小于表空间头的大小
    ib::error(ER_IB_MSG_1149, ulong{tablespace_size_in_header},
              ulong{sum_of_data_file_sizes}); // 记录错误信息

    if (srv_force_recovery == 0) { // 如果 srv_force_recovery 为 0
      ib::error(ER_IB_MSG_1150); // 记录错误信息

      return (srv_init_abort(DB_ERROR)); // 中止初始化
    }
  }

  if (!srv_file_per_table && srv_pass_corrupt_table) { // 如果不是每个表一个文件且通过损坏的表
    ib::warn() << "The option innodb_file_per_table is disabled, so using the "
                  "option innodb_pass_corrupt_table doesn't make sense."; // 记录警告信息
  }

  /* Finish clone files recovery. This call is idempotent and is no op
  if it is already done before creating new log files. */
  /* 完成克隆文件恢复。此调用是幂等的，如果在创建新日志文件之前已经完成，则无操作。 */
  clone_files_recovery(true); // 完成克隆文件恢复

  ib::info(ER_IB_MSG_1151,
           "Percona XtraDB (http://www.percona.com) " INNODB_VERSION_STR,
           ulonglong{log_get_lsn(*log_sys)}); // 记录 Percona XtraDB 信息

  return (DB_SUCCESS); // 返回 DB_SUCCESS
}

/** Applier of dynamic metadata */
struct metadata_applier {
  /** Default constructor */
  metadata_applier() = default;
  /** Visitor.
  @param[in]      table   table to visit */
  void operator()(dict_table_t *table) const {
    ut_ad(dict_sys->dynamic_metadata != nullptr);
    uint64_t autoinc = table->autoinc;
    dict_table_load_dynamic_metadata(table);
    /* For those tables which were not opened by
    ha_innobase::open() and not initialized by
    innobase_initialize_autoinc(), the next counter should be
    advanced properly */
    if (autoinc != table->autoinc && table->autoinc != ~0ULL) {
      ++table->autoinc;
    }
  }
};

/** Apply the dynamic metadata to all tables */
static void apply_dynamic_metadata() {
  const metadata_applier applier;

  dict_sys->for_each_table(applier);

  if (srv_dict_metadata != nullptr) {
    srv_dict_metadata->apply();
    ut::delete_(srv_dict_metadata);
    srv_dict_metadata = nullptr;
  }
}

/** On a restart, initialize the remaining InnoDB subsystems so that
any tables (including data dictionary tables) can be accessed. */
/** 在重启时，初始化剩余的 InnoDB 子系统，以便可以访问任何表（包括数据字典表）。 */
void srv_dict_recover_on_restart() {
  /* Resurrect locks for dictionary transactions */
  /* 恢复数据字典事务的锁 */
  trx_resurrect_locks(false);

  /* Roll back any recovered data dictionary transactions, so
  that the data dictionary tables will be free of any locks.
  The data dictionary latch should guarantee that there is at
  most one data dictionary transaction active at a time. */
  /* 回滚任何恢复的数据字典事务，以便数据字典表没有任何锁。
  数据字典锁应保证一次最多只有一个数据字典事务处于活动状态。 */
  if (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO && trx_sys_need_rollback()) {
    trx_rollback_or_clean_recovered(false);
  }

  /* Resurrect locks for non-dictionary transactions only after rolling back all
  dictionary transactions. This is required as of today since we read
  uncommitted data while constructing table object in dd_table_open_on_id_low.
  This is done only while looking for the DD space object
  client->acquire_uncached_uncommitted<dd::Tablespace>().

  TODO-1: dd_table_open_on_id_low : Reading uncommitted data doesn't seem
  correct and needs to be analyzed and possibly fixed.

  Till that time we let all DD transactions to rollback to avoid reading dirty
  data from incomplete DDL commands while resurrecting locks. It essentially
  fixes two independent issues.

  1. Not able to resurrect table locks for uncommitted transaction.

  2. Not able to load innodb dict_* object for the table involved in the DDL.
     This could result in much more serious issue when binary log is enabled
     and crash happens after the transaction is prepared. Currently in binlog
     transaction recovery path no session THD is created and we rely on cached
     dict_* object to find out if a table is dropped. If the dict_table_t
     object is not already loaded, the table is considered dropped and undo
     apply is skipped. This would further result in uncommitted but prepared
     transaction data being committed and persisted.

  TODO-2: Have session (THD) while doing binary log recovery. The lack of
  THD seems not correct since rollback requires DD metadata. This alone would
  have prevented transaction inconsistency between innodb and binlog even if we
  failed to resurrect the table locks.
  binlog_recover->ha_recover->xarecover_handlerton->innobase_rollback_by_xid
  ->innobase_rollback_trx

  Note: The current work around fixes both issues but ideally should not be
  required if base issues [TODOs] are fixed. */
  /* 仅在回滚所有数据字典事务后恢复非数据字典事务的锁。因为在构建表对象时我们读取未提交的数据，
  这是必须的。这仅在查找 DD 空间对象时完成
  client->acquire_uncached_uncommitted<dd::Tablespace>()。

  TODO-1: dd_table_open_on_id_low：读取未提交的数据似乎不正确，需要分析并可能修复。

  在此之前，我们让所有 DD 事务回滚，以避免在恢复锁时读取不完整 DDL 命令的脏数据。这本质上解决了两个独立的问题。

  1. 无法恢复未提交事务的表锁。

  2. 无法加载涉及 DDL 的表的 innodb dict_* 对象。
     当启用二进制日志并且在事务准备后发生崩溃时，这可能会导致更严重的问题。目前在 binlog 事务恢复路径中没有创建会话 THD，
     我们依赖缓存的 dict_* 对象来确定表是否已删除。如果 dict_table_t 对象尚未加载，则认为表已删除并跳过撤销应用。
     这将进一步导致未提交但已准备的事务数据被提交和持久化。

  TODO-2: 在执行二进制日志恢复时拥有会话（THD）。缺少 THD 似乎不正确，因为回滚需要 DD 元数据。
  即使我们未能恢复表锁，这也可以防止 innodb 和 binlog 之间的事务不一致。
  binlog_recover->ha_recover->xarecover_handlerton->innobase_rollback_by_xid
  ->innobase_rollback_trx

  注意：当前的解决方法解决了这两个问题，但理想情况下，如果基础问题 [TODOs] 得到解决，则不需要。 */
  trx_resurrect_locks(true);

  trx_clear_resurrected_table_ids();

  /* Do after all DD transactions recovery, to get consistent metadata */
  /* 在所有 DD 事务恢复后执行，以获得一致的元数据 */
  apply_dynamic_metadata();

  if (srv_force_recovery < SRV_FORCE_NO_IBUF_MERGE) {
    srv_sys_tablespaces_open = true;
  }
}

/** Start purge threads. During upgrade we start
purge threads early to apply purge. */
void srv_start_purge_threads() {
  /* Start purge threads only if they are not started earlier. */
  if (srv_start_state_is_set(SRV_START_STATE_PURGE)) {
    return;
  }

  srv_threads.m_purge_coordinator =
      os_thread_create(srv_purge_thread_key, 0, srv_purge_coordinator_thread);

  srv_threads.m_purge_workers[0] = srv_threads.m_purge_coordinator;

  /* We've already created the purge coordinator thread above. */
  for (size_t i = 1; i < srv_threads.m_purge_workers_n; ++i) {
    srv_threads.m_purge_workers[i] =
        os_thread_create(srv_worker_thread_key, i, srv_worker_thread);
  }

  for (size_t i = 0; i < srv_threads.m_purge_workers_n; ++i) {
    srv_threads.m_purge_workers[i].start();
  }

  srv_start_wait_for_purge_to_start();

  srv_start_state_set(SRV_START_STATE_PURGE);
}

/** Start up the InnoDB service threads which are independent of DDL recovery
@param[in]      bootstrap       True if this is in bootstrap */
/** 启动独立于DDL恢复的InnoDB服务线程
@param[in]      bootstrap       如果是引导启动则为True */
void srv_start_threads(bool bootstrap) {
  if (!srv_read_only_mode) {
    /* Before 8.0, it was master thread that was doing periodical
    checkpoints (every 7s). Since 8.0, it is the log checkpointer
    thread, which is owned by log_sys, that is responsible for
    periodical checkpoints (every innodb_log_checkpoint_every ms).
    Note that the log checkpointer thread was created earlier and
    is already active, but the periodical checkpoints were disabled.
    Only the required checkpoints were allowed, which includes:
            - checkpoints because of too old last_checkpoint_lsn,
            - checkpoints explicitly requested (because of call to
              log_make_latest_checkpoint()).
    The reason was to make the situation more deterministic during
    the startup, because then:
            - it is easier to write mtr tests,
            - there are less possible flows - smaller risk of bug.
    Now we start allowing periodical checkpoints! Since now, it's
    hard to predict when checkpoints are written! */
    
    // 启用定期检查点
    log_limits_mutex_enter(*log_sys);
    log_sys->periodical_checkpoints_enabled = true; 
    log_limits_mutex_exit(*log_sys);
  }

  // 创建缓冲池大小调整线程
  srv_threads.m_buf_resize =
      os_thread_create(buf_resize_thread_key, 0, buf_resize_thread);

  srv_threads.m_buf_resize.start();

  // 只读模式下禁用清除操作
  if (srv_read_only_mode) {
    purge_sys->state = PURGE_STATE_DISABLED;
    return;
  }

  // 如果需要回滚恢复的事务,创建事务恢复回滚线程
  if (!bootstrap && srv_force_recovery < SRV_FORCE_NO_TRX_UNDO &&
      trx_sys_need_rollback()) {
    /* Rollback all recovered transactions that are
    not in committed nor in XA PREPARE state. */
    srv_threads.m_trx_recovery_rollback = os_thread_create(
        trx_recovery_rollback_thread_key, 0, trx_recovery_rollback_thread);

    srv_threads.m_trx_recovery_rollback.start();
  }

  /* Enable row log encryption if it is set */
  // 如果设置了行日志加密,则启用它
  log_tmp_enable_encryption_if_set();

  /* Create the master thread which does purge and other utility
  operations */
  // 创建主线程,用于清除和其他实用操作
  srv_threads.m_master =
      os_thread_create(srv_master_thread_key, 0, srv_master_thread);

  srv_threads.m_master.start();

  if (srv_force_recovery == 0) {
    /* In the insert buffer we may have even bigger tablespace
    id's, because we may have dropped those tablespaces, but
    insert buffer merge has not had time to clean the records from
    the ibuf tree. */

    // 更新插入缓冲区的最大表空间ID
    ibuf_update_max_tablespace_id();
  }

  /* Create the dict stats gathering thread */
  // 创建数据字典统计信息收集线程
  srv_threads.m_dict_stats =
      os_thread_create(dict_stats_thread_key, 0, dict_stats_thread);

  dict_stats_thread_init();

  srv_threads.m_dict_stats.start();

  /* Create the thread that will optimize the FTS sub-system. */
  // 创建优化全文搜索子系统的线程
  fts_optimize_init();

  // 设置启动状态为STAT
  srv_start_state_set(SRV_START_STATE_STAT);
}

void srv_start_threads_after_ddl_recovery() {
  /* Start the buffer pool dump/load thread, which will access spaces thus
        must wait for DDL recovery */
  srv_threads.m_buf_dump =
      os_thread_create(buf_dump_thread_key, 0, buf_dump_thread);

  srv_threads.m_buf_dump.start();

  /* Resume unfinished (un)encryption process in background thread. */
  if (!ts_encrypt_ddl_records.empty()) {
    srv_threads.m_ts_alter_encrypt =
        os_thread_create(srv_ts_alter_encrypt_thread_key, 0,
                         fsp_init_resume_alter_encrypt_tablespace);

    mysql_mutex_lock(&resume_encryption_cond_m);
    srv_threads.m_ts_alter_encrypt.start();
    /* Wait till shared MDL is taken by background thread for all tablespaces,
    for which (un)encryption is to be rolled forward. */
    mysql_cond_wait(&resume_encryption_cond, &resume_encryption_cond_m);
    mysql_mutex_unlock(&resume_encryption_cond_m);
  }

  /* Start and consume all GTIDs for recovered transactions. */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  gtid_persistor.start();

  DBUG_EXECUTE_IF("crash_before_purge_thread", DBUG_SUICIDE(););

  /* Now the InnoDB Metadata and file system should be consistent.
  Start the Purge thread */
  srv_start_purge_threads();

  /* If recovered, should do write back the dynamic metadata. */
  dict_persist_to_dd_table_buffer();
}

/** Set srv_shutdown_state to a given state and validate change is proper.
@remarks This function is used only from the main thread, and only during
startup or shutdown.
@param[in]  new_state   new state to set */
static void srv_shutdown_set_state(srv_shutdown_t new_state) {
  ut_a(static_cast<int>(srv_shutdown_state.load()) + 1 ==
       static_cast<int>(new_state));

  srv_shutdown_state.store(new_state);
}

static void srv_shutdown_cleanup_and_master_stop();

bool srv_shutdown_waits_for_rollback_of_recovered_transactions() {
  return (srv_force_recovery < SRV_FORCE_NO_TRX_UNDO && srv_fast_shutdown == 0);
}

/** Shut down all InnoDB background tasks that may look up objects in
the data dictionary. */
void srv_pre_dd_shutdown() {
  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_NONE);

  /* Warn and wait if there are still some query threads alive.
  If all is correct, then all user threads should already be gone,
  because before clean_up() -> srv_pre_dd_shutdown() is called,
  we are joining signal_hand thread, which before exiting waits
  for all connections to be closed (close_connections()). */
  for (size_t count = 0; count < 10; ++count) {
    const auto threads_count = srv_conc_get_active_threads();
    if (threads_count == 0) {
      break;
    }
    ib::warn(ER_IB_MSG_1154, threads_count);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }

  /* Crash if some query threads are still alive. */
  ut_a(srv_conc_get_active_threads() == 0);

  ut_a(!srv_thread_is_active(srv_threads.m_recv_writer));

  /* Avoid fast shutdown, if redo logging is disabled. Otherwise, we won't be
  able to recover. */
  if (mtr_t::s_logging.is_disabled() && srv_fast_shutdown == 2) {
    ib::warn(ER_IB_WRN_FAST_SHUTDOWN_REDO_DISABLED);
    srv_fast_shutdown = 1;
  }

  /* Stop service for persisting GTID */
  auto &gtid_persistor = clone_sys->get_gtid_persistor();
  gtid_persistor.stop();

  if (srv_read_only_mode) {
    /* Check that goal of SRV_SHUTDOWN_RECOVERY_ROLLBACK is reached:
    1. In read-only mode, no rollbacks should be executed.
    2. The trx_recovery_rollback thread should not be started. */
    ut_ad(trx_sys_recovered_active_trxs_count() == 0);
    ut_a(!srv_thread_is_active(srv_threads.m_trx_recovery_rollback));

    /* Check the goal of SRV_SHUTDOWN_PRE_DD_AND_SYSTEM_TRANSACTIONS,
    the following threads should not be started in read-only mode: */
    ut_a(!srv_thread_is_active(srv_threads.m_dict_stats));
    ut_a(!srv_thread_is_active(srv_threads.m_fts_optimize));
    ut_a(!srv_thread_is_active(srv_threads.m_ts_alter_encrypt));

    /* In read-only mode, there is no master thread. */
    ut_a(!srv_thread_is_active(srv_threads.m_master));

    /* In read-only mode, no purge should be done, so goal of the
    SRV_SHUTDOWN_PURGE is already satisfied (no purge threads). */
    ut_a(!srv_purge_threads_active());

    /* Advance quickly through all states to SRV_SHUTDOWN_DD. */
    srv_shutdown_set_state(SRV_SHUTDOWN_RECOVERY_ROLLBACK);
    srv_shutdown_set_state(SRV_SHUTDOWN_PRE_DD_AND_SYSTEM_TRANSACTIONS);
    srv_shutdown_set_state(SRV_SHUTDOWN_PURGE);
    srv_shutdown_set_state(SRV_SHUTDOWN_DD);
    return;
  }

  srv_shutdown_set_state(SRV_SHUTDOWN_RECOVERY_ROLLBACK);

  if (srv_shutdown_waits_for_rollback_of_recovered_transactions()) {
    /* We need to wait for rollback of recovered transactions. */
    for (uint32_t count = 0;; ++count) {
      /* Should not loop and wait if rollback thread isn't there. */
      if (!srv_thread_is_active(srv_threads.m_trx_recovery_rollback)) {
        break;
      }
      const auto total_trx = trx_sys_recovered_active_trxs_count();
      if (total_trx == 0) {
        break;
      }
      if (count >= SHUTDOWN_SLEEP_ROUNDS) {
        ib::info(ER_IB_MSG_1249, total_trx);
        count = 0;
      }
      std::this_thread::sleep_for(
          std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));
    }
  }

  if (srv_thread_is_active(srv_threads.m_trx_recovery_rollback)) {
    /* We should wait until rollback after recovery end to avoid
    adding more for purge and to avoid touching transaction objects
    since this point. */
    srv_threads.m_trx_recovery_rollback.wait();
  }

  srv_shutdown_set_state(SRV_SHUTDOWN_PRE_DD_AND_SYSTEM_TRANSACTIONS);

  if (srv_start_state_is_set(SRV_START_STATE_STAT)) {
    fts_optimize_shutdown();
    dict_stats_shutdown();
    dict_stats_thread_deinit();
  }
  ut_a(!srv_thread_is_active(srv_threads.m_fts_optimize));
  ut_a(!srv_thread_is_active(srv_threads.m_dict_stats));

  for (uint32_t count = 1; srv_thread_is_active(srv_threads.m_ts_alter_encrypt);
       ++count) {
    if (count % SHUTDOWN_SLEEP_ROUNDS == 0) {
      ib::info(ER_IB_MSG_WAIT_FOR_ENCRYPT_THREAD);
    }
    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));
  }

  /* Wait until the master thread exits its main loop and notices that:
    - it should do shutdown-cleanup,
    - and still is allowed to access DD objects. */
  if (srv_thread_is_active(srv_threads.m_master)) {
    srv_wake_master_thread();
    os_event_wait(srv_threads.m_master_ready_for_dd_shutdown);
  }

  /* Since this point we do not expect accesses to DD coming from InnoDB. */
  ut_d(trx_sys_before_pre_dd_shutdown_validate());

  srv_shutdown_set_state(SRV_SHUTDOWN_PURGE);

  for (uint32_t count = 1; srv_purge_threads_active(); ++count) {
    srv_purge_wakeup();
    if (count % SHUTDOWN_SLEEP_ROUNDS == 0) {
      ib::info(ER_IB_MSG_1152);
    }
    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));
  }
  switch (trx_purge_state()) {
    case PURGE_STATE_INIT:
    case PURGE_STATE_EXIT:
    case PURGE_STATE_DISABLED:
      srv_start_state &= ~SRV_START_STATE_PURGE;
      break;
    case PURGE_STATE_RUN:
    case PURGE_STATE_STOP:
      ut_d(ut_error);
  }

  /* After this phase plugins are asked to be shut down, in which case they
  will be marked as DELETED. Note: we cannot leave any transaction in the THD,
  because the mechanism which cleans resources in THD would not be able to
  unregister those transactions from mysql_trx_list, because the handler
  of close_connection in InnoDB handlerton would not be called, because
  InnoDB has already been marked as DELETED. You should close your thread
  here, in the srv_pre_dd_shutdown, if it might do lookups in DD objects.
  No other transactions should be useful, so for sake of simplicity we
  require to have no transactions at all here, except transactions:
    - with state = TRX_STATE_PREPARED,
    - with state = TRX_STATE_ACTIVE and with is_recovered == true */

  ut_d(trx_sys_after_pre_dd_shutdown_validate());

  srv_shutdown_set_state(SRV_SHUTDOWN_DD);

  DBUG_EXECUTE_IF("wait_for_threads_in_pre_dd_shutdown",
                  srv_shutdown_cleanup_and_master_stop(););
}

/** Shutdown background threads of InnoDB at the start of the shutdown phase.
Handles shutdown phases: SRV_SHUTDOWN_CLEANUP and SRV_SHUTDOWN_MASTER_STOP. */
static void srv_shutdown_cleanup_and_master_stop() {
  DBUG_EXECUTE_IF("threads_wait_on_cleanup",
                  os_event_set(srv_threads.m_shutdown_cleanup_dbg););

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_DD);

  srv_shutdown_set_state(SRV_SHUTDOWN_CLEANUP);

  const srv_shutdown_t max_wait_on_state{SRV_SHUTDOWN_MASTER_STOP};

  uint32_t count = 0;

  for (;;) {
    /* Print messages every 60 seconds when we are waiting for any
    of those threads to exit. */
    bool print;
    if (count >= SHUTDOWN_SLEEP_ROUNDS) {
      print = true;
      count = 0;
    } else {
      print = false;
    }

    size_t active_found = 0;
    for (const auto &thread_info : threads_to_stop) {
      ut_a(thread_info.m_wait_on_state <= max_wait_on_state);
      if (thread_info.m_wait_on_state == srv_shutdown_state.load() &&
          srv_thread_is_active(thread_info.m_thread)) {
        ++active_found;
        if (print) {
          ib::info(ER_IB_MSG_1248, thread_info.m_name);
        }
        thread_info.m_notify();
      }
    }

    if (active_found == 0) {
      if (srv_shutdown_state.load() == max_wait_on_state) {
        break;
      }
      srv_shutdown_set_state(static_cast<srv_shutdown_t>(
          static_cast<int>(srv_shutdown_state.load()) + 1));
    }

    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));
    ++count;
  }

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_MASTER_STOP);

  ut_d(trx_sys_after_background_threads_shutdown_validate());
}

/** Waits for page cleaners exit. */
static void srv_shutdown_page_cleaners() {
  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_MASTER_STOP);
  ut_a(!srv_master_thread_is_active());

  srv_shutdown_set_state(SRV_SHUTDOWN_FLUSH_PHASE);

  buf_pool_wait_for_no_pending_io();

  /* At this point only page_cleaner should be active. We wait
  here to let it complete the flushing of the buffer pools
  before proceeding further. */

  for (uint32_t count = 0; buf_flush_page_cleaner_is_active() ||
                           buf_flush_active_lru_managers() > 0;
       ++count) {
    if (count >= SHUTDOWN_SLEEP_ROUNDS) {
      ib::info(ER_IB_MSG_1251);
      count = 0;
    }
    os_event_set(buf_flush_event);
    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));
  }

  ut_ad(buf_flush_active_lru_managers() == 0);

  ut_ad(buf_pool_pending_io_reads_count() == 0);
  ut_ad(buf_pool_pending_io_writes_count() == 0);
}

/** Closes redo log. If this is not fast shutdown, it forces to write a
checkpoint which should be written for logically empty redo log. Note that we
forced to flush all dirty pages in the last stages of page cleaners activity
(unless it was fast shutdown). After checkpoint is written, the flushed_lsn is
updated within header of the system tablespace. This is lsn of the last clean
shutdown. */
/** 关闭重做日志。如果这不是快速关闭，它会强制写入一个检查点，该检查点应该为逻辑上为空的重做日志写入。请注意，我们在页面清理器活动的最后阶段强制刷新所有脏页（除非是快速关闭）。检查点写入后，flushed_lsn 会在系统表空间的头部更新。这是最后一次干净关闭的 lsn。 */
static lsn_t srv_shutdown_log() {
  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_FLUSH_PHASE); // 断言关闭状态为 SRV_SHUTDOWN_FLUSH_PHASE
  ut_a(!buf_flush_page_cleaner_is_active()); // 断言页面清理器未激活
  ut_ad(buf_pool_pending_io_reads_count() == 0); // 断言缓冲池待处理的 IO 读取计数为 0
  ut_ad(buf_pool_pending_io_writes_count() == 0); // 断言缓冲池待处理的 IO 写入计数为 0

  if (srv_fast_shutdown == 2) { // 如果是快速关闭
    if (!srv_read_only_mode) { // 如果不是只读模式
      ib::info(ER_IB_MSG_1253); // 打印信息

      /* In this fastest shutdown we do not flush the
      buffer pool:

      it is essentially a 'crash' of the InnoDB server.
      Make sure that the log is all flushed to disk, so
      that we can recover all committed transactions in
      a crash recovery. We must not write the lsn stamps
      to the data files, since at a startup InnoDB deduces
      from the stamps if the previous shutdown was clean. */

      /* 在这种最快的关闭中，我们不会刷新缓冲池：

      它本质上是 InnoDB 服务器的“崩溃”。
      确保日志全部刷新到磁盘，以便我们可以在崩溃恢复中恢复所有已提交的事务。
      我们不能将 lsn 标记写入数据文件，因为在启动时 InnoDB 会从标记中推断出上次关闭是否干净。 */

      log_stop_background_threads(*log_sys); // 停止后台线程
    }

    /* No redo log might be generated since now. */
    /* 从现在起不会生成重做日志。 */
    log_background_threads_inactive_validate(); // 验证后台线程未激活

    srv_shutdown_set_state(SRV_SHUTDOWN_LAST_PHASE); // 设置关闭状态为 SRV_SHUTDOWN_LAST_PHASE

    return (log_get_lsn(*log_sys)); // 返回当前 lsn
  }

  if (!srv_read_only_mode) { // 如果不是只读模式
    log_make_empty_and_stop_background_threads(*log_sys); // 清空重做日志并停止后台线程
  }

  /* No redo log might be generated since now. */
  /* 从现在起不会生成重做日志。 */
  log_background_threads_inactive_validate(); // 验证后台线程未激活
  buf_must_be_all_freed(); // 确保缓冲池已全部释放

  lsn_t lsn = log_get_lsn(*log_sys); // 获取当前 lsn

  if (!srv_read_only_mode) { // 如果不是只读模式
    /* Redo log has been flushed at the log_flusher's exit. */
    /* 重做日志已在 log_flusher 退出时刷新。 */
    fil_flush_file_spaces(); // 刷新文件空间
  }

  srv_shutdown_set_state(SRV_SHUTDOWN_LAST_PHASE); // 设置关闭状态为 SRV_SHUTDOWN_LAST_PHASE

  /* Validate lsn and write it down. */
  /* 验证 lsn 并写入。 */
  ut_a(log_is_data_lsn(lsn) || srv_force_recovery >= SRV_FORCE_NO_LOG_REDO); // 断言 lsn 有效或强制恢复级别大于等于 SRV_FORCE_NO_LOG_REDO

  ut_a(lsn == log_sys->last_checkpoint_lsn.load() ||
       srv_force_recovery >= SRV_FORCE_NO_LOG_REDO); // 断言 lsn 等于最后一个检查点 lsn 或强制恢复级别大于等于 SRV_FORCE_NO_LOG_REDO
  ut_a(lsn == log_get_lsn(*log_sys)); // 断言 lsn 等于当前 lsn

  if (!srv_read_only_mode) { // 如果不是只读模式
    ut_a(srv_force_recovery < SRV_FORCE_NO_LOG_REDO); // 断言强制恢复级别小于 SRV_FORCE_NO_LOG_REDO

    auto err = fil_write_flushed_lsn(lsn); // 写入 flushed_lsn
    ut_a(err == DB_SUCCESS); // 断言写入成功
  }
  buf_must_be_all_freed(); // 确保缓冲池已全部释放
  ut_a(lsn == log_get_lsn(*log_sys)); // 断言 lsn 等于当前 lsn

  if (srv_downgrade_logs) { // 如果降级日志
    ut_a(!srv_read_only_mode); // 断言不是只读模式

    /* InnoDB in any version is able to start on empty set of redo files. */
    /* 任何版本的 InnoDB 都能够在空的重做文件集上启动。 */
    log_files_remove(*log_sys); // 删除重做日志文件
  }

  return (lsn); // 返回 lsn
}

/** Copy all remaining data and shutdown archiver threads. */
static void srv_shutdown_arch() {
  uint32_t count = 0;

  while (arch_wake_threads()) {
    ++count;
    std::this_thread::sleep_for(
        std::chrono::microseconds(SHUTDOWN_SLEEP_TIME_US));

    if (count > SHUTDOWN_SLEEP_ROUNDS) {
      ib::info(ER_IB_MSG_1246);
      count = 0;
    }
  }
}

void srv_thread_delay_cleanup_if_needed(bool wait_for_signal) {
  DBUG_EXECUTE_IF("threads_wait_on_cleanup", {
    if (wait_for_signal) {
      os_event_wait(srv_threads.m_shutdown_cleanup_dbg);
    } else {
      /* In some cases we cannot wait for the signal, because we would otherwise
      never reach the end of pre_dd_shutdown, because pre_dd_shutdown is waiting
      for this thread before it ends. Then we would never reach shutdown phase
      in which the signal becomes signalled. Still we would like to have a way
      to detect situation in which someone broke the code and pre_dd_shutdown
      no longer waits for this thread. */
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
}

extern bool innodb_inited;

/** Shut down the InnoDB database. */
/** 关闭 InnoDB 数据库。 */
void srv_shutdown() {
  ut_d(trx_sys_after_pre_dd_shutdown_validate()); // 调用事务系统的关闭验证函数

  /* Need to revert partition file names if minor upgrade fails. */
  /* 如果小版本升级失败，需要恢复分区文件名。 */
  uint data_version = MYSQL_VERSION_ID; // 获取 MySQL 版本 ID

  if (!fsp_header_dict_get_server_version(&data_version) &&
      data_version != MYSQL_VERSION_ID) { // 如果获取服务器版本失败且版本不匹配
    srv_downgrade_partition_files = true; // 设置降级分区文件标志
  }

  ib::info(ER_IB_MSG_1247); // 打印信息

  if (innodb_inited) ut_a(!srv_is_being_started); // 如果 InnoDB 已初始化，断言未在启动中

  /* Ensure threads below have been stopped. */
  /* 确保以下线程已停止。 */
  const auto threads_stopped_before_shutdown = {
      std::cref(srv_threads.m_purge_coordinator), // 清理协调器线程
      std::cref(srv_threads.m_ts_alter_encrypt), // 表空间加密线程
      std::cref(srv_threads.m_fts_optimize), // 全文搜索优化线程
      std::cref(srv_threads.m_recv_writer), // 恢复写入线程
      std::cref(srv_threads.m_dict_stats)}; // 数据字典统计线程

  for (const auto &thread : threads_stopped_before_shutdown) {
    ut_a(!srv_thread_is_active(thread)); // 断言线程未激活
  }

#ifdef UNIV_DEBUG
  /* In DEBUG we might be testing scenario in which we forced to
  call srv_shutdown_cleanup_and_master_stop() to stop all threads
  at the end of the srv_pre_dd_shutdown(). */
  /* 在调试模式下，我们可能会测试强制调用 srv_shutdown_cleanup_and_master_stop() 在 srv_pre_dd_shutdown() 结束时停止所有线程的场景。 */
  DBUG_EXECUTE_IF("wait_for_threads_in_pre_dd_shutdown",
                  srv_shutdown_state.store(SRV_SHUTDOWN_DD);); // 在调试模式下，设置关闭状态为 SRV_SHUTDOWN_DD
#endif /* UNIV_DEBUG */

  /* The SRV_SHUTDOWN_DD state was set during pre_dd_shutdown phase. */
  /* SRV_SHUTDOWN_DD 状态在 pre_dd_shutdown 阶段设置。 */
  if (!innodb_inited) srv_shutdown_state.store(SRV_SHUTDOWN_DD); // 如果 InnoDB 未初始化，设置关闭状态为 SRV_SHUTDOWN_DD
  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_DD); // 断言关闭状态为 SRV_SHUTDOWN_DD

  /* Write dynamic metadata to DD buffer table. */
  /* 将动态元数据写入 DD 缓冲表。 */
  dict_persist_to_dd_table_buffer(); // 将数据字典持久化到 DD 表缓冲区

  /* 0. Stop remaining background threads except:
    - page-cleaners - we are shutting down page cleaners in step 1
    - redo-log-threads - these need to be shutdown after page cleaners,
    - archiver threads - these need to be shutdown after redo threads.
  After this call the state of shutdown is advanced to SRV_SHUTDOWN_MASTER_STOP.
  */
  /* 0. 停止剩余的后台线程，除了：
    - 页面清理器 - 我们在步骤1中关闭页面清理器
    - 重做日志线程 - 这些需要在页面清理器之后关闭，
    - 归档线程 - 这些需要在重做线程之后关闭。
  在此调用之后，关闭状态将推进到 SRV_SHUTDOWN_MASTER_STOP。 */
  srv_shutdown_cleanup_and_master_stop(); // 清理并停止主线程

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_MASTER_STOP); // 断言关闭状态为 SRV_SHUTDOWN_MASTER_STOP

  /* Check again and write dynamic metadata to DD buffer table. Ideally we
  would not have dynamic metadata written so late in shutdown phase but
  currently we have certain operations done in master thread which could
  generate metadata. It is safe to check and write it here before we flush
  buffer pool to disk. */
  /* 再次检查并将动态元数据写入 DD 缓冲表。理想情况下，我们不会在关闭阶段这么晚写入动态元数据，但目前我们在主线程中完成某些操作，这些操作可能会生成元数据。在将缓冲池刷新到磁盘之前，在此处检查并写入是安全的。 */
  dict_persist_to_dd_table_buffer(); // 将数据字典持久化到 DD 表缓冲区

  /* The steps 1-4 is the real InnoDB shutdown.
  All before was to stop activity which could produce new changes.
  All after is just cleaning up (freeing memory). */
  /* 步骤1-4是实际的InnoDB关闭。
  之前的所有操作都是为了停止可能产生新更改的活动。
  之后的所有操作只是清理（释放内存）。 */

  /* 1. Flush the buffer pool to disk. */
  /* 1. 将缓冲池刷新到磁盘。 */
  srv_shutdown_page_cleaners(); // 关闭页面清理器

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_FLUSH_PHASE); // 断言关闭状态为 SRV_SHUTDOWN_FLUSH_PHASE

  /* 2. Write the current lsn to the tablespace header(s). */
  /* 2. 将当前 lsn 写入表空间头。 */
  const lsn_t shutdown_lsn = srv_shutdown_log(); // 获取关闭 lsn

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_LAST_PHASE); // 断言关闭状态为 SRV_SHUTDOWN_LAST_PHASE

  /* 3. Close all opened files. */
  /* 3. 关闭所有打开的文件。 */
  ibt::close_files(); // 关闭 ibt 文件
  fil_close_all_files(); // 关闭所有文件
  if (srv_monitor_file) {
    fclose(srv_monitor_file); // 关闭监控文件
  }
  if (srv_misc_tmpfile) {
    fclose(srv_misc_tmpfile); // 关闭临时文件
  }

  /* 4. Copy all log data to archive and stop archiver threads. */
  /* 4. 将所有日志数据复制到归档并停止归档线程。 */
  srv_shutdown_arch(); // 关闭归档

  /* This is to preserve the old style, we should finally get rid of the call
  here. For that, we need to ensure we have already effectively closed all
  threads. */
  /* 这是为了保留旧的风格，我们最终应该去掉这里的调用。为此，我们需要确保已经有效地关闭了所有线程。 */
  srv_shutdown_exit_threads(); // 退出线程

  ut_a(srv_shutdown_state.load() == SRV_SHUTDOWN_EXIT_THREADS); // 断言关闭状态为 SRV_SHUTDOWN_EXIT_THREADS
  ut_ad(!os_thread_any_active()); // 断言没有活动的操作系统线程

  /* 5. Free all the resources acquired by InnoDB (mutexes, events, memory). */
  /* 5. 释放 InnoDB 获取的所有资源（互斥锁、事件、内存）。 */
  ibt::delete_pool_manager(); // 删除池管理器

  if (srv_monitor_file) {
    srv_monitor_file = nullptr; // 置空监控文件指针
    if (srv_monitor_file_name) {
      unlink(srv_monitor_file_name); // 删除监控文件
      ut::free(srv_monitor_file_name); // 释放监控文件名
    }
    mutex_free(&srv_monitor_file_mutex); // 释放监控文件互斥锁
  }

  if (srv_misc_tmpfile) {
    srv_misc_tmpfile = nullptr; // 置空临时文件指针
    mutex_free(&srv_misc_tmpfile_mutex); // 释放临时文件互斥锁
  }

  /* This must be disabled before closing the buffer pool
  and closing the data dictionary.  */
  /* 在关闭缓冲池和数据字典之前，必须禁用此功能。 */
  btr_search_disable(); // 禁用 B 树搜索

  ibuf_close(); // 关闭插入缓冲区
  ddl_log_close(); // 关闭 DDL 日志
  log_sys_close(); // 关闭日志系统
  recv_sys_free(); // 释放恢复系统
  recv_sys_close(); // 关闭恢复系统
  trx_sys_close(); // 关闭事务系统
  lock_sys_close(); // 关闭锁系统
  trx_pool_close(); // 关闭事务池

  dict_close(); // 关闭数据字典
  dict_persist_close(); // 关闭数据字典持久化
  btr_search_sys_free(); // 释放 B 树搜索系统
  undo_spaces_deinit(); // 反初始化撤销表空间

  ut::delete_(srv_dict_metadata); // 删除数据字典元数据

  os_aio_free(); // 释放异步 IO
  que_close(); // 关闭查询
  row_mysql_close(); // 关闭行 MySQL
  srv_free(); // 释放服务
  fil_close(); // 关闭文件
  pars_close(); // 关闭解析器

  pars_lexer_close(); // 关闭解析器词法分析器
  buf_pool_free_all(); // 释放所有缓冲池

  /* 6. Free the thread management resources. */
  /* 6. 释放线程管理资源。 */
  clone_free(); // 释放克隆
  arch_free(); // 释放归档

  dblwr::close(); // 关闭双写缓冲区
  os_thread_close(); // 关闭操作系统线程

  meb::redo_log_archive_deinit(); // 反初始化重做日志归档

  /* 6. Free the synchronisation infrastructure. */
  /* 6. 释放同步基础设施。 */
  sync_check_close(); // 关闭同步检查

  ib::info(ER_IB_MSG_1155, ulonglong{shutdown_lsn}); // 打印信息

  srv_start_has_been_called = false; // 设置启动标志为 false
  srv_start_state = SRV_START_STATE_NONE; // 设置启动状态为 SRV_START_STATE_NONE
}

void srv_get_encryption_data_filename(dict_table_t *table, char *filename,
                                      ulint max_len) {
  /* Make sure the data_dir_path is set. */
  dd_get_and_save_data_dir_path<dd::Table>(table, nullptr, false);

  std::string path = dict_table_get_datadir(table);

  auto filepath = Fil_path::make(path, table->name.m_name, CFP, true);

  size_t len = strlen(filepath);
  ut_a(max_len >= len);

  strcpy(filename, filepath);

  ut::free(filepath);
}

/** Call std::_Exit(3) */
void srv_fatal_error() {
  ib::error(ER_IB_MSG_1156);

  fflush(stderr);

  ut_d(innodb_calling_exit = true);

  flush_error_log_messages();

  std::_Exit(3);
}
