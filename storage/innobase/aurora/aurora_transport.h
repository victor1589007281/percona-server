/*****************************************************************************
Copyright (c) 2025, Aurora Project

Aurora Transport Abstraction
Unified interface for TCP/gRPC and RDMA transport.

Reference: 01_compute_layer.md Section 13

*****************************************************************************/

#ifndef AURORA_TRANSPORT_H
#define AURORA_TRANSPORT_H

#include <cstdint>
#include <memory>
#include <string>
#include <functional>

namespace aurora {

// Forward declaration
struct AuroraConfig;

/**
 * Transport Type
 */
enum class TransportType {
  TCP_GRPC,   // Standard gRPC over TCP/IP
  RDMA_RC,    // RDMA Reliable Connection
  RDMA_UD     // RDMA Unreliable Datagram
};

/**
 * Transport Configuration
 */
struct TransportConfig {
  TransportType type;
  std::string local_addr;
  int local_port;
  
  // TCP/gRPC settings
  struct {
    int max_connections;
    int keepalive_time_ms;
    int max_message_size;
    int connection_pool_size;
  } tcp;
  
  // RDMA settings
  struct {
    std::string device_name;  // e.g., "mlx5_0"
    int port_num;
    int gid_index;
    int max_qp_wr;
    int max_cq_entries;
    int max_send_sge;
    int max_recv_sge;
    size_t inline_size;
    bool use_srq;
  } rdma;
};

/**
 * Send completion callback
 */
using SendCallback = std::function<void(bool success, int64_t latency_us)>;

/**
 * Receive callback
 */
using RecvCallback = std::function<void(const std::string& remote_addr,
                                         const unsigned char* data,
                                         size_t len)>;

/**
 * Memory Region (for RDMA)
 */
struct MemoryRegion {
  uintptr_t addr;
  size_t length;
  uint32_t lkey;
  uint32_t rkey;
};

/**
 * Transport Statistics
 */
struct TransportStats {
  uint64_t bytes_sent;
  uint64_t bytes_received;
  uint64_t messages_sent;
  uint64_t messages_received;
  uint64_t errors;
  int active_connections;
  double avg_latency_us;
  double p99_latency_us;
};

/**
 * Abstract Transport Interface
 */
class AuroraTransport {
public:
  virtual ~AuroraTransport() = default;

  /**
   * Initialize the transport
   */
  virtual bool initialize(const TransportConfig& config) = 0;

  /**
   * Connect to remote address
   */
  virtual bool connect(const std::string& remote_addr, int port) = 0;

  /**
   * Disconnect from remote address
   */
  virtual void disconnect(const std::string& remote_addr) = 0;

  /**
   * Check if connected
   */
  virtual bool is_connected(const std::string& remote_addr) const = 0;

  /**
   * Send data synchronously
   */
  virtual bool send(const std::string& remote_addr,
                    const unsigned char* data,
                    size_t len) = 0;

  /**
   * Send data asynchronously
   */
  virtual void send_async(const std::string& remote_addr,
                          const unsigned char* data,
                          size_t len,
                          SendCallback callback) = 0;

  /**
   * Receive data (blocking)
   */
  virtual bool recv(std::string* remote_addr,
                    unsigned char* buf,
                    size_t* len,
                    int timeout_ms) = 0;

  /**
   * Register receive callback
   */
  virtual void set_recv_callback(RecvCallback callback) = 0;

  /**
   * RDMA Write (one-sided operation)
   * Only works in RDMA mode
   */
  virtual bool rdma_write(const std::string& remote_addr,
                          uint64_t remote_addr_ptr,
                          uint32_t remote_rkey,
                          const unsigned char* local_data,
                          size_t len) = 0;

  /**
   * RDMA Read (one-sided operation)
   * Only works in RDMA mode
   */
  virtual bool rdma_read(const std::string& remote_addr,
                         uint64_t remote_addr_ptr,
                         uint32_t remote_rkey,
                         unsigned char* local_buf,
                         size_t len) = 0;

  /**
   * Register memory for RDMA
   */
  virtual MemoryRegion* register_memory(void* addr, size_t len) = 0;

  /**
   * Deregister memory
   */
  virtual void deregister_memory(MemoryRegion* mr) = 0;

  /**
   * Get transport type
   */
  virtual TransportType get_type() const = 0;

  /**
   * Get transport name
   */
  virtual const char* get_name() const = 0;

  /**
   * Get statistics
   */
  virtual TransportStats get_stats() const = 0;

  /**
   * Shutdown transport
   */
  virtual void shutdown() = 0;
};

/**
 * TCP/gRPC Transport Implementation
 */
class TCPTransport : public AuroraTransport {
public:
  TCPTransport();
  ~TCPTransport() override;

  bool initialize(const TransportConfig& config) override;
  bool connect(const std::string& remote_addr, int port) override;
  void disconnect(const std::string& remote_addr) override;
  bool is_connected(const std::string& remote_addr) const override;
  bool send(const std::string& remote_addr,
            const unsigned char* data, size_t len) override;
  void send_async(const std::string& remote_addr,
                  const unsigned char* data, size_t len,
                  SendCallback callback) override;
  bool recv(std::string* remote_addr,
            unsigned char* buf, size_t* len, int timeout_ms) override;
  void set_recv_callback(RecvCallback callback) override;
  
  // RDMA operations not supported
  bool rdma_write(const std::string& remote_addr,
                  uint64_t remote_addr_ptr, uint32_t remote_rkey,
                  const unsigned char* local_data, size_t len) override;
  bool rdma_read(const std::string& remote_addr,
                 uint64_t remote_addr_ptr, uint32_t remote_rkey,
                 unsigned char* local_buf, size_t len) override;
  MemoryRegion* register_memory(void* addr, size_t len) override;
  void deregister_memory(MemoryRegion* mr) override;
  
  TransportType get_type() const override { return TransportType::TCP_GRPC; }
  const char* get_name() const override { return "TCP/gRPC"; }
  TransportStats get_stats() const override;
  void shutdown() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * RDMA Transport Implementation (placeholder)
 * Real implementation would use libibverbs
 */
class RDMATransport : public AuroraTransport {
public:
  RDMATransport();
  ~RDMATransport() override;

  bool initialize(const TransportConfig& config) override;
  bool connect(const std::string& remote_addr, int port) override;
  void disconnect(const std::string& remote_addr) override;
  bool is_connected(const std::string& remote_addr) const override;
  bool send(const std::string& remote_addr,
            const unsigned char* data, size_t len) override;
  void send_async(const std::string& remote_addr,
                  const unsigned char* data, size_t len,
                  SendCallback callback) override;
  bool recv(std::string* remote_addr,
            unsigned char* buf, size_t* len, int timeout_ms) override;
  void set_recv_callback(RecvCallback callback) override;
  bool rdma_write(const std::string& remote_addr,
                  uint64_t remote_addr_ptr, uint32_t remote_rkey,
                  const unsigned char* local_data, size_t len) override;
  bool rdma_read(const std::string& remote_addr,
                 uint64_t remote_addr_ptr, uint32_t remote_rkey,
                 unsigned char* local_buf, size_t len) override;
  MemoryRegion* register_memory(void* addr, size_t len) override;
  void deregister_memory(MemoryRegion* mr) override;
  
  TransportType get_type() const override { return TransportType::RDMA_RC; }
  const char* get_name() const override { return "RDMA"; }
  TransportStats get_stats() const override;
  void shutdown() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/**
 * Create transport based on type
 */
std::unique_ptr<AuroraTransport> create_transport(TransportType type);

/**
 * Create transport based on configuration
 */
std::unique_ptr<AuroraTransport> create_transport_from_config(
    const AuroraConfig& config);

/**
 * Check if RDMA is available on this system
 */
bool is_rdma_available();

/**
 * Global transport instance
 */
extern std::unique_ptr<AuroraTransport> g_transport;

/**
 * Initialize transport
 */
bool aurora_transport_init(const AuroraConfig& config);

/**
 * Shutdown transport
 */
void aurora_transport_shutdown();

}  // namespace aurora

#endif  // AURORA_TRANSPORT_H
