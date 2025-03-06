/*****************************************************************************

Copyright (c) 1996, 2022, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file trx/trx0roll.cc
 Transaction rollback

 Created 3/26/1996 Heikki Tuuri
 *******************************************************/

#include <sys/types.h>

#include "clone0clone.h"
#include "dict0dd.h"
#include "fsp0fsp.h"
#include "ha_prototypes.h"
#include "lock0lock.h"
#include "mach0data.h"
#include "os0thread-create.h"
#include "pars0pars.h"
#include "que0que.h"
#include "read0read.h"
#include "row0mysql.h"
#include "row0undo.h"
#include "sql_thd_internal_api.h"
#include "srv0mon.h"
#include "srv0start.h"
#include "trx0rec.h"
#include "trx0roll.h"
#include "trx0rseg.h"
#include "trx0sys.h"
#include "trx0trx.h"
#include "trx0undo.h"
#include "usr0sess.h"

#include "current_thd.h"

/** This many pages must be undone before a truncate is tried within
rollback */
static const ulint TRX_ROLL_TRUNC_THRESHOLD = 1;

/** In crash recovery, the current trx to be rolled back; NULL otherwise */
static const trx_t *trx_roll_crash_recv_trx = nullptr;

/** In crash recovery we set this to the undo n:o of the current trx to be
rolled back. Then we can print how many % the rollback has progressed. */
static undo_no_t trx_roll_max_undo_no;

/** Auxiliary variable which tells the previous progress % we printed */
static ulint trx_roll_progress_printed_pct;

/** Finishes a transaction rollback. */
static void trx_rollback_finish(trx_t *trx); /*!< in: transaction */

/**
 * @brief 回滚 MySQL 中使用的事务。
 * @brief Rollback a transaction used in MySQL.
 *
 * @param trx 事务句柄
 * @param savept 保存点的 undo 编号指针，如果请求部分回滚，则为 NULL 表示完全回滚
 */
static void trx_rollback_to_savepoint_low(
    trx_t *trx,           /*!< in: transaction handle */
    trx_savept_t *savept) /*!< in: pointer to savepoint undo number, if
                          partial rollback requested, or NULL for
                          complete rollback */
{
  que_thr_t *thr;  // 查询线程
  mem_heap_t *heap;  // 内存堆
  roll_node_t *roll_node;  // 回滚节点

  heap = mem_heap_create(512, UT_LOCATION_HERE);  // 创建内存堆

  roll_node = roll_node_create(heap);  // 创建回滚节点

  if (savept != nullptr) {  // 如果请求部分回滚
    roll_node->partial = true;  // 设置回滚节点为部分回滚
    roll_node->savept = *savept;  // 设置保存点
    check_trx_state(trx);  // 检查事务状态
  } else {  // 如果请求完全回滚
    assert_trx_nonlocking_or_in_list(trx);  // 断言事务是非锁定事务或在列表中
  }

  trx->error_state = DB_SUCCESS;  // 设置事务错误状态为成功

  if (trx_is_rseg_updated(trx)) {  // 如果事务更新了回滚段
    ut_ad(trx->rsegs.m_redo.rseg != nullptr ||
          trx->rsegs.m_noredo.rseg != nullptr);  // 断言 redo 或 noredo 回滚段存在

    thr = pars_complete_graph_for_exec(roll_node, trx, heap, nullptr);  // 完成执行图的解析

    ut_a(thr == que_fork_start_command(
                    static_cast<que_fork_t *>(que_node_get_parent(thr))));  // 启动查询线程

    que_run_threads(thr);  // 运行查询线程

    ut_a(roll_node->undo_thr != nullptr);  // 断言 undo 线程存在
    que_run_threads(roll_node->undo_thr);  // 运行 undo 线程

    /* Free the memory reserved by the undo graph. */
    /* 释放 undo 图保留的内存。 */
    que_graph_free(static_cast<que_t *>(roll_node->undo_thr->common.parent));  // 释放 undo 图
  }

  if (savept == nullptr) {  // 如果请求完全回滚
    trx_rollback_finish(trx);  // 完成事务回滚
    MONITOR_INC(MONITOR_TRX_ROLLBACK);  // 增加回滚监控计数
  } else {  // 如果请求部分回滚
    trx->lock.que_state = TRX_QUE_RUNNING;  // 设置事务队列状态为运行中
    MONITOR_INC(MONITOR_TRX_ROLLBACK_SAVEPOINT);  // 增加保存点回滚监控计数
  }

  ut_a(trx->error_state == DB_SUCCESS);  // 断言事务错误状态为成功
  ut_a(trx->lock.que_state == TRX_QUE_RUNNING);  // 断言事务队列状态为运行中

  mem_heap_free(heap);  // 释放内存堆

  /* There might be work for utility threads.*/
  /* 可能会有工作给工具线程。 */
  srv_active_wake_master_thread();  // 唤醒主线程

  MONITOR_DEC(MONITOR_TRX_ACTIVE);  // 减少活跃事务监控计数
}

/** Rollback a transaction to a given savepoint or do a complete rollback.
 @return error code or DB_SUCCESS */
dberr_t trx_rollback_to_savepoint(
    trx_t *trx,           /*!< in: transaction handle */
    trx_savept_t *savept) /*!< in: pointer to savepoint undo number, if
                          partial rollback requested, or NULL for
                          complete rollback */
{
  ut_ad(!trx_mutex_own(trx));

  trx_start_if_not_started_xa(trx, true, UT_LOCATION_HERE);

  trx_rollback_to_savepoint_low(trx, savept);

  return (trx->error_state);
}

/**
 * @brief 回滚 MySQL 中使用的事务。
 * @brief Rollback a transaction used in MySQL.
 *
 * @param trx 事务对象
 * @return 错误码或 DB_SUCCESS
 * @return error code or DB_SUCCESS
 */
static dberr_t trx_rollback_for_mysql_low(
    trx_t *trx) /*!< in/out: transaction */
{
  trx->op_info = "rollback";  // 设置操作信息为 "rollback"

  /* If we are doing the XA recovery of prepared transactions,
  then the transaction object does not have an InnoDB session
  object, and we set a dummy session that we use for all MySQL
  transactions. */
  /* 如果我们正在恢复已准备的 XA 事务，
     那么事务对象没有 InnoDB 会话对象，
     我们设置一个用于所有 MySQL 事务的虚拟会话。 */

  trx_rollback_to_savepoint_low(trx, nullptr);  // 回滚到保存点

  trx->op_info = "";  // 清空操作信息

  ut_a(trx->error_state == DB_SUCCESS);  // 断言事务状态为成功

  return (trx->error_state);  // 返回事务状态
}

/**
 * @brief 回滚 MySQL 中使用的事务。
 * @brief Rollback a transaction used in MySQL.
 *
 * @param[in, out] trx 事务对象
 * @return 错误码或 DB_SUCCESS
 * @return error code or DB_SUCCESS
 */
static dberr_t trx_rollback_low(trx_t *trx) {
  /* We are reading trx->state without mutex protection here,
  because the rollback should either be invoked for:
    - a running active MySQL transaction associated
      with the current thread,
    - or a recovered prepared transaction,
    - or a transaction which is a victim being killed by HP transaction
      run by the current thread, in which case it is guaranteed that
      thread owning the transaction, which is being killed, is not
      inside InnoDB (thanks to TRX_FORCE_ROLLBACK and TrxInInnoDB::wait()). */
  /* 我们在这里读取 trx->state 而不加互斥锁保护，
     因为回滚应该被调用用于以下情况：
    - 与当前线程关联的正在运行的活跃 MySQL 事务，
    - 或者一个恢复的已准备事务，
    - 或者一个被当前线程运行的 HP 事务杀死的受害者事务，
      在这种情况下，保证拥有该事务的线程（正在被杀死）不在 InnoDB 内部
      （感谢 TRX_FORCE_ROLLBACK 和 TrxInInnoDB::wait()）。 */
  ut_ad(trx_can_be_handled_by_current_thread_or_is_hp_victim(trx));  // 断言当前线程可以处理该事务或该事务是 HP 受害者

  switch (trx->state.load(std::memory_order_relaxed)) {  // 根据事务状态进行分支处理
    case TRX_STATE_FORCED_ROLLBACK:  // 事务被强制回滚
    case TRX_STATE_NOT_STARTED:  // 事务未启动
      trx->will_lock = 0;  // 标记事务不会持有锁
      ut_ad(trx->in_mysql_trx_list);  // 断言事务在 MySQL 事务列表中
      return (DB_SUCCESS);  // 返回成功

    case TRX_STATE_ACTIVE:  // 事务处于活跃状态
      ut_ad(trx->in_mysql_trx_list);  // 断言事务在 MySQL 事务列表中
      assert_trx_nonlocking_or_in_list(trx);  // 断言事务是非锁定事务或在列表中
      /* Check an validate that undo is available for GTID. */
      /* 检查并验证 GTID 的 undo 是否可用。 */
      trx_undo_gtid_add_update_undo(trx, false, true);  // 添加更新 undo 日志
      return (trx_rollback_for_mysql_low(trx));  // 调用低级别回滚函数

    case TRX_STATE_PREPARED:  // 事务处于准备状态
      /* Check an validate that undo is available for GTID. */
      /* 检查并验证 GTID 的 undo 是否可用。 */
      trx_undo_gtid_add_update_undo(trx, false, true);  // 添加更新 undo 日志
      ut_ad(!trx_is_autocommit_non_locking(trx));  // 断言事务不是自动提交的非锁定事务
      if (trx->rsegs.m_redo.rseg != nullptr && trx_is_redo_rseg_updated(trx)) {  // 如果事务更新了 redo 回滚段
        /* Change the undo log state back from
        TRX_UNDO_PREPARED to TRX_UNDO_ACTIVE
        so that if the system gets killed,
        recovery will perform the rollback. */
        /* 将 undo 日志的状态从 TRX_UNDO_PREPARED 改回 TRX_UNDO_ACTIVE，
           以便如果系统崩溃，恢复时将执行回滚。 */
        trx_undo_ptr_t *undo_ptr = &trx->rsegs.m_redo;

        mtr_t mtr;

        mtr.start();  // 启动 mini-transaction

        trx->rsegs.m_redo.rseg->latch();  // 加锁 redo 回滚段

        if (undo_ptr->insert_undo != nullptr) {  // 如果插入 undo 日志存在
          trx_undo_set_state_at_prepare(trx, undo_ptr->insert_undo, true, &mtr);  // 设置插入 undo 日志状态
        }

        if (undo_ptr->update_undo != nullptr) {  // 如果更新 undo 日志存在
          trx_undo_gtid_set(trx, undo_ptr->update_undo, false);  // 设置 GTID
          trx_undo_set_state_at_prepare(trx, undo_ptr->update_undo, true, &mtr);  // 设置更新 undo 日志状态
        }
        trx->rsegs.m_redo.rseg->unlatch();  // 解锁 redo 回滚段

        /* Persist the XA ROLLBACK, so that crash
        recovery will replay the rollback in case
        the redo log gets applied past this point. */
        /* 持久化 XA ROLLBACK，以便在崩溃恢复时重放回滚，
           以防 redo 日志应用超过此点。 */
        mtr.commit();  // 提交 mini-transaction
        ut_ad(mtr.commit_lsn() > 0 || !mtr_t::s_logging.is_enabled());  // 断言提交 LSN 大于 0 或日志未启用
      }
#ifdef ENABLED_DEBUG_SYNC
      if (trx->mysql_thd == nullptr) {
        /* We could be executing XA ROLLBACK after
        XA PREPARE and a server restart. */
        /* 我们可能在 XA PREPARE 和服务器重启后执行 XA ROLLBACK。 */
      } else if (!trx_is_redo_rseg_updated(trx)) {
        /* innobase_close_connection() may roll back a
        transaction that did not generate any
        persistent undo log. The DEBUG_SYNC
        would cause an assertion failure for a
        disconnected thread.

        NOTE: InnoDB will not know about the XID
        if no persistent undo log was generated. */
        /* innobase_close_connection() 可能会回滚一个未生成任何持久 undo 日志的事务。
           DEBUG_SYNC 会导致断开连接的线程断言失败。

           注意：如果没有生成持久 undo 日志，InnoDB 将不知道 XID。 */
      } else {
        DEBUG_SYNC_C("trx_xa_rollback");  // 调试同步点
      }
#endif /* ENABLED_DEBUG_SYNC */
      return (trx_rollback_for_mysql_low(trx));  // 调用低级别回滚函数

    case TRX_STATE_COMMITTED_IN_MEMORY:  // 事务已在内存中提交
      check_trx_state(trx);  // 检查事务状态
      break;
  }

  ut_error;  // 触发错误
}

/**
 * @brief 回滚 MySQL 中使用的事务。
 * @brief Rollback a transaction used in MySQL.
 *
 * @param trx 事务对象
 * @return 错误码或 DB_SUCCESS
 * @return error code or DB_SUCCESS
 */
dberr_t trx_rollback_for_mysql(trx_t *trx) /*!< in/out: transaction */
{
  /* Avoid the tracking of async rollback killer
  thread to enter into InnoDB. */
  /* 避免异步回滚 killer 线程进入 InnoDB 的跟踪。 */
  if (TrxInInnoDB::is_async_rollback(trx)) {  // 如果事务是异步回滚
    return (trx_rollback_low(trx));  // 直接调用低级别回滚函数

  } else {
    TrxInInnoDB trx_in_innodb(trx, true);  // 确保事务在 InnoDB 中
    return (trx_rollback_low(trx));  // 调用低级别回滚函数
  }
}

/** Rollback the latest SQL statement for MySQL.
 @return error code or DB_SUCCESS */
dberr_t trx_rollback_last_sql_stat_for_mysql(
    trx_t *trx) /*!< in/out: transaction */
{
  dberr_t err;

  ut_ad(trx->in_mysql_trx_list);

  /* We are reading trx->state without mutex protection here,
  because the rollback should either be invoked for:
    - a running active MySQL transaction associated
      with the current thread,
    - or a recovered prepared transaction,
    - or a transaction which is a victim being killed by HP transaction
      run by the current thread, in which case it is guaranteed that
      thread owning the transaction, which is being killed, is not
      inside InnoDB (thanks to TRX_FORCE_ROLLBACK and TrxInInnoDB::wait()). */

  ut_ad(trx_can_be_handled_by_current_thread_or_is_hp_victim(trx));

  switch (trx->state.load(std::memory_order_relaxed)) {
    case TRX_STATE_FORCED_ROLLBACK:
    case TRX_STATE_NOT_STARTED:
      return (DB_SUCCESS);

    case TRX_STATE_ACTIVE:
      assert_trx_nonlocking_or_in_list(trx);

      trx->op_info = "rollback of SQL statement";

      err = trx_rollback_to_savepoint(trx, &trx->last_sql_stat_start);

      if (trx->fts_trx != nullptr) {
        fts_savepoint_rollback_last_stmt(trx);
      }

      /* The following call should not be needed,
      but we play it safe: */
      trx_mark_sql_stat_end(trx);

      trx->op_info = "";

      return (err);

    case TRX_STATE_PREPARED:
    case TRX_STATE_COMMITTED_IN_MEMORY:
      /* The statement rollback is only allowed on an ACTIVE
      transaction, not a PREPARED or COMMITTED one. */
      break;
  }

  ut_error;
}

/** Search for a savepoint using name.
 @return savepoint if found else NULL */
static trx_named_savept_t *trx_savepoint_find(
    trx_t *trx,       /*!< in: transaction */
    const char *name) /*!< in: savepoint name */
{
  for (auto savep : trx->trx_savepoints) {
    if (0 == ut_strcmp(savep->name, name)) {
      return (savep);
    }
  }

  return (nullptr);
}

/** Frees a single savepoint struct. */
static void trx_roll_savepoint_free(
  trx_t *trx,                /*!< in: transaction handle */
  trx_named_savept_t *savep) /*!< in: savepoint to free */
{
UT_LIST_REMOVE(trx->trx_savepoints, savep);  // 从事务的保存点列表中移除指定的保存点

ut::free(savep->name);  // 释放保存点的名称内存
ut::free(savep);  // 释放保存点结构体的内存
}

/** Frees savepoint structs starting from savep.
@param[in] trx Transaction handle
@param[in] savep Free all savepoints starting with this savepoint i, if savep is
nullptr free all save points */
void trx_roll_savepoints_free(trx_t *trx, trx_named_savept_t *savep) {
  while (savep != nullptr) {  // 当 savep 不为空时继续循环
    trx_named_savept_t *next_savep;  // 定义下一个保存点指针

    next_savep = UT_LIST_GET_NEXT(trx_savepoints, savep);  // 获取下一个保存点

    trx_roll_savepoint_free(trx, savep);  // 释放当前保存点

    savep = next_savep;  // 将当前保存点设置为下一个保存点
  }
}

/** Rolls back a transaction back to a named savepoint. Modifications after the
 savepoint are undone but InnoDB does NOT release the corresponding locks
 which are stored in memory. If a lock is 'implicit', that is, a new inserted
 row holds a lock where the lock information is carried by the trx id stored in
 the row, these locks are naturally released in the rollback. Savepoints which
 were set after this savepoint are deleted.
 @return if no savepoint of the name found then DB_NO_SAVEPOINT,
 otherwise DB_SUCCESS */
[[nodiscard]] static dberr_t trx_rollback_to_savepoint_for_mysql_low(
    trx_t *trx,                /*!< in/out: transaction */
    trx_named_savept_t *savep, /*!< in/out: savepoint */
    int64_t *mysql_binlog_cache_pos)
/*!< out: the MySQL binlog
cache position corresponding
to this savepoint; MySQL needs
this information to remove the
binlog entries of the queries
executed after the savepoint */
{
  dberr_t err;

  ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
  ut_ad(trx->in_mysql_trx_list);

  /* Free all savepoints strictly later than savep. */

  trx_roll_savepoints_free(trx, UT_LIST_GET_NEXT(trx_savepoints, savep));

  *mysql_binlog_cache_pos = savep->mysql_binlog_cache_pos;

  trx->op_info = "rollback to a savepoint";

  err = trx_rollback_to_savepoint(trx, &savep->savept);

  /* Store the current undo_no of the transaction so that
  we know where to roll back if we have to roll back the
  next SQL statement: */

  trx_mark_sql_stat_end(trx);

  trx->op_info = "";

  return (err);
}

/** Rolls back a transaction back to a named savepoint. Modifications after the
 savepoint are undone but InnoDB does NOT release the corresponding locks
 which are stored in memory. If a lock is 'implicit', that is, a new inserted
 row holds a lock where the lock information is carried by the trx id stored in
 the row, these locks are naturally released in the rollback. Savepoints which
 were set after this savepoint are deleted.
 @return if no savepoint of the name found then DB_NO_SAVEPOINT,
 otherwise DB_SUCCESS */
dberr_t trx_rollback_to_savepoint_for_mysql(
    trx_t *trx,                      /*!< in: transaction handle */
    const char *savepoint_name,      /*!< in: savepoint name */
    int64_t *mysql_binlog_cache_pos) /*!< out: the MySQL binlog cache
                                     position corresponding to this
                                     savepoint; MySQL needs this
                                     information to remove the
                                     binlog entries of the queries
                                     executed after the savepoint */
{
  trx_named_savept_t *savep;

  ut_ad(trx->in_mysql_trx_list);

  savep = trx_savepoint_find(trx, savepoint_name);

  if (savep == nullptr) {
    return (DB_NO_SAVEPOINT);
  }

  /* We are reading trx->state without mutex protection here,
  because the rollback should either be invoked for:
    - a running active MySQL transaction associated
      with the current thread,
    - or a recovered prepared transaction,
    - or a transaction which is a victim being killed by HP transaction
      run by the current thread, in which case it is guaranteed that
      thread owning the transaction, which is being killed, is not
      inside InnoDB (thanks to TRX_FORCE_ROLLBACK and TrxInInnoDB::wait()). */

  ut_ad(trx_can_be_handled_by_current_thread_or_is_hp_victim(trx));

  switch (trx->state.load(std::memory_order_relaxed)) {
    case TRX_STATE_NOT_STARTED:
    case TRX_STATE_FORCED_ROLLBACK:

      ib::error(ER_IB_MSG_1185) << "Transaction has a savepoint " << savep->name
                                << " though it is not started";

      return (DB_ERROR);

    case TRX_STATE_ACTIVE:

      return (trx_rollback_to_savepoint_for_mysql_low(trx, savep,
                                                      mysql_binlog_cache_pos));

    case TRX_STATE_PREPARED:
    case TRX_STATE_COMMITTED_IN_MEMORY:
      /* The savepoint rollback is only allowed on an ACTIVE
      transaction, not a PREPARED or COMMITTED one. */
      break;
  }

  ut_error;
}

/** Creates a named savepoint. If the transaction is not yet started, starts it.
 If there is already a savepoint of the same name, this call erases that old
 savepoint and replaces it with a new. Savepoints are deleted in a transaction
 commit or rollback.
 @return always DB_SUCCESS */
dberr_t trx_savepoint_for_mysql(
    trx_t *trx,                 /*!< in: transaction handle */
    const char *savepoint_name, /*!< in: savepoint name */
    int64_t binlog_cache_pos)   /*!< in: MySQL binlog cache
                                position corresponding to this
                                connection at the time of the
                                savepoint */
{
  trx_named_savept_t *savep;

  trx_start_if_not_started_xa(trx, false, UT_LOCATION_HERE);

  savep = trx_savepoint_find(trx, savepoint_name);

  if (savep) {
    /* There is a savepoint with the same name: free that */

    UT_LIST_REMOVE(trx->trx_savepoints, savep);

    ut::free(savep->name);
    ut::free(savep);
  }

  /* Create a new savepoint and add it as the last in the list */

  savep = static_cast<trx_named_savept_t *>(
      ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(*savep)));

  savep->name = mem_strdup(savepoint_name);

  savep->savept = trx_savept_take(trx);

  savep->mysql_binlog_cache_pos = binlog_cache_pos;

  UT_LIST_ADD_LAST(trx->trx_savepoints, savep);

  return (DB_SUCCESS);
}

/** Releases only the named savepoint. Savepoints which were set after this
 savepoint are left as is.
 @return if no savepoint of the name found then DB_NO_SAVEPOINT,
 otherwise DB_SUCCESS */
dberr_t trx_release_savepoint_for_mysql(
    trx_t *trx,                 /*!< in: transaction handle */
    const char *savepoint_name) /*!< in: savepoint name */
{
  trx_named_savept_t *savep;

  ut_ad(trx_state_eq(trx, TRX_STATE_ACTIVE));
  ut_ad(trx->in_mysql_trx_list);

  savep = trx_savepoint_find(trx, savepoint_name);

  if (savep != nullptr) {
    trx_roll_savepoint_free(trx, savep);
  }

  return (savep != nullptr ? DB_SUCCESS : DB_NO_SAVEPOINT);
}

/** Determines if this transaction is rolling back an incomplete transaction
 in crash recovery.
 @return true if trx is an incomplete transaction that is being rolled
 back in crash recovery */
bool trx_is_recv(const trx_t *trx) /*!< in: transaction */
{
  return (trx == trx_roll_crash_recv_trx);
}

/** Returns a transaction savepoint taken at this point in time.
 @return savepoint */
trx_savept_t trx_savept_take(trx_t *trx) /*!< in: transaction */
{
  trx_savept_t savept;

  savept.least_undo_no = trx->undo_no;

  return (savept);
}

/** Roll back an active transaction. */
/** 回滚一个活动事务。 */
static void trx_rollback_active(trx_t *trx) /*!< in/out: transaction */ // 事务
{
  mem_heap_t *heap; // 内存堆
  que_fork_t *fork; // 查询叉
  que_thr_t *thr; // 查询线程
  roll_node_t *roll_node; // 回滚节点
  int64_t rows_to_undo; // 需要撤销的行数
  const char *unit = ""; // 单位

  heap = mem_heap_create(512, UT_LOCATION_HERE); // 创建内存堆

  fork = que_fork_create(nullptr, nullptr, QUE_FORK_RECOVERY, heap); // 创建查询叉
  fork->trx = trx; // 设置事务

  thr = que_thr_create(fork, heap, nullptr); // 创建查询线程

  roll_node = roll_node_create(heap); // 创建回滚节点

  thr->child = roll_node; // 设置查询线程的子节点
  roll_node->common.parent = thr; // 设置回滚节点的父节点

  trx->graph = fork; // 设置事务的图

  ut_a(thr == que_fork_start_command(fork)); // 断言查询线程等于查询叉启动命令

  trx_sys_mutex_enter(); // 进入事务系统互斥锁

  trx_roll_crash_recv_trx = trx; // 设置回滚崩溃接收事务

  trx_roll_max_undo_no = trx->undo_no; // 设置最大撤销号

  trx_roll_progress_printed_pct = 0; // 设置回滚进度打印百分比

  rows_to_undo = trx_roll_max_undo_no; // 设置需要撤销的行数

  trx_sys_mutex_exit(); // 退出事务系统互斥锁

  if (rows_to_undo > 1000000000) { // 如果需要撤销的行数大于 10 亿
    rows_to_undo = rows_to_undo / 1000000; // 将行数除以 100 万
    unit = "M"; // 设置单位为百万
  }

  const trx_id_t trx_id = trx_get_id_for_print(trx); // 获取事务 ID

  ib::info(ER_IB_MSG_1186) << "Rolling back trx with id " << trx_id << ", "
                           << rows_to_undo << unit << " rows to undo"; // 打印回滚信息

  que_run_threads(thr); // 运行查询线程
  ut_a(roll_node->undo_thr != nullptr); // 断言回滚节点的撤销线程不为空

  que_run_threads(roll_node->undo_thr); // 运行回滚节点的撤销线程

  trx_rollback_finish(thr_get_trx(roll_node->undo_thr)); // 完成事务回滚

  /* Free the memory reserved by the undo graph */
  /* 释放撤销图保留的内存 */
  que_graph_free(static_cast<que_t *>(roll_node->undo_thr->common.parent)); // 释放查询图

  ut_a(trx->lock.que_state == TRX_QUE_RUNNING); // 断言事务的锁查询状态为运行

  ib::info(ER_IB_MSG_1187) << "Rollback of trx with id " << trx_id
                           << " completed"; // 打印回滚完成信息

  mem_heap_free(heap); // 释放内存堆

  trx_roll_crash_recv_trx = nullptr; // 设置回滚崩溃接收事务为空
}

/** Rollback or clean up any resurrected incomplete transactions. It assumes
 that the caller holds the trx_sys_t::mutex and it will release the
 lock if it does a clean up or rollback.
 @return true if the transaction was cleaned up or rolled back
 and trx_sys->mutex was released. */
/** 回滚或清理任何恢复的不完整事务。假设调用者持有 trx_sys_t::mutex，如果进行清理或回滚，它将释放锁。
 @return 如果事务被清理或回滚并且 trx_sys->mutex 被释放，则返回 true。 */
static bool trx_rollback_or_clean_resurrected(
    trx_t *trx, /*!< in: transaction to rollback or clean */ // 事务
    bool all)   /*!< in: false=roll back dictionary transactions;
                 true=roll back all non-PREPARED transactions */ // 是否回滚所有非预处理事务
{
  ut_ad(trx_sys_mutex_own()); // 断言持有 trx_sys 互斥锁
  ut_ad(trx->in_rw_trx_list); // 断言事务在读写事务列表中

  /* Generally, an HA transaction with is_recovered && state==TRX_STATE_PREPARED
  can be committed or rolled back by a client who knows its XID at any time.
  To prove that no such state transition is possible while our thread operates,
  observe that we hold trx_sys->mutex which is required by both commit and
  rollback to deregister the trx from trx_sys->rw_trx_list during
  trx_release_impl_and_expl_locks() and we see the trx is still in this list.
  Thus, if we see is_recovered==true, then the state can not change until we
  release the trx_sys->mutex. Moreover for TRX_STATE_PREPARED we do nothing, so
  we will not interfere with an HA COMMIT or ROLLBACK. So, if XA ROLLBACK or
  COMMIT latches trx_sys->mutex before us, then we will not see the trx in the
  rw_trx_list (so trx_rollback_or_clean_resurrected() would not be called for
  this transaction in the first place), and if we latch first, then we will
  leave the trx intact. */
  /* 通常，具有 is_recovered && state==TRX_STATE_PREPARED 的 HA 事务可以由知道其 XID 的客户端随时提交或回滚。
  为了证明在我们的线程操作时没有这种状态转换的可能性，请注意我们持有 trx_sys->mutex，
  这在提交和回滚期间都需要从 trx_sys->rw_trx_list 中注销事务，并且我们看到事务仍在此列表中。
  因此，如果我们看到 is_recovered==true，那么状态在我们释放 trx_sys->mutex 之前不会改变。
  此外，对于 TRX_STATE_PREPARED，我们什么都不做，因此我们不会干扰 HA COMMIT 或 ROLLBACK。
  因此，如果 XA ROLLBACK 或 COMMIT 在我们之前锁定 trx_sys->mutex，那么我们将不会在 rw_trx_list 中看到事务
  （因此 trx_rollback_or_clean_resurrected() 根本不会被调用），如果我们先锁定，那么我们将保持事务不变。 */

  trx_mutex_enter(trx); // 进入事务互斥锁
  const bool is_recovered = trx->is_recovered; // 获取事务是否已恢复
  const trx_state_t state = trx->state.load(std::memory_order_relaxed); // 获取事务状态
  trx_mutex_exit(trx); // 退出事务互斥锁

  if (!is_recovered) { // 如果事务未恢复
    ut_ad(state != TRX_STATE_COMMITTED_IN_MEMORY); // 断言事务状态不是内存中已提交
    return false; // 返回 false
  }

  switch (state) {
    case TRX_STATE_COMMITTED_IN_MEMORY: // 内存中已提交状态
      trx_sys_mutex_exit(); // 退出事务系统互斥锁
      ib::info(ER_IB_MSG_1188)
          << "Cleaning up trx with id " << trx_get_id_for_print(trx); // 打印清理事务信息

      trx_cleanup_at_db_startup(trx); // 在数据库启动时清理事务
      trx_free_resurrected(trx); // 释放恢复的事务
      ut_ad(!trx->is_recovered); // 断言事务未恢复
      return true; // 返回 true
    case TRX_STATE_ACTIVE: // 活动状态
      if (all || trx->ddl_operation) { // 如果回滚所有或事务为 DDL 操作
        trx_sys_mutex_exit(); // 退出事务系统互斥锁
        trx_rollback_active(trx); // 回滚活动事务
        trx_free_for_background(trx); // 释放后台事务
        ut_ad(!trx->is_recovered); // 断言事务未恢复
        return true; // 返回 true
      }
      return false; // 返回 false
    case TRX_STATE_PREPARED: // 预处理状态
      return false; // 返回 false
    case TRX_STATE_NOT_STARTED: // 未开始状态
    case TRX_STATE_FORCED_ROLLBACK: // 强制回滚状态
      break; // 跳出 switch 语句
  }

  ut_error; // 触发错误：ut_error 是一个宏，用于触发一个错误断言，表示代码执行到了不应该到达的地方
}

/** Rollback or clean up any incomplete transactions which were
 encountered in crash recovery.  If the transaction already was
 committed, then we clean up a possible insert undo log. If the
 transaction was not yet committed, then we roll it back. */
/** 回滚或清理在崩溃恢复中遇到的任何不完整事务。如果事务已经提交，则清理可能的插入撤销日志。
 如果事务尚未提交，则回滚它。 */
void trx_rollback_or_clean_recovered(
    bool all) /*!< in: false=roll back dictionary transactions;
               true=roll back all non-PREPARED transactions */ // 是否回滚所有非预处理事务
{
  ut_ad(!srv_read_only_mode); // 断言不是只读模式

  ut_a(srv_force_recovery < SRV_FORCE_NO_TRX_UNDO); // 断言强制恢复级别小于不撤销事务
  ut_ad(!all || trx_sys_need_rollback()); // 断言需要回滚

  if (all) {
    ib::info(ER_IB_MSG_1189) << "Starting in background the rollback"
                                " of uncommitted transactions"; // 打印开始回滚未提交事务的信息
  }

  /* Note: For XA recovered transactions, we rely on MySQL to
  do rollback. They will be in TRX_STATE_PREPARED state. If the server
  is shutdown and they are still lingering in trx_sys_t::trx_list
  then the shutdown will hang. */
  /* 注意：对于 XA 恢复的事务，我们依赖 MySQL 进行回滚。它们将处于 TRX_STATE_PREPARED 状态。
  如果服务器关闭并且它们仍然停留在 trx_sys_t::trx_list 中，则关闭将挂起。 */

  /* Loop over the transaction list as long as there are
  recovered transactions to clean up or recover. */
  /* 只要有恢复的事务需要清理或恢复，就循环遍历事务列表。 */

  trx_sys_mutex_enter(); // 进入事务系统互斥锁
  for (bool need_one_more_scan = true; need_one_more_scan;) { // 循环遍历事务列表
    need_one_more_scan = false; // 重置需要再次扫描标志
    for (auto trx : trx_sys->rw_trx_list) { // 遍历读写事务列表
      assert_trx_in_rw_list(trx); // 断言事务在读写列表中

      /* In case of slow shutdown, we have to wait for the background
      thread (trx_recovery_rollback) which is doing the rollbacks of
      recovered transactions. Note that it can add undo to purge.
      In case of fast shutdown we do not care if we left transactions
      not rolled back. But still we want to stop the thread, so since
      certain point of shutdown we might be sure there are no changes
      to transactions / undo. */
      /* 在慢速关闭的情况下，我们必须等待正在回滚恢复事务的后台线程（trx_recovery_rollback）。
      请注意，它可以将撤销添加到清除中。在快速关闭的情况下，我们不关心是否留下未回滚的事务。
      但我们仍然希望停止线程，因此从关闭的某个时间点起，我们可以确保事务/撤销没有变化。 */
      if (srv_shutdown_state.load() >= SRV_SHUTDOWN_RECOVERY_ROLLBACK &&
          srv_fast_shutdown != 0) { // 如果关闭状态大于等于恢复回滚且快速关闭不为 0
        ut_a(srv_shutdown_state_matches([](auto state) {
          return state == SRV_SHUTDOWN_RECOVERY_ROLLBACK ||
                 state == SRV_SHUTDOWN_EXIT_THREADS;
        })); // 断言关闭状态匹配

        trx_sys_mutex_exit(); // 退出事务系统互斥锁

        if (all) {
          ib::info(ER_IB_MSG_TRX_RECOVERY_ROLLBACK_NOT_COMPLETED); // 打印回滚未完成信息
        }
        return; // 返回
      }

      /* If this function does a cleanup or rollback
      then it will release the trx_sys->mutex, therefore
      we need to reacquire it before retrying the loop. */
      /* 如果此函数进行清理或回滚，则它将释放 trx_sys->mutex，因此在重试循环之前需要重新获取它。 */
      if (trx_rollback_or_clean_resurrected(trx, all)) { // 如果回滚或清理恢复的事务
        trx_sys_mutex_enter(); // 进入事务系统互斥锁
        need_one_more_scan = true; // 设置需要再次扫描标志
        break; // 跳出循环
      }
    }
  }
  trx_sys_mutex_exit(); // 退出事务系统互斥锁

  if (all) {
    ib::info(ER_IB_MSG_TRX_RECOVERY_ROLLBACK_COMPLETED); // 打印回滚完成信息
  }
}

/** Rollback or clean up any incomplete transactions which were
encountered in crash recovery.  If the transaction already was
committed, then we clean up a possible insert undo log. If the
transaction was not yet committed, then we roll it back.
Note: this is done in a background thread. */
/** 回滚或清理在崩溃恢复中遇到的任何不完整事务。如果事务已经提交，则清理可能的插入撤销日志。
 如果事务尚未提交，则回滚它。
 注意：这是在后台线程中完成的。 */
void trx_recovery_rollback_thread() {
  THD *thd = create_internal_thd(); // 创建内部线程

  ut_ad(!srv_read_only_mode); // 断言不是只读模式

  while (DBUG_EVALUATE_IF("pause_rollback_on_recovery", true, false)) { // 调试代码，暂停回滚
    if (srv_shutdown_state.load() >= SRV_SHUTDOWN_RECOVERY_ROLLBACK) { // 如果关闭状态大于等于恢复回滚
      break; // 跳出循环
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 休眠 1 毫秒
  }

  trx_rollback_or_clean_recovered(true); // 回滚或清理恢复的事务

  destroy_internal_thd(thd); // 销毁内部线程
}

/** Tries truncate the undo logs. */
static void trx_roll_try_truncate(
    trx_t *trx,               /*!< in/out: transaction */
    trx_undo_ptr_t *undo_ptr) /*!< in: rollback segment to look
                              for next undo log record. */
{
  ut_ad(mutex_own(&trx->undo_mutex));
  ut_ad(mutex_own(&undo_ptr->rseg->mutex));

  trx->pages_undone = 0;

  if (undo_ptr->insert_undo) {
    trx_undo_truncate_end(trx, undo_ptr->insert_undo, trx->undo_no);
  }

  if (undo_ptr->update_undo) {
    trx_undo_truncate_end(trx, undo_ptr->update_undo, trx->undo_no);
  }
}

/** Pops the topmost undo log record in a single undo log and updates the info
about the topmost record in the undo log memory struct.
@param[in]      trx             transaction
@param[in]      undo            undo log
@param[in]      mtr             mtr
@param[out]     undo_offset     offset of undo record in the page
@return Undo page where undo log record resides, the page s-latched */
static const page_t *trx_roll_pop_top_rec(trx_t *trx, trx_undo_t *undo,
                                          mtr_t *mtr, uint32_t *undo_offset) {
  ut_ad(mutex_own(&trx->undo_mutex));

  const page_t *undo_page = trx_undo_page_get_s_latched(
      page_id_t(undo->space, undo->top_page_no), undo->page_size, mtr);

  *undo_offset = static_cast<uint32_t>(undo->top_offset);

  trx_undo_rec_t *prev_rec =
      trx_undo_get_prev_rec((trx_undo_rec_t *)(undo_page + *undo_offset),
                            undo->hdr_page_no, undo->hdr_offset, true, mtr);

  if (prev_rec == nullptr) {
    undo->empty = true;
  } else {
    page_t *prev_rec_page = page_align(prev_rec);

    if (prev_rec_page != undo_page) {
      trx->pages_undone++;
    }

    undo->top_page_no = page_get_page_no(prev_rec_page);
    undo->top_offset = prev_rec - prev_rec_page;
    undo->top_undo_no = trx_undo_rec_get_undo_no(prev_rec);
  }

  return (undo_page);
}

/** Pops the topmost record when the two undo logs of a transaction are seen
 as a single stack of records ordered by their undo numbers.
 @return undo log record copied to heap, NULL if none left, or if the
 undo number of the top record would be less than the limit */
static trx_undo_rec_t *trx_roll_pop_top_rec_of_trx_low(
    trx_t *trx,               /*!< in/out: transaction */
    trx_undo_ptr_t *undo_ptr, /*!< in: rollback segment to look
                              for next undo log record. */
    undo_no_t limit,          /*!< in: least undo number we need */
    roll_ptr_t *roll_ptr,     /*!< out: roll pointer to undo record */
    mem_heap_t *heap)         /*!< in/out: memory heap where copied */
{
  trx_undo_t *undo;
  trx_undo_t *ins_undo;
  trx_undo_t *upd_undo;
  trx_undo_rec_t *undo_rec_copy;
  const page_t *undo_page;
  undo_no_t undo_no;
  uint32_t undo_offset;
  trx_rseg_t *rseg;
  mtr_t mtr;

  rseg = undo_ptr->rseg;

  mutex_enter(&trx->undo_mutex);

  if (trx->pages_undone >= TRX_ROLL_TRUNC_THRESHOLD) {
    rseg->latch();

    trx_roll_try_truncate(trx, undo_ptr);

    rseg->unlatch();
  }

  ins_undo = undo_ptr->insert_undo;
  upd_undo = undo_ptr->update_undo;

  if (!ins_undo || ins_undo->empty) {
    undo = upd_undo;
  } else if (!upd_undo || upd_undo->empty) {
    undo = ins_undo;
  } else if (upd_undo->top_undo_no > ins_undo->top_undo_no) {
    undo = upd_undo;
  } else {
    undo = ins_undo;
  }

  if (!undo || undo->empty || limit > undo->top_undo_no) {
    rseg->latch();
    trx_roll_try_truncate(trx, undo_ptr);
    rseg->unlatch();
    mutex_exit(&trx->undo_mutex);
    return (nullptr);
  }

  auto is_insert = (undo == ins_undo);

  *roll_ptr = trx_undo_build_roll_ptr(is_insert, undo->rseg->space_id,
                                      undo->top_page_no, undo->top_offset);

  mtr_start(&mtr);

  undo_page = trx_roll_pop_top_rec(trx, undo, &mtr, &undo_offset);

  undo_no = trx_undo_rec_get_undo_no(undo_page + undo_offset);

  ut_ad(trx_roll_check_undo_rec_ordering(undo_no, undo->rseg->space_id, trx));

  /* We print rollback progress info if we are in a crash recovery
  and the transaction has at least 1000 row operations to undo. */

  if (trx == trx_roll_crash_recv_trx && trx_roll_max_undo_no > 1000) {
    ulint progress_pct = 100 - (ulint)((undo_no * 100) / trx_roll_max_undo_no);
    if (progress_pct != trx_roll_progress_printed_pct) {
      if (trx_roll_progress_printed_pct == 0) {
        fprintf(stderr,
                "\nInnoDB: Progress in percents:"
                " %lu",
                (ulong)progress_pct);
      } else {
        fprintf(stderr, " %lu", (ulong)progress_pct);
      }
      fflush(stderr);
      trx_roll_progress_printed_pct = progress_pct;
    }
  }

  trx->undo_no = undo_no;
  trx->undo_rseg_space = undo->rseg->space_id;

  undo_rec_copy =
      trx_undo_rec_copy(undo_page, static_cast<uint32_t>(undo_offset), heap);

  mutex_exit(&trx->undo_mutex);

  mtr_commit(&mtr);

  return (undo_rec_copy);
}

/** Get next undo log record from redo and noredo rollback segments.
 @return undo log record copied to heap, NULL if none left, or if the
 undo number of the top record would be less than the limit */
trx_undo_rec_t *trx_roll_pop_top_rec_of_trx(
    trx_t *trx,           /*!< in: transaction */
    undo_no_t limit,      /*!< in: least undo number we need */
    roll_ptr_t *roll_ptr, /*!< out: roll pointer to undo record */
    mem_heap_t *heap)     /*!< in: memory heap where copied */
{
  trx_undo_rec_t *undo_rec = nullptr;

  if (trx_is_redo_rseg_updated(trx)) {
    undo_rec = trx_roll_pop_top_rec_of_trx_low(trx, &trx->rsegs.m_redo, limit,
                                               roll_ptr, heap);
  }

  if (undo_rec == nullptr && trx_is_temp_rseg_updated(trx)) {
    undo_rec = trx_roll_pop_top_rec_of_trx_low(trx, &trx->rsegs.m_noredo, limit,
                                               roll_ptr, heap);
  }

  return (undo_rec);
}

/** Builds an undo 'query' graph for a transaction. The actual rollback is
 performed by executing this query graph like a query subprocedure call.
 The reply about the completion of the rollback will be sent by this
 graph.
@param[in,out]  trx                     transaction
@param[in]      partial_rollback        true if partial rollback
@return the query graph */
static que_t *trx_roll_graph_build(trx_t *trx, bool partial_rollback) {
  mem_heap_t *heap;  // 定义内存堆指针
  que_fork_t *fork;  // 定义查询分叉指针
  que_thr_t *thr;  // 定义查询线程指针

  ut_ad(trx_mutex_own(trx));  // 断言当前线程持有事务的互斥锁

  heap = mem_heap_create(512, UT_LOCATION_HERE);  // 创建内存堆
  fork = que_fork_create(nullptr, nullptr, QUE_FORK_ROLLBACK, heap);  // 创建查询分叉
  fork->trx = trx;  // 将事务赋值给查询分叉的事务字段

  thr = que_thr_create(fork, heap, nullptr);  // 创建查询线程

  thr->child = row_undo_node_create(trx, thr, heap, partial_rollback);  // 创建撤销节点并赋值给查询线程的子节点

  return (fork);  // 返回查询分叉
}

/** Starts a rollback operation, creates the UNDO graph that will do the
 actual undo operation.
@param[in]      trx     transaction
@param[in]      roll_limit       rollback to undo no (for
                                 partial undo), 0 if we are rolling back
                                 the entire transaction
@param[in]      partial_rollback true if partial rollback
@return query graph thread that will perform the UNDO operations. */
static que_thr_t *trx_rollback_start(trx_t *trx, ib_id_t roll_limit,
                                     bool partial_rollback) {
  ut_ad(trx_mutex_own(trx));  // 断言当前线程持有事务的互斥锁

  /* Initialize the rollback field in the transaction */  // 初始化事务中的回滚字段

  ut_ad(!trx->roll_limit);  // 断言事务的回滚限制为 0
  ut_ad(!trx->in_rollback);  // 断言事务不在回滚中

  trx->roll_limit = roll_limit;  // 设置事务的回滚限制
  ut_d(trx->in_rollback = true);  // 在调试模式下设置事务的回滚标志为 true

  ut_a(trx->roll_limit <= trx->undo_no);  // 断言回滚限制小于等于事务的 undo 编号

  trx->pages_undone = 0;  // 初始化已撤销的页面数为 0

  /* Build a 'query' graph which will perform the undo operations */  // 构建一个执行撤销操作的查询图

  que_t *roll_graph = trx_roll_graph_build(trx, partial_rollback);  // 构建回滚图

  trx->graph = roll_graph;  // 将回滚图赋值给事务的 graph 字段

  trx->lock.que_state = TRX_QUE_ROLLING_BACK;  // 设置事务的锁队列状态为回滚中

  return (que_fork_start_command(roll_graph));  // 启动回滚图的命令并返回查询线程
}

/** Finishes a transaction rollback. */
static void trx_rollback_finish(trx_t *trx) /*!< in: transaction */
{
  trx_commit(trx);  // 提交事务

  trx->mod_tables.clear();  // 清空事务的修改表集合

  trx->lock.que_state = TRX_QUE_RUNNING;  // 将事务的锁队列状态设置为运行中
}

/** Creates a rollback command node struct.
 @return own: rollback node struct */
roll_node_t *roll_node_create(
    mem_heap_t *heap) /*!< in: mem heap where created */
{
  roll_node_t *node;

  node = static_cast<roll_node_t *>(mem_heap_zalloc(heap, sizeof(*node)));

  node->state = ROLL_NODE_SEND;

  node->common.type = QUE_NODE_ROLLBACK;

  return (node);
}

/**
 * @brief 在查询图中执行回滚命令节点的一个步骤。
 * @brief Performs an execution step for a rollback command node in a query graph.
 *
 * @return 下一个要运行的查询线程，或 NULL
 * @return query thread to run next, or NULL
 */
que_thr_t *trx_rollback_step(que_thr_t *thr) /*!< in: 查询线程 */ /*!< in: query thread */
{
  roll_node_t *node;  // 回滚节点

  node = static_cast<roll_node_t *>(thr->run_node);  // 获取当前运行的节点

  ut_ad(que_node_get_type(node) == QUE_NODE_ROLLBACK);  // 断言：确保节点类型是回滚节点

  if (thr->prev_node == que_node_get_parent(node)) {
    // 如果前一个节点是回滚节点的父节点，设置回滚节点的状态为发送状态
    node->state = ROLL_NODE_SEND;
  }

  if (node->state == ROLL_NODE_SEND) {
    // 如果回滚节点处于发送状态
    trx_t *trx;  // 事务对象
    ib_id_t roll_limit;  // 回滚限制

    trx = thr_get_trx(thr);  // 获取查询线程所属的事务

    trx_mutex_enter(trx);  // 进入事务的互斥锁

    node->state = ROLL_NODE_WAIT;  // 设置回滚节点的状态为等待状态

    ut_a(node->undo_thr == nullptr);  // 断言：确保回滚节点的 undo 线程为空

    roll_limit = node->partial ? node->savept.least_undo_no : 0;  // 设置回滚限制

    trx_commit_or_rollback_prepare(trx);  // 准备事务的提交或回滚

    node->undo_thr = trx_rollback_start(trx, roll_limit, node->partial);  // 启动回滚操作

    trx_mutex_exit(trx);  // 退出事务的互斥锁

  } else {
    // 如果回滚节点处于等待状态
    ut_ad(node->state == ROLL_NODE_WAIT);  // 断言：确保回滚节点处于等待状态

    thr->run_node = que_node_get_parent(node);  // 设置下一个要运行的节点为回滚节点的父节点
  }

  return (thr);  // 返回下一个要运行的查询线程
}
