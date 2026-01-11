/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Binlog Adapter Implementation

*****************************************************************************/

#include "aurora_binlog.h"
#include "aurora_types.h"
#include <chrono>
#include <cstring>

namespace aurora {

// Global instances
std::unique_ptr<BinlogAdapter> g_binlog_adapter;
std::unique_ptr<BinlogBuffer> g_binlog_buffer;

//============================================================================
// BinlogAdapter Implementation
//============================================================================

BinlogAdapter::BinlogAdapter()
    : server_id_(1),
      current_position_(4),  // Start after magic number
      current_file_("mysql-bin.000001"),
      file_sequence_(1) {}

BinlogAdapter::~BinlogAdapter() {}

std::vector<std::unique_ptr<BinlogEvent>> BinlogAdapter::convert(
    const RedoRecord* redo,
    const GTID& gtid) {
  std::vector<std::unique_ptr<BinlogEvent>> events;
  
  if (!redo) return events;
  
  // Add GTID event
  events.push_back(make_gtid_event(gtid));
  
  // Add Query BEGIN event
  events.push_back(make_query_event("BEGIN"));
  
  // Convert based on redo type
  switch (redo->type) {
    case RedoType::INSERT: {
      // Add table map event
      TableMapInfo table_info;
      table_info.table_id = redo->space_id;
      table_info.database = "aurora";
      table_info.table = "table_" + std::to_string(redo->space_id);
      
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (table_map_cache_.find(redo->space_id) == table_map_cache_.end()) {
        table_map_cache_[redo->space_id] = table_info;
        events.push_back(make_table_map_event(table_info));
      }
      
      RowData row = parse_insert_redo(redo);
      events.push_back(make_write_rows_event(redo->space_id, row));
      break;
    }
    
    case RedoType::UPDATE: {
      TableMapInfo table_info;
      table_info.table_id = redo->space_id;
      
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (table_map_cache_.find(redo->space_id) == table_map_cache_.end()) {
        table_info.database = "aurora";
        table_info.table = "table_" + std::to_string(redo->space_id);
        table_map_cache_[redo->space_id] = table_info;
        events.push_back(make_table_map_event(table_info));
      }
      
      RowData row = parse_update_redo(redo);
      events.push_back(make_update_rows_event(redo->space_id, row));
      break;
    }
    
    case RedoType::DELETE: {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      if (table_map_cache_.find(redo->space_id) == table_map_cache_.end()) {
        TableMapInfo table_info;
        table_info.table_id = redo->space_id;
        table_info.database = "aurora";
        table_info.table = "table_" + std::to_string(redo->space_id);
        table_map_cache_[redo->space_id] = table_info;
        events.push_back(make_table_map_event(table_info));
      }
      
      RowData row = parse_delete_redo(redo);
      events.push_back(make_delete_rows_event(redo->space_id, row));
      break;
    }
    
    default:
      break;
  }
  
  // Add XID event (commit)
  events.push_back(make_xid_event(redo->trx_id));
  
  // Update positions
  for (auto& event : events) {
    event->start_lsn = redo->lsn;
    event->end_lsn = redo->lsn;
    current_position_ += event->size();
  }
  
  return events;
}

void BinlogAdapter::set_server_id(uint32_t server_id) {
  server_id_ = server_id;
}

uint64_t BinlogAdapter::get_current_position() const {
  return current_position_;
}

std::string BinlogAdapter::get_current_file() const {
  return current_file_;
}

void BinlogAdapter::rotate(uint64_t lsn) {
  file_sequence_++;
  char buf[32];
  snprintf(buf, sizeof(buf), "mysql-bin.%06d", file_sequence_);
  current_file_ = buf;
  current_position_ = 4;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_format_description_event() {
  auto event = std::make_unique<BinlogEvent>();
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  
  event->header.timestamp = static_cast<uint32_t>(timestamp);
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::FORMAT_DESCRIPTION_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  // FDE data: binlog version, server version, timestamp, header length, etc.
  event->data.resize(100);
  // Binlog version 4
  event->data[0] = 4;
  event->data[1] = 0;
  // Server version string
  const char* version = "8.4.3-3-Aurora";
  memcpy(&event->data[2], version, strlen(version));
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_gtid_event(const GTID& gtid) {
  auto event = std::make_unique<BinlogEvent>();
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  
  event->header.timestamp = static_cast<uint32_t>(timestamp);
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::GTID_LOG_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  // GTID event data: flags + UUID + GNO + TSID type + logical clock timestamps
  event->data.resize(42);
  event->data[0] = 1;  // Flags: GTID specified
  memcpy(&event->data[1], gtid.server_uuid, 16);
  memcpy(&event->data[17], &gtid.gno, 8);
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_query_event(const std::string& sql) {
  auto event = std::make_unique<BinlogEvent>();
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  
  event->header.timestamp = static_cast<uint32_t>(timestamp);
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::QUERY_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  // Query event data: thread_id, exec_time, db_len, error_code, status_vars_len, db, query
  size_t fixed_len = 4 + 4 + 1 + 2 + 2;  // thread_id, exec_time, db_len, error_code, status_vars_len
  size_t db_len = 6;  // "aurora"
  
  event->data.resize(fixed_len + db_len + 1 + sql.length());
  
  uint32_t thread_id = 1;
  memcpy(&event->data[0], &thread_id, 4);
  event->data[8] = static_cast<uint8_t>(db_len);
  memcpy(&event->data[fixed_len], "aurora", db_len);
  event->data[fixed_len + db_len] = 0;
  memcpy(&event->data[fixed_len + db_len + 1], sql.c_str(), sql.length());
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_table_map_event(const TableMapInfo& info) {
  auto event = std::make_unique<BinlogEvent>();
  
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
      now.time_since_epoch()).count();
  
  event->header.timestamp = static_cast<uint32_t>(timestamp);
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::TABLE_MAP_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  // Table map event data: table_id, flags, db_name, table_name, column_count, column_types...
  event->data.resize(50 + info.database.length() + info.table.length());
  
  memcpy(&event->data[0], &info.table_id, 6);
  event->data[6] = 0;  // flags
  event->data[7] = static_cast<uint8_t>(info.database.length());
  memcpy(&event->data[8], info.database.c_str(), info.database.length());
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_write_rows_event(uint64_t table_id, const RowData& data) {
  auto event = std::make_unique<BinlogEvent>();
  
  event->header.timestamp = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::WRITE_ROWS_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  // WRITE_ROWS_EVENT data: table_id + flags + extra_data_len + columns_bitmap + rows
  event->data.resize(10 + data.after_image.size());
  memcpy(&event->data[0], &table_id, 6);
  memcpy(&event->data[10], data.after_image.data(), data.after_image.size());
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_update_rows_event(uint64_t table_id, const RowData& data) {
  auto event = std::make_unique<BinlogEvent>();
  
  event->header.timestamp = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::UPDATE_ROWS_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  event->data.resize(10 + data.before_image.size() + data.after_image.size());
  memcpy(&event->data[0], &table_id, 6);
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_delete_rows_event(uint64_t table_id, const RowData& data) {
  auto event = std::make_unique<BinlogEvent>();
  
  event->header.timestamp = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::DELETE_ROWS_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  event->data.resize(10 + data.before_image.size());
  memcpy(&event->data[0], &table_id, 6);
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

std::unique_ptr<BinlogEvent> BinlogAdapter::make_xid_event(uint64_t xid) {
  auto event = std::make_unique<BinlogEvent>();
  
  event->header.timestamp = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::XID_EVENT);
  event->header.server_id = server_id_;
  event->header.flags = 0;
  
  event->data.resize(8);
  memcpy(&event->data[0], &xid, 8);
  
  event->header.event_length = sizeof(BinlogEventHeader) + event->data.size();
  event->header.next_position = current_position_ + event->header.event_length;
  
  return event;
}

RowData BinlogAdapter::parse_insert_redo(const RedoRecord* redo) {
  RowData row;
  row.after_image = redo->data;
  return row;
}

RowData BinlogAdapter::parse_update_redo(const RedoRecord* redo) {
  RowData row;
  // In real implementation, parse before/after images from redo data
  size_t half = redo->data.size() / 2;
  row.before_image.assign(redo->data.begin(), redo->data.begin() + half);
  row.after_image.assign(redo->data.begin() + half, redo->data.end());
  return row;
}

RowData BinlogAdapter::parse_delete_redo(const RedoRecord* redo) {
  RowData row;
  row.before_image = redo->data;
  return row;
}

//============================================================================
// BinlogBuffer Implementation
//============================================================================

BinlogBuffer::BinlogBuffer(size_t max_size) : max_size_(max_size) {
  events_.reserve(max_size);
}

BinlogBuffer::~BinlogBuffer() {}

void BinlogBuffer::add(std::unique_ptr<BinlogEvent> event) {
  std::lock_guard<std::mutex> lock(mutex_);
  
  if (events_.size() >= max_size_) {
    events_.erase(events_.begin());
  }
  
  events_.push_back(std::move(event));
  cv_.notify_all();
}

std::vector<BinlogEvent*> BinlogBuffer::get_events(uint64_t from_lsn, size_t max_count) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<BinlogEvent*> result;
  
  for (const auto& event : events_) {
    if (event->start_lsn >= from_lsn) {
      result.push_back(event.get());
      if (result.size() >= max_count) break;
    }
  }
  
  return result;
}

bool BinlogBuffer::wait_for_events(uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !events_.empty(); });
}

uint64_t BinlogBuffer::get_oldest_lsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) return 0;
  return events_.front()->start_lsn;
}

uint64_t BinlogBuffer::get_newest_lsn() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) return 0;
  return events_.back()->end_lsn;
}

size_t BinlogBuffer::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return events_.size();
}

void BinlogBuffer::notify() {
  cv_.notify_all();
}

//============================================================================
// BinlogDumpHandler Implementation
//============================================================================

BinlogDumpHandler::BinlogDumpHandler(BinlogBuffer* buffer, AuroraGTIDManager* gtid_manager)
    : buffer_(buffer), gtid_manager_(gtid_manager) {}

BinlogDumpHandler::~BinlogDumpHandler() {}

void BinlogDumpHandler::handle_dump_gtid(
    const std::string& slave_gtid_set,
    SendCallback callback,
    std::atomic<bool>& stop_flag) {
  
  // Parse slave GTID set
  GTIDSet slave_set = GTIDSet::parse(slave_gtid_set);
  
  // Get executed set and compute difference
  GTIDSet executed = gtid_manager_->get_executed_set();
  GTIDSet to_send = executed.subtract(slave_set);
  
  // Find start LSN
  uint64_t current_lsn = find_start_lsn(to_send);
  
  // Send events
  while (!stop_flag) {
    auto events = buffer_->get_events(current_lsn, 1000);
    
    if (events.empty()) {
      buffer_->wait_for_events(1000);
      continue;
    }
    
    for (auto* event : events) {
      if (!callback(event)) {
        return;
      }
      current_lsn = event->end_lsn + 1;
    }
  }
}

void BinlogDumpHandler::handle_dump_pos(
    const std::string& binlog_file,
    uint64_t binlog_pos,
    SendCallback callback,
    std::atomic<bool>& stop_flag) {
  
  uint64_t start_lsn = binlog_pos_to_lsn(binlog_file, binlog_pos);
  uint64_t current_lsn = start_lsn;
  
  while (!stop_flag) {
    auto events = buffer_->get_events(current_lsn, 1000);
    
    if (events.empty()) {
      buffer_->wait_for_events(1000);
      continue;
    }
    
    for (auto* event : events) {
      if (!callback(event)) {
        return;
      }
      current_lsn = event->end_lsn + 1;
    }
  }
}

std::unique_ptr<BinlogEvent> BinlogDumpHandler::make_heartbeat_event(uint64_t lsn) {
  auto event = std::make_unique<BinlogEvent>();
  
  event->header.timestamp = 0;
  event->header.type_code = static_cast<uint8_t>(BinlogEventType::HEARTBEAT_LOG_EVENT);
  event->header.server_id = 0;
  event->header.event_length = sizeof(BinlogEventHeader);
  event->header.next_position = 0;
  event->header.flags = 0;
  event->start_lsn = lsn;
  event->end_lsn = lsn;
  
  return event;
}

uint64_t BinlogDumpHandler::find_start_lsn(const GTIDSet& to_send) {
  if (to_send.empty()) {
    return buffer_->get_newest_lsn();
  }
  
  GTID first = to_send.get_first();
  return gtid_manager_->gtid_to_lsn(first);
}

uint64_t BinlogDumpHandler::binlog_pos_to_lsn(const std::string& file, uint64_t pos) {
  // In real implementation, maintain file:pos -> LSN mapping
  return pos;
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_binlog_init(uint32_t server_id, size_t buffer_size) {
  g_binlog_adapter = std::make_unique<BinlogAdapter>();
  g_binlog_adapter->set_server_id(server_id);
  
  g_binlog_buffer = std::make_unique<BinlogBuffer>(buffer_size);
  
  return true;
}

void aurora_binlog_shutdown() {
  g_binlog_buffer.reset();
  g_binlog_adapter.reset();
}

void aurora_binlog_process_redo(const RedoRecord* redo, const GTID& gtid) {
  if (!g_binlog_adapter || !g_binlog_buffer) return;
  
  auto events = g_binlog_adapter->convert(redo, gtid);
  for (auto& event : events) {
    g_binlog_buffer->add(std::move(event));
  }
}

void aurora_get_binlog_position(std::string* file, uint64_t* pos) {
  if (g_binlog_adapter) {
    *file = g_binlog_adapter->get_current_file();
    *pos = g_binlog_adapter->get_current_position();
  }
}

}  // namespace aurora
