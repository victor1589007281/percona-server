/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Type Definitions
Core data structures for Aurora distributed storage.

*****************************************************************************/

#ifndef AURORA_TYPES_H
#define AURORA_TYPES_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <chrono>

namespace aurora {

// LSN type (Log Sequence Number)
using lsn_t = uint64_t;

// Page ID
using page_id_t = uint64_t;

// Space ID (tablespace)
using space_id_t = uint64_t;

// Transaction ID
using trx_id_t = uint64_t;

// Mini-transaction ID
using mtr_id_t = uint64_t;

// Page size (16KB)
constexpr size_t PAGE_SIZE = 16384;

// WAL block size
constexpr size_t WAL_BLOCK_SIZE = 512;

/**
 * Redo record types
 */
enum class RedoType : uint16_t {
  UNKNOWN = 0,
  INSERT = 1,
  UPDATE = 2,
  DELETE = 3,
  PAGE_CREATE = 4,
  PAGE_INIT = 5,
  TRX_COMMIT = 6,
  TRX_ROLLBACK = 7,
  DDL = 8,
  CHECKPOINT = 9,
  MTR_COMMIT = 10
};

/**
 * Redo record flags
 */
enum RedoFlags : uint16_t {
  FLAG_MTR_START = 0x01,
  FLAG_MTR_END = 0x02,
  FLAG_SYNC = 0x04
};

/**
 * Redo record structure
 */
struct RedoRecord {
  lsn_t lsn;
  space_id_t space_id;
  page_id_t page_id;
  trx_id_t trx_id;
  mtr_id_t mtr_id;
  RedoType type;
  uint16_t flags;
  std::vector<uint8_t> data;
  
  // Timestamp for latency tracking
  std::chrono::steady_clock::time_point created_at;
  
  size_t size() const {
    return sizeof(lsn) + sizeof(space_id) + sizeof(page_id) +
           sizeof(trx_id) + sizeof(mtr_id) + sizeof(type) +
           sizeof(flags) + data.size();
  }
};

/**
 * Page data structure
 */
struct PageData {
  space_id_t space_id;
  page_id_t page_id;
  lsn_t page_lsn;
  std::vector<uint8_t> data;  // PAGE_SIZE bytes
  
  bool is_valid() const {
    return data.size() == PAGE_SIZE;
  }
};

/**
 * Write result from storage node
 */
struct WriteResult {
  std::string node_id;
  bool success;
  lsn_t persisted_lsn;
  std::string error_message;
  std::chrono::microseconds latency;
};

/**
 * Batch of redo records
 */
struct RedoBatch {
  std::string volume_id;
  std::vector<RedoRecord> records;
  lsn_t min_lsn;
  lsn_t max_lsn;
  
  size_t size() const {
    size_t total = 0;
    for (const auto& r : records) {
      total += r.size();
    }
    return total;
  }
  
  bool empty() const {
    return records.empty();
  }
  
  void clear() {
    records.clear();
    min_lsn = 0;
    max_lsn = 0;
  }
  
  void add(RedoRecord record) {
    if (records.empty()) {
      min_lsn = record.lsn;
    }
    max_lsn = record.lsn;
    records.push_back(std::move(record));
  }
};

/**
 * Volume Durable LSN (VDL) info
 */
struct VDLInfo {
  std::string volume_id;
  lsn_t vdl;
  lsn_t min_lsn;
  lsn_t max_lsn;
  std::chrono::steady_clock::time_point updated_at;
};

/**
 * Storage node status
 */
struct NodeStatus {
  std::string node_id;
  bool is_healthy;
  lsn_t current_lsn;
  int64_t wal_size_bytes;
  int64_t page_count;
  bool is_frozen;
  std::chrono::seconds uptime;
};

/**
 * Quorum write tracker
 */
class QuorumTracker {
public:
  QuorumTracker(int total, int quorum)
      : total_(total), quorum_(quorum), acks_(0) {}
  
  bool ack() {
    return ++acks_ >= quorum_;
  }
  
  bool is_complete() const {
    return acks_ >= quorum_;
  }
  
  int ack_count() const {
    return acks_.load();
  }
  
  int remaining() const {
    int r = quorum_ - acks_.load();
    return r > 0 ? r : 0;
  }

private:
  int total_;
  int quorum_;
  std::atomic<int> acks_;
};

}  // namespace aurora

#endif  // AURORA_TYPES_H
