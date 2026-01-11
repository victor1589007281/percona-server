/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Transport Implementation
TCP/gRPC and RDMA transport implementations.

*****************************************************************************/

#include "aurora_transport.h"
#include "aurora_config.h"

#include <mutex>
#include <unordered_map>
#include <queue>
#include <thread>
#include <condition_variable>

// For RDMA detection
#ifdef __linux__
#include <sys/stat.h>
#endif

namespace aurora {

// Global transport instance
std::unique_ptr<AuroraTransport> g_transport;

//============================================================================
// TCPTransport Implementation
//============================================================================

struct TCPTransport::Impl {
  TransportConfig config;
  std::mutex mutex;
  std::unordered_map<std::string, bool> connections;
  RecvCallback recv_callback;
  TransportStats stats{};
  bool running{false};
  
  // Send queue for async operations
  struct SendRequest {
    std::string remote_addr;
    std::vector<unsigned char> data;
    SendCallback callback;
  };
  std::queue<SendRequest> send_queue;
  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::thread sender_thread;
};

TCPTransport::TCPTransport() : impl_(std::make_unique<Impl>()) {}

TCPTransport::~TCPTransport() {
  shutdown();
}

bool TCPTransport::initialize(const TransportConfig& config) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->config = config;
  impl_->running = true;
  
  // Start async sender thread
  impl_->sender_thread = std::thread([this]() {
    while (impl_->running) {
      SendRequest req;
      {
        std::unique_lock<std::mutex> lock(impl_->queue_mutex);
        impl_->queue_cv.wait_for(lock, std::chrono::milliseconds(100),
                                  [this]() { return !impl_->send_queue.empty() || !impl_->running; });
        if (!impl_->running && impl_->send_queue.empty()) break;
        if (impl_->send_queue.empty()) continue;
        req = std::move(impl_->send_queue.front());
        impl_->send_queue.pop();
      }
      
      auto start = std::chrono::steady_clock::now();
      bool success = send(req.remote_addr, req.data.data(), req.data.size());
      auto end = std::chrono::steady_clock::now();
      int64_t latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
      
      if (req.callback) {
        req.callback(success, latency);
      }
    }
  });
  
  return true;
}

bool TCPTransport::connect(const std::string& remote_addr, int port) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  // In real implementation, this would create a gRPC channel
  std::string key = remote_addr + ":" + std::to_string(port);
  impl_->connections[key] = true;
  impl_->stats.active_connections++;
  
  return true;
}

void TCPTransport::disconnect(const std::string& remote_addr) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  auto it = impl_->connections.find(remote_addr);
  if (it != impl_->connections.end()) {
    impl_->connections.erase(it);
    impl_->stats.active_connections--;
  }
}

bool TCPTransport::is_connected(const std::string& remote_addr) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connections.find(remote_addr) != impl_->connections.end();
}

bool TCPTransport::send(const std::string& remote_addr,
                        const unsigned char* data, size_t len) {
  // In real implementation, this would use gRPC to send data
  // For now, simulate success
  
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->stats.bytes_sent += len;
  impl_->stats.messages_sent++;
  
  return true;
}

void TCPTransport::send_async(const std::string& remote_addr,
                              const unsigned char* data, size_t len,
                              SendCallback callback) {
  Impl::SendRequest req;
  req.remote_addr = remote_addr;
  req.data.assign(data, data + len);
  req.callback = callback;
  
  {
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    impl_->send_queue.push(std::move(req));
  }
  impl_->queue_cv.notify_one();
}

bool TCPTransport::recv(std::string* remote_addr,
                        unsigned char* buf, size_t* len, int timeout_ms) {
  // In real implementation, this would receive from gRPC
  return false;
}

void TCPTransport::set_recv_callback(RecvCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->recv_callback = callback;
}

bool TCPTransport::rdma_write(const std::string& remote_addr,
                              uint64_t remote_addr_ptr, uint32_t remote_rkey,
                              const unsigned char* local_data, size_t len) {
  // TCP transport doesn't support RDMA operations
  return false;
}

bool TCPTransport::rdma_read(const std::string& remote_addr,
                             uint64_t remote_addr_ptr, uint32_t remote_rkey,
                             unsigned char* local_buf, size_t len) {
  // TCP transport doesn't support RDMA operations
  return false;
}

MemoryRegion* TCPTransport::register_memory(void* addr, size_t len) {
  // TCP transport doesn't use memory registration
  return nullptr;
}

void TCPTransport::deregister_memory(MemoryRegion* mr) {
  // No-op for TCP
}

TransportStats TCPTransport::get_stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}

void TCPTransport::shutdown() {
  impl_->running = false;
  impl_->queue_cv.notify_all();
  
  if (impl_->sender_thread.joinable()) {
    impl_->sender_thread.join();
  }
  
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->connections.clear();
}

//============================================================================
// RDMATransport Implementation
//============================================================================

struct RDMATransport::Impl {
  TransportConfig config;
  std::mutex mutex;
  TransportStats stats{};
  bool initialized{false};
  bool running{false};
  
  // RDMA resources (in real implementation, these would be ibverbs structs)
  // struct ibv_context* context;
  // struct ibv_pd* pd;
  // struct ibv_cq* cq;
  // struct ibv_qp* qp;
  
  std::unordered_map<std::string, bool> connections;
  std::vector<MemoryRegion*> registered_regions;
};

RDMATransport::RDMATransport() : impl_(std::make_unique<Impl>()) {}

RDMATransport::~RDMATransport() {
  shutdown();
}

bool RDMATransport::initialize(const TransportConfig& config) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  impl_->config = config;
  
  // In real implementation:
  // 1. Open RDMA device (ibv_open_device)
  // 2. Allocate protection domain (ibv_alloc_pd)
  // 3. Create completion queue (ibv_create_cq)
  // 4. Create queue pair (ibv_create_qp)
  
  // Check if RDMA device is available
  if (!is_rdma_available()) {
    return false;
  }
  
  impl_->initialized = true;
  impl_->running = true;
  
  return true;
}

bool RDMATransport::connect(const std::string& remote_addr, int port) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!impl_->initialized) return false;
  
  // In real implementation:
  // 1. Exchange QP info with remote
  // 2. Transition QP to RTR (Ready to Receive)
  // 3. Transition QP to RTS (Ready to Send)
  
  std::string key = remote_addr + ":" + std::to_string(port);
  impl_->connections[key] = true;
  impl_->stats.active_connections++;
  
  return true;
}

void RDMATransport::disconnect(const std::string& remote_addr) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  auto it = impl_->connections.find(remote_addr);
  if (it != impl_->connections.end()) {
    impl_->connections.erase(it);
    impl_->stats.active_connections--;
  }
}

bool RDMATransport::is_connected(const std::string& remote_addr) const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connections.find(remote_addr) != impl_->connections.end();
}

bool RDMATransport::send(const std::string& remote_addr,
                         const unsigned char* data, size_t len) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!impl_->initialized) return false;
  
  // In real implementation:
  // 1. Post send work request (ibv_post_send)
  // 2. Poll completion queue (ibv_poll_cq)
  
  impl_->stats.bytes_sent += len;
  impl_->stats.messages_sent++;
  
  return true;
}

void RDMATransport::send_async(const std::string& remote_addr,
                               const unsigned char* data, size_t len,
                               SendCallback callback) {
  // In real RDMA, sends are naturally async
  auto start = std::chrono::steady_clock::now();
  bool success = send(remote_addr, data, len);
  auto end = std::chrono::steady_clock::now();
  int64_t latency = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
  
  if (callback) {
    callback(success, latency);
  }
}

bool RDMATransport::recv(std::string* remote_addr,
                         unsigned char* buf, size_t* len, int timeout_ms) {
  // In real implementation:
  // 1. Post receive work request (ibv_post_recv)
  // 2. Poll completion queue
  return false;
}

void RDMATransport::set_recv_callback(RecvCallback callback) {
  // In real implementation, start a polling thread
}

bool RDMATransport::rdma_write(const std::string& remote_addr,
                               uint64_t remote_addr_ptr, uint32_t remote_rkey,
                               const unsigned char* local_data, size_t len) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!impl_->initialized) return false;
  
  // In real implementation:
  // 1. Create RDMA Write work request with IBV_WR_RDMA_WRITE
  // 2. Set remote address and rkey
  // 3. Post send (ibv_post_send)
  // 4. Poll completion
  
  impl_->stats.bytes_sent += len;
  
  return true;
}

bool RDMATransport::rdma_read(const std::string& remote_addr,
                              uint64_t remote_addr_ptr, uint32_t remote_rkey,
                              unsigned char* local_buf, size_t len) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!impl_->initialized) return false;
  
  // In real implementation:
  // 1. Create RDMA Read work request with IBV_WR_RDMA_READ
  // 2. Set remote address and rkey
  // 3. Post send (ibv_post_send)
  // 4. Poll completion
  
  impl_->stats.bytes_received += len;
  
  return true;
}

MemoryRegion* RDMATransport::register_memory(void* addr, size_t len) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!impl_->initialized) return nullptr;
  
  // In real implementation:
  // struct ibv_mr* mr = ibv_reg_mr(pd, addr, len, 
  //     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
  
  MemoryRegion* mr = new MemoryRegion();
  mr->addr = reinterpret_cast<uintptr_t>(addr);
  mr->length = len;
  mr->lkey = 0;  // Would be from ibv_reg_mr
  mr->rkey = 0;  // Would be from ibv_reg_mr
  
  impl_->registered_regions.push_back(mr);
  
  return mr;
}

void RDMATransport::deregister_memory(MemoryRegion* mr) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  if (!mr) return;
  
  // In real implementation:
  // ibv_dereg_mr(mr->internal_mr);
  
  auto it = std::find(impl_->registered_regions.begin(),
                      impl_->registered_regions.end(), mr);
  if (it != impl_->registered_regions.end()) {
    impl_->registered_regions.erase(it);
  }
  
  delete mr;
}

TransportStats RDMATransport::get_stats() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->stats;
}

void RDMATransport::shutdown() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  
  impl_->running = false;
  
  // Deregister all memory regions
  for (auto mr : impl_->registered_regions) {
    delete mr;
  }
  impl_->registered_regions.clear();
  
  // In real implementation:
  // ibv_destroy_qp(qp);
  // ibv_destroy_cq(cq);
  // ibv_dealloc_pd(pd);
  // ibv_close_device(context);
  
  impl_->connections.clear();
  impl_->initialized = false;
}

//============================================================================
// Factory Functions
//============================================================================

std::unique_ptr<AuroraTransport> create_transport(TransportType type) {
  switch (type) {
    case TransportType::TCP_GRPC:
      return std::make_unique<TCPTransport>();
    case TransportType::RDMA_RC:
    case TransportType::RDMA_UD:
      return std::make_unique<RDMATransport>();
    default:
      return std::make_unique<TCPTransport>();
  }
}

std::unique_ptr<AuroraTransport> create_transport_from_config(const AuroraConfig& config) {
  TransportType type = TransportType::TCP_GRPC;
  
  if (config.transport_type == "rdma") {
    if (is_rdma_available()) {
      type = TransportType::RDMA_RC;
    } else if (config.tcp_fallback) {
      type = TransportType::TCP_GRPC;
    } else {
      return nullptr;
    }
  }
  
  auto transport = create_transport(type);
  
  TransportConfig tc;
  tc.type = type;
  tc.tcp.max_connections = config.grpc_max_connections;
  tc.tcp.connection_pool_size = config.grpc_connection_pool_size;
  tc.rdma.device_name = config.rdma_device;
  tc.rdma.max_qp_wr = 1024;
  tc.rdma.max_cq_entries = 1024;
  
  if (!transport->initialize(tc)) {
    return nullptr;
  }
  
  return transport;
}

bool is_rdma_available() {
#ifdef __linux__
  // Check if InfiniBand devices exist
  struct stat st;
  if (stat("/sys/class/infiniband", &st) == 0 && S_ISDIR(st.st_mode)) {
    // Further check for available devices
    // In real implementation, use ibv_get_device_list()
    return true;
  }
#endif
  return false;
}

bool aurora_transport_init(const AuroraConfig& config) {
  g_transport = create_transport_from_config(config);
  return g_transport != nullptr;
}

void aurora_transport_shutdown() {
  if (g_transport) {
    g_transport->shutdown();
    g_transport.reset();
  }
}

}  // namespace aurora
