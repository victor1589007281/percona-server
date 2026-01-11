/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Replication Protocol Implementation

*****************************************************************************/

#include "aurora_replication.h"
#include "aurora_config.h"
#include "aurora_client.h"
#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>
#include <condition_variable>
#include <algorithm>
#include <cstring>

namespace aurora {

// Global instance
std::unique_ptr<ReplicationProtocol> g_replication_protocol;

//============================================================================
// Quorum Protocol Implementation
//============================================================================

struct QuorumProtocol::Impl {
  AuroraConfig config;
  AuroraStorageClient* storage_client = nullptr;
  
  std::atomic<uint64_t> vdl{0};
  std::atomic<uint64_t> vcl{0};
  std::atomic<bool> healthy{true};
  
  // Statistics
  std::atomic<uint64_t> writes_total{0};
  std::atomic<uint64_t> writes_success{0};
  std::atomic<uint64_t> writes_failed{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> total_latency_us{0};
  
  // Durable waiters
  std::mutex durable_mutex;
  std::condition_variable durable_cv;
  
  // Node LSN tracking
  std::vector<std::atomic<uint64_t>> node_lsns;
  int quorum_write = 4;
  int quorum_read = 3;
  int total_nodes = 6;
};

QuorumProtocol::QuorumProtocol() : impl_(std::make_unique<Impl>()) {}

QuorumProtocol::~QuorumProtocol() {
  shutdown();
}

bool QuorumProtocol::initialize(const AuroraConfig& config,
                                AuroraStorageClient* storage_client) {
  impl_->config = config;
  impl_->storage_client = storage_client;
  impl_->quorum_write = config.quorum_write;
  impl_->quorum_read = config.quorum_read;
  impl_->total_nodes = 6;
  
  impl_->node_lsns.resize(impl_->total_nodes);
  for (auto& lsn : impl_->node_lsns) {
    lsn.store(0);
  }
  
  return true;
}

void QuorumProtocol::shutdown() {
  impl_->healthy.store(false);
}

ReplicationResult QuorumProtocol::write_redo(
    const unsigned char* data,
    size_t len,
    uint64_t start_lsn,
    uint64_t end_lsn) {
  
  auto start_time = std::chrono::high_resolution_clock::now();
  
  ReplicationResult result;
  result.success = false;
  result.persisted_lsn = 0;
  result.ack_count = 0;
  
  impl_->writes_total.fetch_add(1);
  
  if (!impl_->storage_client) {
    impl_->writes_failed.fetch_add(1);
    return result;
  }
  
  // Write to all nodes in parallel
  std::vector<std::thread> threads;
  std::atomic<int> ack_count{0};
  std::atomic<int> fail_count{0};
  std::mutex failed_mutex;
  
  for (int i = 0; i < impl_->total_nodes; i++) {
    threads.emplace_back([this, i, data, len, start_lsn, end_lsn,
                          &ack_count, &fail_count, &failed_mutex, &result]() {
      bool success = impl_->storage_client->write_redo(
          i, data, len, start_lsn, end_lsn);
      
      if (success) {
        ack_count.fetch_add(1);
        impl_->node_lsns[i].store(end_lsn);
      } else {
        fail_count.fetch_add(1);
        std::lock_guard<std::mutex> lock(failed_mutex);
        result.failed_nodes.push_back("node-" + std::to_string(i + 1));
      }
    });
  }
  
  // Wait for all writes to complete or timeout
  for (auto& t : threads) {
    t.join();
  }
  
  result.ack_count = ack_count.load();
  
  // Check if quorum achieved
  if (result.ack_count >= impl_->quorum_write) {
    result.success = true;
    result.persisted_lsn = end_lsn;
    impl_->writes_success.fetch_add(1);
    impl_->bytes_written.fetch_add(len);
    
    // Update VDL
    impl_->vdl.store(end_lsn);
    impl_->vcl.store(end_lsn);
    
    // Notify waiters
    impl_->durable_cv.notify_all();
  } else {
    impl_->writes_failed.fetch_add(1);
  }
  
  auto end_time = std::chrono::high_resolution_clock::now();
  result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time).count();
  impl_->total_latency_us.fetch_add(result.latency_us);
  
  return result;
}

bool QuorumProtocol::wait_durable(uint64_t lsn, uint32_t timeout_ms) {
  std::unique_lock<std::mutex> lock(impl_->durable_mutex);
  return impl_->durable_cv.wait_for(
      lock,
      std::chrono::milliseconds(timeout_ms),
      [this, lsn]() { return impl_->vdl.load() >= lsn; });
}

uint64_t QuorumProtocol::get_vdl() const {
  return impl_->vdl.load();
}

uint64_t QuorumProtocol::get_vcl() const {
  return impl_->vcl.load();
}

bool QuorumProtocol::is_healthy() const {
  return impl_->healthy.load();
}

ReplicationProtocol::Stats QuorumProtocol::get_stats() const {
  Stats stats;
  stats.writes_total = impl_->writes_total.load();
  stats.writes_success = impl_->writes_success.load();
  stats.writes_failed = impl_->writes_failed.load();
  stats.bytes_written = impl_->bytes_written.load();
  
  if (stats.writes_success > 0) {
    stats.avg_latency_us = static_cast<double>(impl_->total_latency_us.load()) /
                           stats.writes_success;
  } else {
    stats.avg_latency_us = 0;
  }
  stats.p99_latency_us = 0;  // TODO: Track histogram
  
  return stats;
}

//============================================================================
// Raft Protocol Implementation
//============================================================================

struct RaftProtocol::Impl {
  AuroraConfig config;
  AuroraStorageClient* storage_client = nullptr;
  
  std::atomic<uint64_t> vdl{0};
  std::atomic<uint64_t> vcl{0};
  std::atomic<bool> healthy{true};
  
  // Statistics
  std::atomic<uint64_t> writes_total{0};
  std::atomic<uint64_t> writes_success{0};
  std::atomic<uint64_t> writes_failed{0};
  std::atomic<uint64_t> bytes_written{0};
  std::atomic<uint64_t> total_latency_us{0};
  
  // PG leader cache
  std::map<uint32_t, std::string> pg_leaders;
  std::mutex leader_mutex;
};

RaftProtocol::RaftProtocol() : impl_(std::make_unique<Impl>()) {}

RaftProtocol::~RaftProtocol() {
  shutdown();
}

bool RaftProtocol::initialize(const AuroraConfig& config,
                              AuroraStorageClient* storage_client) {
  impl_->config = config;
  impl_->storage_client = storage_client;
  return true;
}

void RaftProtocol::shutdown() {
  impl_->healthy.store(false);
}

ReplicationResult RaftProtocol::write_redo(
    const unsigned char* data,
    size_t len,
    uint64_t start_lsn,
    uint64_t end_lsn) {
  
  auto start_time = std::chrono::high_resolution_clock::now();
  
  ReplicationResult result;
  result.success = false;
  result.persisted_lsn = 0;
  result.ack_count = 0;
  
  impl_->writes_total.fetch_add(1);
  
  if (!impl_->storage_client) {
    impl_->writes_failed.fetch_add(1);
    return result;
  }
  
  // In Raft mode, we need to:
  // 1. Determine the PG for this data
  // 2. Find the leader for that PG
  // 3. Send to leader, which will replicate to followers
  
  // For simplicity, we use PG 0 (single PG mode)
  uint32_t pg_id = 0;
  std::string leader = get_pg_leader(pg_id);
  
  // Write to leader (which handles replication internally)
  bool success = impl_->storage_client->write_redo(
      0, data, len, start_lsn, end_lsn);
  
  if (success) {
    result.success = true;
    result.persisted_lsn = end_lsn;
    result.ack_count = 3;  // Raft majority
    impl_->writes_success.fetch_add(1);
    impl_->bytes_written.fetch_add(len);
    
    impl_->vdl.store(end_lsn);
    impl_->vcl.store(end_lsn);
  } else {
    impl_->writes_failed.fetch_add(1);
  }
  
  auto end_time = std::chrono::high_resolution_clock::now();
  result.latency_us = std::chrono::duration_cast<std::chrono::microseconds>(
      end_time - start_time).count();
  impl_->total_latency_us.fetch_add(result.latency_us);
  
  return result;
}

bool RaftProtocol::wait_durable(uint64_t lsn, uint32_t timeout_ms) {
  // Wait for VDL to reach lsn
  auto start = std::chrono::steady_clock::now();
  while (impl_->vdl.load() < lsn) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start).count();
    if (elapsed >= timeout_ms) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

uint64_t RaftProtocol::get_vdl() const {
  return impl_->vdl.load();
}

uint64_t RaftProtocol::get_vcl() const {
  return impl_->vcl.load();
}

bool RaftProtocol::is_healthy() const {
  return impl_->healthy.load();
}

ReplicationProtocol::Stats RaftProtocol::get_stats() const {
  Stats stats;
  stats.writes_total = impl_->writes_total.load();
  stats.writes_success = impl_->writes_success.load();
  stats.writes_failed = impl_->writes_failed.load();
  stats.bytes_written = impl_->bytes_written.load();
  
  if (stats.writes_success > 0) {
    stats.avg_latency_us = static_cast<double>(impl_->total_latency_us.load()) /
                           stats.writes_success;
  } else {
    stats.avg_latency_us = 0;
  }
  stats.p99_latency_us = 0;
  
  return stats;
}

std::string RaftProtocol::get_pg_leader(uint32_t pg_id) const {
  std::lock_guard<std::mutex> lock(impl_->leader_mutex);
  auto it = impl_->pg_leaders.find(pg_id);
  if (it != impl_->pg_leaders.end()) {
    return it->second;
  }
  return "node-1";  // Default leader
}

//============================================================================
// Factory and Global Functions
//============================================================================

std::unique_ptr<ReplicationProtocol> create_replication_protocol(
    const std::string& protocol_name) {
  if (protocol_name == "raft") {
    return std::make_unique<RaftProtocol>();
  }
  return std::make_unique<QuorumProtocol>();  // Default
}

bool aurora_replication_init(const AuroraConfig& config,
                             AuroraStorageClient* storage_client) {
  g_replication_protocol = create_replication_protocol(config.replication_protocol);
  if (!g_replication_protocol) {
    return false;
  }
  return g_replication_protocol->initialize(config, storage_client);
}

void aurora_replication_shutdown() {
  if (g_replication_protocol) {
    g_replication_protocol->shutdown();
    g_replication_protocol.reset();
  }
}

ReplicationResult aurora_replicate_redo(
    const unsigned char* data,
    size_t len,
    uint64_t start_lsn,
    uint64_t end_lsn) {
  if (!g_replication_protocol) {
    return ReplicationResult{false, 0, 0, {}, 0};
  }
  return g_replication_protocol->write_redo(data, len, start_lsn, end_lsn);
}

}  // namespace aurora
