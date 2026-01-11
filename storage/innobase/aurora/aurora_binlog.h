/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Binlog Adapter
Converts Redo logs to Binlog format for MySQL replication compatibility.

Reference: 01_compute_layer.md Section 9.4

*****************************************************************************/

#ifndef AURORA_BINLOG_H
#define AURORA_BINLOG_H

#include "aurora_gtid.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <condition_variable>

namespace aurora {

/**
 * Binlog Event Types (MySQL compatible)
 */
enum class BinlogEventType : uint8_t {
  UNKNOWN_EVENT = 0,
  START_EVENT_V3 = 1,
  QUERY_EVENT = 2,
  STOP_EVENT = 3,
  ROTATE_EVENT = 4,
  INTVAR_EVENT = 5,
  LOAD_EVENT = 6,
  SLAVE_EVENT = 7,
  CREATE_FILE_EVENT = 8,
  APPEND_BLOCK_EVENT = 9,
  EXEC_LOAD_EVENT = 10,
  DELETE_FILE_EVENT = 11,
  NEW_LOAD_EVENT = 12,
  RAND_EVENT = 13,
  USER_VAR_EVENT = 14,
  FORMAT_DESCRIPTION_EVENT = 15,
  XID_EVENT = 16,
  BEGIN_LOAD_QUERY_EVENT = 17,
  EXECUTE_LOAD_QUERY_EVENT = 18,
  TABLE_MAP_EVENT = 19,
  PRE_GA_WRITE_ROWS_EVENT = 20,
  PRE_GA_UPDATE_ROWS_EVENT = 21,
  PRE_GA_DELETE_ROWS_EVENT = 22,
  WRITE_ROWS_EVENT_V1 = 23,
  UPDATE_ROWS_EVENT_V1 = 24,
  DELETE_ROWS_EVENT_V1 = 25,
  INCIDENT_EVENT = 26,
  HEARTBEAT_LOG_EVENT = 27,
  IGNORABLE_LOG_EVENT = 28,
  ROWS_QUERY_LOG_EVENT = 29,
  WRITE_ROWS_EVENT = 30,
  UPDATE_ROWS_EVENT = 31,
  DELETE_ROWS_EVENT = 32,
  GTID_LOG_EVENT = 33,
  ANONYMOUS_GTID_LOG_EVENT = 34,
  PREVIOUS_GTIDS_LOG_EVENT = 35,
};

/**
 * Binlog Event Header (19 bytes for v4)
 */
struct BinlogEventHeader {
  uint32_t timestamp;
  uint8_t type_code;
  uint32_t server_id;
  uint32_t event_length;
  uint32_t next_position;
  uint16_t flags;
};

/**
 * Binlog Event
 */
struct BinlogEvent {
  BinlogEventHeader header;
  std::vector<uint8_t> data;
  uint64_t start_lsn;  // Aurora LSN
  uint64_t end_lsn;
  
  size_t size() const {
    return sizeof(BinlogEventHeader) + data.size();
  }
};

/**
 * Table Map Info for row events
 */
struct TableMapInfo {
  uint64_t table_id;
  std::string database;
  std::string table;
  std::vector<uint8_t> column_types;
  std::vector<uint16_t> column_meta;
  std::vector<uint8_t> null_bitmap;
};

/**
 * Row data for INSERT/UPDATE/DELETE
 */
struct RowData {
  std::vector<uint8_t> before_image;  // For UPDATE/DELETE
  std::vector<uint8_t> after_image;   // For INSERT/UPDATE
};

// Forward declaration
struct RedoRecord;

/**
 * Binlog Adapter
 * Converts Redo logs to Binlog events
 */
class BinlogAdapter {
public:
  BinlogAdapter();
  ~BinlogAdapter();

  /**
   * Convert Redo record to Binlog events
   */
  std::vector<std::unique_ptr<BinlogEvent>> convert(
      const RedoRecord* redo,
      const GTID& gtid);

  /**
   * Set server ID for binlog events
   */
  void set_server_id(uint32_t server_id);

  /**
   * Get current binlog position
   */
  uint64_t get_current_position() const;

  /**
   * Get current binlog file name
   */
  std::string get_current_file() const;

  /**
   * Rotate to new binlog file
   */
  void rotate(uint64_t lsn);

private:
  uint32_t server_id_;
  uint64_t current_position_;
  std::string current_file_;
  uint32_t file_sequence_;
  
  // Table map cache
  std::map<uint64_t, TableMapInfo> table_map_cache_;
  std::mutex cache_mutex_;
  
  // Event generation helpers
  std::unique_ptr<BinlogEvent> make_format_description_event();
  std::unique_ptr<BinlogEvent> make_gtid_event(const GTID& gtid);
  std::unique_ptr<BinlogEvent> make_query_event(const std::string& sql);
  std::unique_ptr<BinlogEvent> make_table_map_event(const TableMapInfo& info);
  std::unique_ptr<BinlogEvent> make_write_rows_event(uint64_t table_id, const RowData& data);
  std::unique_ptr<BinlogEvent> make_update_rows_event(uint64_t table_id, const RowData& data);
  std::unique_ptr<BinlogEvent> make_delete_rows_event(uint64_t table_id, const RowData& data);
  std::unique_ptr<BinlogEvent> make_xid_event(uint64_t xid);
  std::unique_ptr<BinlogEvent> make_rotate_event(const std::string& next_file);
  
  // Redo parsing helpers
  RowData parse_insert_redo(const RedoRecord* redo);
  RowData parse_update_redo(const RedoRecord* redo);
  RowData parse_delete_redo(const RedoRecord* redo);
};

/**
 * Binlog Ring Buffer
 * In-memory buffer for binlog events
 */
class BinlogBuffer {
public:
  explicit BinlogBuffer(size_t max_size);
  ~BinlogBuffer();

  /**
   * Add event to buffer
   */
  void add(std::unique_ptr<BinlogEvent> event);

  /**
   * Get events from LSN
   */
  std::vector<BinlogEvent*> get_events(uint64_t from_lsn, size_t max_count);

  /**
   * Wait for new events
   */
  bool wait_for_events(uint32_t timeout_ms);

  /**
   * Get the oldest LSN in buffer
   */
  uint64_t get_oldest_lsn() const;

  /**
   * Get the newest LSN in buffer
   */
  uint64_t get_newest_lsn() const;

  /**
   * Get buffer size
   */
  size_t size() const;

  /**
   * Notify waiters
   */
  void notify();

private:
  size_t max_size_;
  std::vector<std::unique_ptr<BinlogEvent>> events_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
};

/**
 * Binlog Dump Handler
 * Handles COM_BINLOG_DUMP and COM_BINLOG_DUMP_GTID
 */
class BinlogDumpHandler {
public:
  BinlogDumpHandler(BinlogBuffer* buffer, AuroraGTIDManager* gtid_manager);
  ~BinlogDumpHandler();

  /**
   * Handle COM_BINLOG_DUMP_GTID request
   * @param slave_gtid_set GTID set string from slave
   * @param callback Called for each event to send
   */
  using SendCallback = std::function<bool(const BinlogEvent* event)>;
  
  void handle_dump_gtid(
      const std::string& slave_gtid_set,
      SendCallback callback,
      std::atomic<bool>& stop_flag);

  /**
   * Handle COM_BINLOG_DUMP request (legacy position-based)
   */
  void handle_dump_pos(
      const std::string& binlog_file,
      uint64_t binlog_pos,
      SendCallback callback,
      std::atomic<bool>& stop_flag);

  /**
   * Send heartbeat event
   */
  std::unique_ptr<BinlogEvent> make_heartbeat_event(uint64_t lsn);

private:
  BinlogBuffer* buffer_;
  AuroraGTIDManager* gtid_manager_;
  
  uint64_t find_start_lsn(const GTIDSet& to_send);
  uint64_t binlog_pos_to_lsn(const std::string& file, uint64_t pos);
};

/**
 * Global binlog adapter and buffer
 */
extern std::unique_ptr<BinlogAdapter> g_binlog_adapter;
extern std::unique_ptr<BinlogBuffer> g_binlog_buffer;

/**
 * Initialize binlog compatibility layer
 */
bool aurora_binlog_init(uint32_t server_id, size_t buffer_size);

/**
 * Shutdown binlog compatibility layer
 */
void aurora_binlog_shutdown();

/**
 * Process redo record and generate binlog events
 */
void aurora_binlog_process_redo(const RedoRecord* redo, const GTID& gtid);

/**
 * Get current binlog file and position
 */
void aurora_get_binlog_position(std::string* file, uint64_t* pos);

}  // namespace aurora

#endif  // AURORA_BINLOG_H
