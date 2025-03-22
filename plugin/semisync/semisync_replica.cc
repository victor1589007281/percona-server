/* Copyright (c) 2008, 2022, Oracle and/or its affiliates.

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

#include "plugin/semisync/semisync_replica.h"

#include <assert.h>
#include <sys/types.h>

#include "my_byteorder.h"
#include "my_dbug.h"
#include "mysql.h"
#include "sql/current_thd.h"
#include "sql/debug_sync.h"

bool rpl_semi_sync_replica_enabled;
char rpl_semi_sync_replica_status = 0;
unsigned long rpl_semi_sync_replica_trace_level;

// 初始化半同步复制从库对象
// Initialize the semi-synchronous replication slave object
int ReplSemiSyncSlave::initObject() {
  int result = 0; // 初始化返回值为 0，表示成功
  const char *kWho = "ReplSemiSyncSlave::initObject"; // 当前函数的标识符，用于日志记录

  // 如果对象已经初始化，记录警告日志并返回错误码 1
  // If the object is already initialized, log a warning and return error code 1
  if (init_done_) {
    LogErr(WARNING_LEVEL, ER_SEMISYNC_FUNCTION_CALLED_TWICE, kWho);
    return 1;
  }
  init_done_ = true; // 标记对象已初始化

  // 设置从库是否启用半同步复制
  // Set whether the slave is enabled for semi-synchronous replication
  /* References to the parameter works after set_options(). */
  setSlaveEnabled(rpl_semi_sync_replica_enabled);

  // 设置跟踪级别
  // Set the trace level
  setTraceLevel(rpl_semi_sync_replica_trace_level);

  return result; // 返回结果
}

// 读取半同步复制的同步头部信息
// Read the semi-synchronous replication sync header
int ReplSemiSyncSlave::slaveReadSyncHeader(const char *header,
                                           unsigned long total_len,
                                           bool *need_reply,
                                           const char **payload,
                                           unsigned long *payload_len) {
  const char *kWho = "ReplSemiSyncSlave::slaveReadSyncHeader"; // 当前函数的标识符
  int read_res = 0; // 初始化返回值为 0，表示成功
  function_enter(kWho); // 记录函数进入日志

  // 检查数据包的魔术字节是否匹配
  // Check if the packet's magic byte matches
  if ((unsigned char)(header[0]) == kPacketMagicNum) {
    *need_reply = (header[1] & kPacketFlagSync); // 判断是否需要回复
    *payload_len = total_len - 2; // 计算有效负载长度
    *payload = header + 2; // 设置有效负载的起始位置

    // 如果跟踪级别启用了详细信息，记录日志
    // Log detailed information if trace level is enabled
    if (trace_level_ & kTraceDetail)
      LogErr(INFORMATION_LEVEL, ER_SEMISYNC_SLAVE_REPLY, kWho, *need_reply);
  } else {
    // 如果魔术字节不匹配，记录错误日志并返回错误码 -1
    // Log an error if the magic byte does not match and return error code -1
    LogErr(ERROR_LEVEL, ER_SEMISYNC_MISSING_MAGIC_NO_FOR_SEMISYNC_PKT,
           total_len);
    read_res = -1;
  }

  return function_exit(kWho, read_res); // 记录函数退出日志并返回结果
}

// 启动半同步复制从库
// Start the semi-synchronous replication slave
int ReplSemiSyncSlave::slaveStart(Binlog_relay_IO_param *param) {
  bool semi_sync = getSlaveEnabled(); // 获取从库是否启用半同步复制

  // 记录从库启动日志，包括模式（半同步或异步）和主库信息
  // Log the slave start information, including mode (semi-sync or async) and master info
  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_SLAVE_START,
         semi_sync ? "semi-sync" : "asynchronous", param->user, param->host,
         param->port,
         param->master_log_name[0] ? param->master_log_name : "FIRST",
         (unsigned long)param->master_log_pos);

  // 如果启用了半同步复制且状态未激活，则激活状态
  // Activate the status if semi-sync is enabled and not already active
  if (semi_sync && !rpl_semi_sync_replica_status)
    rpl_semi_sync_replica_status = 1;
  return 0; // 返回成功
}

// 停止半同步复制从库
// Stop the semi-synchronous replication slave
int ReplSemiSyncSlave::slaveStop(Binlog_relay_IO_param *) {
  // 如果半同步复制状态已激活，则将其停用
  // Deactivate the status if semi-sync is active
  if (rpl_semi_sync_replica_status) rpl_semi_sync_replica_status = 0;

  // 关闭与主库的连接
  // Close the connection to the master
  if (mysql_reply) mysql_close(mysql_reply);
  mysql_reply = nullptr; // 清空连接指针
  return 0; // 返回成功
}

// 从库发送 ACK 消息到主库
// Send an ACK message from the slave to the master
int ReplSemiSyncSlave::slaveReply(MYSQL *mysql, const char *binlog_filename,
                                  my_off_t binlog_filepos) {
  const char *kWho = "ReplSemiSyncSlave::slaveReply"; // 当前函数的标识符
  NET *net = &mysql->net; // 获取网络对象
  uchar reply_buffer[REPLY_MAGIC_NUM_LEN + REPLY_BINLOG_POS_LEN +
                     REPLY_BINLOG_NAME_LEN]; // 定义回复缓冲区
  int reply_res; // 回复结果
  size_t name_len = strlen(binlog_filename); // 获取 binlog 文件名长度

  function_enter(kWho); // 记录函数进入日志

  // 调试同步点，用于测试和调试
  // Debug sync point for testing and debugging
  DBUG_EXECUTE_IF("rpl_semisync_before_send_ack", {
    const char act[] = "now WAIT_FOR continue";
    assert(opt_debug_sync_timeout > 0);
    assert(!debug_sync_set_action(current_thd, STRING_WITH_LEN(act)));
  };);

  // 准备回复缓冲区
  /* Prepare the buffer of the reply. */
  reply_buffer[REPLY_MAGIC_NUM_OFFSET] = kPacketMagicNum; // 设置魔术字节
  int8store(reply_buffer + REPLY_BINLOG_POS_OFFSET, binlog_filepos); // 存储 binlog 位置
  memcpy(reply_buffer + REPLY_BINLOG_NAME_OFFSET, binlog_filename,
         name_len + 1 /* including trailing '\0' */); // 存储 binlog 文件名

  // 如果启用了详细跟踪级别，记录日志
  // Log detailed information if trace level is enabled
  if (trace_level_ & kTraceDetail)
    LogErr(INFORMATION_LEVEL, ER_SEMISYNC_SLAVE_REPLY_WITH_BINLOG_INFO, kWho,
           binlog_filename, (ulong)binlog_filepos);

  net_clear(net, false); // 清除网络缓冲区
  /* Send the reply. */
  // 发送回复消息
  // Send the reply message
  reply_res =
      my_net_write(net, reply_buffer, name_len + REPLY_BINLOG_NAME_OFFSET);
  if (!reply_res) {
    reply_res = net_flush(net); // 刷新网络缓冲区
    if (reply_res)
      LogErr(ERROR_LEVEL, ER_SEMISYNC_SLAVE_NET_FLUSH_REPLY_FAILED); // 记录错误日志
  } else {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_SLAVE_SEND_REPLY_FAILED, net->last_error,
           net->last_errno); // 记录发送失败日志
  }

  // 如果使用压缩协议，清除网络缓冲区
  // Clear the network buffer if using compressed protocol
  /*
    The progress of the internal state of the NET object differs a bit between
    compressed and non-compressed protocol. For compressed protocol, it is
    necessary to call net_clear when switching between reading and writing.
    For non-compressed protocol, it does not work when we call net_clear here
  */
  if (net->compress) net_clear(net, false);
  return function_exit(kWho, reply_res); // 记录函数退出日志并返回结果
}
