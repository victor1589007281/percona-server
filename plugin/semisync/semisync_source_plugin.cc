/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008, 2022, Oracle and/or its affiliates.

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

#include <stddef.h>
#include <sys/types.h>

#include "my_inttypes.h"
#include "my_macros.h"
#include "my_psi_config.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/psi/mysql_stage.h"
#include "plugin/semisync/semisync_source.h"
#include "plugin/semisync/semisync_source_ack_receiver.h"
#include "sql/current_thd.h"
#include "sql/derror.h"  // ER_THD
#include "sql/protocol_classic.h"
#include "sql/raii/sentry.h"  // raii::Sentry
#include "sql/sql_class.h"    // THD
#include "sql/sql_lex.h"      // thd->lex
#include "typelib.h"

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
#define SOURCE_NAME "master"
#define REPLICA_NAME "slave"
#define SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_master"
#define OTHER_SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_source"
#define WAIT_NO_REPLICA_NAME wait_no_slave
#define WAIT_FOR_REPLICA_COUNT_NAME wait_for_slave_count
#define STATUS_VAR_PREFIX "Rpl_semi_sync_master_"
#define DEPRECATED_SEMISYNC_LIBRARY
#else
#define SOURCE_NAME "source"
#define REPLICA_NAME "replica"
#define SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_source"
#define OTHER_SEMI_SYNC_PLUGIN_NAME "rpl_semi_sync_master"
#define WAIT_NO_REPLICA_NAME wait_no_replica
#define WAIT_FOR_REPLICA_COUNT_NAME wait_for_replica_count
#define STATUS_VAR_PREFIX "Rpl_semi_sync_source_"
#endif

ReplSemiSyncMaster *repl_semisync = nullptr;  // 半同步复制主库对象
Ack_receiver *ack_receiver = nullptr;        // 用于接收从库 ACK 的对象

/* 定义主库等待从库 ACK 的位置 */
/* The places at where semisync waits for binlog ACKs. */
enum enum_wait_point { WAIT_AFTER_SYNC, WAIT_AFTER_COMMIT };

static ulong rpl_semi_sync_source_wait_point = WAIT_AFTER_COMMIT;  // 默认等待点为事务提交后

thread_local bool THR_RPL_SEMI_SYNC_DUMP = false;  // 标记当前线程是否为半同步复制的 binlog dump 线程

static SERVICE_TYPE(registry) *reg_srv = nullptr;  // 注册服务
SERVICE_TYPE(log_builtins) *log_bi = nullptr;      // 内置日志服务
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;  // 字符串日志服务

/* 检查当前线程是否为半同步 dump 线程 */
static inline bool is_semi_sync_dump() { return THR_RPL_SEMI_SYNC_DUMP; }

/**
  在 binlog 更新时报告事务信息。

  @param log_file 二进制日志文件名。
  @param log_pos  二进制日志位置。

  @retval 0 成功。
          Success.
  @retval 非零值 表示错误。
          Non-zero value indicates an error.
*/
static int repl_semi_report_binlog_update(Binlog_storage_param *,
                                          const char *log_file,
                                          my_off_t log_pos) {
  int error = 0;

  if (repl_semisync->getMasterEnabled()) {
    /*
      存储 binlog 文件名和位置，以便主库知道需要等待多长时间。
      Store the binlog file name and position so the master knows how long
      to wait for the binlog to be replicated to the slave.
    */
    error = repl_semisync->writeTranxInBinlog(log_file, log_pos);
  }

  return error;
}

/**
  在 binlog 同步完成后报告事务信息。

  @param log_file 二进制日志文件名。
  @param log_pos  二进制日志位置。

  @retval 0 成功。
          Success.
  @retval 非零值 表示错误。
          Non-zero value indicates an error.
*/
static int repl_semi_report_binlog_sync(Binlog_storage_param *,
                                        const char *log_file,
                                        my_off_t log_pos) {
  // 如果等待点设置为 WAIT_AFTER_SYNC，则调用 commitTrx 方法提交事务。
  if (rpl_semi_sync_source_wait_point == WAIT_AFTER_SYNC)
    return repl_semisync->commitTrx(log_file, log_pos);

  // 如果等待点不是 WAIT_AFTER_SYNC，则直接返回成功。
  return 0;
}


static int repl_semi_report_before_dml(Trans_param *, int &) { return 0; }

static int repl_semi_report_before_commit(Trans_param *) { return 0; }

static int repl_semi_report_before_rollback(Trans_param *) { return 0; }

static int repl_semi_report_commit(Trans_param *param) {
  bool is_real_trans = param->flags & TRANS_IS_REAL_TRANS;

  if (rpl_semi_sync_source_wait_point == WAIT_AFTER_COMMIT && is_real_trans &&
      param->log_pos) {
    const char *binlog_name = param->log_file;
    return repl_semisync->commitTrx(binlog_name, param->log_pos);
  }
  return 0;
}

static int repl_semi_report_rollback(Trans_param *param) {
  return repl_semi_report_commit(param);
}

static int repl_semi_report_begin(Trans_param *, int &) { return 0; }

static int repl_semi_binlog_dump_start(Binlog_transmit_param *param,
                                       const char *log_file, my_off_t log_pos) {
  long long semi_sync_slave = 0;

  /*
    检查从库是否设置了半同步复制的用户变量。
    Check if the replica has identified itself as a semisync replica
    by setting a user variable.  The old library sets the user
    variable rpl_semi_sync_slave on the session, and the new library
    sets rpl_semi_sync_replica.  The value returned through the
    argument will be whatever the replica has set it to in the
    session, or 0 if the replica has not set it.
  */
  get_user_var_int("rpl_semi_sync_replica", &semi_sync_slave, nullptr);
  if (semi_sync_slave == 0)
    get_user_var_int("rpl_semi_sync_slave", &semi_sync_slave, nullptr);

  if (semi_sync_slave != 0) {
    if (ack_receiver->add_slave(current_thd)) {
      LogErr(ERROR_LEVEL, ER_SEMISYNC_FAILED_REGISTER_SLAVE_TO_RECEIVER);
      return -1;
    }

    THR_RPL_SEMI_SYNC_DUMP = true;  // 标记当前线程为半同步 dump 线程

    /* One more semi-sync slave */
    /* 增加一个半同步从库 */
    repl_semisync->add_slave();

    /* Tell server it will observe the transmission.*/
    /* 通知服务器将观察传输 */
    param->set_observe_flag();

    /*
      假设该半同步从库已经接收到它请求的文件名和位置之前的所有 binlog 事件。
      Assume this semi-sync slave has already received all binlog events
      before the filename and position it requests.
    */
    repl_semisync->handleAck(param->server_id, log_file, log_pos);
  } else
    param->set_dont_observe_flag();  // 设置为异步模式

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_START_BINLOG_DUMP_TO_SLAVE,
         semi_sync_slave != 0 ? "semi-sync" : "asynchronous", param->server_id,
         log_file, (unsigned long)log_pos);
  return 0;
}

/**
  在 binlog dump 结束时清理半同步状态。

  @param param binlog 传输参数。

  @retval 0 成功。
          Success.
*/
static int repl_semi_binlog_dump_end(Binlog_transmit_param *param) {
  bool semi_sync_slave = is_semi_sync_dump();

  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_STOP_BINLOG_DUMP_TO_SLAVE,
         semi_sync_slave ? "semi-sync" : "asynchronous", param->server_id);

  if (semi_sync_slave) {
    ack_receiver->remove_slave(current_thd);  // 移除从库
    /* One less semi-sync slave */
    repl_semisync->remove_slave();           // 减少一个半同步从库
    THR_RPL_SEMI_SYNC_DUMP = false;          // 清除线程标记
  }
  return 0;
}

/**
  为 binlog 事件保留半同步复制的头部信息。

  @param header 指向 binlog 事件头部的指针。
  @param size   当前头部的大小。
  @param len    头部的总长度。

  @retval 0 成功。
          Success.
*/
static int repl_semi_reserve_header(Binlog_transmit_param *,
                                    unsigned char *header, unsigned long size,
                                    unsigned long *len) {
  // 如果当前线程是半同步 dump 线程，则为头部保留额外的空间。
  if (is_semi_sync_dump())
    *len += repl_semisync->reserveSyncHeader(header, size);
  return 0;
}

/**
  在发送 binlog 事件之前更新半同步复制的头部信息。

  @param param     binlog 传输参数。
  @param packet    指向 binlog 事件数据包的指针。
  @param log_file  当前 binlog 文件名。
  @param log_pos   当前 binlog 文件中的位置。

  @retval 0 成功。
          Success.
*/
static int repl_semi_before_send_event(Binlog_transmit_param *param,
                                       unsigned char *packet, unsigned long,
                                       const char *log_file, my_off_t log_pos) {
  // 如果当前线程不是半同步 dump 线程，则直接返回。
  if (!is_semi_sync_dump()) return 0;

  // 更新 binlog 事件的头部信息以支持半同步复制。
  return repl_semisync->updateSyncHeader(packet, log_file, log_pos,
                                         param->server_id);
}

/**
  在发送 binlog 事件之后处理半同步复制的逻辑。

  @param param            binlog 传输参数。
  @param event_buf        指向 binlog 事件缓冲区的指针。
  @param skipped_log_file 如果跳过了某些事件，提供对应的 binlog 文件名。
  @param skipped_log_pos  如果跳过了某些事件，提供对应的 binlog 文件位置。

  @retval 0 成功。
          Success.
*/
static int repl_semi_after_send_event(Binlog_transmit_param *param,
                                      const char *event_buf, unsigned long,
                                      const char *skipped_log_file,
                                      my_off_t skipped_log_pos) {
  // 如果当前线程是半同步 dump 线程，则处理半同步逻辑。
  if (is_semi_sync_dump()) {
    if (skipped_log_pos > 0) {
      // 如果跳过了某些事件，通知主库跳过从库的 ACK。
      repl_semisync->skipSlaveReply(event_buf, param->server_id,
                                    skipped_log_file, skipped_log_pos);
    } else {
      THD *thd = current_thd;
      /*
        尝试读取从库的 ACK 回复。
        如果读取失败，忽略错误以避免线程退出。      
        Possible errors in reading slave reply are ignored deliberately
        because we do not want dump thread to quit on this. Error
        messages are already reported.
      */
      (void)repl_semisync->readSlaveReply(
          thd->get_protocol_classic()->get_net(), event_buf);
      thd->clear_error();  // 清除可能的错误状态。
    }
  }
  return 0;
}

/**
  重置主库的半同步复制状态。

  @retval 0 成功。
          Success.
  @retval 1 失败。
          Failure.
*/
static int repl_semi_reset_master(Binlog_transmit_param *) {
  // 调用半同步复制主库对象的 resetMaster 方法重置状态。
  if (repl_semisync->resetMaster()) return 1;
  return 0;
}

/*
  semisync system variables
 */
static void fix_rpl_semi_sync_source_timeout(MYSQL_THD thd, SYS_VAR *var,
                                             void *ptr, const void *val);

static void fix_rpl_semi_sync_source_trace_level(MYSQL_THD thd, SYS_VAR *var,
                                                 void *ptr, const void *val);

static void fix_rpl_semi_sync_source_wait_no_replica(MYSQL_THD thd,
                                                     SYS_VAR *var, void *ptr,
                                                     const void *val);

static void fix_rpl_semi_sync_source_enabled(MYSQL_THD thd, SYS_VAR *var,
                                             void *ptr, const void *val);

static void fix_rpl_semi_sync_source_wait_for_replica_count(MYSQL_THD thd,
                                                            SYS_VAR *var,
                                                            void *ptr,
                                                            const void *val);

/**
  定义一个布尔型系统变量，用于启用或禁用半同步复制。

  @param enabled 变量名称。
  @param rpl_semi_sync_source_enabled 变量值，表示是否启用半同步复制。
  @param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
  @param 描述信息：启用半同步复制源（默认禁用）。
  @param nullptr 检查函数（未定义）。
  @param fix_rpl_semi_sync_source_enabled 更新函数，用于处理变量值的更改。
  @param 0 默认值，表示禁用。
*/
static MYSQL_SYSVAR_BOOL(
  enabled, rpl_semi_sync_source_enabled, PLUGIN_VAR_OPCMDARG,
  "Enable semi-synchronous replication source (disabled by default). ",
  nullptr,                            // check
  &fix_rpl_semi_sync_source_enabled,  // update
  0);

/**
定义一个无符号长整型系统变量，用于设置半同步复制的超时时间。

@param timeout 变量名称。
@param rpl_semi_sync_source_timeout 变量值，表示超时时间（以毫秒为单位）。
@param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
@param 描述信息：半同步复制的超时时间。如果在超时时间内未收到足够的从库确认，则切换到异步复制。
@param nullptr 检查函数（未定义）。
@param fix_rpl_semi_sync_source_timeout 更新函数，用于处理变量值的更改。
@param 10000 默认值，表示 10 秒。
@param 0 最小值。
@param ~0UL 最大值。
@param 1 步长。
*/
static MYSQL_SYSVAR_ULONG(
  timeout, rpl_semi_sync_source_timeout, PLUGIN_VAR_OPCMDARG,
  "The timeout value (in milliseconds) for semi-synchronous replication on "
  "the source. If less than "
  "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME
  "_count "
  "replicas have replied after this amount of time, switch to asynchronous "
  "replication.",
  nullptr,                           // check
  fix_rpl_semi_sync_source_timeout,  // update
  10000, 0, ~0UL, 1);

/**
定义一个布尔型系统变量，用于控制是否在特定条件下切换到异步复制。

@param NAME 变量名称（根据宏定义为 wait_no_replica 或 wait_no_slave）。
@param rpl_semi_sync_source_wait_no_replica 变量值，表示是否启用该功能。
@param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
@param 描述信息：如果启用，仅当从库数量少于指定值时才切换到异步复制。
@param nullptr 检查函数（未定义）。
@param fix_rpl_semi_sync_source_wait_no_replica 更新函数，用于处理变量值的更改。
@param 1 默认值，表示启用。
*/
#define DEFINE_WAIT_NO_REPLICA(NAME)                                           \
static MYSQL_SYSVAR_BOOL(                                                    \
    NAME, rpl_semi_sync_source_wait_no_replica, PLUGIN_VAR_OPCMDARG,         \
    "If enabled, revert to asynchronous replication only if less "           \
    "than "                                                                  \
    "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME                   \
    "_count "                                                                \
    "replicas have replied when "                                            \
    "rpl_semi_sync_" SOURCE_NAME                                             \
    "_timeout "                                                              \
    "seconds have passed. If disabled, revert to asynchronous "              \
    "replication also as soon as the number of connected replicas "          \
    "drops below "                                                           \
    "rpl_semi_sync_" SOURCE_NAME "_wait_for_" REPLICA_NAME "_count.",        \
    nullptr /*check*/, &fix_rpl_semi_sync_source_wait_no_replica /*update*/, \
    1);

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
DEFINE_WAIT_NO_REPLICA(wait_no_slave)
#else
DEFINE_WAIT_NO_REPLICA(wait_no_replica)
#endif

/**
定义一个无符号长整型系统变量，用于设置半同步复制的跟踪级别。

@param trace_level 变量名称。
@param rpl_semi_sync_source_trace_level 变量值，表示跟踪级别。
@param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
@param 描述信息：半同步复制的跟踪级别。
@param nullptr 检查函数（未定义）。
@param fix_rpl_semi_sync_source_trace_level 更新函数，用于处理变量值的更改。
@param 32 默认值。
@param 0 最小值。
@param ~0UL 最大值。
@param 1 步长。
*/
static MYSQL_SYSVAR_ULONG(trace_level, rpl_semi_sync_source_trace_level,
                        PLUGIN_VAR_OPCMDARG,
                        "The tracing level for semi-sync replication.",
                        nullptr,                                // check
                        &fix_rpl_semi_sync_source_trace_level,  // update
                        32, 0, ~0UL, 1);

/**
定义一个枚举型系统变量，用于设置主库等待从库确认的时机。

@param wait_point 变量名称。
@param rpl_semi_sync_source_wait_point 变量值，表示等待时机。
@param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
@param 描述信息：主库可以在两个时机之一等待从库确认：AFTER_SYNC 或 AFTER_COMMIT。
@param nullptr 检查函数（未定义）。
@param nullptr 更新函数（未定义）。
@param WAIT_AFTER_SYNC 默认值，表示在 binlog 同步到磁盘后等待。
@param wait_point_typelib 枚举类型定义。
*/
static const char *wait_point_names[] = {"AFTER_SYNC", "AFTER_COMMIT", NullS};
static TYPELIB wait_point_typelib = {array_elements(wait_point_names) - 1, "",
                                   wait_point_names, nullptr};
static MYSQL_SYSVAR_ENUM(
  wait_point,                      /* name     */
  rpl_semi_sync_source_wait_point, /* var      */
  PLUGIN_VAR_OPCMDARG,             /* flags    */
  "The semisync source plugin can wait for replica replies at one of two "
  "alternative points: AFTER_SYNC or AFTER_COMMIT. "
  "AFTER_SYNC is the default value. AFTER_SYNC means that the "
  "source-side semisynchronous plugin waits for the replies just after it "
  "has synced the binary log file (or would have synced, but may have "
  "skipped it, when sync_binlog!=1), but before it has committed in the "
  "engine on the source side. Therefore, it guarantees that no other "
  "sessions on the source can see the effects of the transaction before "
  "the replica has received it. "
  "AFTER_COMMIT means that the source-side semisynchronous plugin "
  "waits for the replies from the replica just after the source has "
  "committed the transaction in the engine, and before it sends an ACK "
  "packet to the client session. Other sessions may see the effects of "
  "the transaction before it has been replicated, even though the current "
  "session is still waiting for the replies from the replica.",
  nullptr,            /* check()  */
  nullptr,            /* update() */
  WAIT_AFTER_SYNC,    /* default  */
  &wait_point_typelib /* typelib  */
);

/**
定义一个无符号整型系统变量，用于设置需要从库确认的数量。

@param NAME 变量名称（根据宏定义为 wait_for_replica_count 或 wait_for_slave_count）。
@param rpl_semi_sync_source_wait_for_replica_count 变量值，表示需要确认的从库数量。
@param PLUGIN_VAR_OPCMDARG 标志，表示该变量可以通过命令行或配置文件设置。
@param 描述信息：需要从库确认的数量。
@param nullptr 检查函数（未定义）。
@param fix_rpl_semi_sync_source_wait_for_replica_count 更新函数，用于处理变量值的更改。
@param 1 默认值。
@param 1 最小值。
@param 65535 最大值。
@param 1 步长。
*/
#define DEFINE_WAIT_FOR_REPLICA_COUNT(NAME)                             \
static MYSQL_SYSVAR_UINT(                                             \
    NAME,                                        /* name  */          \
    rpl_semi_sync_source_wait_for_replica_count, /* var   */          \
    PLUGIN_VAR_OPCMDARG,                         /* flags */          \
    "The number of replicas that need to acknowledge that they have " \
    "received a transaction, before the transaction can complete on " \
    "the source.",                                                    \
    nullptr /* check */,                                              \
    &fix_rpl_semi_sync_source_wait_for_replica_count, /* update */    \
    1, 1, 65535, 1);

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
DEFINE_WAIT_FOR_REPLICA_COUNT(wait_for_slave_count)
#else
DEFINE_WAIT_FOR_REPLICA_COUNT(wait_for_replica_count)
#endif

/**
  定义一个数组，包含半同步复制插件的系统变量。

  - `MYSQL_SYSVAR(enabled)`：控制是否启用半同步复制。
  - `MYSQL_SYSVAR(timeout)`：设置半同步复制的超时时间。
  - `MYSQL_SYSVAR(WAIT_NO_REPLICA_NAME)`：控制是否在特定条件下切换到异步复制。
  - `MYSQL_SYSVAR(trace_level)`：设置半同步复制的跟踪级别。
  - `MYSQL_SYSVAR(wait_point)`：设置主库等待从库确认的时机。
  - `MYSQL_SYSVAR(WAIT_FOR_REPLICA_COUNT_NAME)`：设置需要从库确认的数量。
*/
static SYS_VAR *semi_sync_master_system_vars[] = {
    MYSQL_SYSVAR(enabled),
    MYSQL_SYSVAR(timeout),
    MYSQL_SYSVAR(WAIT_NO_REPLICA_NAME),
    MYSQL_SYSVAR(trace_level),
    MYSQL_SYSVAR(wait_point),
    MYSQL_SYSVAR(WAIT_FOR_REPLICA_COUNT_NAME),
    nullptr,  // 结束标记
};

/**
  更新函数：设置半同步复制的超时时间。

  @param thd 当前线程。
  @param var 系统变量。
  @param ptr 指向变量值的指针。
  @param val 新的变量值。
*/
static void fix_rpl_semi_sync_source_timeout(MYSQL_THD, SYS_VAR *, void *ptr,
                                             const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  repl_semisync->setWaitTimeout(rpl_semi_sync_source_timeout);  // 更新超时时间
  return;
}

/**
  更新函数：设置半同步复制的跟踪级别。

  @param thd 当前线程。
  @param var 系统变量。
  @param ptr 指向变量值的指针。
  @param val 新的变量值。
*/
static void fix_rpl_semi_sync_source_trace_level(MYSQL_THD, SYS_VAR *,
                                                 void *ptr, const void *val) {
  *static_cast<unsigned long *>(ptr) = *static_cast<const unsigned long *>(val);
  repl_semisync->setTraceLevel(rpl_semi_sync_source_trace_level);  // 更新主库跟踪级别
  ack_receiver->setTraceLevel(rpl_semi_sync_source_trace_level);   // 更新 ACK 接收器跟踪级别
  return;
}

/**
  更新函数：启用或禁用半同步复制。

  @param thd 当前线程。
  @param var 系统变量。
  @param ptr 指向变量值的指针。
  @param val 新的变量值。
*/
static void fix_rpl_semi_sync_source_enabled(MYSQL_THD, SYS_VAR *, void *ptr,
                                             const void *val) {
  *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
  if (rpl_semi_sync_source_enabled) {
    // 启用半同步复制
    if (repl_semisync->enableMaster() != 0)
      rpl_semi_sync_source_enabled = false;  // 启用失败
    else if (ack_receiver->start()) {
      repl_semisync->disableMaster();  // 停止主库
      rpl_semi_sync_source_enabled = false;
    }
  } else {
    // 禁用半同步复制
    if (repl_semisync->disableMaster() != 0)
      rpl_semi_sync_source_enabled = true;  // 禁用失败
    ack_receiver->stop();  // 停止 ACK 接收器
  }

  return;
}

/**
  更新函数：设置需要从库确认的数量。

  @param thd 当前线程。
  @param var 系统变量。
  @param ptr 指向变量值的指针。
  @param val 新的变量值。
*/
static void fix_rpl_semi_sync_source_wait_for_replica_count(MYSQL_THD,
                                                            SYS_VAR *, void *,
                                                            const void *val) {
  (void)repl_semisync->setWaitSlaveCount(
      *static_cast<const unsigned int *>(val));  // 更新从库确认数量
}

/**
  更新函数：控制是否在特定条件下切换到异步复制。

  @param thd 当前线程。
  @param var 系统变量。
  @param ptr 指向变量值的指针。
  @param val 新的变量值。
*/
static void fix_rpl_semi_sync_source_wait_no_replica(MYSQL_THD, SYS_VAR *,
                                                     void *ptr,
                                                     const void *val) {
  if (rpl_semi_sync_source_wait_no_replica != *static_cast<const bool *>(val)) {
    *static_cast<bool *>(ptr) = *static_cast<const bool *>(val);
    repl_semisync->set_wait_no_replica(val);  // 更新异步切换条件
  }
}

/**
  定义事务观察器，用于监控事务的生命周期事件。

  - `before_dml`：在 DML 操作之前触发。
  - `before_commit`：在事务提交之前触发。
  - `before_rollback`：在事务回滚之前触发。
  - `after_commit`：在事务提交之后触发。
  - `after_rollback`：在事务回滚之后触发。
  - `begin`：在事务开始时触发。
*/
Trans_observer trans_observer = {
  sizeof(Trans_observer),  // 结构体大小

  repl_semi_report_before_dml,       // 在 DML 操作之前触发
  repl_semi_report_before_commit,    // 在事务提交之前触发
  repl_semi_report_before_rollback,  // 在事务回滚之前触发
  repl_semi_report_commit,           // 在事务提交之后触发
  repl_semi_report_rollback,         // 在事务回滚之后触发
  repl_semi_report_begin,            // 在事务开始时触发
};

/**
定义 binlog 存储观察器，用于监控 binlog 存储相关事件。

- `report_update`：在 binlog 更新时触发。
- `after_sync`：在 binlog 同步完成后触发。
*/
Binlog_storage_observer storage_observer = {
  sizeof(Binlog_storage_observer),  // 结构体大小

  repl_semi_report_binlog_update,  // 在 binlog 更新时触发
  repl_semi_report_binlog_sync,    // 在 binlog 同步完成后触发
};

/**
定义 binlog 传输观察器，用于监控 binlog 传输相关事件。

- `start`：在 binlog dump 开始时触发。
- `stop`：在 binlog dump 结束时触发。
- `reserve_header`：为 binlog 事件保留头部信息。
- `before_send_event`：在发送 binlog 事件之前触发。
- `after_send_event`：在发送 binlog 事件之后触发。
- `reset`：重置主库的半同步复制状态。
*/
Binlog_transmit_observer transmit_observer = {
  sizeof(Binlog_transmit_observer),  // 结构体大小

  repl_semi_binlog_dump_start,  // 在 binlog dump 开始时触发
  repl_semi_binlog_dump_end,    // 在 binlog dump 结束时触发
  repl_semi_reserve_header,     // 为 binlog 事件保留头部信息
  repl_semi_before_send_event,  // 在发送 binlog 事件之前触发
  repl_semi_after_send_event,   // 在发送 binlog 事件之后触发
  repl_semi_reset_master,       // 重置主库的半同步复制状态
};

/**
定义一个宏，用于生成 SHOW 变量的函数。

- `name`：变量名称。
- `show_type`：变量的显示类型（如 BOOL、LONG 等）。
*/
#define SHOW_FNAME(name) rpl_semi_sync_source_show_##name

#define DEF_SHOW_FUNC(name, show_type)                             \
static int SHOW_FNAME(name)(MYSQL_THD, SHOW_VAR * var, char *) { \
  repl_semisync->setExportStats();                               \
  var->type = show_type;                                         \
  var->value = (char *)&rpl_semi_sync_source_##name;             \
  return 0;                                                      \
}

/* 定义多个 SHOW 变量的函数，用于显示插件的状态信息 */
DEF_SHOW_FUNC(status, SHOW_BOOL)
DEF_SHOW_FUNC(clients, SHOW_LONG)
DEF_SHOW_FUNC(wait_sessions, SHOW_LONG)
DEF_SHOW_FUNC(trx_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(trx_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_time, SHOW_LONGLONG)
DEF_SHOW_FUNC(net_wait_num, SHOW_LONGLONG)
DEF_SHOW_FUNC(avg_net_wait_time, SHOW_LONG)
DEF_SHOW_FUNC(avg_trx_wait_time, SHOW_LONG)

/**
定义插件的状态变量，用于显示半同步复制的运行状态。

- `status`：是否启用了半同步复制。
- `clients`：当前连接的从库数量。
- `yes_tx`：成功的半同步事务数量。
- `no_tx`：失败的半同步事务数量。
- `wait_sessions`：当前等待的会话数量。
- `tx_wait_time`：事务等待时间。
- `net_wait_time`：网络等待时间。
*/
static SHOW_VAR semi_sync_master_status_vars[] = {
  {STATUS_VAR_PREFIX "status", (char *)&SHOW_FNAME(status), SHOW_FUNC,
   SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "clients", (char *)&SHOW_FNAME(clients), SHOW_FUNC,
   SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "yes_tx", (char *)&rpl_semi_sync_source_yes_transactions,
   SHOW_LONG, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "no_tx", (char *)&rpl_semi_sync_source_no_transactions,
   SHOW_LONG, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "wait_sessions", (char *)&SHOW_FNAME(wait_sessions),
   SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "no_times", (char *)&rpl_semi_sync_source_off_times,
   SHOW_LONG, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "timefunc_failures",
   (char *)&rpl_semi_sync_source_timefunc_fails, SHOW_LONG,
   SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "wait_pos_backtraverse",
   (char *)&rpl_semi_sync_source_wait_pos_backtraverse, SHOW_LONG,
   SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "tx_wait_time", (char *)&SHOW_FNAME(trx_wait_time),
   SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "tx_waits", (char *)&SHOW_FNAME(trx_wait_num), SHOW_FUNC,
   SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "tx_avg_wait_time",
   (char *)&SHOW_FNAME(avg_trx_wait_time), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "net_wait_time", (char *)&SHOW_FNAME(net_wait_time),
   SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "net_waits", (char *)&SHOW_FNAME(net_wait_num),
   SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {STATUS_VAR_PREFIX "net_avg_wait_time",
   (char *)&SHOW_FNAME(avg_net_wait_time), SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {nullptr, nullptr, SHOW_LONG, SHOW_SCOPE_GLOBAL},  // 结束标记
};

#ifdef HAVE_PSI_INTERFACE

/**
  定义与 Performance Schema (PSI) 相关的互斥锁、条件变量和线程信息。
  这些信息用于监控和调试半同步复制插件的性能。
*/

// 定义互斥锁的 PSI 键
PSI_mutex_key key_ss_mutex_LOCK_binlog_;
PSI_mutex_key key_ss_mutex_Ack_receiver_mutex;

// 定义互斥锁信息数组
static PSI_mutex_info all_semisync_mutexes[] = {
    {&key_ss_mutex_LOCK_binlog_, "LOCK_binlog_", 0, 0, PSI_DOCUMENT_ME},
    {&key_ss_mutex_Ack_receiver_mutex, "Ack_receiver::m_mutex", 0, 0,
     PSI_DOCUMENT_ME}};

// 定义条件变量的 PSI 键
PSI_cond_key key_ss_cond_COND_binlog_send_;
PSI_cond_key key_ss_cond_Ack_receiver_cond;

// 定义条件变量信息数组
static PSI_cond_info all_semisync_conds[] = {
    {&key_ss_cond_COND_binlog_send_, "COND_binlog_send_", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_ss_cond_Ack_receiver_cond, "Ack_receiver::m_cond", 0, 0,
     PSI_DOCUMENT_ME}};

// 定义线程的 PSI 键
PSI_thread_key key_ss_thread_Ack_receiver_thread;

// 定义线程信息数组
static PSI_thread_info all_semisync_threads[] = {
    {&key_ss_thread_Ack_receiver_thread, "Ack_receiver", "ss_ack",
     PSI_FLAG_SINGLETON | PSI_FLAG_THREAD_SYSTEM, 0, PSI_DOCUMENT_ME}};
#endif /* HAVE_PSI_INTERFACE */

#ifdef USE_OLD_SEMI_SYNC_TERMINOLOGY
/**
  定义与旧术语兼容的 PSI 阶段信息。
  这些阶段用于描述半同步复制的不同状态。
*/
PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica = {
    0, "Waiting for semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_semi_sync_replica = {
    0, "Waiting for semi-sync slave connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_semi_sync_ack = {
    0, "Reading semi-sync ACK from slave", 0, PSI_DOCUMENT_ME};
#else
/**
  定义与新术语兼容的 PSI 阶段信息。
*/
PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica = {
    0, "Waiting for semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_waiting_for_semi_sync_replica = {
    0, "Waiting for semi-sync replica connection", 0, PSI_DOCUMENT_ME};

PSI_stage_info stage_reading_semi_sync_ack = {
    0, "Reading semi-sync ACK from replica", 0, PSI_DOCUMENT_ME};
#endif

/* 定义内存分配的 PSI 键 */
/* Always defined. */
PSI_memory_key key_ss_memory_TranxNodeAllocator_block;

#ifdef HAVE_PSI_INTERFACE
/**
  定义所有的 PSI 信息，包括阶段、内存、互斥锁、条件变量和线程。
*/
PSI_stage_info *all_semisync_stages[] = {
    &stage_waiting_for_semi_sync_ack_from_replica,
    &stage_waiting_for_semi_sync_replica, &stage_reading_semi_sync_ack};

PSI_memory_info all_semisync_memory[] = {
    {&key_ss_memory_TranxNodeAllocator_block, "TranxNodeAllocator::block", 0, 0,
     PSI_DOCUMENT_ME}};

/**
  初始化 PSI 键的函数。
  该函数注册所有的 PSI 信息到 Performance Schema。
*/
static void init_semisync_psi_keys(void) {
  const char *category = "semisync";
  int count;

  count = static_cast<int>(array_elements(all_semisync_mutexes));
  mysql_mutex_register(category, all_semisync_mutexes, count);

  count = static_cast<int>(array_elements(all_semisync_conds));
  mysql_cond_register(category, all_semisync_conds, count);

  count = static_cast<int>(array_elements(all_semisync_stages));
  mysql_stage_register(category, all_semisync_stages, count);

  count = static_cast<int>(array_elements(all_semisync_memory));
  mysql_memory_register(category, all_semisync_memory, count);

  count = static_cast<int>(array_elements(all_semisync_threads));
  mysql_thread_register(category, all_semisync_threads, count);
}
#endif /* HAVE_PSI_INTERFACE */

/**
  检查是否安装了另一个半同步复制插件。
  Return true if this is the new library and the old library is installed, or
  vice versa.

  @retval true This is semisync_master, and semisync_source is
  installed already, or this is semisync_source, and semisync_master
  is installed already.

  @retval false Otherwise
*/
static bool is_other_semi_sync_source_plugin_installed() {
  return is_sysvar_defined(OTHER_SEMI_SYNC_PLUGIN_NAME "_enabled");
}

/**
  初始化半同步复制插件。

  @param p 插件上下文。

  @retval 0 成功。
  @retval 1 失败。
*/
static int semi_sync_master_plugin_init(void *p) {
  // Initialize error logging service.初始化日志服务
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return 1;
  // 使用 RAII 确保失败时自动清理日志服务
  // Auto-deinitialize the error logging service if this function fails.
  bool success = false;
  raii::Sentry<> logging_service_guard{[&]() {
    if (!success) deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  }};

  // Check for duplicate libraries.检查是否存在重复的插件
  bool is_client =
      current_thd && current_thd->lex->sql_command == SQLCOM_INSTALL_PLUGIN;
  if (is_other_semi_sync_source_plugin_installed()) {
    /*
      Unfortunately, two semisync libraries don't make one sync library. :-)
      If user installs both the old-named library and the new-named
      library, we generate an error, since the two would interfere with
      each other.
    */
    if (is_client)
      my_error(ER_INSTALL_PLUGIN_CONFLICT_CLIENT, MYF(0), SEMI_SYNC_PLUGIN_NAME,
               OTHER_SEMI_SYNC_PLUGIN_NAME);
    else
      LogErr(ERROR_LEVEL, ER_INSTALL_PLUGIN_CONFLICT_LOG, SEMI_SYNC_PLUGIN_NAME,
             OTHER_SEMI_SYNC_PLUGIN_NAME);
    return 1;
  }

#ifdef HAVE_PSI_INTERFACE
  init_semisync_psi_keys();  // 初始化 PSI 键
#endif

#ifdef DEPRECATED_SEMISYNC_LIBRARY
  /*
    如果使用了旧插件，记录警告信息
    This function can be invoked in two contexts: either from the SQL
    statement INSTALL PLUGIN executed by a client, or during server
    startup, for example, in case --plugin-load is used.

    For INSTALL PLUGIN, return a warning to the client, so the person
    that issued INSTALL PLUGIN gets notified.

    In both cases, write a warning to the log, because the
    administrator needs to know that we are using an old library and
    make the new library available if it is not.
  */
  if (is_client)
    push_warning_printf(current_thd, Sql_condition::SL_NOTE,
                        ER_WARN_DEPRECATED_SYNTAX,
                        ER_THD(current_thd, ER_WARN_DEPRECATED_SYNTAX),
                        "rpl_semi_sync_master", "rpl_semi_sync_source");
  LogErr(WARNING_LEVEL, ER_DEPRECATE_MSG_WITH_REPLACEMENT,
         "rpl_semi_sync_master", "rpl_semi_sync_source");
#endif

  // 初始化全局变量
  THR_RPL_SEMI_SYNC_DUMP = false;

  /*
    In case the plugin has been unloaded, and reloaded, we may need to
    re-initialize some global variables.
    These are initialized to zero by the linker, but may need to be
    re-initialized
  */
  rpl_semi_sync_source_no_transactions = 0;
  rpl_semi_sync_source_yes_transactions = 0;

  // 创建半同步复制对象
  repl_semisync = new ReplSemiSyncMaster();
  ack_receiver = new Ack_receiver();

  // 初始化对象并注册观察器
  if (repl_semisync->initObject()) return 1;
  if (ack_receiver->init()) return 1;
  if (register_trans_observer(&trans_observer, p)) return 1;
  if (register_binlog_storage_observer(&storage_observer, p)) return 1;
  if (register_binlog_transmit_observer(&transmit_observer, p)) return 1;

  success = true;
  return 0;
}

/**
  检查是否可以卸载半同步复制插件。

  @param p 插件上下文。

  @retval 0 可以卸载。
  @retval 1 不可以卸载。
*/
static int semi_sync_source_plugin_check_uninstall(void *) {
  int ret = rpl_semi_sync_source_clients ? 1 : 0;
  if (ret) {
    my_error(ER_PLUGIN_CANNOT_BE_UNINSTALLED, MYF(0), SEMI_SYNC_PLUGIN_NAME,
             "Stop any active semisynchronous slaves of this master first.");
  }
  return ret;
}
/**
  插件的卸载函数，用于清理半同步复制插件的资源。

  @param p 插件上下文。

  @retval 0 成功。
  @retval 1 失败。
*/
static int semi_sync_master_plugin_deinit(void *p) {
  // the plugin was not initialized, there is nothing to do here
  // 如果插件未初始化，则无需执行任何操作
  if (ack_receiver == nullptr || repl_semisync == nullptr) return 0;

  // 停止 binlog dump 的半同步标记
  THR_RPL_SEMI_SYNC_DUMP = false;

  // 注销事务观察器
  if (unregister_trans_observer(&trans_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_TRX_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }

  // 注销 binlog 存储观察器
  if (unregister_binlog_storage_observer(&storage_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_BINLOG_STORAGE_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }

  // 注销 binlog 传输观察器
  if (unregister_binlog_transmit_observer(&transmit_observer, p)) {
    LogErr(ERROR_LEVEL, ER_SEMISYNC_UNREGISTER_BINLOG_TRANSMIT_OBSERVER_FAILED);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }

  // 删除 ACK 接收器对象
  delete ack_receiver;
  ack_receiver = nullptr;

  // 删除半同步复制主库对象
  delete repl_semisync;
  repl_semisync = nullptr;

  // 记录插件卸载成功的日志
  LogErr(INFORMATION_LEVEL, ER_SEMISYNC_UNREGISTERED_REPLICATOR);

  // 释放日志服务资源
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);

  return 0;
}

/**
  定义插件的核心结构体，用于描述插件的功能和元信息。
*/
struct Mysql_replication semi_sync_master_plugin = {
    MYSQL_REPLICATION_INTERFACE_VERSION  // 插件接口版本
};

/**
  插件库描述符，定义插件的初始化、卸载和元信息。

  - `MYSQL_REPLICATION_PLUGIN`：插件类型为复制插件。
  - `semi_sync_master_plugin`：插件的核心结构体。
  - `SEMI_SYNC_PLUGIN_NAME`：插件名称。
  - `PLUGIN_AUTHOR_ORACLE`：插件作者。
  - `semi_sync_master_plugin_init`：插件初始化函数。
  - `semi_sync_source_plugin_check_uninstall`：插件卸载检查函数。
  - `semi_sync_master_plugin_deinit`：插件卸载函数。
  - `semi_sync_master_status_vars`：插件的状态变量。
  - `semi_sync_master_system_vars`：插件的系统变量。
*/
/*
  Plugin library descriptor
*/

mysql_declare_plugin(semi_sync_master){
    MYSQL_REPLICATION_PLUGIN,
    &semi_sync_master_plugin,
    SEMI_SYNC_PLUGIN_NAME,
    PLUGIN_AUTHOR_ORACLE,
    "Source-side semi-synchronous replication.",
    PLUGIN_LICENSE_GPL,
    semi_sync_master_plugin_init,            /* Plugin Init */
    semi_sync_source_plugin_check_uninstall, /* Plugin Check uninstall */
    semi_sync_master_plugin_deinit,          /* Plugin Deinit */
    0x0100 /* 1.0 */,
    semi_sync_master_status_vars, /* status variables */
    semi_sync_master_system_vars, /* system variables */
    nullptr,                      /* config options */
    0,                            /* flags */
} mysql_declare_plugin_end;
