/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Redo Log Types
Complete redo log type definitions for Aurora.

Reference: 01_compute_layer.md Section 2

*****************************************************************************/

#ifndef AURORA_REDO_TYPES_H
#define AURORA_REDO_TYPES_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace aurora {

/**
 * Aurora Redo Type Enumeration
 * Compatible with InnoDB redo types but with Aurora extensions
 */
enum class RedoType : uint16_t {
  // ========== Record Operations ==========
  MLOG_REC_INSERT           = 1,   // Insert record
  MLOG_REC_UPDATE_IN_PLACE  = 2,   // Update in place
  MLOG_REC_UPDATE           = 3,   // Update (may move)
  MLOG_REC_DELETE           = 4,   // Delete record
  MLOG_REC_CLUST_DELETE_MARK = 5,  // Mark clustered index delete
  MLOG_REC_SEC_DELETE_MARK  = 6,   // Mark secondary index delete

  // ========== Page Operations ==========
  MLOG_PAGE_CREATE          = 10,  // Create page
  MLOG_PAGE_INIT            = 11,  // Initialize page
  MLOG_PAGE_REORGANIZE      = 12,  // Page reorganization
  MLOG_PAGE_SPLIT           = 13,  // Page split
  MLOG_PAGE_MERGE           = 14,  // Page merge

  // ========== B+Tree Operations ==========
  MLOG_BTREE_INSERT         = 20,  // B+tree insert
  MLOG_BTREE_DELETE         = 21,  // B+tree delete
  MLOG_BTREE_SPLIT          = 22,  // B+tree split
  MLOG_BTREE_MERGE          = 23,  // B+tree merge

  // ========== Transaction Operations ==========
  MLOG_TRX_PREPARE          = 30,  // Transaction prepare
  MLOG_TRX_COMMIT           = 31,  // Transaction commit
  MLOG_TRX_ROLLBACK         = 32,  // Transaction rollback

  // ========== MTR Operations ==========
  MLOG_MTR_COMMIT           = 40,  // MTR commit

  // ========== DDL Operations ==========
  MLOG_DDL_CREATE_TABLE     = 50,  // Create table
  MLOG_DDL_DROP_TABLE       = 51,  // Drop table
  MLOG_DDL_ALTER_TABLE      = 52,  // Alter table
  MLOG_DDL_CREATE_INDEX     = 53,  // Create index
  MLOG_DDL_DROP_INDEX       = 54,  // Drop index
  MLOG_DDL_TRUNCATE         = 55,  // Truncate table

  // ========== Space Operations ==========
  MLOG_SPACE_CREATE         = 60,  // Create tablespace
  MLOG_SPACE_DROP           = 61,  // Drop tablespace
  MLOG_SPACE_EXTEND         = 62,  // Extend tablespace

  // ========== Undo Operations ==========
  MLOG_UNDO_INSERT          = 70,  // Undo insert
  MLOG_UNDO_UPDATE          = 71,  // Undo update
  MLOG_UNDO_ERASE           = 72,  // Undo erase

  // ========== System Operations ==========
  MLOG_CHECKPOINT           = 80,  // Checkpoint
  MLOG_BARRIER              = 81,  // DDL Barrier
  MLOG_FILE_OP              = 82,  // File operation

  // ========== Aurora Extensions ==========
  MLOG_AURORA_LSN_SYNC      = 100, // LSN synchronization
  MLOG_AURORA_VDL_UPDATE    = 101, // VDL update
  MLOG_AURORA_PAGE_VERSION  = 102, // Page version info
  MLOG_AURORA_READER_SYNC   = 103, // Reader sync marker
  MLOG_AURORA_FREEZE        = 104, // Freeze writes
  MLOG_AURORA_UNFREEZE      = 105, // Unfreeze writes
  MLOG_AURORA_FAILOVER      = 106, // Failover marker
};

/**
 * Redo Record Flags
 */
constexpr uint16_t REDO_FLAG_MTR_START   = 0x0001;  // MTR start
constexpr uint16_t REDO_FLAG_MTR_END     = 0x0002;  // MTR end
constexpr uint16_t REDO_FLAG_SYNC        = 0x0004;  // Synchronous write
constexpr uint16_t REDO_FLAG_DDL         = 0x0008;  // DDL operation
constexpr uint16_t REDO_FLAG_BARRIER     = 0x0010;  // Barrier
constexpr uint16_t REDO_FLAG_COMPRESSED  = 0x0020;  // Data is compressed
constexpr uint16_t REDO_FLAG_ENCRYPTED   = 0x0040;  // Data is encrypted

/**
 * Redo Record Header (48 bytes, 8-byte aligned)
 */
struct RedoRecordHeader {
  uint64_t lsn;           // Log sequence number
  uint64_t space_id;      // Tablespace ID
  uint64_t page_id;       // Page ID
  uint64_t trx_id;        // Transaction ID
  uint64_t mtr_id;        // Mini-transaction ID
  RedoType type;          // Redo type (2 bytes)
  uint16_t flags;         // Flags
  uint32_t data_len;      // Data length
} __attribute__((packed));

static_assert(sizeof(RedoRecordHeader) == 48, "RedoRecordHeader must be 48 bytes");

/**
 * Redo Record
 */
struct RedoRecord {
  RedoRecordHeader header;
  std::vector<uint8_t> data;
  uint32_t checksum;
  
  // Convenience getters
  uint64_t get_lsn() const { return header.lsn; }
  uint64_t get_space_id() const { return header.space_id; }
  uint64_t get_page_id() const { return header.page_id; }
  uint64_t get_trx_id() const { return header.trx_id; }
  RedoType get_type() const { return header.type; }
  
  bool is_mtr_start() const { return header.flags & REDO_FLAG_MTR_START; }
  bool is_mtr_end() const { return header.flags & REDO_FLAG_MTR_END; }
  bool is_ddl() const { return header.flags & REDO_FLAG_DDL; }
  bool is_barrier() const { return header.flags & REDO_FLAG_BARRIER; }
  
  size_t total_size() const {
    return sizeof(RedoRecordHeader) + data.size() + sizeof(checksum);
  }
};

/**
 * Get redo type name
 */
inline const char* redo_type_name(RedoType type) {
  switch (type) {
    case RedoType::MLOG_REC_INSERT: return "REC_INSERT";
    case RedoType::MLOG_REC_UPDATE_IN_PLACE: return "REC_UPDATE_IN_PLACE";
    case RedoType::MLOG_REC_UPDATE: return "REC_UPDATE";
    case RedoType::MLOG_REC_DELETE: return "REC_DELETE";
    case RedoType::MLOG_REC_CLUST_DELETE_MARK: return "REC_CLUST_DELETE_MARK";
    case RedoType::MLOG_REC_SEC_DELETE_MARK: return "REC_SEC_DELETE_MARK";
    case RedoType::MLOG_PAGE_CREATE: return "PAGE_CREATE";
    case RedoType::MLOG_PAGE_INIT: return "PAGE_INIT";
    case RedoType::MLOG_PAGE_REORGANIZE: return "PAGE_REORGANIZE";
    case RedoType::MLOG_PAGE_SPLIT: return "PAGE_SPLIT";
    case RedoType::MLOG_PAGE_MERGE: return "PAGE_MERGE";
    case RedoType::MLOG_BTREE_INSERT: return "BTREE_INSERT";
    case RedoType::MLOG_BTREE_DELETE: return "BTREE_DELETE";
    case RedoType::MLOG_BTREE_SPLIT: return "BTREE_SPLIT";
    case RedoType::MLOG_BTREE_MERGE: return "BTREE_MERGE";
    case RedoType::MLOG_TRX_PREPARE: return "TRX_PREPARE";
    case RedoType::MLOG_TRX_COMMIT: return "TRX_COMMIT";
    case RedoType::MLOG_TRX_ROLLBACK: return "TRX_ROLLBACK";
    case RedoType::MLOG_MTR_COMMIT: return "MTR_COMMIT";
    case RedoType::MLOG_DDL_CREATE_TABLE: return "DDL_CREATE_TABLE";
    case RedoType::MLOG_DDL_DROP_TABLE: return "DDL_DROP_TABLE";
    case RedoType::MLOG_DDL_ALTER_TABLE: return "DDL_ALTER_TABLE";
    case RedoType::MLOG_DDL_CREATE_INDEX: return "DDL_CREATE_INDEX";
    case RedoType::MLOG_DDL_DROP_INDEX: return "DDL_DROP_INDEX";
    case RedoType::MLOG_DDL_TRUNCATE: return "DDL_TRUNCATE";
    case RedoType::MLOG_SPACE_CREATE: return "SPACE_CREATE";
    case RedoType::MLOG_SPACE_DROP: return "SPACE_DROP";
    case RedoType::MLOG_SPACE_EXTEND: return "SPACE_EXTEND";
    case RedoType::MLOG_UNDO_INSERT: return "UNDO_INSERT";
    case RedoType::MLOG_UNDO_UPDATE: return "UNDO_UPDATE";
    case RedoType::MLOG_UNDO_ERASE: return "UNDO_ERASE";
    case RedoType::MLOG_CHECKPOINT: return "CHECKPOINT";
    case RedoType::MLOG_BARRIER: return "BARRIER";
    case RedoType::MLOG_FILE_OP: return "FILE_OP";
    case RedoType::MLOG_AURORA_LSN_SYNC: return "AURORA_LSN_SYNC";
    case RedoType::MLOG_AURORA_VDL_UPDATE: return "AURORA_VDL_UPDATE";
    case RedoType::MLOG_AURORA_PAGE_VERSION: return "AURORA_PAGE_VERSION";
    case RedoType::MLOG_AURORA_READER_SYNC: return "AURORA_READER_SYNC";
    case RedoType::MLOG_AURORA_FREEZE: return "AURORA_FREEZE";
    case RedoType::MLOG_AURORA_UNFREEZE: return "AURORA_UNFREEZE";
    case RedoType::MLOG_AURORA_FAILOVER: return "AURORA_FAILOVER";
    default: return "UNKNOWN";
  }
}

/**
 * Check if redo type affects a specific page
 */
inline bool redo_affects_page(RedoType type) {
  switch (type) {
    case RedoType::MLOG_REC_INSERT:
    case RedoType::MLOG_REC_UPDATE_IN_PLACE:
    case RedoType::MLOG_REC_UPDATE:
    case RedoType::MLOG_REC_DELETE:
    case RedoType::MLOG_REC_CLUST_DELETE_MARK:
    case RedoType::MLOG_REC_SEC_DELETE_MARK:
    case RedoType::MLOG_PAGE_CREATE:
    case RedoType::MLOG_PAGE_INIT:
    case RedoType::MLOG_PAGE_REORGANIZE:
    case RedoType::MLOG_PAGE_SPLIT:
    case RedoType::MLOG_PAGE_MERGE:
    case RedoType::MLOG_BTREE_INSERT:
    case RedoType::MLOG_BTREE_DELETE:
    case RedoType::MLOG_BTREE_SPLIT:
    case RedoType::MLOG_BTREE_MERGE:
      return true;
    default:
      return false;
  }
}

/**
 * Check if redo type is a transaction operation
 */
inline bool redo_is_trx_op(RedoType type) {
  return type == RedoType::MLOG_TRX_PREPARE ||
         type == RedoType::MLOG_TRX_COMMIT ||
         type == RedoType::MLOG_TRX_ROLLBACK;
}

/**
 * Check if redo type is a DDL operation
 */
inline bool redo_is_ddl(RedoType type) {
  return static_cast<uint16_t>(type) >= 50 &&
         static_cast<uint16_t>(type) < 60;
}

/**
 * Check if redo type is an Aurora extension
 */
inline bool redo_is_aurora_type(RedoType type) {
  return static_cast<uint16_t>(type) >= 100;
}

//============================================================================
// Redo Data Structures for specific types
//============================================================================

/**
 * Insert record data
 * MLOG_REC_INSERT: slot_no(2) + rec_len(2) + rec_data(N)
 */
struct InsertRecordData {
  uint16_t slot_no;
  uint16_t rec_len;
  std::vector<uint8_t> rec_data;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(4 + rec_data.size());
    memcpy(&out[0], &slot_no, 2);
    memcpy(&out[2], &rec_len, 2);
    memcpy(&out[4], rec_data.data(), rec_data.size());
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 4) return false;
    memcpy(&slot_no, data, 2);
    memcpy(&rec_len, data + 2, 2);
    if (len < 4 + rec_len) return false;
    rec_data.assign(data + 4, data + 4 + rec_len);
    return true;
  }
};

/**
 * Update in place data
 * MLOG_REC_UPDATE_IN_PLACE: slot_no(2) + offset(2) + old_len(2) + new_len(2) + old_data(M) + new_data(N)
 */
struct UpdateInPlaceData {
  uint16_t slot_no;
  uint16_t offset;
  uint16_t old_len;
  uint16_t new_len;
  std::vector<uint8_t> old_data;
  std::vector<uint8_t> new_data;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(8 + old_data.size() + new_data.size());
    size_t pos = 0;
    memcpy(&out[pos], &slot_no, 2); pos += 2;
    memcpy(&out[pos], &offset, 2); pos += 2;
    memcpy(&out[pos], &old_len, 2); pos += 2;
    memcpy(&out[pos], &new_len, 2); pos += 2;
    memcpy(&out[pos], old_data.data(), old_data.size()); pos += old_data.size();
    memcpy(&out[pos], new_data.data(), new_data.size());
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 8) return false;
    size_t pos = 0;
    memcpy(&slot_no, data + pos, 2); pos += 2;
    memcpy(&offset, data + pos, 2); pos += 2;
    memcpy(&old_len, data + pos, 2); pos += 2;
    memcpy(&new_len, data + pos, 2); pos += 2;
    if (len < 8 + old_len + new_len) return false;
    old_data.assign(data + pos, data + pos + old_len); pos += old_len;
    new_data.assign(data + pos, data + pos + new_len);
    return true;
  }
};

/**
 * Transaction commit data
 * MLOG_TRX_COMMIT: commit_lsn(8) + timestamp(8)
 */
struct TrxCommitData {
  uint64_t commit_lsn;
  uint64_t timestamp;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(16);
    memcpy(&out[0], &commit_lsn, 8);
    memcpy(&out[8], &timestamp, 8);
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 16) return false;
    memcpy(&commit_lsn, data, 8);
    memcpy(&timestamp, data + 8, 8);
    return true;
  }
};

/**
 * Page init data
 * MLOG_PAGE_INIT: page_type(4) + flags(4)
 */
struct PageInitData {
  uint32_t page_type;
  uint32_t flags;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(8);
    memcpy(&out[0], &page_type, 4);
    memcpy(&out[4], &flags, 4);
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 8) return false;
    memcpy(&page_type, data, 4);
    memcpy(&flags, data + 4, 4);
    return true;
  }
};

/**
 * Checkpoint data
 * MLOG_CHECKPOINT: checkpoint_lsn(8) + checkpoint_no(8)
 */
struct CheckpointData {
  uint64_t checkpoint_lsn;
  uint64_t checkpoint_no;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(16);
    memcpy(&out[0], &checkpoint_lsn, 8);
    memcpy(&out[8], &checkpoint_no, 8);
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 16) return false;
    memcpy(&checkpoint_lsn, data, 8);
    memcpy(&checkpoint_no, data + 8, 8);
    return true;
  }
};

/**
 * Aurora VDL update data
 * MLOG_AURORA_VDL_UPDATE: new_vdl(8) + old_vdl(8) + quorum_count(4) + padding(4)
 */
struct AuroraVDLUpdateData {
  uint64_t new_vdl;
  uint64_t old_vdl;
  uint32_t quorum_count;
  uint32_t padding;
  
  void serialize(std::vector<uint8_t>& out) const {
    out.resize(24);
    memcpy(&out[0], &new_vdl, 8);
    memcpy(&out[8], &old_vdl, 8);
    memcpy(&out[16], &quorum_count, 4);
    memcpy(&out[20], &padding, 4);
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 24) return false;
    memcpy(&new_vdl, data, 8);
    memcpy(&old_vdl, data + 8, 8);
    memcpy(&quorum_count, data + 16, 4);
    memcpy(&padding, data + 20, 4);
    return true;
  }
};

/**
 * Aurora Reader Sync data
 * MLOG_AURORA_READER_SYNC: read_point(8) + target_vdl(8) + reader_id_len(4) + reader_id(N)
 */
struct AuroraReaderSyncData {
  uint64_t read_point;
  uint64_t target_vdl;
  std::string reader_id;
  
  void serialize(std::vector<uint8_t>& out) const {
    uint32_t id_len = static_cast<uint32_t>(reader_id.size());
    out.resize(20 + id_len);
    memcpy(&out[0], &read_point, 8);
    memcpy(&out[8], &target_vdl, 8);
    memcpy(&out[16], &id_len, 4);
    memcpy(&out[20], reader_id.data(), id_len);
  }
  
  bool deserialize(const uint8_t* data, size_t len) {
    if (len < 20) return false;
    uint32_t id_len;
    memcpy(&read_point, data, 8);
    memcpy(&target_vdl, data + 8, 8);
    memcpy(&id_len, data + 16, 4);
    if (len < 20 + id_len) return false;
    reader_id.assign(reinterpret_cast<const char*>(data + 20), id_len);
    return true;
  }
};

}  // namespace aurora

#endif  // AURORA_REDO_TYPES_H
