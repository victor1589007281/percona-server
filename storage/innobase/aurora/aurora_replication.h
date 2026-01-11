/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Replication Protocol Abstraction
Supports both Quorum and Multi-Raft protocols.

Reference: 01_compute_layer.md Section 12

*****************************************************************************/

#ifndef AURORA_REPLICATION_H
#define AURORA_REPLICATION_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace aurora {

// Forward declarations
struct AuroraConfig;
class AuroraStorageClient;

/**
 * Replication Protocol Type
 */
enum class ReplicationProtocolType {
  QUORUM,   // No leader, write to all nodes, wait for 4/6 acks
  RAFT      // Per-PG Raft groups with leader election
};

/**
 * Write Result
 */
struct ReplicationResult {
  bool success;
  uint64_t persisted_lsn;
  int ack_count;
  std::vector<std::string> failed_nodes;
  int64_t latency_us;
};

/**
 * Abstract Replication Protocol Interface
 */
class ReplicationProtocol {
public:
  virtual ~ReplicationProtocol() = default;

  /**
   * Initialize the protocol
   */
  virtual bool initialize(const AuroraConfig& config,
                          AuroraStorageClient* storage_client) = 0;

  /**
   * Shutdown the protocol
   */
  virtual void shutdown() = 0;

  /**
   * Write redo data
   */
  virtual ReplicationResult write_redo(
      const unsigned char* data,
      size_t len,
      uint64_t start_lsn,
      uint64_t end_lsn) = 0;

  /**
   * Wait for durable to given LSN
   */
  virtual bool wait_durable(uint64_t lsn, uint32_t timeout_ms) = 0;

  /**
   * Get current VDL (Volume Durable LSN)
   */
  virtual uint64_t get_vdl() const = 0;

  /**
   * Get current VCL (Volume Complete LSN)
   */
  virtual uint64_t get_vcl() const = 0;

  /**
   * Get protocol type
   */
  virtual ReplicationProtocolType get_type() const = 0;

  /**
   * Get protocol name for display
   */
  virtual const char* get_name() const = 0;

  /**
   * Check if protocol is healthy
   */
  virtual bool is_healthy() const = 0;

  /**
   * Get statistics
   */
  struct Stats {
    uint64_t writes_total;
    uint64_t writes_success;
    uint64_t writes_failed;
    double avg_latency_us;
    double p99_latency_us;
    uint64_t bytes_written;
  };
  virtual Stats get_stats() const = 0;
};

/**
 * Quorum Protocol Implementation
 * Writes to all 6 nodes, waits for 4 acks
 */
class QuorumProtocol : public ReplicationProtocol {
public:
  QuorumProtocol();
  ~QuorumProtocol() override;

  bool initialize(const AuroraConfig& config,
                  AuroraStorageClient* storage_client) override;
  void shutdown() override;

  ReplicationResult write_redo(
      const unsigned char* data,
      size_t len,
      uint64_t start_lsn,
      uint64_t end_lsn) override;

  bool wait_durable(uint64_t lsn, uint32_t timeout_ms) override;
  uint64_t get_vdl() const override;
  uint64_t get_vcl() const override;
  ReplicationProtocolType get_type() const override { return ReplicationProtocolType::QUORUM; }
  const char* get_name() const override { return "Quorum"; }
  bool is_healthy() const override;
  Stats get_stats() const override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Raft Protocol Implementation
 * Per-PG Raft groups with leader election
 */
class RaftProtocol : public ReplicationProtocol {
public:
  RaftProtocol();
  ~RaftProtocol() override;

  bool initialize(const AuroraConfig& config,
                  AuroraStorageClient* storage_client) override;
  void shutdown() override;

  ReplicationResult write_redo(
      const unsigned char* data,
      size_t len,
      uint64_t start_lsn,
      uint64_t end_lsn) override;

  bool wait_durable(uint64_t lsn, uint32_t timeout_ms) override;
  uint64_t get_vdl() const override;
  uint64_t get_vcl() const override;
  ReplicationProtocolType get_type() const override { return ReplicationProtocolType::RAFT; }
  const char* get_name() const override { return "Multi-Raft"; }
  bool is_healthy() const override;
  Stats get_stats() const override;

  /**
   * Get leader for a protection group
   */
  std::string get_pg_leader(uint32_t pg_id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Create replication protocol based on configuration
 */
std::unique_ptr<ReplicationProtocol> create_replication_protocol(
    const std::string& protocol_name);

/**
 * Global replication protocol instance
 */
extern std::unique_ptr<ReplicationProtocol> g_replication_protocol;

/**
 * Initialize replication protocol
 */
bool aurora_replication_init(const AuroraConfig& config,
                             AuroraStorageClient* storage_client);

/**
 * Shutdown replication protocol
 */
void aurora_replication_shutdown();

/**
 * Write redo through replication protocol
 */
ReplicationResult aurora_replicate_redo(
    const unsigned char* data,
    size_t len,
    uint64_t start_lsn,
    uint64_t end_lsn);

}  // namespace aurora

#endif  // AURORA_REPLICATION_H
