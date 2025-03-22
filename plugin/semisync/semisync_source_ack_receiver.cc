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

#include "plugin/semisync/semisync_source_ack_receiver.h"

#include "my_config.h"

#include <errno.h>

#include "my_compiler.h"
#include "my_dbug.h"
#include "my_psi_config.h"
#include "mysql/psi/mysql_stage.h"
#include "mysqld_error.h"
#include "plugin/semisync/semisync.h"
#include "plugin/semisync/semisync_source.h"
#include "plugin/semisync/semisync_source_socket_listener.h"
#include "sql/protocol_classic.h"
#include "sql/sql_class.h"

extern ReplSemiSyncMaster *repl_semisync;

#ifdef HAVE_PSI_INTERFACE
extern PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica;
extern PSI_stage_info stage_waiting_for_semi_sync_replica;
extern PSI_stage_info stage_reading_semi_sync_ack;
extern PSI_mutex_key key_ss_mutex_Ack_receiver_mutex;
extern PSI_cond_key key_ss_cond_Ack_receiver_cond;
extern PSI_thread_key key_ss_thread_Ack_receiver_thread;
#endif

/* Callback function of ack receive thread */
extern "C" {
static void *ack_receive_handler(void *arg) {
  my_thread_init();
  reinterpret_cast<Ack_receiver *>(arg)->run();
  my_thread_end();
  my_thread_exit(nullptr);
  return nullptr;
}
}  // extern "C"

Ack_receiver::Ack_receiver() {
  const char *kWho = "Ack_receiver::Ack_receiver";
  function_enter(kWho);

  m_status = ST_DOWN;
  mysql_mutex_init(key_ss_mutex_Ack_receiver_mutex, &m_mutex,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_ss_cond_Ack_receiver_cond, &m_cond);

  function_exit(kWho);
}

Ack_receiver::~Ack_receiver() {
  const char *kWho = "Ack_receiver::~Ack_receiver";
  function_enter(kWho);

  stop();
  mysql_mutex_destroy(&m_mutex);
  mysql_cond_destroy(&m_cond);

  function_exit(kWho);
}

/**
 * @brief Starts the ACK receive thread.
 *        启动 ACK 接收线程。
 *
 * This function initializes and starts the thread responsible for receiving
 * acknowledgments (ACKs) from slaves. If the thread creation fails, it logs
 * an error and returns a failure status.
 * 
 * 该函数初始化并启动负责接收从库确认（ACK）的线程。如果线程创建失败，
 * 则记录错误日志并返回失败状态。
 *
 * @return `false` on success, `true` on failure.
 *         成功时返回 `false`，失败时返回 `true`。
 */
bool Ack_receiver::start() {
  const char *kWho = "Ack_receiver::start";  // 函数标识符，用于日志记录。
  function_enter(kWho);  // 记录函数进入日志。

  if (m_status == ST_DOWN) {  // 如果当前状态为 ST_DOWN，则启动线程。
    my_thread_attr_t attr;

    m_status = ST_UP;  // 将状态设置为 ST_UP。

    // 初始化线程属性并创建线程。
    if (DBUG_EVALUATE_IF("rpl_semisync_simulate_create_thread_failure", 1, 0) ||
        my_thread_attr_init(&attr) != 0 ||  // 初始化线程属性。
        my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE) != 0 ||  // 设置线程为可连接状态。
#ifndef _WIN32
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) != 0 ||  // 设置线程范围（非 Windows 系统）。
#endif
        mysql_thread_create(key_ss_thread_Ack_receiver_thread, &m_pid, &attr,
                            ack_receive_handler, this)) {  // 创建线程。
      // 如果线程创建失败，记录错误日志并将状态重置为 ST_DOWN。
      LogErr(ERROR_LEVEL, ER_SEMISYNC_FAILED_TO_START_ACK_RECEIVER_THD, errno);

      m_status = ST_DOWN;
      return function_exit(kWho, true);  // 返回失败状态。
    }
    (void)my_thread_attr_destroy(&attr);  // 销毁线程属性。
  }
  return function_exit(kWho, false);  // 返回成功状态。
}

/**
 * @brief Stops the ACK receive thread.
 *        停止 ACK 接收线程。
 *
 * This function stops the thread responsible for receiving acknowledgments
 * from slaves. It ensures that the thread is properly joined and cleaned up.
 * 
 * 该函数停止负责接收从库确认的线程。确保线程被正确地连接和清理。
 */
void Ack_receiver::stop() {
  const char *kWho = "Ack_receiver::stop";  // 函数标识符，用于日志记录。
  function_enter(kWho);  // 记录函数进入日志。
  int ret;

  if (m_status == ST_UP) {  // 如果当前状态为 ST_UP，则停止线程。
    mysql_mutex_lock(&m_mutex);  // 加锁以保护状态变量。
    m_status = ST_STOPPING;  // 将状态设置为 ST_STOPPING。
    mysql_cond_broadcast(&m_cond);  // 广播信号，通知线程停止。

    // 等待线程状态变为 ST_DOWN。
    while (m_status == ST_STOPPING) mysql_cond_wait(&m_cond, &m_mutex);
    mysql_mutex_unlock(&m_mutex);  // 解锁。

    /*
      When arriving here, the ack thread already exists. Join failure has no
      side effect against semisync. So we don't return an error.
      到达此处时，ACK 线程已经存在。线程连接失败不会对半同步复制产生影响，
      因此不会返回错误。
    */
    ret = my_thread_join(&m_pid, nullptr);  // 回收ACK线程的资源
    if (DBUG_EVALUATE_IF("rpl_semisync_simulate_thread_join_failure", -1, ret))
      LogErr(ERROR_LEVEL, ER_SEMISYNC_FAILED_TO_STOP_ACK_RECEIVER_THD, errno);  // 如果连接失败，记录错误日志。
  }
  function_exit(kWho);  // 记录函数退出日志。
}

/**
 * @brief Adds a new slave to the list of semi-synchronous replication slaves.
 *        将一个新的从库添加到半同步复制从库列表中。
 *
 * This function initializes a `Slave` object with the provided thread's
 * information (e.g., thread ID, server ID, compression settings, and VIO).
 * It then adds the `Slave` object to the internal list of slaves (`m_slaves`).
 * 
 * 该函数使用提供的线程信息（如线程 ID、服务器 ID、压缩设置和 VIO）
 * 初始化一个 `Slave` 对象，并将其添加到内部从库列表（`m_slaves`）中。
 *
 * @param thd The thread object representing the slave connection.
 *            表示从库连接的线程对象。
 *
 * @return `false` on success, `true` on failure.
 *         成功时返回 `false`，失败时返回 `true`。
 */
bool Ack_receiver::add_slave(THD *thd) {
  Slave slave;  // 定义一个从库对象。
  const char *kWho = "Ack_receiver::add_slave";  // 函数标识符，用于日志记录。
  function_enter(kWho);  // 记录函数进入日志。

  // 初始化从库对象的基本信息。
  slave.thread_id = thd->thread_id();  // 设置线程 ID。
  slave.server_id = thd->server_id;    // 设置服务器 ID。
  slave.compress_ctx.algorithm = enum_compression_algorithm::MYSQL_UNCOMPRESSED;  // 默认不启用压缩。

  // 获取从库的压缩算法名称。
  char *cmp_algorithm_name = thd->get_protocol()->get_compression_algorithm();
  if (cmp_algorithm_name != nullptr) {
    // 根据压缩算法名称获取对应的枚举值。
    enum enum_compression_algorithm algorithm =
        get_compression_algorithm(cmp_algorithm_name);
    if (algorithm != enum_compression_algorithm::MYSQL_UNCOMPRESSED &&
        algorithm != enum_compression_algorithm::MYSQL_INVALID) {
      // 如果压缩算法有效，则初始化压缩上下文。
      mysql_compress_context_init(
          &slave.compress_ctx, algorithm,
          thd->get_protocol_classic()->get_compression_level());
    }
  }

  // 设置从库的 VIO（虚拟 I/O）对象。
  slave.vio = thd->get_protocol_classic()->get_vio();
  slave.vio->mysql_socket.m_psi = nullptr;  // 清除 PSI（性能架构接口）信息。

  // 将从库对象添加到从库列表中。
  /* push_back() may throw an exception */  
  try {
    mysql_mutex_lock(&m_mutex);  // 加锁以确保线程安全。

    // 调试点：模拟添加从库失败的场景。
    DBUG_EXECUTE_IF("rpl_semisync_simulate_add_replica_failure", throw 1;);

    m_slaves.push_back(slave);  // 将从库对象添加到从库列表。
    m_slaves_changed = true;    // 标记从库列表已更改。
    mysql_cond_broadcast(&m_cond);  // 广播信号，通知其他线程从库列表已更新。
    mysql_mutex_unlock(&m_mutex);  // 解锁。
  } catch (...) {
    // 如果发生异常，解锁并返回失败。
    mysql_mutex_unlock(&m_mutex);
    return function_exit(kWho, true);  // 记录函数退出日志并返回失败。
  }

  return function_exit(kWho, false);  // 记录函数退出日志并返回成功。
}

void Ack_receiver::remove_slave(THD *thd) {
  const char *kWho = "Ack_receiver::remove_slave";
  function_enter(kWho);

  mysql_mutex_lock(&m_mutex);
  Slave_vector_it it;

  /*
    Mark in the slave object that remove slave request
    is received. And also inform to Ack_receiver::run()
    that slaves vector is changed.
  */
  for (it = m_slaves.begin(); it != m_slaves.end(); ++it) {
    if (it->thread_id == thd->thread_id()) {
      it->m_status = Slave::EnumStatus::leaving;
      m_slaves_changed = true;
      break;
    }
  }
  assert(it != m_slaves.end());
  /*
    Wait till Ack_receiver::run() is done reading from the
    slave's socket.
  */
  while ((it != m_slaves.end()) &&
         (it->m_status == Slave::EnumStatus::leaving) && (m_status == ST_UP)) {
    mysql_cond_wait(&m_cond, &m_mutex);
    /*
      In above cond_wait, we release and reacquire m_mutex.
      So it can happen that slave vector is changed.
      So rescan the vector to get the correct slave
      object.
    */
    for (it = m_slaves.begin(); it != m_slaves.end(); ++it) {
      if (it->thread_id == thd->thread_id()) break;
    }
  }
  if (it != m_slaves.end()) {
    mysql_compress_context_deinit(&it->compress_ctx);
    m_slaves.erase(it);
  }

  m_slaves_changed = true;
  mysql_mutex_unlock(&m_mutex);
  function_exit(kWho);
}

inline void Ack_receiver::set_stage_info(const PSI_stage_info &stage
                                         [[maybe_unused]]) {
#ifdef HAVE_PSI_STAGE_INTERFACE
  MYSQL_SET_STAGE(stage.m_key, __FILE__, __LINE__);
#endif /* HAVE_PSI_STAGE_INTERFACE */
}

inline void Ack_receiver::wait_for_slave_connection() {
  set_stage_info(stage_waiting_for_semi_sync_replica);
  mysql_cond_wait(&m_cond, &m_mutex);
}

/* Auxiliary function to initialize a NET object with given net buffer. */
static void init_net(NET *net, unsigned char *buff, unsigned int buff_len) {
  memset(net, 0, sizeof(NET));
  net->max_packet = buff_len;
  net->buff = buff;
  net->buff_end = buff + buff_len;
  net->read_pos = net->buff;
}

void Ack_receiver::run() {
  NET net;
  unsigned char net_buff[REPLY_MESSAGE_MAX_LENGTH];
  uint i;
  Socket_listener listener;

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_STARTING_ACK_RECEIVER_THD);

  init_net(&net, net_buff, REPLY_MESSAGE_MAX_LENGTH);
  NET_SERVER server_extn;
  net_server_ext_init(&server_extn);
  net.extension = &server_extn;

  mysql_mutex_lock(&m_mutex);
  m_slaves_changed = true;
  mysql_mutex_unlock(&m_mutex);

  while (true) {
    int ret;

    mysql_mutex_lock(&m_mutex);
    if (unlikely(m_status == ST_STOPPING)) goto end;

    set_stage_info(stage_waiting_for_semi_sync_ack_from_replica);
    if (unlikely(m_slaves_changed)) {
      if (unlikely(m_slaves.empty())) {
        wait_for_slave_connection();
        mysql_mutex_unlock(&m_mutex);
        continue;
      }
      if (!listener.init_replica_sockets(m_slaves)) goto end;
      m_slaves_changed = false;
      mysql_cond_broadcast(&m_cond);
    }
    mysql_mutex_unlock(&m_mutex);
    ret = listener.listen_on_sockets();
    if (ret <= 0) {
      ret = DBUG_EVALUATE_IF("rpl_semisync_simulate_select_error", -1, ret);

      if (ret == -1 && errno != EINTR)
        LogErr(INFORMATION_LEVEL, ER_SEMISYNC_FAILED_TO_WAIT_ON_DUMP_SOCKET,
               socket_errno);
      /* Sleep 1us, so other threads can catch the m_mutex easily. */
      my_sleep(1);
      continue;
    }

    set_stage_info(stage_reading_semi_sync_ack);
    i = 0;
    while (i < listener.number_of_slave_sockets() && m_status == ST_UP) {
      if (listener.is_socket_active(i)) {
        Slave slave_obj = listener.get_slave_obj(i);
        ulong len;
        net.vio = slave_obj.vio;
        /*
          Set compress flag. This is needed to support
          Slave_compress_protocol flag enabled Slaves
        */

        NET_SERVER *server_extension = static_cast<NET_SERVER *>(net.extension);
        server_extension->compress_ctx = slave_obj.compress_ctx;
        net.compress =
            (server_extension->compress_ctx.algorithm == MYSQL_ZLIB) ||
            (server_extension->compress_ctx.algorithm == MYSQL_ZSTD);

        do {
          net_clear(&net, false);

          len = my_net_read(&net);
          if (likely(len != packet_error))
            repl_semisync->reportReplyPacket(slave_obj.server_id, net.read_pos,
                                             len);
          else if (net.last_errno == ER_NET_READ_ERROR) {
            listener.clear_socket_info(i);
          }
        } while (net.vio->has_data(net.vio) && m_status == ST_UP);
      }
      i++;
    }
  }
end:
  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_STOPPING_ACK_RECEIVER_THREAD);
  m_status = ST_DOWN;
  mysql_cond_broadcast(&m_cond);
  mysql_mutex_unlock(&m_mutex);
}
