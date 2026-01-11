/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Page Reader
Provides page read functionality from Aurora storage layer.

*****************************************************************************/

#ifndef AURORA_PAGE_READER_H
#define AURORA_PAGE_READER_H

#include "aurora_config.h"
#include "aurora_types.h"
#include "aurora_client.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace aurora {

/**
 * Page read statistics
 */
struct PageReaderStats {
  std::atomic<uint64_t> reads_total{0};
  std::atomic<uint64_t> reads_success{0};
  std::atomic<uint64_t> reads_failed{0};
  std::atomic<uint64_t> bytes_read{0};
  std::atomic<uint64_t> total_latency_us{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_misses{0};
  
  void reset() {
    reads_total = 0;
    reads_success = 0;
    reads_failed = 0;
    bytes_read = 0;
    total_latency_us = 0;
    cache_hits = 0;
    cache_misses = 0;
  }
  
  double avg_latency_ms() const {
    uint64_t reads = reads_success.load();
    if (reads == 0) return 0;
    return static_cast<double>(total_latency_us.load()) / reads / 1000.0;
  }
  
  double cache_hit_rate() const {
    uint64_t total = cache_hits.load() + cache_misses.load();
    if (total == 0) return 0;
    return static_cast<double>(cache_hits.load()) / total;
  }
};

/**
 * Page key for caching
 */
struct PageKey {
  space_id_t space_id;
  page_id_t page_id;
  
  bool operator==(const PageKey& other) const {
    return space_id == other.space_id && page_id == other.page_id;
  }
};

/**
 * Hash function for PageKey
 */
struct PageKeyHash {
  size_t operator()(const PageKey& key) const {
    return std::hash<uint64_t>{}(
        (static_cast<uint64_t>(key.space_id) << 32) | key.page_id);
  }
};

/**
 * Cached page entry
 */
struct CachedPage {
  PageData data;
  std::chrono::steady_clock::time_point cached_at;
  std::atomic<uint32_t> access_count{0};
};

/**
 * Aurora Page Reader
 * 
 * This class provides page read functionality from the Aurora storage layer.
 * It is used to:
 * 1. Read pages on cache miss in Buffer Pool
 * 2. Prefetch pages for read-ahead
 * 3. Support reader instance synchronization
 * 
 * Features:
 * - Read from any of 6 storage nodes
 * - Automatic node selection (lowest latency)
 * - Retry on failure
 * - Optional local caching
 */
class AuroraPageReader {
public:
  explicit AuroraPageReader(const AuroraConfig& config);
  ~AuroraPageReader();
  
  // Disable copy
  AuroraPageReader(const AuroraPageReader&) = delete;
  AuroraPageReader& operator=(const AuroraPageReader&) = delete;
  
  /**
   * Initialize the reader
   */
  bool init();
  
  /**
   * Shutdown the reader
   */
  void shutdown();
  
  /**
   * Read a single page
   * @param space_id Tablespace ID
   * @param page_id Page ID within the tablespace
   * @param target_lsn Target LSN (0 for latest)
   * @param page Output page data
   * @return true on success
   */
  bool read_page(space_id_t space_id, page_id_t page_id,
                 lsn_t target_lsn, PageData* page);
  
  /**
   * Read a page directly into InnoDB buffer
   * This is the primary interface for Buffer Pool integration
   */
  bool read_page_to_buffer(space_id_t space_id, page_id_t page_id,
                           lsn_t target_lsn, uint8_t* buffer);
  
  /**
   * Read multiple pages (batch read)
   */
  bool read_pages(const std::vector<std::pair<space_id_t, page_id_t>>& pages,
                  lsn_t target_lsn, std::vector<PageData>* results);
  
  /**
   * Prefetch pages asynchronously
   */
  void prefetch_pages(const std::vector<std::pair<space_id_t, page_id_t>>& pages,
                      lsn_t target_lsn);
  
  /**
   * Invalidate cached page (called when local modification happens)
   */
  void invalidate_page(space_id_t space_id, page_id_t page_id);
  
  /**
   * Clear all cached pages
   */
  void clear_cache();
  
  /**
   * Get statistics
   */
  const PageReaderStats& get_stats() const;
  
  /**
   * Check if reader is healthy
   */
  bool is_healthy() const;

private:
  const AuroraConfig& config_;
  AuroraStorageClient* storage_client_;
  AuroraMetadataClient* metadata_client_;
  
  // Local page cache (optional, for frequently accessed pages)
  std::unordered_map<PageKey, std::unique_ptr<CachedPage>, PageKeyHash> cache_;
  std::mutex cache_mutex_;
  size_t max_cache_pages_ = 1000;
  
  // Statistics
  PageReaderStats stats_;
  
  // Node selection
  std::vector<std::string> preferred_nodes_;
  std::mutex nodes_mutex_;
  
  // Internal methods
  bool try_read_from_cache(space_id_t space_id, page_id_t page_id,
                           lsn_t target_lsn, PageData* page);
  void add_to_cache(const PageData& page);
  void evict_cache_if_needed();
  std::vector<std::string> get_nodes_for_page(space_id_t space_id, page_id_t page_id);
};

/**
 * Global page reader instance
 */
extern std::unique_ptr<AuroraPageReader> g_page_reader;

/**
 * Initialize page reader
 */
bool aurora_page_reader_init();

/**
 * Shutdown page reader
 */
void aurora_page_reader_shutdown();

/**
 * Read a page from Aurora storage
 * This is the main entry point called from InnoDB
 */
bool aurora_read_page(space_id_t space_id, page_id_t page_id,
                      lsn_t target_lsn, uint8_t* buffer);

/**
 * Prefetch pages
 */
void aurora_prefetch_pages(const std::vector<std::pair<space_id_t, page_id_t>>& pages);

/**
 * Invalidate page in cache
 */
void aurora_invalidate_page(space_id_t space_id, page_id_t page_id);

}  // namespace aurora

#endif  // AURORA_PAGE_READER_H
