/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Page Reader Implementation

*****************************************************************************/

#include "aurora_page_reader.h"
#include <algorithm>
#include <cstring>

namespace aurora {

// Global page reader instance
std::unique_ptr<AuroraPageReader> g_page_reader;

AuroraPageReader::AuroraPageReader(const AuroraConfig& config)
    : config_(config),
      storage_client_(nullptr),
      metadata_client_(nullptr) {}

AuroraPageReader::~AuroraPageReader() {
  shutdown();
}

bool AuroraPageReader::init() {
  storage_client_ = g_storage_client.get();
  metadata_client_ = g_metadata_client.get();
  
  if (!storage_client_ || !metadata_client_) {
    return false;
  }
  
  // Initialize preferred nodes list
  {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    preferred_nodes_ = config_.storage_nodes;
  }
  
  return true;
}

void AuroraPageReader::shutdown() {
  clear_cache();
  storage_client_ = nullptr;
  metadata_client_ = nullptr;
}

bool AuroraPageReader::read_page(space_id_t space_id, page_id_t page_id,
                                  lsn_t target_lsn, PageData* page) {
  if (!storage_client_) return false;
  
  stats_.reads_total++;
  
  // Try cache first
  if (try_read_from_cache(space_id, page_id, target_lsn, page)) {
    stats_.cache_hits++;
    return true;
  }
  stats_.cache_misses++;
  
  auto start = std::chrono::steady_clock::now();
  
  // Read from storage
  bool success = storage_client_->read_page(space_id, page_id, target_lsn, page);
  
  auto end = std::chrono::steady_clock::now();
  auto latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
  
  if (success) {
    stats_.reads_success++;
    stats_.bytes_read += PAGE_SIZE;
    stats_.total_latency_us += latency.count();
    
    // Add to cache
    add_to_cache(*page);
  } else {
    stats_.reads_failed++;
  }
  
  return success;
}

bool AuroraPageReader::read_page_to_buffer(space_id_t space_id, page_id_t page_id,
                                            lsn_t target_lsn, uint8_t* buffer) {
  PageData page;
  if (!read_page(space_id, page_id, target_lsn, &page)) {
    return false;
  }
  
  if (page.data.size() != PAGE_SIZE) {
    return false;
  }
  
  std::memcpy(buffer, page.data.data(), PAGE_SIZE);
  return true;
}

bool AuroraPageReader::read_pages(
    const std::vector<std::pair<space_id_t, page_id_t>>& pages,
    lsn_t target_lsn, std::vector<PageData>* results) {
  return storage_client_->read_pages(pages, target_lsn, results);
}

void AuroraPageReader::prefetch_pages(
    const std::vector<std::pair<space_id_t, page_id_t>>& pages,
    lsn_t target_lsn) {
  // Async prefetch
  std::thread([this, pages, target_lsn]() {
    std::vector<PageData> results;
    storage_client_->read_pages(pages, target_lsn, &results);
    
    for (const auto& page : results) {
      add_to_cache(page);
    }
  }).detach();
}

void AuroraPageReader::invalidate_page(space_id_t space_id, page_id_t page_id) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  
  PageKey key{space_id, page_id};
  cache_.erase(key);
}

void AuroraPageReader::clear_cache() {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  cache_.clear();
}

const PageReaderStats& AuroraPageReader::get_stats() const {
  return stats_;
}

bool AuroraPageReader::is_healthy() const {
  return storage_client_ && storage_client_->is_healthy();
}

bool AuroraPageReader::try_read_from_cache(space_id_t space_id, page_id_t page_id,
                                            lsn_t target_lsn, PageData* page) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  
  PageKey key{space_id, page_id};
  auto it = cache_.find(key);
  
  if (it == cache_.end()) {
    return false;
  }
  
  auto& cached = it->second;
  
  // Check if cached page is recent enough
  if (target_lsn > 0 && cached->data.page_lsn < target_lsn) {
    return false;
  }
  
  *page = cached->data;
  cached->access_count++;
  
  return true;
}

void AuroraPageReader::add_to_cache(const PageData& page) {
  std::lock_guard<std::mutex> lock(cache_mutex_);
  
  // Evict if needed
  evict_cache_if_needed();
  
  PageKey key{page.space_id, page.page_id};
  
  auto cached = std::make_unique<CachedPage>();
  cached->data = page;
  cached->cached_at = std::chrono::steady_clock::now();
  cached->access_count = 1;
  
  cache_[key] = std::move(cached);
}

void AuroraPageReader::evict_cache_if_needed() {
  // Called with cache_mutex_ held
  
  if (cache_.size() < max_cache_pages_) {
    return;
  }
  
  // Simple LRU: find least recently accessed
  auto oldest_it = cache_.end();
  uint32_t min_access = UINT32_MAX;
  
  for (auto it = cache_.begin(); it != cache_.end(); ++it) {
    uint32_t access = it->second->access_count.load();
    if (access < min_access) {
      min_access = access;
      oldest_it = it;
    }
  }
  
  if (oldest_it != cache_.end()) {
    cache_.erase(oldest_it);
  }
}

std::vector<std::string> AuroraPageReader::get_nodes_for_page(
    space_id_t space_id, page_id_t page_id) {
  std::vector<std::string> nodes;
  metadata_client_->get_page_location(space_id, page_id, &nodes);
  
  if (nodes.empty()) {
    std::lock_guard<std::mutex> lock(nodes_mutex_);
    return preferred_nodes_;
  }
  
  return nodes;
}

//============================================================================
// Global Functions
//============================================================================

bool aurora_page_reader_init() {
  if (!aurora_config.enabled) return true;
  
  g_page_reader = std::make_unique<AuroraPageReader>(aurora_config);
  return g_page_reader->init();
}

void aurora_page_reader_shutdown() {
  if (g_page_reader) {
    g_page_reader->shutdown();
    g_page_reader.reset();
  }
}

bool aurora_read_page(space_id_t space_id, page_id_t page_id,
                      lsn_t target_lsn, uint8_t* buffer) {
  if (!g_page_reader) return false;
  return g_page_reader->read_page_to_buffer(space_id, page_id, target_lsn, buffer);
}

void aurora_prefetch_pages(
    const std::vector<std::pair<space_id_t, page_id_t>>& pages) {
  if (!g_page_reader) return;
  g_page_reader->prefetch_pages(pages, 0);
}

void aurora_invalidate_page(space_id_t space_id, page_id_t page_id) {
  if (!g_page_reader) return;
  g_page_reader->invalidate_page(space_id, page_id);
}

}  // namespace aurora
