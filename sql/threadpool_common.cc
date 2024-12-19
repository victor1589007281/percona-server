/* Copyright (C) 2012 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
   USA */

#include "my_thread_local.h"
#include "mysql/psi/mysql_idle.h"
#include "mysql/psi/mysql_socket.h"
#include "mysql/thread_pool_priv.h"
#include "sql/conn_handler/channel_info.h"
#include "sql/conn_handler/connection_handler_manager.h"
#include "sql/debug_sync.h"
#include "sql/mysqld.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/sql_audit.h"
#include "sql/sql_class.h"
#include "sql/sql_connect.h"
#include "sql/protocol_classic.h"
#include "sql/sql_parse.h"
#include "sql/threadpool.h"
#include "violite.h"

/* Threadpool parameters */

uint threadpool_min_threads;
uint threadpool_idle_timeout;
uint threadpool_size;
uint threadpool_stall_limit;
uint threadpool_max_threads;
uint threadpool_oversubscribe;

/* Stats */
TP_STATISTICS tp_stats;

/*
  Worker threads contexts, and THD contexts.
  =========================================

  Both worker threads and connections have their sets of thread local variables
  At the moment it is mysys_var (this has specific data for dbug, my_error and
  similar goodies), and PSI per-client structure.

  Whenever query is executed following needs to be done:

  1. Save worker thread context.
  2. Change TLS variables to connection specific ones using thread_attach(THD*).
     This function does some additional work.
  3. Process query
  4. Restore worker thread context.

  Connection login and termination follows similar schema w.r.t saving and
  restoring contexts.

  For both worker thread, and for the connection, mysys variables are created
  using my_thread_init() and freed with my_thread_end().

*/
class Worker_thread_context {
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_thread *const psi_thread;
#endif
#ifndef NDEBUG
  const my_thread_id thread_id;
#endif
 public:
  Worker_thread_context() noexcept
      :
#ifdef HAVE_PSI_THREAD_INTERFACE
        psi_thread(PSI_THREAD_CALL(get_thread)())
#endif
#ifndef NDEBUG
        ,
        thread_id(my_thread_var_id())
#endif
  {
  }

  ~Worker_thread_context() noexcept {
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_THREAD_CALL(set_thread)(psi_thread);
#endif
#ifndef NDEBUG
    set_my_thread_var_id(thread_id);
#endif
    THR_MALLOC = nullptr;
  }
};

/*
  Attach/associate the connection with the OS thread,
*/
static bool thread_attach(THD *thd) {
#ifndef NDEBUG
  set_my_thread_var_id(thd->thread_id());
#endif
  thd->thread_stack = (char *)&thd;
  thd->store_globals();
#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(set_thread)(thd->get_psi());
#endif
  mysql_socket_set_thread_owner(
      thd->get_protocol_classic()->get_vio()->mysql_socket);
  return 0;
}

#ifdef HAVE_PSI_STATEMENT_INTERFACE
extern PSI_statement_info stmt_info_new_packet;
#endif

static void threadpool_net_before_header_psi_noop(NET * /* net */,
                                                  void * /* user_data */,
                                                  size_t /* count */) {}

static void threadpool_init_net_server_extension(THD *thd) {
#ifdef HAVE_PSI_INTERFACE
  // socket_connection.cc:init_net_server_extension should have been called
  // already for us. We only need to overwrite the "before" callback
  assert(thd->m_net_server_extension.m_user_data == thd);
  thd->m_net_server_extension.m_before_header =
      threadpool_net_before_header_psi_noop;
#else
  assert(thd->get_protocol_classic()->get_net()->extension == NULL);
#endif
}

int threadpool_add_connection(THD *thd) {
  int retval = 1;
  Worker_thread_context worker_context;

  my_thread_init();

  /* Create new PSI thread for use with the THD. */
#ifdef HAVE_PSI_THREAD_INTERFACE
  thd->set_psi(PSI_THREAD_CALL(new_thread)(key_thread_one_connection, 0, thd,
                                           thd->thread_id()));
#endif

  /* Login. */
  thread_attach(thd);
  thd->start_utime = my_micro_time();
  thd->store_globals();

  if (thd_prepare_connection(thd)) {
    goto end;
  }

  /*
    Check if THD is ok, as prepare_new_connection_state()
    can fail, for example if init command failed.
  */
  if (thd_connection_alive(thd)) {
    retval = 0;
    thd_set_net_read_write(thd, 1);
    thd->skip_wait_timeout = true;
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE);
    thd->m_server_idle = true;
    threadpool_init_net_server_extension(thd);
  }

end:
  if (retval) {
    Connection_handler_manager *handler_manager =
        Connection_handler_manager::get_instance();
    handler_manager->inc_aborted_connects();
  }
  return retval;
}

void threadpool_remove_connection(THD *thd) {
  Worker_thread_context worker_context;

  thread_attach(thd);
  thd_set_net_read_write(thd, 0);

  end_connection(thd);
  close_connection(thd, 0);

  thd->release_resources();

#ifdef HAVE_PSI_THREAD_INTERFACE
  PSI_THREAD_CALL(delete_thread)(thd->get_psi());
#endif

  Global_THD_manager::get_instance()->remove_thd(thd);
  Connection_handler_manager::dec_connection_count();
  delete thd;
}

/**
 Process a single client request or a single batch.
 处理单个客户端请求或单个批处理。
*/
int threadpool_process_request(THD *thd) { // thd: 线程上下文
  int retval = 0; // 返回值，初始为0
  Worker_thread_context worker_context; // 创建工作线程上下文

  thread_attach(thd); // 附加线程

  if (thd->killed == THD::KILL_CONNECTION) { // 检查是否被杀死
    /*
      killed 标志由超时处理程序或 KILL 命令设置。返回错误。
    */
    retval = 1; // 设置返回值为1
    goto end; // 跳转到结束部分
  }

  /*
    In the loop below, the flow is essentially the copy of thead-per-connections
    logic, see do_handle_one_connection() in sql_connect.c
    在下面的循环中，流程基本上是线程每连接逻辑的副本，
    参见 sql_connect.c 中的 do_handle_one_connection()

    The goal is to execute a single query, thus the loop is normally executed
    only once. However for SSL connections, it can be executed multiple times
    (SSL can preread and cache incoming data, and vio->has_data() checks if it
    was the case).
    目标是执行单个查询，因此循环通常只执行一次。
    但是对于 SSL 连接，它可以多次执行
    （SSL 可以预读取和缓存传入数据，vio->has_data() 检查是否发生这种情况）。
  */
  for (;;) { // 无限循环
    Vio *vio; // Vio 指针
    thd_set_net_read_write(thd, 0); // 设置网络读写状态为0

    if ((retval = do_command(thd)) != 0) goto end; // 执行命令并检查返回值

    if (!thd_connection_alive(thd)) { // 检查连接是否存活
      retval = 1; // 设置返回值为1
      goto end; // 跳转到结束部分
    }

    vio = thd->get_protocol_classic()->get_vio(); // 获取 Vio 对象
    if (!vio->has_data(vio)) { // 检查是否有数据
      /* 更多关于此调试同步的信息在 sql_parse.cc 中 */
      DEBUG_SYNC(thd, "before_do_command_net_read"); // 调试同步
      thd_set_net_read_write(thd, 1); // 设置网络读写状态为1
      goto end; // 跳转到结束部分
    }
    if (!thd->m_server_idle) { // 检查服务器是否空闲
      MYSQL_SOCKET_SET_STATE(vio->mysql_socket, PSI_SOCKET_STATE_IDLE); // 设置套接字状态为空闲
      MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state); // 开始空闲等待
      thd->m_server_idle = true; // 设置服务器为空闲状态
    }
  }

end: // 结束部分
  if (!retval && !thd->m_server_idle) { // 检查返回值和服务器状态
    MYSQL_SOCKET_SET_STATE(thd->get_protocol_classic()->get_vio()->mysql_socket,
                           PSI_SOCKET_STATE_IDLE); // 设置套接字状态为空闲
    MYSQL_START_IDLE_WAIT(thd->m_idle_psi, &thd->m_idle_state); // 开始空闲等待
    thd->m_server_idle = true; // 设置服务器为空闲状态
  }

  return retval; // 返回结果
}

THD_event_functions tp_event_functions = {tp_wait_begin, tp_wait_end,
                                          tp_post_kill_notification}; // 事件函数结构体