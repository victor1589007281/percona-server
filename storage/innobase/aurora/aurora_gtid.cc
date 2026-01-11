/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora GTID Manager Implementation

*****************************************************************************/

#include "aurora_gtid.h"
#include <sstream>
#include <iomanip>
#include <cstring>
#include <algorithm>

namespace aurora {

// Global instance
std::unique_ptr<AuroraGTIDManager> g_gtid_manager;

//============================================================================
// GTID Implementation
//============================================================================

std::string GTID::to_string() const {
  std::ostringstream oss;
  
  // Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx:gno
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 4; i++) oss << std::setw(2) << (int)server_uuid[i];
  oss << "-";
  for (int i = 4; i < 6; i++) oss << std::setw(2) << (int)server_uuid[i];
  oss << "-";
  for (int i = 6; i < 8; i++) oss << std::setw(2) << (int)server_uuid[i];
  oss << "-";
  for (int i = 8; i < 10; i++) oss << std::setw(2) << (int)server_uuid[i];
  oss << "-";
  for (int i = 10; i < 16; i++) oss << std::setw(2) << (int)server_uuid[i];
  oss << ":" << std::dec << gno;
  
  return oss.str();
}

GTID GTID::parse(const std::string& str) {
  GTID gtid;
  memset(&gtid, 0, sizeof(gtid));
  
  // Parse UUID:GNO format
  size_t colon = str.rfind(':');
  if (colon == std::string::npos) return gtid;
  
  std::string uuid_str = str.substr(0, colon);
  gtid.gno = std::stoull(str.substr(colon + 1));
  
  // Parse UUID (remove dashes)
  std::string clean_uuid;
  for (char c : uuid_str) {
    if (c != '-') clean_uuid += c;
  }
  
  if (clean_uuid.length() == 32) {
    for (size_t i = 0; i < 16; i++) {
      std::string byte_str = clean_uuid.substr(i * 2, 2);
      gtid.server_uuid[i] = (uint8_t)std::stoul(byte_str, nullptr, 16);
    }
  }
  
  return gtid;
}

bool GTID::operator<(const GTID& other) const {
  int cmp = memcmp(server_uuid, other.server_uuid, 16);
  if (cmp != 0) return cmp < 0;
  return gno < other.gno;
}

bool GTID::operator==(const GTID& other) const {
  return memcmp(server_uuid, other.server_uuid, 16) == 0 && gno == other.gno;
}

//============================================================================
// GTIDSet Implementation
//============================================================================

void GTIDSet::add(const GTID& gtid) {
  // Convert UUID to string key
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 16; i++) {
    oss << std::setw(2) << (int)gtid.server_uuid[i];
  }
  std::string uuid_key = oss.str();
  
  auto& intervals = intervals_[uuid_key];
  
  // Simple implementation: just add as a single point interval
  // A real implementation would merge overlapping intervals
  intervals.push_back({gtid.gno, gtid.gno});
  
  // Sort and merge intervals
  std::sort(intervals.begin(), intervals.end());
  
  std::vector<std::pair<uint64_t, uint64_t>> merged;
  for (const auto& interval : intervals) {
    if (merged.empty() || merged.back().second + 1 < interval.first) {
      merged.push_back(interval);
    } else {
      merged.back().second = std::max(merged.back().second, interval.second);
    }
  }
  intervals = std::move(merged);
}

void GTIDSet::merge(const GTIDSet& other) {
  for (const auto& [uuid, intervals] : other.intervals_) {
    for (const auto& [start, end] : intervals) {
      GTID gtid;
      // Parse UUID back
      for (size_t i = 0; i < 16 && i * 2 + 1 < uuid.length(); i++) {
        std::string byte_str = uuid.substr(i * 2, 2);
        gtid.server_uuid[i] = (uint8_t)std::stoul(byte_str, nullptr, 16);
      }
      for (uint64_t gno = start; gno <= end; gno++) {
        gtid.gno = gno;
        add(gtid);
      }
    }
  }
}

bool GTIDSet::contains(const GTID& gtid) const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 16; i++) {
    oss << std::setw(2) << (int)gtid.server_uuid[i];
  }
  std::string uuid_key = oss.str();
  
  auto it = intervals_.find(uuid_key);
  if (it == intervals_.end()) return false;
  
  for (const auto& [start, end] : it->second) {
    if (gtid.gno >= start && gtid.gno <= end) return true;
  }
  return false;
}

GTIDSet GTIDSet::subtract(const GTIDSet& other) const {
  GTIDSet result;
  
  for (const auto& [uuid, intervals] : intervals_) {
    auto other_it = other.intervals_.find(uuid);
    
    for (const auto& [start, end] : intervals) {
      GTID gtid;
      for (size_t i = 0; i < 16 && i * 2 + 1 < uuid.length(); i++) {
        std::string byte_str = uuid.substr(i * 2, 2);
        gtid.server_uuid[i] = (uint8_t)std::stoul(byte_str, nullptr, 16);
      }
      
      for (uint64_t gno = start; gno <= end; gno++) {
        gtid.gno = gno;
        if (other_it == other.intervals_.end() || !other.contains(gtid)) {
          result.add(gtid);
        }
      }
    }
  }
  
  return result;
}

std::string GTIDSet::to_string() const {
  if (intervals_.empty()) return "";
  
  std::ostringstream oss;
  bool first_uuid = true;
  
  for (const auto& [uuid, intervals] : intervals_) {
    if (!first_uuid) oss << ",";
    first_uuid = false;
    
    // Format UUID with dashes
    oss << uuid.substr(0, 8) << "-"
        << uuid.substr(8, 4) << "-"
        << uuid.substr(12, 4) << "-"
        << uuid.substr(16, 4) << "-"
        << uuid.substr(20, 12) << ":";
    
    bool first_interval = true;
    for (const auto& [start, end] : intervals) {
      if (!first_interval) oss << ":";
      first_interval = false;
      
      if (start == end) {
        oss << start;
      } else {
        oss << start << "-" << end;
      }
    }
  }
  
  return oss.str();
}

GTIDSet GTIDSet::parse(const std::string& str) {
  GTIDSet result;
  // Simple parser - a real implementation would be more robust
  // Format: uuid:1-100:200-300,uuid2:1-50
  return result;
}

bool GTIDSet::empty() const {
  return intervals_.empty();
}

size_t GTIDSet::count() const {
  size_t total = 0;
  for (const auto& [uuid, intervals] : intervals_) {
    for (const auto& [start, end] : intervals) {
      total += (end - start + 1);
    }
  }
  return total;
}

//============================================================================
// AuroraGTIDManager Implementation
//============================================================================

AuroraGTIDManager::AuroraGTIDManager() {
  memset(server_uuid_, 0, sizeof(server_uuid_));
}

AuroraGTIDManager::~AuroraGTIDManager() {}

void AuroraGTIDManager::init(const uint8_t* server_uuid) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  memcpy(server_uuid_, server_uuid, 16);
  next_gno_ = 1;
}

GTID AuroraGTIDManager::allocate() {
  GTID gtid;
  memcpy(gtid.server_uuid, server_uuid_, 16);
  gtid.gno = next_gno_.fetch_add(1);
  return gtid;
}

void AuroraGTIDManager::bind_lsn(const GTID& gtid, uint64_t lsn) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  
  lsn_to_gtid_map_[lsn] = gtid;
  gtid_to_lsn_map_[gtid] = lsn;
  executed_set_.add(gtid);
  
  // Trim if too many mappings
  if (lsn_to_gtid_map_.size() > MAX_MAPPING_SIZE) {
    auto it = lsn_to_gtid_map_.begin();
    gtid_to_lsn_map_.erase(it->second);
    lsn_to_gtid_map_.erase(it);
  }
}

GTIDSet AuroraGTIDManager::get_executed_set() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return executed_set_;
}

GTID AuroraGTIDManager::lsn_to_gtid(uint64_t lsn) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  
  auto it = lsn_to_gtid_map_.find(lsn);
  if (it != lsn_to_gtid_map_.end()) {
    return it->second;
  }
  
  // Find the closest GTID before this LSN
  auto lower = lsn_to_gtid_map_.lower_bound(lsn);
  if (lower != lsn_to_gtid_map_.begin()) {
    --lower;
    return lower->second;
  }
  
  GTID empty;
  memset(&empty, 0, sizeof(empty));
  return empty;
}

uint64_t AuroraGTIDManager::gtid_to_lsn(const GTID& gtid) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  
  auto it = gtid_to_lsn_map_.find(gtid);
  if (it != gtid_to_lsn_map_.end()) {
    return it->second;
  }
  return 0;
}

uint64_t AuroraGTIDManager::get_next_gno() const {
  return next_gno_.load();
}

void AuroraGTIDManager::set_next_gno(uint64_t gno) {
  next_gno_.store(gno);
}

void AuroraGTIDManager::persist() {
  // In real implementation, persist to metadata service
}

void AuroraGTIDManager::recover() {
  // In real implementation, recover from metadata service
}

std::string AuroraGTIDManager::get_server_uuid_string() const {
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (int i = 0; i < 4; i++) oss << std::setw(2) << (int)server_uuid_[i];
  oss << "-";
  for (int i = 4; i < 6; i++) oss << std::setw(2) << (int)server_uuid_[i];
  oss << "-";
  for (int i = 6; i < 8; i++) oss << std::setw(2) << (int)server_uuid_[i];
  oss << "-";
  for (int i = 8; i < 10; i++) oss << std::setw(2) << (int)server_uuid_[i];
  oss << "-";
  for (int i = 10; i < 16; i++) oss << std::setw(2) << (int)server_uuid_[i];
  return oss.str();
}

void AuroraGTIDManager::trim_before_lsn(uint64_t lsn) {
  std::unique_lock<std::shared_mutex> lock(mutex_);
  
  auto it = lsn_to_gtid_map_.begin();
  while (it != lsn_to_gtid_map_.end() && it->first < lsn) {
    gtid_to_lsn_map_.erase(it->second);
    it = lsn_to_gtid_map_.erase(it);
  }
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_gtid_manager_init(const uint8_t* server_uuid) {
  g_gtid_manager = std::make_unique<AuroraGTIDManager>();
  g_gtid_manager->init(server_uuid);
  g_gtid_manager->recover();
  return true;
}

void aurora_gtid_manager_shutdown() {
  if (g_gtid_manager) {
    g_gtid_manager->persist();
    g_gtid_manager.reset();
  }
}

GTID aurora_allocate_gtid(uint64_t trx_id, uint64_t commit_lsn) {
  if (!g_gtid_manager) {
    GTID empty;
    memset(&empty, 0, sizeof(empty));
    return empty;
  }
  
  GTID gtid = g_gtid_manager->allocate();
  g_gtid_manager->bind_lsn(gtid, commit_lsn);
  return gtid;
}

std::string aurora_get_executed_gtid_set() {
  if (!g_gtid_manager) {
    return "";
  }
  return g_gtid_manager->get_executed_set().to_string();
}

}  // namespace aurora
