/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora GTID Manager
Manages GTID assignment and LSN-GTID mapping for binlog compatibility.

Reference: 01_compute_layer.md Section 9.3

*****************************************************************************/

#ifndef AURORA_GTID_H
#define AURORA_GTID_H

#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <atomic>

namespace aurora {

/**
 * GTID structure
 */
struct GTID {
  uint8_t server_uuid[16];  // RFC 4122 UUID
  uint64_t gno;             // Global Transaction Number

  std::string to_string() const;
  static GTID parse(const std::string& str);
  
  bool operator<(const GTID& other) const;
  bool operator==(const GTID& other) const;
};

/**
 * GTID Set - represents a set of executed GTIDs
 */
class GTIDSet {
public:
  // Add a single GTID
  void add(const GTID& gtid);
  
  // Merge another set into this one
  void merge(const GTIDSet& other);
  
  // Check if contains a GTID
  bool contains(const GTID& gtid) const;
  
  // Subtract another set and return difference
  GTIDSet subtract(const GTIDSet& other) const;
  
  // Convert to string format: uuid:1-100,uuid:200-300
  std::string to_string() const;
  
  // Parse from string
  static GTIDSet parse(const std::string& str);
  
  // Get the first (minimum) GTID
  GTID get_first() const;
  
  // Get the last (maximum) GTID
  GTID get_last() const;
  
  // Check if empty
  bool empty() const;
  
  // Get count of GTIDs
  size_t count() const;

private:
  // server_uuid -> list of [start, end] intervals
  std::map<std::string, std::vector<std::pair<uint64_t, uint64_t>>> intervals_;
};

/**
 * Aurora GTID Manager
 * Manages GTID allocation and LSN-GTID mapping
 */
class AuroraGTIDManager {
public:
  AuroraGTIDManager();
  ~AuroraGTIDManager();

  // Disable copy
  AuroraGTIDManager(const AuroraGTIDManager&) = delete;
  AuroraGTIDManager& operator=(const AuroraGTIDManager&) = delete;

  /**
   * Initialize with server UUID
   */
  void init(const uint8_t* server_uuid);

  /**
   * Allocate a new GTID for a transaction
   */
  GTID allocate();

  /**
   * Bind GTID to LSN (called at commit)
   */
  void bind_lsn(const GTID& gtid, uint64_t lsn);

  /**
   * Get the set of executed GTIDs
   */
  GTIDSet get_executed_set() const;

  /**
   * Convert LSN to GTID
   */
  GTID lsn_to_gtid(uint64_t lsn) const;

  /**
   * Convert GTID to LSN
   */
  uint64_t gtid_to_lsn(const GTID& gtid) const;

  /**
   * Get the next GNO to be assigned
   */
  uint64_t get_next_gno() const;

  /**
   * Set the next GNO (for recovery)
   */
  void set_next_gno(uint64_t gno);

  /**
   * Persist GTID state to storage
   */
  void persist();

  /**
   * Recover GTID state from storage
   */
  void recover();

  /**
   * Get the server UUID as string
   */
  std::string get_server_uuid_string() const;

  /**
   * Trim old mappings before given LSN
   */
  void trim_before_lsn(uint64_t lsn);

private:
  uint8_t server_uuid_[16];
  std::atomic<uint64_t> next_gno_{1};
  GTIDSet executed_set_;

  // LSN <-> GTID bidirectional mapping
  std::map<uint64_t, GTID> lsn_to_gtid_map_;
  std::map<GTID, uint64_t> gtid_to_lsn_map_;

  mutable std::shared_mutex mutex_;
  
  // Maximum mappings to keep in memory
  static constexpr size_t MAX_MAPPING_SIZE = 1000000;
};

/**
 * Global GTID manager instance
 */
extern std::unique_ptr<AuroraGTIDManager> g_gtid_manager;

/**
 * Initialize GTID manager
 */
bool aurora_gtid_manager_init(const uint8_t* server_uuid);

/**
 * Shutdown GTID manager
 */
void aurora_gtid_manager_shutdown();

/**
 * Allocate and bind GTID for a transaction
 */
GTID aurora_allocate_gtid(uint64_t trx_id, uint64_t commit_lsn);

/**
 * Get executed GTID set as string
 */
std::string aurora_get_executed_gtid_set();

}  // namespace aurora

#endif  // AURORA_GTID_H
