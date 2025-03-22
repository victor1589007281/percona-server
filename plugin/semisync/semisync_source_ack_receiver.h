/* Copyright (c) 2014, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef SEMISYNC_SOURCE_ACK_RECEIVER_DEFINED
#define SEMISYNC_SOURCE_ACK_RECEIVER_DEFINED

#include <sys/types.h>
#include <vector>

#include "my_inttypes.h"
#include "my_io.h"
#include "my_thread.h"
#include "plugin/semisync/semisync.h"
#include "plugin/semisync/semisync_source.h"
#include "sql/sql_class.h"

struct Slave {
  enum class EnumStatus { up, leaving, down };
  uint32_t thread_id;
  Vio *vio;
  uint server_id;
  mysql_compress_context compress_ctx;
  EnumStatus m_status = EnumStatus::up;

  my_socket sock_fd() const { return vio->mysql_socket.fd; }
};

typedef std::vector<Slave> Slave_vector;
typedef Slave_vector::iterator Slave_vector_it;

/**
 * @class Ack_receiver
 * 
 * @brief Manages the acknowledgment (ACK) receive thread and maintains slave information.
 *        管理 ACK 接收线程并维护从库信息。
 *
 * This class is responsible for controlling the ACK receive thread and maintaining
 * the list of semi-synchronous replication slaves. It provides methods to start and
 * stop the ACK receive thread, as well as to add or remove slave information.
 * 
 * 该类负责控制 ACK 接收线程并维护半同步复制从库的列表。提供启动和停止 ACK 接收线程的方法，
 * 以及添加或移除从库信息的功能。
 *
 * Main operations:
 * - `start`: Starts the ACK receive thread.
 * - `stop`: Stops the ACK receive thread.
 * - `add_slave`: Adds a new semi-synchronous slave's information.
 * - `remove_slave`: Removes a semi-synchronous slave's information.
 */
class Ack_receiver : public ReplSemiSyncBase {
 public:
  Ack_receiver();  // 构造函数，初始化 ACK 接收器。
  ~Ack_receiver(); // 析构函数，清理资源。

  /**
   * @brief Adds a new slave to the list of semi-synchronous replication slaves.
   *        将一个新的从库添加到半同步复制从库列表中。
   *
   * This method adds the given dump thread to the slave list and wakes up the
   * ACK thread if it is waiting for a slave to connect.
   * 
   * 该方法将提供的 dump 线程添加到从库列表中，并唤醒正在等待从库连接的 ACK 线程。
   *
   * @param[in] thd  The thread object representing the dump thread.
   *                 表示 dump 线程的线程对象。
   *
   * @return `false` on success, `true` on failure.
   *         成功时返回 `false`，失败时返回 `true`。
   */
  bool add_slave(THD *thd);

  /**
   * @brief Removes a slave from the list of semi-synchronous replication slaves.
   *        从半同步复制从库列表中移除一个从库。
   *
   * This method removes the given dump thread from the slave list.
   * 
   * 该方法从从库列表中移除提供的 dump 线程。
   *
   * @param[in] thd  The thread object representing the dump thread.
   *                 表示 dump 线程的线程对象。
   */
  void remove_slave(THD *thd);

  /**
   * @brief Starts the ACK receive thread.
   *        启动 ACK 接收线程。
   *
   * This method initializes and starts the thread responsible for receiving
   * acknowledgments from slaves.
   * 
   * 该方法初始化并启动负责接收从库确认的线程。
   *
   * @return `false` on success, `true` on failure.
   *         成功时返回 `false`，失败时返回 `true`。
   */
  bool start();

  /**
   * @brief Stops the ACK receive thread.
   *        停止 ACK 接收线程。
   *
   * This method stops the thread responsible for receiving acknowledgments
   * from slaves.
   * 
   * 该方法停止负责接收从库确认的线程。
   */
  void stop();

  /**
   * @brief The core logic of the ACK receive thread.
   *        ACK 接收线程的核心逻辑。
   *
   * This method monitors all slave sockets and processes acknowledgments
   * when they are received.
   * 
   * 该方法监控所有从库的套接字，并在接收到确认时处理它们。
   */
  void run();

  /**
   * @brief Sets the trace level for debugging.
   *        设置调试的跟踪级别。
   *
   * @param trace_level  The trace level to set.
   *                     要设置的跟踪级别。
   */
  void setTraceLevel(unsigned long trace_level) { trace_level_ = trace_level; }

  /**
   * @brief Initializes the ACK receiver.
   *        初始化 ACK 接收器。
   *
   * This method sets the trace level and starts the ACK receive thread if
   * semi-synchronous replication is enabled.
   * 
   * 该方法设置跟踪级别，并在启用半同步复制时启动 ACK 接收线程。
   *
   * @return `false` if initialization fails, otherwise `true`.
   *         初始化失败时返回 `false`，否则返回 `true`。
   */
  bool init() {
    setTraceLevel(rpl_semi_sync_source_trace_level);
    if (rpl_semi_sync_source_enabled) return start();
    return false;
  }

 private:
  enum status { ST_UP, ST_DOWN, ST_STOPPING };  // ACK 接收线程的状态枚举。
  uint8 m_status;  // 当前状态。

  /**
   * Protects `m_status`, `m_slaves_changed`, and `m_slaves`.
   * Used to ensure thread safety when accessing these variables.
   * 
   * 保护 `m_status`、`m_slaves_changed` 和 `m_slaves`。
   * 用于确保访问这些变量时的线程安全。
   */
  mysql_mutex_t m_mutex;

  /**
   * Condition variable used to signal changes in the slave list.
   * 
   * 条件变量，用于通知从库列表的变化。
   */
  mysql_cond_t m_cond;

  /**
   * Indicates whether the slave list has been updated (added or removed).
   * 
   * 指示从库列表是否已更新（添加或移除）。
   */
  bool m_slaves_changed;

  /**
   * A vector containing the list of semi-synchronous replication slaves.
   * 
   * 包含半同步复制从库列表的向量。
   */
  Slave_vector m_slaves;

  /**
   * The thread handle for the ACK receive thread.
   * 
   * ACK 接收线程的线程句柄。
   */
  my_thread_handle m_pid;

  /**
   * Prevents copying of the `Ack_receiver` object.
   * 
   * 禁止拷贝 `Ack_receiver` 对象。
   */
  Ack_receiver(const Ack_receiver &ack_receiver);
  Ack_receiver &operator=(const Ack_receiver &ack_receiver);

  /**
   * Sets the stage information for performance schema instrumentation.
   * 
   * 设置性能架构工具的阶段信息。
   */
  void set_stage_info(const PSI_stage_info &stage);

  /**
   * Waits for a slave to connect.
   * 
   * 等待从库连接。
   */
  void wait_for_slave_connection();
};

extern Ack_receiver *ack_receiver;  // 全局 ACK 接收器实例。
#endif
