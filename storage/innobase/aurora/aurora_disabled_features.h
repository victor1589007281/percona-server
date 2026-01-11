/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Disabled Features
Defines which MySQL/InnoDB features are disabled in Aurora mode.

Reference: 01_compute_layer.md Section 5

*****************************************************************************/

#ifndef AURORA_DISABLED_FEATURES_H
#define AURORA_DISABLED_FEATURES_H

#include <cstdint>

namespace aurora {

/**
 * Features disabled in Aurora mode
 * These features are unnecessary or incompatible with Aurora architecture
 */
enum class DisabledFeature : uint32_t {
  NONE             = 0,
  
  // ========== InnoDB Storage Features ==========
  DOUBLE_WRITE     = (1 << 0),   // Double write buffer (pages go to storage layer)
  INSERT_BUFFER    = (1 << 1),   // Insert buffer / Change buffer
  ADAPTIVE_HASH    = (1 << 2),   // Adaptive hash index (optional)
  
  // ========== Redo/Recovery Features ==========
  LOCAL_REDO_LOG   = (1 << 3),   // Local redo log files
  LOCAL_CHECKPOINT = (1 << 4),   // Local checkpoints
  LOCAL_RECOVERY   = (1 << 5),   // Recovery from local redo
  
  // ========== Flush Features ==========
  PAGE_FLUSH       = (1 << 6),   // Dirty page flushing to local files
  LRU_FLUSH        = (1 << 7),   // LRU flushing
  
  // ========== File Management ==========
  DATA_FILES       = (1 << 8),   // Local data files (ibdata, .ibd)
  REDO_FILES       = (1 << 9),   // Local redo log files
  UNDO_FILES       = (1 << 10),  // Local undo tablespace files
  TEMP_FILES       = (1 << 11),  // Temp tablespace (keep local for performance)
  
  // ========== Replication Features ==========
  NATIVE_BINLOG    = (1 << 12),  // Native MySQL binlog (use Aurora Binlog Adapter)
  NATIVE_RELAY_LOG = (1 << 13),  // Native relay log
  NATIVE_REPL      = (1 << 14),  // Native MySQL replication
  
  // ========== Backup Features ==========
  LOCAL_BACKUP     = (1 << 15),  // Local backup (use Aurora backup)
  
  // ========== Presets ==========
  
  // Writer preset: all features except temp files
  WRITER_PRESET = DOUBLE_WRITE | INSERT_BUFFER | LOCAL_REDO_LOG | 
                  LOCAL_CHECKPOINT | LOCAL_RECOVERY | PAGE_FLUSH |
                  NATIVE_BINLOG | NATIVE_RELAY_LOG | LOCAL_BACKUP,
  
  // Reader preset: same as writer
  READER_PRESET = WRITER_PRESET,
  
  // All features
  ALL = 0xFFFFFFFF
};

inline DisabledFeature operator|(DisabledFeature a, DisabledFeature b) {
  return static_cast<DisabledFeature>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline DisabledFeature operator&(DisabledFeature a, DisabledFeature b) {
  return static_cast<DisabledFeature>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

/**
 * Check if a feature is disabled
 */
inline bool is_feature_disabled(DisabledFeature disabled, DisabledFeature feature) {
  return (disabled & feature) != DisabledFeature::NONE;
}

/**
 * Get disabled features for current mode
 */
DisabledFeature get_disabled_features();

/**
 * Check if double write is disabled
 */
inline bool is_double_write_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::DOUBLE_WRITE);
}

/**
 * Check if insert buffer is disabled
 */
inline bool is_insert_buffer_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::INSERT_BUFFER);
}

/**
 * Check if local page flush is disabled
 */
inline bool is_page_flush_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::PAGE_FLUSH);
}

/**
 * Check if local checkpoint is disabled
 */
inline bool is_local_checkpoint_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::LOCAL_CHECKPOINT);
}

/**
 * Check if local recovery is disabled
 */
inline bool is_local_recovery_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::LOCAL_RECOVERY);
}

/**
 * Check if native binlog is disabled
 */
inline bool is_native_binlog_disabled() {
  return is_feature_disabled(get_disabled_features(), DisabledFeature::NATIVE_BINLOG);
}

}  // namespace aurora

#endif  // AURORA_DISABLED_FEATURES_H
