/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Startup/Shutdown Logic
Implements Writer and Reader instance startup and shutdown sequences.

Reference: 01_compute_layer.md Section 4

*****************************************************************************/

#ifndef AURORA_STARTUP_H
#define AURORA_STARTUP_H

#include <cstdint>
#include <string>
#include <atomic>

namespace aurora {

// Forward declarations
struct AuroraConfig;

/**
 * Aurora Instance Mode
 */
enum class InstanceMode {
  WRITER,   // Single writer instance
  READER    // Read replica instance
};

/**
 * Aurora Instance State
 */
enum class InstanceState {
  UNINITIALIZED,
  INITIALIZING,
  RECOVERING,
  SYNCING,      // Reader: catching up with VDL
  READY,
  RUNNING,
  STOPPING,
  STOPPED,
  ERROR
};

/**
 * Get string representation of instance state
 */
inline const char* instance_state_name(InstanceState state) {
  switch (state) {
    case InstanceState::UNINITIALIZED: return "UNINITIALIZED";
    case InstanceState::INITIALIZING: return "INITIALIZING";
    case InstanceState::RECOVERING: return "RECOVERING";
    case InstanceState::SYNCING: return "SYNCING";
    case InstanceState::READY: return "READY";
    case InstanceState::RUNNING: return "RUNNING";
    case InstanceState::STOPPING: return "STOPPING";
    case InstanceState::STOPPED: return "STOPPED";
    case InstanceState::ERROR: return "ERROR";
    default: return "UNKNOWN";
  }
}

/**
 * Aurora Startup Context
 * Contains all state needed during startup
 */
struct StartupContext {
  InstanceMode mode;
  InstanceState state;
  std::string volume_id;
  std::string instance_id;
  uint64_t current_lsn;
  uint64_t vdl;
  uint64_t read_point;  // For reader
  bool is_first_start;
  std::string error_message;
};

/**
 * Aurora startup sequence for Writer instance (first start)
 * 
 * 1. Load configuration
 * 2. Initialize MySQL core components
 * 3. Register with metadata service (create volume)
 * 4. Initialize storage layer
 * 5. Initialize Redo Generator with LSN=0
 * 6. Initialize Buffer Pool
 * 7. Initialize Quorum Manager
 * 8. Create system tablespace
 * 9. Write initial redo to storage
 * 10. Update VDL in metadata service
 * 11. Start listening on port 3306
 */
bool aurora_writer_first_start(StartupContext* ctx);

/**
 * Aurora startup sequence for Writer instance (subsequent start)
 * 
 * 1. Load configuration
 * 2. Initialize MySQL core components
 * 3. Get volume info from metadata service (includes current VDL)
 * 4. Set current_lsn = VDL
 * 5. Initialize Redo Generator
 * 6. Initialize Buffer Pool (empty - pages loaded on demand)
 * 7. Initialize Quorum Manager
 * 8. Register with metadata service
 * 9. Start listening on port 3306
 */
bool aurora_writer_start(StartupContext* ctx);

/**
 * Aurora startup sequence for Reader instance (first start)
 * 
 * 1. Load configuration
 * 2. Initialize MySQL core components
 * 3. Get volume info from metadata service
 * 4. Register as reader with metadata service
 * 5. Set read_point = 0
 * 6. Initialize Buffer Pool (empty)
 * 7. Initialize Page Reader
 * 8. Start Redo sync thread
 * 9. Catch up to VDL (sync phase)
 * 10. Start listening on port 3306 (read-only)
 */
bool aurora_reader_first_start(StartupContext* ctx);

/**
 * Aurora startup sequence for Reader instance (subsequent start)
 * 
 * 1. Load configuration
 * 2. Get volume info from metadata service
 * 3. Set read_point = 0 (or checkpoint)
 * 4. Clear Buffer Pool
 * 5. Start Redo sync thread
 * 6. Catch up to VDL
 * 7. Start listening
 */
bool aurora_reader_start(StartupContext* ctx);

/**
 * Main Aurora startup entry point
 * Called from srv_start() in srv0start.cc
 */
bool aurora_startup(const AuroraConfig& config);

/**
 * Aurora shutdown sequence
 * Called from srv_shutdown() in srv0start.cc
 */
void aurora_shutdown();

/**
 * Check if Aurora startup is complete
 */
bool aurora_is_ready();

/**
 * Get current instance state
 */
InstanceState aurora_get_state();

/**
 * Get current instance mode
 */
InstanceMode aurora_get_mode();

/**
 * Wait for Aurora to be ready (blocking)
 * Returns false if timeout or error
 */
bool aurora_wait_ready(uint32_t timeout_ms);

/**
 * Global startup context
 */
extern StartupContext g_startup_ctx;

/**
 * Global state
 */
extern std::atomic<InstanceState> g_instance_state;
extern std::atomic<bool> g_is_ready;

}  // namespace aurora

#endif  // AURORA_STARTUP_H
