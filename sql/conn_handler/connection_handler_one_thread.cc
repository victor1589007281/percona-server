/*
   Copyright (c) 2013, 2022, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <stddef.h>

#include "my_systime.h"  //my_getsystime
#include "mysql/psi/mysql_socket.h"
#include "mysql/psi/mysql_thread.h"
#include "mysql_com.h"
#include "mysqld_error.h"                   // ER_*
#include "sql/conn_handler/channel_info.h"  // Channel_info
#include "sql/conn_handler/connection_handler_impl.h"
#include "sql/conn_handler/connection_handler_manager.h"  // Connection_handler_manager
#include "sql/mysqld.h"              // connection_errors_internal
#include "sql/mysqld_thd_manager.h"  // Global_THD_manager
#include "sql/protocol_classic.h"
#include "sql/sql_class.h"             // THD
#include "sql/sql_connect.h"           // close_connection
#include "sql/sql_parse.h"             // do_command
#include "sql/sql_thd_internal_api.h"  // thd_set_thread_stack

bool One_thread_connection_handler::add_connection(Channel_info *channel_info) {
  // 添加连接的函数
  // 参数: channel_info - 连接通道信息

  if (my_thread_init()) {
    connection_errors_internal++;
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false);
    Connection_handler_manager::dec_connection_count();
    return true;  // 初始化线程失败，返回true
  }

  THD *thd = channel_info->create_thd();
  // 创建线程描述符
  if (thd == nullptr) {
    connection_errors_internal++;
    channel_info->send_error_and_close_channel(ER_OUT_OF_RESOURCES, 0, false);
    Connection_handler_manager::dec_connection_count();
    return true;  // 创建线程失败，返回true
  }

  thd->set_new_thread_id();  // 设置新的线程ID

  /*
    handle_one_connection() is normally the only way a thread would
    start and would always be on the very high end of the stack ,
    therefore, the thread stack always starts at the address of the
    first local variable of handle_one_connection, which is thd. We
    need to know the start of the stack so that we could check for
    stack overruns.
  */
  thd_set_thread_stack(thd, (char *)&thd);  // 设置线程栈
  thd->store_globals();  // 存储全局变量

  mysql_thread_set_psi_id(thd->thread_id());  // 设置线程的PSI ID
  mysql_socket_set_thread_owner(
      thd->get_protocol_classic()->get_vio()->mysql_socket);  // 设置线程拥有的socket

  Global_THD_manager *thd_manager = Global_THD_manager::get_instance();  // 获取全局线程管理器
  thd_manager->add_thd(thd);  // 添加线程到管理器

  bool error = false;  // 错误标志
  bool create_user = true;  // 用户创建标志
  if (thd_prepare_connection(thd)) {
    error = true;  // Returning true causes inc_aborted_connects() to be called.
    create_user = false;  // 不创建用户
  } else {
    delete channel_info;  // 删除通道信息
    while (thd_connection_alive(thd)) {
      if (do_command(thd)) break;  // 执行命令
    }
    end_connection(thd);  // 结束连接
  }
  close_connection(thd, 0, false, false);  // 关闭连接

  if (unlikely(opt_userstat)) {
    thd->update_stats(false);  // 更新线程统计信息
    update_global_user_stats(thd, create_user, my_getsystime());  // 更新全局用户统计信息
  }

  thd->release_resources();  // 释放资源
  thd_manager->remove_thd(thd);  // 从管理器中移除线程
  Connection_handler_manager::dec_connection_count();  // 减少连接计数
  delete thd;  // 删除线程描述符
  return error;  // 返回错误标志
}