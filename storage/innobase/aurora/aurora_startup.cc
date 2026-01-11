/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Startup/Shutdown Implementation

*****************************************************************************/

#include "aurora_startup.h"
#include "aurora.h"
#include "aurora_hook.h"
#include "aurora_config.h"
#include "aurora_client.h"
#include "aurora_redo_sender.h"
#include "aurora_page_reader.h"
#include "aurora_reader_sync.h"
#include "aurora_gtid.h"
#include "aurora_binlog.h"
#include "aurora_replication.h"
#include "aurora_sysvars.h"

#include <thread>
#include <chrono>

namespace aurora {

// Global state
StartupContext g_startup_ctx;
std::atomic<InstanceState> g_instance_state{InstanceState::UNINITIALIZED};
std::atomic<bool> g_is_ready{false};

// Forward declarations
extern AuroraConfig g_config;
extern std::unique_ptr<AuroraStorageClient> g_storage_client;
extern std::unique_ptr<AuroraMetadataClient> g_metadata_client;
extern std::unique_ptr<AuroraRedoSender> g_redo_sender;
extern std::unique_ptr<AuroraPageReader> g_page_reader;
extern std::unique_ptr<AuroraReaderSync> g_reader_sync;
extern std::atomic<uint64_t> g_current_lsn;
extern std::atomic<uint64_t> g_vdl;
extern std::atomic<bool> g_is_writer;

//============================================================================
// Helper Functions
//============================================================================

static void set_state(InstanceState state) {
  g_instance_state.store(state);
  g_startup_ctx.state = state;
}

static bool init_grpc_clients(const AuroraConfig& config) {
  // Initialize storage client
  g_storage_client = std::make_unique<AuroraStorageClient>();
  if (!g_storage_client->initialize(config.storage_endpoints)) {
    g_startup_ctx.error_message = "Failed to initialize storage client";
    return false;
  }
  
  // Initialize metadata client
  g_metadata_client = std::make_unique<AuroraMetadataClient>();
  if (!g_metadata_client->initialize(config.metadata_endpoints)) {
    g_startup_ctx.error_message = "Failed to initialize metadata client";
    return false;
  }
  
  return true;
}

static bool init_redo_sender(const AuroraConfig& config) {
  g_redo_sender = std::make_unique<AuroraRedoSender>();
  if (!g_redo_sender->initialize(config, g_storage_client.get())) {
    g_startup_ctx.error_message = "Failed to initialize redo sender";
    return false;
  }
  return true;
}

static bool init_page_reader(const AuroraConfig& config) {
  g_page_reader = std::make_unique<AuroraPageReader>();
  if (!g_page_reader->initialize(config, g_storage_client.get())) {
    g_startup_ctx.error_message = "Failed to initialize page reader";
    return false;
  }
  return true;
}

static bool init_reader_sync(const AuroraConfig& config) {
  g_reader_sync = std::make_unique<AuroraReaderSync>();
  if (!g_reader_sync->initialize(config, g_storage_client.get(), g_metadata_client.get())) {
    g_startup_ctx.error_message = "Failed to initialize reader sync";
    return false;
  }
  return true;
}

static void register_aurora_hooks() {
  // Register Redo write hook
  AURORA_HOOKS.register_redo_write_hook(
      [](const unsigned char* data, size_t len, uint64_t start_lsn, uint64_t end_lsn) -> bool {
        if (!g_redo_sender) return false;
        return g_redo_sender->send(data, len, start_lsn, end_lsn);
      });
  
  // Register Redo flush hook (Quorum wait)
  AURORA_HOOKS.register_redo_flush_hook(
      [](uint64_t flush_to_lsn) -> bool {
        if (!g_redo_sender) return false;
        return g_redo_sender->wait_durable(flush_to_lsn, g_config.quorum_timeout_ms);
      });
  
  // Register Page read hook
  AURORA_HOOKS.register_page_read_hook(
      [](uint32_t space_id, uint32_t page_no, unsigned char* buf, uint64_t target_lsn) -> bool {
        if (!g_page_reader) return false;
        return g_page_reader->read_page(space_id, page_no, buf, target_lsn);
      });
  
  // Register Page write hook (disable local writes)
  AURORA_HOOKS.register_page_write_hook(
      [](uint32_t space_id, uint32_t page_no, const unsigned char* buf) -> bool {
        // In Aurora mode, pages are not written locally
        // Redo logs are the source of truth
        return true;  // Skip local write
      });
  
  // Register Checkpoint hook (disable local checkpoint)
  AURORA_HOOKS.register_checkpoint_hook(
      [](uint64_t checkpoint_lsn) -> bool {
        // Aurora doesn't need local checkpoints
        // VDL serves as the durable point
        return true;  // Skip local checkpoint
      });
  
  // Register Recovery hook (skip local recovery)
  AURORA_HOOKS.register_recovery_hook(
      []() -> bool {
        // Aurora recovers from remote storage
        // Skip local redo log recovery
        return true;
      });
  
  // Register Transaction commit hook (GTID assignment)
  AURORA_HOOKS.register_trx_commit_hook(
      [](uint64_t trx_id, uint64_t commit_lsn) {
        if (g_config.binlog_compat && g_gtid_manager) {
          aurora_allocate_gtid(trx_id, commit_lsn);
        }
      });
}

//============================================================================
// Writer Startup
//============================================================================

bool aurora_writer_first_start(StartupContext* ctx) {
  set_state(InstanceState::INITIALIZING);
  
  // Step 1-2: Configuration already loaded
  
  // Step 3: Register with metadata service, create volume
  VolumeInfo volume_info;
  if (!g_metadata_client->create_volume(ctx->volume_id, &volume_info)) {
    ctx->error_message = "Failed to create volume";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Step 4: Initialize storage layer
  if (!g_storage_client->initialize_volume(ctx->volume_id)) {
    ctx->error_message = "Failed to initialize storage volume";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Step 5: Initialize Redo Generator with LSN=0
  ctx->current_lsn = 0;
  g_current_lsn.store(0);
  
  // Step 6-7: Initialize components
  if (!init_redo_sender(g_config)) return false;
  if (!init_page_reader(g_config)) return false;
  
  // Initialize replication protocol
  if (!aurora_replication_init(g_config, g_storage_client.get())) {
    ctx->error_message = "Failed to initialize replication protocol";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Initialize binlog compatibility layer
  if (g_config.binlog_compat) {
    uint8_t server_uuid[16] = {0};  // TODO: Get real server UUID
    aurora_gtid_manager_init(server_uuid);
    aurora_binlog_init(g_config.server_id, g_config.binlog_buffer_size);
  }
  
  // Step 8: Register hooks
  register_aurora_hooks();
  AURORA_HOOKS.set_aurora_mode(true);
  AURORA_HOOKS.set_writer_mode(true);
  
  // Step 9-10: System tablespace creation is handled by MySQL
  // VDL update will happen after first redo is written
  
  set_state(InstanceState::READY);
  g_is_writer.store(true);
  g_is_ready.store(true);
  
  return true;
}

bool aurora_writer_start(StartupContext* ctx) {
  set_state(InstanceState::INITIALIZING);
  
  // Step 3: Get volume info from metadata service
  VolumeInfo volume_info;
  if (!g_metadata_client->get_volume(ctx->volume_id, &volume_info)) {
    ctx->error_message = "Failed to get volume info";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Step 4: Set current_lsn = VDL
  ctx->vdl = volume_info.vdl;
  ctx->current_lsn = volume_info.vdl;
  g_vdl.store(volume_info.vdl);
  g_current_lsn.store(volume_info.vdl);
  
  // Step 5-7: Initialize components
  if (!init_redo_sender(g_config)) return false;
  if (!init_page_reader(g_config)) return false;
  
  // Initialize replication
  if (!aurora_replication_init(g_config, g_storage_client.get())) {
    ctx->error_message = "Failed to initialize replication protocol";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Initialize binlog
  if (g_config.binlog_compat) {
    uint8_t server_uuid[16] = {0};
    aurora_gtid_manager_init(server_uuid);
    aurora_binlog_init(g_config.server_id, g_config.binlog_buffer_size);
    
    // Recover GTID state
    if (g_gtid_manager) {
      g_gtid_manager->recover();
    }
  }
  
  // Step 8: Register with metadata service
  if (!g_metadata_client->register_instance(ctx->instance_id, true /* is_writer */)) {
    ctx->error_message = "Failed to register instance";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Register hooks
  register_aurora_hooks();
  AURORA_HOOKS.set_aurora_mode(true);
  AURORA_HOOKS.set_writer_mode(true);
  
  set_state(InstanceState::READY);
  g_is_writer.store(true);
  g_is_ready.store(true);
  
  return true;
}

//============================================================================
// Reader Startup
//============================================================================

bool aurora_reader_first_start(StartupContext* ctx) {
  set_state(InstanceState::INITIALIZING);
  
  // Step 3: Get volume info
  VolumeInfo volume_info;
  if (!g_metadata_client->get_volume(ctx->volume_id, &volume_info)) {
    ctx->error_message = "Failed to get volume info";
    set_state(InstanceState::ERROR);
    return false;
  }
  ctx->vdl = volume_info.vdl;
  g_vdl.store(volume_info.vdl);
  
  // Step 4: Register as reader
  if (!g_metadata_client->register_instance(ctx->instance_id, false /* is_reader */)) {
    ctx->error_message = "Failed to register as reader";
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Step 5: Set read_point = 0
  ctx->read_point = 0;
  
  // Step 6-7: Initialize components
  if (!init_page_reader(g_config)) return false;
  if (!init_reader_sync(g_config)) return false;
  
  // Register hooks
  register_aurora_hooks();
  AURORA_HOOKS.set_aurora_mode(true);
  AURORA_HOOKS.set_writer_mode(false);
  
  // Step 8-9: Start sync thread and catch up
  set_state(InstanceState::SYNCING);
  
  g_reader_sync->start();
  
  // Wait for sync to catch up
  while (g_reader_sync->get_lag_lsn() > g_config.max_reader_lag_lsn) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Check for errors
    if (g_reader_sync->has_error()) {
      ctx->error_message = "Reader sync failed";
      set_state(InstanceState::ERROR);
      return false;
    }
  }
  
  set_state(InstanceState::READY);
  g_is_writer.store(false);
  g_is_ready.store(true);
  
  return true;
}

bool aurora_reader_start(StartupContext* ctx) {
  set_state(InstanceState::INITIALIZING);
  
  // Get volume info
  VolumeInfo volume_info;
  if (!g_metadata_client->get_volume(ctx->volume_id, &volume_info)) {
    ctx->error_message = "Failed to get volume info";
    set_state(InstanceState::ERROR);
    return false;
  }
  ctx->vdl = volume_info.vdl;
  g_vdl.store(volume_info.vdl);
  
  // Set read_point = 0 (or from checkpoint)
  ctx->read_point = 0;
  
  // Initialize components
  if (!init_page_reader(g_config)) return false;
  if (!init_reader_sync(g_config)) return false;
  
  // Register hooks
  register_aurora_hooks();
  AURORA_HOOKS.set_aurora_mode(true);
  AURORA_HOOKS.set_writer_mode(false);
  
  // Start sync
  set_state(InstanceState::SYNCING);
  g_reader_sync->start();
  
  // Wait for catch up
  while (g_reader_sync->get_lag_lsn() > g_config.max_reader_lag_lsn) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_reader_sync->has_error()) {
      ctx->error_message = "Reader sync failed";
      set_state(InstanceState::ERROR);
      return false;
    }
  }
  
  set_state(InstanceState::READY);
  g_is_writer.store(false);
  g_is_ready.store(true);
  
  return true;
}

//============================================================================
// Main Entry Points
//============================================================================

bool aurora_startup(const AuroraConfig& config) {
  // Store config globally
  g_config = config;
  
  // Initialize context
  g_startup_ctx.mode = config.is_writer ? InstanceMode::WRITER : InstanceMode::READER;
  g_startup_ctx.volume_id = config.volume_id;
  g_startup_ctx.instance_id = config.instance_id;
  g_startup_ctx.is_first_start = config.is_first_start;
  
  // Initialize gRPC clients
  if (!init_grpc_clients(config)) {
    set_state(InstanceState::ERROR);
    return false;
  }
  
  // Check if this is first start by querying metadata service
  VolumeInfo volume_info;
  bool volume_exists = g_metadata_client->get_volume(config.volume_id, &volume_info);
  
  bool success = false;
  
  if (config.is_writer) {
    if (!volume_exists) {
      success = aurora_writer_first_start(&g_startup_ctx);
    } else {
      success = aurora_writer_start(&g_startup_ctx);
    }
  } else {
    if (!volume_exists) {
      g_startup_ctx.error_message = "Volume does not exist for reader";
      set_state(InstanceState::ERROR);
      return false;
    }
    success = aurora_reader_start(&g_startup_ctx);
  }
  
  if (success) {
    set_state(InstanceState::RUNNING);
  }
  
  return success;
}

void aurora_shutdown() {
  set_state(InstanceState::STOPPING);
  g_is_ready.store(false);
  
  // Stop reader sync first
  if (g_reader_sync) {
    g_reader_sync->stop();
    g_reader_sync.reset();
  }
  
  // Flush remaining redo
  if (g_redo_sender) {
    g_redo_sender->flush();
    g_redo_sender->stop();
    g_redo_sender.reset();
  }
  
  // Persist GTID state
  if (g_gtid_manager) {
    g_gtid_manager->persist();
  }
  
  // Shutdown binlog
  aurora_binlog_shutdown();
  aurora_gtid_manager_shutdown();
  
  // Shutdown replication
  aurora_replication_shutdown();
  
  // Close page reader
  if (g_page_reader) {
    g_page_reader.reset();
  }
  
  // Deregister from metadata service
  if (g_metadata_client) {
    g_metadata_client->deregister_instance(g_startup_ctx.instance_id);
  }
  
  // Close clients
  if (g_storage_client) {
    g_storage_client->shutdown();
    g_storage_client.reset();
  }
  if (g_metadata_client) {
    g_metadata_client->shutdown();
    g_metadata_client.reset();
  }
  
  // Clear hooks
  AURORA_HOOKS.clear_all_hooks();
  AURORA_HOOKS.set_aurora_mode(false);
  
  set_state(InstanceState::STOPPED);
}

bool aurora_is_ready() {
  return g_is_ready.load();
}

InstanceState aurora_get_state() {
  return g_instance_state.load();
}

InstanceMode aurora_get_mode() {
  return g_startup_ctx.mode;
}

bool aurora_wait_ready(uint32_t timeout_ms) {
  auto start = std::chrono::steady_clock::now();
  
  while (!g_is_ready.load()) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start);
    
    if (elapsed.count() >= timeout_ms) {
      return false;
    }
    
    if (g_instance_state.load() == InstanceState::ERROR) {
      return false;
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  
  return true;
}

}  // namespace aurora
