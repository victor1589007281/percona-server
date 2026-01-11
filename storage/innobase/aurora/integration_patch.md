# Aurora Integration Patches for InnoDB

This document describes the patches needed to integrate Aurora with InnoDB.

## 1. Log Writer Integration (log0write.cc)

Add Aurora redo sending after buffer write:

```cpp
// File: storage/innobase/log/log0write.cc
// Location: After log buffer write, before fsync

#include "aurora/aurora.h"

// In log_writer_write_buffer() after memcpy to buffer:
static void log_writer_write_buffer(log_t &log, lsn_t start_lsn, 
                                    lsn_t end_lsn) {
  // ... existing code ...
  
  // Aurora integration: send redo to storage layer
  if (aurora::aurora_is_active() && aurora::aurora_is_writer()) {
    const byte *buf = log.buf + (start_lsn % log.buf_size);
    size_t len = end_lsn - start_lsn;
    aurora::aurora_on_redo_write(start_lsn, buf, len);
  }
  
  // ... rest of function ...
}
```

## 2. Buffer Pool Page Read (buf0rea.cc)

Intercept page reads:

```cpp
// File: storage/innobase/buf/buf0rea.cc
// Location: In buf_read_page_low()

#include "aurora/aurora.h"

static buf_page_t *buf_read_page_low(...) {
  // ... existing code to get block ...
  
  // Aurora integration: read from storage layer
  if (aurora::aurora_is_active()) {
    byte *frame = block->frame;
    if (aurora::aurora_on_page_read(space->id, page_no, 
                                     frame, UNIV_PAGE_SIZE)) {
      // Page read from Aurora, skip local file read
      goto aurora_page_done;
    }
  }
  
  // ... existing local file read code ...
  
aurora_page_done:
  // ... common completion code ...
}
```

## 3. Transaction Commit (trx0trx.cc)

Ensure redo durability before commit returns:

```cpp
// File: storage/innobase/trx/trx0trx.cc
// Location: In trx_commit_low()

#include "aurora/aurora.h"

static void trx_commit_low(trx_t *trx, mtr_t *mtr) {
  // ... existing code ...
  
  if (mtr != nullptr) {
    mtr_commit(mtr);
    
    // Aurora integration: wait for redo durability
    if (aurora::aurora_is_active() && aurora::aurora_is_writer()) {
      lsn_t commit_lsn = mtr->commit_lsn;
      uint32_t timeout_ms = aurora::aurora_config.request_timeout_ms;
      
      if (!aurora::aurora_on_trx_commit(commit_lsn, timeout_ms)) {
        // Handle timeout or failure
        ib::error() << "Aurora: redo durability wait failed for LSN " 
                    << commit_lsn;
      }
    }
  }
  
  // ... rest of function ...
}
```

## 4. InnoDB Startup (srv0start.cc)

Initialize Aurora during startup:

```cpp
// File: storage/innobase/srv/srv0start.cc
// Location: In srv_start()

#include "aurora/aurora.h"

dberr_t srv_start(...) {
  // ... existing initialization ...
  
  // Aurora integration: initialize after redo log setup
  if (aurora::aurora_enabled) {
    if (!aurora::aurora_init()) {
      ib::error() << "Aurora: initialization failed";
      return DB_ERROR;
    }
    ib::info() << "Aurora: initialized in " 
               << (aurora::aurora_is_writer() ? "writer" : "reader") 
               << " mode";
  }
  
  // ... rest of startup ...
}
```

## 5. InnoDB Shutdown (srv0start.cc)

Cleanup Aurora during shutdown:

```cpp
// File: storage/innobase/srv/srv0start.cc
// Location: In srv_shutdown()

void srv_shutdown() {
  // ... existing cleanup ...
  
  // Aurora integration: shutdown
  if (aurora::aurora_is_active()) {
    // Flush pending redo
    if (aurora::g_redo_sender) {
      aurora::g_redo_sender->flush_and_wait();
    }
    aurora::aurora_shutdown();
    ib::info() << "Aurora: shutdown complete";
  }
  
  // ... rest of shutdown ...
}
```

## 6. System Variables (ha_innodb.cc)

Register Aurora system variables:

```cpp
// File: storage/innobase/handler/ha_innodb.cc
// Location: In system variable definitions

// Aurora system variables
static MYSQL_SYSVAR_BOOL(aurora_enabled, aurora::aurora_enabled,
  PLUGIN_VAR_READONLY,
  "Enable Aurora storage integration",
  nullptr, nullptr, false);

static MYSQL_SYSVAR_STR(aurora_mode, aurora::aurora_mode_str,
  PLUGIN_VAR_READONLY,
  "Aurora instance mode: writer or reader",
  nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(aurora_instance_id, aurora::aurora_instance_id,
  PLUGIN_VAR_READONLY,
  "Aurora instance ID",
  nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(aurora_volume_id, aurora::aurora_volume_id,
  PLUGIN_VAR_READONLY,
  "Aurora storage volume ID",
  nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(aurora_storage_nodes, aurora::aurora_storage_nodes,
  PLUGIN_VAR_READONLY,
  "Comma-separated list of Aurora storage node addresses",
  nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(aurora_metadata_nodes, aurora::aurora_metadata_nodes,
  PLUGIN_VAR_READONLY,
  "Comma-separated list of Aurora metadata service addresses",
  nullptr, nullptr, "");

static MYSQL_SYSVAR_INT(aurora_quorum_vw, aurora::aurora_quorum_vw,
  PLUGIN_VAR_READONLY,
  "Aurora write quorum (default 4 of 6)",
  nullptr, nullptr, 4, 1, 6, 0);

static MYSQL_SYSVAR_INT(aurora_quorum_vr, aurora::aurora_quorum_vr,
  PLUGIN_VAR_READONLY,
  "Aurora read quorum (default 3 of 6)",
  nullptr, nullptr, 3, 1, 6, 0);

// Add to struct st_mysql_sys_var array:
static struct st_mysql_sys_var *innobase_system_variables[] = {
  // ... existing variables ...
  MYSQL_SYSVAR(aurora_enabled),
  MYSQL_SYSVAR(aurora_mode),
  MYSQL_SYSVAR(aurora_instance_id),
  MYSQL_SYSVAR(aurora_volume_id),
  MYSQL_SYSVAR(aurora_storage_nodes),
  MYSQL_SYSVAR(aurora_metadata_nodes),
  MYSQL_SYSVAR(aurora_quorum_vw),
  MYSQL_SYSVAR(aurora_quorum_vr),
  nullptr
};
```

## 7. Information Schema (i_s.cc)

Add Aurora status table:

```cpp
// File: storage/innobase/handler/i_s.cc
// Add new table INNODB_AURORA_STATUS

static ST_FIELD_INFO aurora_status_fields[] = {
  {"INSTANCE_ID", 64, MYSQL_TYPE_STRING, 0, 0, "", 0},
  {"MODE", 16, MYSQL_TYPE_STRING, 0, 0, "", 0},
  {"VOLUME_ID", 64, MYSQL_TYPE_STRING, 0, 0, "", 0},
  {"CURRENT_VDL", 21, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
  {"SENT_LSN", 21, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
  {"APPLIED_LSN", 21, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
  {"REPLICATION_LAG_MS", 21, MYSQL_TYPE_LONGLONG, 0, 0, "", 0},
  {"STORAGE_NODES_HEALTHY", 21, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
  {"STORAGE_NODES_TOTAL", 21, MYSQL_TYPE_LONGLONG, 0, MY_I_S_UNSIGNED, "", 0},
  {"METADATA_CONNECTED", 1, MYSQL_TYPE_TINY, 0, 0, "", 0},
  {nullptr, 0, MYSQL_TYPE_NULL, 0, 0, "", 0}
};

static int aurora_status_fill(THD *thd, TABLE_LIST *tables, Item *) {
  TABLE *table = tables->table;
  
  auto status = aurora::aurora_get_status();
  if (!status.initialized) {
    return 0;
  }
  
  table->field[0]->store(status.instance_id.c_str(), 
                         status.instance_id.length(), system_charset_info);
  table->field[1]->store(status.mode == aurora::AuroraMode::WRITER ? 
                         "writer" : "reader", 6, system_charset_info);
  table->field[2]->store(status.volume_id.c_str(),
                         status.volume_id.length(), system_charset_info);
  table->field[3]->store(status.current_vdl, true);
  table->field[4]->store(status.sent_lsn, true);
  table->field[5]->store(status.applied_lsn, true);
  table->field[6]->store(status.replication_lag_ms, false);
  table->field[7]->store(status.storage_nodes_healthy, true);
  table->field[8]->store(status.storage_nodes_total, true);
  table->field[9]->store(status.metadata_connected ? 1 : 0, true);
  
  schema_table_store_record(thd, table);
  return 0;
}
```

## Summary of Files to Modify

| File | Changes |
|------|---------|
| `CMakeLists.txt` | Add aurora subdirectory |
| `log/log0write.cc` | Redo sending hook |
| `buf/buf0rea.cc` | Page read hook |
| `trx/trx0trx.cc` | Commit durability hook |
| `srv/srv0start.cc` | Init/shutdown hooks |
| `handler/ha_innodb.cc` | System variables |
| `handler/i_s.cc` | Information schema table |

## Build Integration

Add to `storage/innobase/CMakeLists.txt`:

```cmake
# Aurora storage integration
OPTION(WITH_AURORA "Build with Aurora storage support" OFF)

IF(WITH_AURORA)
  ADD_SUBDIRECTORY(aurora)
  SET(INNOBASE_SOURCES ${INNOBASE_SOURCES}
    aurora/aurora.cc
    aurora/aurora_config.cc
    aurora/aurora_client.cc
    aurora/aurora_redo_sender.cc
    aurora/aurora_page_reader.cc
    aurora/aurora_reader_sync.cc
  )
  ADD_DEFINITIONS(-DWITH_AURORA=1)
ENDIF()
```
