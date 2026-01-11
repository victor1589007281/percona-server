/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Main Header
Main include file for Aurora integration with InnoDB.

*****************************************************************************/

#ifndef AURORA_H
#define AURORA_H

#include "aurora_config.h"
#include "aurora_types.h"
#include "aurora_client.h"
#include "aurora_redo_sender.h"
#include "aurora_page_reader.h"
#include "aurora_reader_sync.h"

namespace aurora {

struct AuroraStatus {
  bool initialized;
  bool healthy;
  AuroraMode mode;
  lsn_t current_vdl;
  lsn_t sent_lsn;
  lsn_t applied_lsn;
  int64_t replication_lag_ms;
  int storage_nodes_healthy;
  int storage_nodes_total;
  bool metadata_connected;
  std::string instance_id;
  std::string volume_id;
};

bool aurora_init();
void aurora_shutdown();
AuroraStatus aurora_get_status();
bool aurora_is_active();
void aurora_on_redo_write(lsn_t lsn, const uint8_t* log_block, size_t len);
bool aurora_on_page_read(uint32_t space_id, uint32_t page_id, uint8_t* buffer, size_t buf_len);
bool aurora_on_trx_commit(lsn_t commit_lsn, uint32_t timeout_ms);
void aurora_on_checkpoint();
bool aurora_on_failover(lsn_t* final_lsn);
void aurora_invalidate_buffer_pool(space_id_t space_id, page_id_t page_id);
RedoType aurora_convert_redo_type(uint8_t innodb_type);

extern bool aurora_enabled;
extern char* aurora_mode_str;
extern char* aurora_instance_id;
extern char* aurora_volume_id;
extern char* aurora_cluster_id;
extern char* aurora_storage_nodes;
extern char* aurora_metadata_nodes;
extern int aurora_quorum_n;
extern int aurora_quorum_vw;
extern int aurora_quorum_vr;
extern int aurora_connection_timeout_ms;
extern int aurora_request_timeout_ms;
extern bool aurora_compression_enabled;
extern int aurora_compression_level;

}  // namespace aurora

#endif  // AURORA_H
