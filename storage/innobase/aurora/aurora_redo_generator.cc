/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Redo Log Generator Implementation

*****************************************************************************/

#include "aurora_redo_generator.h"
#include "aurora_redo_sender.h"
#include <cstring>
#include <chrono>

namespace aurora {

// Thread-local MTR state
thread_local MtrState AuroraRedoGenerator::tls_mtr_;

// Global instance
std::unique_ptr<AuroraRedoGenerator> g_redo_generator;

//============================================================================
// Construction / Destruction
//============================================================================

AuroraRedoGenerator::AuroraRedoGenerator() {}

AuroraRedoGenerator::~AuroraRedoGenerator() {
  shutdown();
}

bool AuroraRedoGenerator::initialize(uint64_t start_lsn, AuroraRedoSender* sender) {
  current_lsn_.store(start_lsn);
  durable_lsn_.store(start_lsn);
  sender_ = sender;
  mtr_id_counter_.store(0);
  memset(&stats_, 0, sizeof(stats_));
  return true;
}

void AuroraRedoGenerator::shutdown() {
  sender_ = nullptr;
}

//============================================================================
// MTR Operations
//============================================================================

void AuroraRedoGenerator::mtr_begin(uint64_t trx_id) {
  tls_mtr_.reset();
  tls_mtr_.mtr_id = mtr_id_counter_.fetch_add(1);
  tls_mtr_.start_lsn = current_lsn_.load();
  tls_mtr_.trx_id = trx_id;
}

void AuroraRedoGenerator::mtr_add_record(
    RedoType type,
    uint64_t space_id,
    uint64_t page_id,
    const uint8_t* data,
    size_t data_len,
    uint16_t flags) {
  
  RedoRecord rec;
  rec.header.lsn = 0;  // Will be assigned at commit
  rec.header.space_id = space_id;
  rec.header.page_id = page_id;
  rec.header.trx_id = tls_mtr_.trx_id;
  rec.header.mtr_id = tls_mtr_.mtr_id;
  rec.header.type = type;
  rec.header.flags = flags;
  rec.header.data_len = static_cast<uint32_t>(data_len);
  
  if (data && data_len > 0) {
    rec.data.assign(data, data + data_len);
  }
  
  // Mark first record
  if (tls_mtr_.records.empty()) {
    rec.header.flags |= REDO_FLAG_MTR_START;
  }
  
  tls_mtr_.records.push_back(std::move(rec));
}

uint64_t AuroraRedoGenerator::mtr_commit() {
  if (tls_mtr_.records.empty()) {
    return current_lsn_.load();
  }
  
  // Mark last record
  tls_mtr_.records.back().header.flags |= REDO_FLAG_MTR_END;
  
  // Calculate total size and allocate LSN range
  size_t total_size = 0;
  for (const auto& rec : tls_mtr_.records) {
    total_size += rec.total_size();
  }
  
  uint64_t start_lsn = allocate_lsn(total_size);
  uint64_t end_lsn = current_lsn_.load();
  
  // Assign LSN to each record
  uint64_t lsn = start_lsn;
  for (auto& rec : tls_mtr_.records) {
    rec.header.lsn = lsn;
    rec.checksum = calculate_checksum(
        reinterpret_cast<const uint8_t*>(&rec.header),
        sizeof(rec.header));
    lsn += rec.total_size();
  }
  
  // Send to storage layer
  send_mtr_records(tls_mtr_, end_lsn);
  
  // Update stats
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.records_generated += tls_mtr_.records.size();
    stats_.bytes_generated += total_size;
    stats_.mtr_commits++;
  }
  
  // Clear MTR
  tls_mtr_.reset();
  
  return end_lsn;
}

void AuroraRedoGenerator::mtr_abort() {
  tls_mtr_.reset();
}

//============================================================================
// Record Operations
//============================================================================

void AuroraRedoGenerator::log_insert(
    uint64_t space_id, uint64_t page_id,
    uint16_t slot_no, const uint8_t* rec, size_t rec_len) {
  
  InsertRecordData data;
  data.slot_no = slot_no;
  data.rec_len = static_cast<uint16_t>(rec_len);
  data.rec_data.assign(rec, rec + rec_len);
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_add_record(RedoType::MLOG_REC_INSERT, space_id, page_id,
                 serialized.data(), serialized.size());
}

void AuroraRedoGenerator::log_update_in_place(
    uint64_t space_id, uint64_t page_id,
    uint16_t slot_no, uint16_t offset,
    const uint8_t* old_data, size_t old_len,
    const uint8_t* new_data, size_t new_len) {
  
  UpdateInPlaceData data;
  data.slot_no = slot_no;
  data.offset = offset;
  data.old_len = static_cast<uint16_t>(old_len);
  data.new_len = static_cast<uint16_t>(new_len);
  data.old_data.assign(old_data, old_data + old_len);
  data.new_data.assign(new_data, new_data + new_len);
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_add_record(RedoType::MLOG_REC_UPDATE_IN_PLACE, space_id, page_id,
                 serialized.data(), serialized.size());
}

void AuroraRedoGenerator::log_delete(
    uint64_t space_id, uint64_t page_id,
    uint16_t slot_no, const uint8_t* rec, size_t rec_len) {
  
  InsertRecordData data;  // Same format as insert
  data.slot_no = slot_no;
  data.rec_len = static_cast<uint16_t>(rec_len);
  data.rec_data.assign(rec, rec + rec_len);
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_add_record(RedoType::MLOG_REC_DELETE, space_id, page_id,
                 serialized.data(), serialized.size());
}

void AuroraRedoGenerator::log_page_init(
    uint64_t space_id, uint64_t page_id,
    uint32_t page_type, uint32_t flags) {
  
  PageInitData data;
  data.page_type = page_type;
  data.flags = flags;
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_add_record(RedoType::MLOG_PAGE_INIT, space_id, page_id,
                 serialized.data(), serialized.size());
}

//============================================================================
// 2PC Operations
//============================================================================

void AuroraRedoGenerator::begin_2pc(uint64_t trx_id, const std::string& xid) {
  std::lock_guard<std::mutex> lock(xa_mutex_);
  
  XATransactionInfo info;
  info.trx_id = trx_id;
  info.xid = xid;
  info.state = TwoPhaseState::NONE;
  info.prepare_lsn = 0;
  info.commit_lsn = 0;
  info.is_external = !xid.empty();
  
  xa_transactions_[trx_id] = info;
}

uint64_t AuroraRedoGenerator::prepare(uint64_t trx_id) {
  // Update state
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    auto it = xa_transactions_.find(trx_id);
    if (it != xa_transactions_.end()) {
      it->second.state = TwoPhaseState::PREPARING;
    }
  }
  
  // Generate PREPARE record
  mtr_begin(trx_id);
  
  // PREPARE record contains: trx_id(8) + timestamp(8)
  uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  
  std::vector<uint8_t> data(16);
  memcpy(&data[0], &trx_id, 8);
  memcpy(&data[8], &timestamp, 8);
  
  mtr_add_record(RedoType::MLOG_TRX_PREPARE, 0, 0,
                 data.data(), data.size(), REDO_FLAG_SYNC);
  
  uint64_t prepare_lsn = mtr_commit();
  
  // Wait for durability (Quorum)
  wait_durable(prepare_lsn, 30000);  // 30s timeout
  
  // Update state
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    auto it = xa_transactions_.find(trx_id);
    if (it != xa_transactions_.end()) {
      it->second.state = TwoPhaseState::PREPARED;
      it->second.prepare_lsn = prepare_lsn;
    }
  }
  
  // Update stats
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.trx_prepares++;
  }
  
  return prepare_lsn;
}

uint64_t AuroraRedoGenerator::commit_2pc(uint64_t trx_id) {
  // Update state
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    auto it = xa_transactions_.find(trx_id);
    if (it != xa_transactions_.end()) {
      it->second.state = TwoPhaseState::COMMITTING;
    }
  }
  
  // Generate COMMIT record
  mtr_begin(trx_id);
  
  TrxCommitData data;
  data.commit_lsn = current_lsn_.load();
  data.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_add_record(RedoType::MLOG_TRX_COMMIT, 0, 0,
                 serialized.data(), serialized.size(), REDO_FLAG_SYNC);
  
  uint64_t commit_lsn = mtr_commit();
  
  // Wait for durability
  wait_durable(commit_lsn, 30000);
  
  // Update state and remove from tracking
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    auto it = xa_transactions_.find(trx_id);
    if (it != xa_transactions_.end()) {
      it->second.state = TwoPhaseState::COMMITTED;
      it->second.commit_lsn = commit_lsn;
    }
  }
  
  // Update stats
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.trx_commits++;
  }
  
  return commit_lsn;
}

void AuroraRedoGenerator::rollback_2pc(uint64_t trx_id) {
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    auto it = xa_transactions_.find(trx_id);
    if (it != xa_transactions_.end()) {
      it->second.state = TwoPhaseState::ROLLING_BACK;
    }
  }
  
  // Generate ROLLBACK record
  mtr_begin(trx_id);
  
  std::vector<uint8_t> data(8);
  memcpy(&data[0], &trx_id, 8);
  
  mtr_add_record(RedoType::MLOG_TRX_ROLLBACK, 0, 0,
                 data.data(), data.size(), REDO_FLAG_SYNC);
  
  uint64_t lsn = mtr_commit();
  wait_durable(lsn, 30000);
  
  {
    std::lock_guard<std::mutex> lock(xa_mutex_);
    xa_transactions_.erase(trx_id);
  }
  
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.trx_rollbacks++;
  }
}

TwoPhaseState AuroraRedoGenerator::get_2pc_state(uint64_t trx_id) const {
  std::lock_guard<std::mutex> lock(xa_mutex_);
  auto it = xa_transactions_.find(trx_id);
  if (it != xa_transactions_.end()) {
    return it->second.state;
  }
  return TwoPhaseState::NONE;
}

bool AuroraRedoGenerator::get_xa_info(uint64_t trx_id, XATransactionInfo* info) const {
  std::lock_guard<std::mutex> lock(xa_mutex_);
  auto it = xa_transactions_.find(trx_id);
  if (it != xa_transactions_.end()) {
    *info = it->second;
    return true;
  }
  return false;
}

//============================================================================
// DDL Operations
//============================================================================

void AuroraRedoGenerator::log_create_table(
    uint64_t table_id,
    const std::string& schema,
    const std::string& table) {
  
  std::vector<uint8_t> data;
  data.resize(8 + 2 + schema.size() + 2 + table.size());
  
  size_t pos = 0;
  memcpy(&data[pos], &table_id, 8); pos += 8;
  uint16_t schema_len = static_cast<uint16_t>(schema.size());
  memcpy(&data[pos], &schema_len, 2); pos += 2;
  memcpy(&data[pos], schema.data(), schema.size()); pos += schema.size();
  uint16_t table_len = static_cast<uint16_t>(table.size());
  memcpy(&data[pos], &table_len, 2); pos += 2;
  memcpy(&data[pos], table.data(), table.size());
  
  mtr_add_record(RedoType::MLOG_DDL_CREATE_TABLE, 0, 0,
                 data.data(), data.size(), REDO_FLAG_DDL);
}

void AuroraRedoGenerator::log_drop_table(
    uint64_t table_id,
    const std::string& schema,
    const std::string& table) {
  
  std::vector<uint8_t> data;
  data.resize(8 + 2 + schema.size() + 2 + table.size());
  
  size_t pos = 0;
  memcpy(&data[pos], &table_id, 8); pos += 8;
  uint16_t schema_len = static_cast<uint16_t>(schema.size());
  memcpy(&data[pos], &schema_len, 2); pos += 2;
  memcpy(&data[pos], schema.data(), schema.size()); pos += schema.size();
  uint16_t table_len = static_cast<uint16_t>(table.size());
  memcpy(&data[pos], &table_len, 2); pos += 2;
  memcpy(&data[pos], table.data(), table.size());
  
  mtr_add_record(RedoType::MLOG_DDL_DROP_TABLE, 0, 0,
                 data.data(), data.size(), REDO_FLAG_DDL);
}

void AuroraRedoGenerator::log_create_index(
    uint64_t table_id,
    uint64_t index_id,
    const std::string& index_name) {
  
  std::vector<uint8_t> data;
  data.resize(16 + 2 + index_name.size());
  
  size_t pos = 0;
  memcpy(&data[pos], &table_id, 8); pos += 8;
  memcpy(&data[pos], &index_id, 8); pos += 8;
  uint16_t name_len = static_cast<uint16_t>(index_name.size());
  memcpy(&data[pos], &name_len, 2); pos += 2;
  memcpy(&data[pos], index_name.data(), index_name.size());
  
  mtr_add_record(RedoType::MLOG_DDL_CREATE_INDEX, 0, 0,
                 data.data(), data.size(), REDO_FLAG_DDL);
}

void AuroraRedoGenerator::log_drop_index(uint64_t table_id, uint64_t index_id) {
  std::vector<uint8_t> data(16);
  memcpy(&data[0], &table_id, 8);
  memcpy(&data[8], &index_id, 8);
  
  mtr_add_record(RedoType::MLOG_DDL_DROP_INDEX, 0, 0,
                 data.data(), data.size(), REDO_FLAG_DDL);
}

uint64_t AuroraRedoGenerator::log_ddl_barrier() {
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_BARRIER, 0, 0, nullptr, 0,
                 REDO_FLAG_BARRIER | REDO_FLAG_SYNC);
  uint64_t lsn = mtr_commit();
  wait_durable(lsn, 60000);  // DDL barrier has longer timeout
  
  {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    stats_.ddl_barriers++;
  }
  
  return lsn;
}

//============================================================================
// Aurora-Specific Operations
//============================================================================

void AuroraRedoGenerator::log_vdl_update(
    uint64_t new_vdl, uint64_t old_vdl, uint32_t quorum_count) {
  
  AuroraVDLUpdateData data;
  data.new_vdl = new_vdl;
  data.old_vdl = old_vdl;
  data.quorum_count = quorum_count;
  data.padding = 0;
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_AURORA_VDL_UPDATE, 0, 0,
                 serialized.data(), serialized.size());
  mtr_commit();
}

void AuroraRedoGenerator::log_reader_sync(
    const std::string& reader_id,
    uint64_t read_point,
    uint64_t target_vdl) {
  
  AuroraReaderSyncData data;
  data.read_point = read_point;
  data.target_vdl = target_vdl;
  data.reader_id = reader_id;
  
  std::vector<uint8_t> serialized;
  data.serialize(serialized);
  
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_AURORA_READER_SYNC, 0, 0,
                 serialized.data(), serialized.size());
  mtr_commit();
}

void AuroraRedoGenerator::log_freeze_writes() {
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_AURORA_FREEZE, 0, 0, nullptr, 0,
                 REDO_FLAG_SYNC);
  uint64_t lsn = mtr_commit();
  wait_durable(lsn, 30000);
}

void AuroraRedoGenerator::log_unfreeze_writes() {
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_AURORA_UNFREEZE, 0, 0, nullptr, 0);
  mtr_commit();
}

void AuroraRedoGenerator::log_failover(
    const std::string& old_writer,
    const std::string& new_writer,
    uint64_t failover_lsn) {
  
  std::vector<uint8_t> data;
  data.resize(8 + 2 + old_writer.size() + 2 + new_writer.size());
  
  size_t pos = 0;
  memcpy(&data[pos], &failover_lsn, 8); pos += 8;
  uint16_t old_len = static_cast<uint16_t>(old_writer.size());
  memcpy(&data[pos], &old_len, 2); pos += 2;
  memcpy(&data[pos], old_writer.data(), old_writer.size()); pos += old_writer.size();
  uint16_t new_len = static_cast<uint16_t>(new_writer.size());
  memcpy(&data[pos], &new_len, 2); pos += 2;
  memcpy(&data[pos], new_writer.data(), new_writer.size());
  
  mtr_begin(0);
  mtr_add_record(RedoType::MLOG_AURORA_FAILOVER, 0, 0,
                 data.data(), data.size(), REDO_FLAG_SYNC);
  uint64_t lsn = mtr_commit();
  wait_durable(lsn, 30000);
}

//============================================================================
// LSN Management
//============================================================================

uint64_t AuroraRedoGenerator::allocate_lsn(size_t record_size) {
  return current_lsn_.fetch_add(record_size);
}

bool AuroraRedoGenerator::wait_durable(uint64_t lsn, uint32_t timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  
  while (durable_lsn_.load() < lsn) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
    
    if (elapsed.count() >= timeout_ms) {
      return false;  // Timeout
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  
  return true;
}

AuroraRedoGenerator::Stats AuroraRedoGenerator::get_stats() const {
  std::lock_guard<std::mutex> lock(stats_mutex_);
  return stats_;
}

//============================================================================
// Helper Methods
//============================================================================

void AuroraRedoGenerator::send_mtr_records(const MtrState& mtr, uint64_t end_lsn) {
  if (!sender_) return;
  
  // Serialize all records
  std::vector<uint8_t> buffer;
  for (const auto& rec : mtr.records) {
    serialize_record(rec, buffer);
  }
  
  // Send via redo sender
  sender_->send(buffer.data(), buffer.size(), mtr.start_lsn, end_lsn);
}

void AuroraRedoGenerator::serialize_record(const RedoRecord& rec, std::vector<uint8_t>& out) {
  size_t start = out.size();
  size_t rec_size = sizeof(RedoRecordHeader) + rec.data.size() + sizeof(uint32_t);
  out.resize(start + rec_size);
  
  // Header
  memcpy(&out[start], &rec.header, sizeof(RedoRecordHeader));
  
  // Data
  if (!rec.data.empty()) {
    memcpy(&out[start + sizeof(RedoRecordHeader)],
           rec.data.data(), rec.data.size());
  }
  
  // Checksum
  memcpy(&out[start + sizeof(RedoRecordHeader) + rec.data.size()],
         &rec.checksum, sizeof(uint32_t));
}

uint32_t AuroraRedoGenerator::calculate_checksum(const uint8_t* data, size_t len) {
  // Simple CRC32 implementation
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
  }
  return ~crc;
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_redo_generator_init(uint64_t start_lsn, AuroraRedoSender* sender) {
  g_redo_generator = std::make_unique<AuroraRedoGenerator>();
  return g_redo_generator->initialize(start_lsn, sender);
}

void aurora_redo_generator_shutdown() {
  if (g_redo_generator) {
    g_redo_generator->shutdown();
    g_redo_generator.reset();
  }
}

}  // namespace aurora
