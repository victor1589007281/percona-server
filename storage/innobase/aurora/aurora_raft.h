/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Raft Protocol (C++ Interface)
Interface to the Golang Raft implementation via gRPC.

Reference: 01_compute_layer.md Section 12

*****************************************************************************/

#ifndef AURORA_RAFT_H
#define AURORA_RAFT_H

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace aurora {

// Forward declarations
class AuroraStorageClient;

/**
 * Raft Node State
 */
enum class RaftState {
  FOLLOWER,
  CANDIDATE,
  LEADER,
  LEARNER
};

/**
 * Raft Log Entry
 */
struct RaftLogEntry {
  uint64_t index;
  uint64_t term;
  uint64_t lsn;
  std::vector<uint8_t> data;
  bool is_config_change;
};

/**
 * Raft Group Info
 */
struct RaftGroupInfo {
  std::string group_id;         // Protection Group ID
  uint64_t term;
  RaftState state;
  std::string leader_id;
  uint64_t commit_index;
  uint64_t applied_index;
  std::vector<std::string> members;
};

/**
 * Raft Replication Result
 */
struct RaftReplicationResult {
  bool success;
  uint64_t replicated_index;
  uint64_t replicated_lsn;
  uint32_t ack_count;
  std::string error_message;
};

/**
 * Raft Protocol Interface
 * Provides C++ interface to interact with Golang Raft implementation
 */
class AuroraRaftClient {
public:
  AuroraRaftClient();
  ~AuroraRaftClient();

  /**
   * Initialize the Raft client
   */
  bool initialize(const std::string& metadata_endpoints);

  /**
   * Shutdown
   */
  void shutdown();

  /**
   * Write redo through Raft
   * Replicates redo log entry to all group members
   */
  RaftReplicationResult replicate_redo(
      const std::string& pg_id,
      uint64_t lsn,
      const uint8_t* data,
      size_t len);

  /**
   * Get group info
   */
  bool get_group_info(const std::string& pg_id, RaftGroupInfo* info);

  /**
   * Get leader for a protection group
   */
  bool get_leader(const std::string& pg_id, std::string* leader_id);

  /**
   * Wait for commit
   */
  bool wait_commit(const std::string& pg_id, 
                   uint64_t index, 
                   uint32_t timeout_ms);

  /**
   * Add member to group
   */
  bool add_member(const std::string& pg_id, const std::string& member_id);

  /**
   * Remove member from group
   */
  bool remove_member(const std::string& pg_id, const std::string& member_id);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Global Raft client instance
 */
extern std::unique_ptr<AuroraRaftClient> g_raft_client;

/**
 * Initialize Raft client
 */
bool aurora_raft_init(const std::string& metadata_endpoints);

/**
 * Shutdown Raft client
 */
void aurora_raft_shutdown();

/**
 * Replicate redo via Raft
 */
RaftReplicationResult aurora_raft_replicate(
    const std::string& pg_id,
    uint64_t lsn,
    const uint8_t* data,
    size_t len);

}  // namespace aurora

#endif  // AURORA_RAFT_H
