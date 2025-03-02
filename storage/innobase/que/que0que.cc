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

/** @file que/que0que.cc
 Query graph

 Created 5/27/1996 Heikki Tuuri
 *******************************************************/

#include <stddef.h>

#include "dict0crea.h"
#include "eval0eval.h"
#include "eval0proc.h"
#include "ha_prototypes.h"
#include "lock0lock.h"
#include "log0chkp.h"
#include "pars0types.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0purge.h"
#include "row0sel.h"
#include "row0undo.h"
#include "row0upd.h"
#include "trx0roll.h"
#include "trx0trx.h"
#include "usr0sess.h"

#include "my_dbug.h"


/**
 * @file que0que.cc
 * @brief Query graph execution and management in InnoDB.
 * @brief InnoDB 中查询图的执行和管理。
 *
 * This file contains the implementation of query graph execution and management
 * in InnoDB. Query graphs are used to represent and execute SQL queries,
 * including stored procedures, loops, and control structures.
 * 该文件包含了 InnoDB 中查询图执行和管理的实现。查询图用于表示和执行 SQL 查询，
 * 包括存储过程、循环和控制结构。
 */

/* Short introduction to query graphs
   ==================================

A query graph consists of nodes linked to each other in various ways. The
execution starts at que_run_threads() which takes a que_thr_t parameter.
que_thr_t contains two fields that control query graph execution: run_node
and prev_node. run_node is the next node to execute and prev_node is the
last node executed.

查询图由以各种方式相互连接的节点组成。执行从 que_run_threads() 开始，该函数接受一个 que_thr_t 参数。
que_thr_t 包含两个控制查询图执行的字段：run_node 和 prev_node。run_node 是下一个要执行的节点，
prev_node 是最后执行的节点。

Each node has a pointer to a 'next' statement, i.e., its brother, and a
pointer to its parent node. The next pointer is NULL in the last statement
of a block.

每个节点都有一个指向“下一个”语句的指针（即它的兄弟节点），以及一个指向其父节点的指针。
在块的最后一个语句中，next 指针为 NULL。

Loop nodes contain a link to the first statement of the enclosed statement
list. While the loop runs, que_thr_step() checks if execution to the loop
node came from its parent or from one of the statement nodes in the loop. If
it came from the parent of the loop node it starts executing the first
statement node in the loop. If it came from one of the statement nodes in
the loop, then it checks if the statement node has another statement node
following it, and runs it if so.

循环节点包含指向其内部语句列表的第一个语句的链接。当循环运行时，que_thr_step() 会检查执行是从循环节点的父节点
还是从循环内部的某个语句节点返回的。如果是从循环节点的父节点返回的，则开始执行循环中的第一个语句节点。
如果是从循环内部的某个语句节点返回的，则检查该语句节点是否有后续的语句节点，如果有则执行它。

To signify loop ending, the loop statements (see e.g. while_step()) set
que_thr_t->run_node to the loop node's parent node. This is noticed on the
next call of que_thr_step() and execution proceeds to the node pointed to by
the loop node's 'next' pointer.

为了表示循环结束，循环语句（例如 while_step()）将 que_thr_t->run_node 设置为循环节点的父节点。
这会在下一次调用 que_thr_step() 时被注意到，执行将继续到循环节点的 'next' 指针所指向的节点。

For example, the code:

例如，以下代码：

X := 1;
WHILE X < 5 LOOP
 X := X + 1;
 X := X + 1;
X := 5

will result in the following node hierarchy, with the X-axis indicating
'next' links and the Y-axis indicating parent/child links:

将生成以下节点层次结构，X 轴表示 'next' 链接，Y 轴表示父子链接：

A - W - A
    |
    |
    A - A

A = assign_node_t, W = while_node_t. */

/* How a stored procedure containing COMMIT or ROLLBACK commands
is executed?

包含 COMMIT 或 ROLLBACK 命令的存储过程是如何执行的？

The commit or rollback can be seen as a subprocedure call.

提交或回滚可以看作是一个子过程调用。

When the transaction starts to handle a rollback or commit.
It builds a query graph which, when executed, will roll back
or commit the incomplete transaction. The transaction
is moved to the TRX_QUE_ROLLING_BACK or TRX_QUE_COMMITTING state.
If specified, the SQL cursors opened by the transaction are closed.
When the execution of the graph completes, it is like returning
from a subprocedure: the query thread which requested the operation
starts running again.

当事务开始处理回滚或提交时，它会构建一个查询图，该查询图在执行时将回滚或提交未完成的事务。
事务会进入 TRX_QUE_ROLLING_BACK 或 TRX_QUE_COMMITTING 状态。如果指定了，
事务打开的 SQL 游标将被关闭。当查询图执行完成时，就像从子过程返回一样：
请求该操作的查询线程将再次开始运行。 */

/** Moves a thread from another state to the QUE_THR_RUNNING state. Increments
 the n_active_thrs counters of the query graph and transaction.
 ***NOTE***: This is the only function in which such a transition is allowed
 to happen! */
/** 将线程从其他状态移动到 QUE_THR_RUNNING 状态。增加查询图和事务的 n_active_thrs 计数器。
 ***注意***：这是唯一允许进行此类转换的函数！ */
static void que_thr_move_to_run_state(
    que_thr_t *thr); /*!< in: an query thread */ /*!< in: 查询线程 */

/** Creates a query graph fork node.
 @return own: fork node */
/** 创建一个查询图的分叉节点。
 @return own: 分叉节点 */
que_fork_t *que_fork_create(
    que_t *graph,       /*!< in: graph, if NULL then this
                        fork node is assumed to be the
                        graph root */ /*!< in: 查询图，如果为 NULL，则当前分叉节点被视为查询图的根节点 */
    que_node_t *parent, /*!< in: parent node */ /*!< in: 父节点 */
    ulint fork_type,    /*!< in: fork type */ /*!< in: 分叉类型 */
    mem_heap_t *heap)   /*!< in: memory heap where created */ /*!< in: 用于分配节点的内存堆 */
{
  que_fork_t *fork;

  ut_ad(heap);  // 断言：确保内存堆不为空

  fork = static_cast<que_fork_t *>(mem_heap_zalloc(heap, sizeof(*fork)));  // 在内存堆中分配分叉节点的内存

  fork->heap = heap;  // 设置分叉节点的内存堆

  fork->fork_type = fork_type;  // 设置分叉类型

  fork->common.parent = parent;  // 设置父节点

  fork->common.type = QUE_NODE_FORK;  // 设置节点类型为分叉节点

  fork->state = QUE_FORK_COMMAND_WAIT;  // 设置分叉节点的状态为等待命令

  fork->graph = (graph != nullptr) ? graph : fork;  // 设置查询图，如果 graph 为 NULL，则当前分叉节点为查询图的根节点

  UT_LIST_INIT(fork->thrs);  // 初始化分叉节点的线程列表

  return (fork);  // 返回新创建的分叉节点
}

/**
 * @brief 创建一个查询图线程节点。
 * @brief Creates a query graph thread node.
 *
 * @param[in] parent 父节点，即一个分叉节点
 * @param[in] heap 用于分配节点的内存堆
 * @param[in] prebuilt 行预构建结构
 * @return own: 查询线程节点
 */
que_thr_t *que_thr_create(que_fork_t *parent, mem_heap_t *heap,
                          row_prebuilt_t *prebuilt) {
  que_thr_t *thr;

  ut_ad(parent != nullptr);  // 断言：确保父节点不为空
  ut_ad(heap != nullptr);    // 断言：确保内存堆不为空

  thr = static_cast<que_thr_t *>(mem_heap_zalloc(heap, sizeof(*thr)));  // 在内存堆中分配线程节点的内存

  thr->graph = parent->graph;  // 设置线程节点的查询图

  thr->common.parent = parent;  // 设置线程节点的父节点

  thr->magic_n = QUE_THR_MAGIC_N;  // 设置线程节点的魔数

  thr->common.type = QUE_NODE_THR;  // 设置节点类型为线程节点

  thr->state = QUE_THR_COMMAND_WAIT;  // 设置线程节点的状态为等待命令

  thr->lock_state = QUE_THR_LOCK_NOLOCK;  // 设置线程节点的锁状态为无锁

  thr->prebuilt = prebuilt;  // 设置线程节点的行预构建结构

  UT_LIST_ADD_LAST(parent->thrs, thr);  // 将线程节点添加到父节点的线程列表末尾

  return (thr);  // 返回新创建的线程节点
}

/** Moves a suspended query thread to the QUE_THR_RUNNING state and may release
 a worker thread to execute it. This function should be used to end
 the wait state of a query thread waiting for a lock or a stored procedure
 completion.
 @return the query thread that needs to be released. */
que_thr_t *que_thr_end_lock_wait(trx_t *trx) /*!< in: transaction with que_state
                                             in QUE_THR_LOCK_WAIT */
{
  ut_ad(locksys::owns_lock_shard(trx->lock.wait_lock));

  ut_ad(trx_mutex_own(trx));

  que_thr_t *const thr = trx->lock.wait_thr;

  ut_ad(thr != nullptr);

  ut_ad(trx->lock.que_state == TRX_QUE_LOCK_WAIT);
  /* In MySQL this is the only possible state here */
  ut_a(thr->state == QUE_THR_LOCK_WAIT);

  bool const was_active = thr->is_active;

  que_thr_move_to_run_state(thr);

  trx->stats.stop_lock_wait(*trx);

  trx->lock.que_state = TRX_QUE_RUNNING;

  trx->lock.wait_thr = nullptr;

  /* In MySQL we let the OS thread (not just the query thread) to wait
  for the lock to be released: */

  return !was_active ? thr : nullptr;
}

/** Inits a query thread for a command. */
static inline void que_thr_init_command(que_thr_t *thr) /*!< in: query thread */
{
  thr->run_node = thr;
  thr->prev_node = thr->common.parent;

  que_thr_move_to_run_state(thr);
}

/** Round robin scheduler.
 @return a query thread of the graph moved to QUE_THR_RUNNING state, or
 NULL; the query thread should be executed by que_run_threads by the
 caller */
que_thr_t *que_fork_scheduler_round_robin(
    que_fork_t *fork, /*!< in: a query fork */
    que_thr_t *thr)   /*!< in: current pos */
{
  trx_mutex_enter(fork->trx);

  /* If no current, start first available. */
  if (thr == nullptr) {
    thr = UT_LIST_GET_FIRST(fork->thrs);
  } else {
    thr = UT_LIST_GET_NEXT(thrs, thr);
  }

  if (thr) {
    fork->state = QUE_FORK_ACTIVE;

    fork->last_sel_node = nullptr;

    switch (thr->state) {
      case QUE_THR_COMMAND_WAIT:
      case QUE_THR_COMPLETED:
        ut_a(!thr->is_active);
        que_thr_init_command(thr);
        break;

      case QUE_THR_SUSPENDED:
      case QUE_THR_LOCK_WAIT:
      default:
        ut_error;
    }
  }

  trx_mutex_exit(fork->trx);

  return (thr);
}


/**
 * @brief 启动查询分叉中的命令执行。选择一个未处于 QUE_THR_RUNNING 状态的查询线程并将其移动到该状态。
 *        如果没有可选的线程（可能在并行化获取时发生），则返回 NULL。
 * @brief Starts execution of a command in a query fork. Picks a query thread which
 *        is not in the QUE_THR_RUNNING state and moves it to that state. If none
 *        can be chosen, a situation which may arise in parallelized fetches, NULL
 *        is returned.
 *
 * @return 移动到 QUE_THR_RUNNING 状态的查询线程，或 NULL；调用者应通过 que_run_threads 执行该查询线程
 * @return a query thread of the graph moved to QUE_THR_RUNNING state, or
 *         NULL; the query thread should be executed by que_run_threads by the
 *         caller
 */
que_thr_t *que_fork_start_command(que_fork_t *fork) /*!< in: 查询分叉 */ /*!< in: a query fork */
{
  que_thr_t *suspended_thr = nullptr;  // 挂起的线程
  que_thr_t *completed_thr = nullptr;  // 已完成的线程

  fork->state = QUE_FORK_ACTIVE;  // 设置分叉状态为活跃

  fork->last_sel_node = nullptr;  // 重置最后一个选择节点

  suspended_thr = nullptr;  // 初始化挂起的线程为 NULL
  completed_thr = nullptr;  // 初始化已完成的线程为 NULL

  /* Choose the query thread to run: usually there is just one thread,
  but in a parallelized select, which necessarily is non-scrollable,
  there may be several to choose from */

  /* First we try to find a query thread in the QUE_THR_COMMAND_WAIT
  state. Then we try to find a query thread in the QUE_THR_SUSPENDED
  state, finally we try to find a query thread in the QUE_THR_COMPLETED
  state */

  /* We make a single pass over the thr list within which we note which
  threads are ready to run. */
  for (auto thr : fork->thrs) {
    switch (thr->state) {
      case QUE_THR_COMMAND_WAIT:

        /* We have to send the initial message to query thread
        to start it */

        que_thr_init_command(thr);  // 初始化线程命令

        return (thr);  // 返回该线程

      case QUE_THR_SUSPENDED:
        /* In this case the execution of the thread was
        suspended: no initial message is needed because
        execution can continue from where it was left */
        if (!suspended_thr) {
          suspended_thr = thr;  // 记录第一个挂起的线程
        }

        break;

      case QUE_THR_COMPLETED:
        /* If a completed thread is found, record the first one */
        if (!completed_thr) {
          completed_thr = thr;  // 记录第一个已完成的线程
        }

        break;

      case QUE_THR_RUNNING:
      case QUE_THR_LOCK_WAIT:
      case QUE_THR_PROCEDURE_WAIT:
        ut_error;  // 如果线程处于这些状态，触发错误
    }
  }

  que_thr_t *thr;
  if (suspended_thr) {
    /* If there is a suspended thread, select it and move it to the running state */
    thr = suspended_thr;
    que_thr_move_to_run_state(thr);  // 将挂起的线程移动到运行状态

  } else if (completed_thr) {
    /* If there is a completed thread, select it and reinitialize its command */
    thr = completed_thr;
    que_thr_init_command(thr);  // 重新初始化已完成的线程
  } else {
    /* If no available thread is found, trigger an error */
    ut_error;
  }

  return (thr);  // 返回选择的线程
}

/** Calls que_graph_free_recursive for statements in a statement list. */
static void que_graph_free_stat_list(
    que_node_t *node) /*!< in: first query graph node in the list */
{
  while (node) {
    que_graph_free_recursive(node);

    node = que_node_get_next(node);
  }
}

/** Frees a query graph, but not the heap where it was created. Does not free
 explicit cursor declarations, they are freed in que_graph_free. */
void que_graph_free_recursive(que_node_t *node) /*!< in: query graph node */
{
  que_fork_t *fork;
  que_thr_t *thr;
  undo_node_t *undo;
  sel_node_t *sel;
  ins_node_t *ins;
  upd_node_t *upd;
  purge_node_t *purge;

  DBUG_TRACE;

  if (node == nullptr) {
    return;
  }

  DBUG_PRINT("que_graph_free_recursive",
             ("node: %p, type: %lu", node, que_node_get_type(node)));

  switch (que_node_get_type(node)) {
    case QUE_NODE_FORK:
      fork = static_cast<que_fork_t *>(node);

      thr = UT_LIST_GET_FIRST(fork->thrs);

      while (thr) {
        que_graph_free_recursive(thr);

        thr = UT_LIST_GET_NEXT(thrs, thr);
      }

      break;
    case QUE_NODE_THR:

      thr = static_cast<que_thr_t *>(node);

      ut_a(thr->magic_n == QUE_THR_MAGIC_N);

      thr->magic_n = QUE_THR_MAGIC_FREED;

      que_graph_free_recursive(thr->child);

      break;

    case QUE_NODE_UNDO:
      undo = static_cast<undo_node_t *>(node);

      /* Free the heap memory. */
      mem_heap_free(undo->heap);
      break;

    case QUE_NODE_SELECT:

      sel = static_cast<sel_node_t *>(node);

      sel_node_free_private(sel);

      break;
    case QUE_NODE_INSERT:

      ins = static_cast<ins_node_t *>(node);

      que_graph_free_recursive(ins->select);
      ins->select = nullptr;

      if (ins->entry_sys_heap != nullptr) {
        mem_heap_free(ins->entry_sys_heap);
        ins->entry_sys_heap = nullptr;
      }

      break;
    case QUE_NODE_PURGE:
      purge = static_cast<purge_node_t *>(node);

      purge->deinit();

      break;

    case QUE_NODE_UPDATE:
      upd = static_cast<upd_node_t *>(node);

      if (upd->update) {
        upd->update->free_per_stmt_heap();
      }

      if (upd->in_mysql_interface) {
        btr_pcur_t::free_for_mysql(upd->pcur);
        upd->in_mysql_interface = false;
      }

      que_graph_free_recursive(upd->cascade_node);
      if (upd->cascade_heap) {
        mem_heap_free(upd->cascade_heap);
        upd->cascade_heap = nullptr;
      }

      que_graph_free_recursive(upd->select);
      upd->select = nullptr;

      if (upd->heap != nullptr) {
        mem_heap_free(upd->heap);
        upd->heap = nullptr;
      }

      break;
    case QUE_NODE_PROC:
      que_graph_free_stat_list(((proc_node_t *)node)->stat_list);

      break;
    case QUE_NODE_IF:
      que_graph_free_stat_list(((if_node_t *)node)->stat_list);
      que_graph_free_stat_list(((if_node_t *)node)->else_part);
      que_graph_free_stat_list(((if_node_t *)node)->elsif_list);

      break;
    case QUE_NODE_ELSIF:
      que_graph_free_stat_list(((elsif_node_t *)node)->stat_list);

      break;
    case QUE_NODE_WHILE:
      que_graph_free_stat_list(((while_node_t *)node)->stat_list);

      break;
    case QUE_NODE_FOR:
      que_graph_free_stat_list(((for_node_t *)node)->stat_list);

      break;

    case QUE_NODE_ASSIGNMENT:
    case QUE_NODE_EXIT:
    case QUE_NODE_RETURN:
    case QUE_NODE_COMMIT:
    case QUE_NODE_ROLLBACK:
    case QUE_NODE_LOCK:
    case QUE_NODE_FUNC:
    case QUE_NODE_ORDER:
    case QUE_NODE_OPEN:
    case QUE_NODE_FETCH:
      /* No need to do anything */

      break;
    default:
      ut_error;
  }
}

/** Frees a query graph. */
void que_graph_free(que_t *graph) /*!< in: query graph; we assume that the
                                  memory heap where this graph was created is
                                  private to this graph: if not, then use
                                  que_graph_free_recursive and free the heap
                                  afterwards! */
{
  ut_ad(graph);
  ut_ad(!dict_sys_mutex_own());

  if (graph->sym_tab) {
    /* The following call frees dynamic memory allocated
    for variables etc. during execution. Frees also explicit
    cursor definitions. */

    sym_tab_free_private(graph->sym_tab);
  }

  if (graph->info && graph->info->graph_owns_us) {
    pars_info_free(graph->info);
  }

  que_graph_free_recursive(graph);

  mem_heap_free(graph->heap);
}

/** Performs an execution step on a thr node.
 @return query thread to run next, or NULL if none */
static que_thr_t *que_thr_node_step(
    que_thr_t *thr) /*!< in: query thread where run_node must
                    be the thread node itself */
{
  ut_ad(thr->run_node == thr);

  if (thr->prev_node == thr->common.parent) {
    /* If control to the node came from above, it is just passed
    on */

    thr->run_node = thr->child;

    return (thr);
  }

  trx_mutex_enter(thr_get_trx(thr));

  if (que_thr_peek_stop(thr)) {
    trx_mutex_exit(thr_get_trx(thr));

    return (thr);
  }

  /* Thread execution completed */

  thr->state = QUE_THR_COMPLETED;

  trx_mutex_exit(thr_get_trx(thr));

  return (nullptr);
}

/** Moves a thread from another state to the QUE_THR_RUNNING state. Increments
 the n_active_thrs counters of the query graph and transaction if thr was
 not active.
 ***NOTE***: This and ..._mysql are  the only functions in which such a
 transition is allowed to happen! */
static void que_thr_move_to_run_state(
    que_thr_t *thr) /*!< in: an query thread */
{
  ut_ad(thr->state != QUE_THR_RUNNING);

  if (!thr->is_active) {
    trx_t *trx;

    trx = thr_get_trx(thr);

    thr->graph->n_active_thrs++;

    trx->lock.n_active_thrs++;

    thr->is_active = true;
  }

  thr->state = QUE_THR_RUNNING;
}

bool que_thr_stop(que_thr_t *thr) {
  que_t *graph = thr->graph;
  trx_t *trx = thr_get_trx(thr);

  ut_ad(trx_mutex_own(trx));

  if (graph->state == QUE_FORK_COMMAND_WAIT) {
    thr->state = QUE_THR_SUSPENDED;

  } else if (trx->lock.que_state == TRX_QUE_LOCK_WAIT) {
    trx->lock.wait_thr = thr;
    thr->state = QUE_THR_LOCK_WAIT;

  } else if (trx->error_state != DB_SUCCESS &&
             trx->error_state != DB_LOCK_WAIT) {
    /* Error handling built for the MySQL interface */
    thr->state = QUE_THR_COMPLETED;

  } else if (graph->fork_type == QUE_FORK_ROLLBACK) {
    thr->state = QUE_THR_SUSPENDED;
  } else {
    ut_ad(graph->state == QUE_FORK_ACTIVE);

    return false;
  }

  return true;
}

/** Decrements the query thread reference counts in the query graph and the
 transaction.
 *** NOTE ***:
 This and que_thr_stop_for_mysql are the only functions where the reference
 count can be decremented and this function may only be called from inside
 que_run_threads! These restrictions exist to make the rollback code easier
 to maintain. */
static void que_thr_dec_refer_count(
    que_thr_t *thr,       /*!< in: query thread */
    que_thr_t **next_thr) /*!< in/out: next query thread to run;
                          if the value which is passed in is
                          a pointer to a NULL pointer, then the
                          calling function can start running
                          a new query thread */
{
  trx_t *trx;
  que_fork_t *fork;

  trx = thr_get_trx(thr);

  ut_a(thr->is_active);
  ut_ad(trx_mutex_own(trx));

  if (thr->state == QUE_THR_RUNNING) {
    if (!que_thr_stop(thr)) {
      ut_a(next_thr != nullptr && *next_thr == nullptr);

      /* The reason for the thr suspension or wait was
      already canceled before we came here: continue
      running the thread.

      This is also possible because in trx_commit_step() we
      assume a single query thread. We set the query thread
      state to QUE_THR_RUNNING. */

      /* fprintf(stderr,
              "Wait already ended: trx: %p\n", trx); */

      /* Normally srv_suspend_mysql_thread resets
      the state to DB_SUCCESS before waiting, but
      in this case we have to do it here,
      otherwise nobody does it. */

      trx->error_state = DB_SUCCESS;

      *next_thr = thr;

      return;
    }
  }

  fork = static_cast<que_fork_t *>(thr->common.parent);

  --trx->lock.n_active_thrs;

  --fork->n_active_thrs;

  thr->is_active = false;
}

/** A patch for MySQL used to 'stop' a dummy query thread used in MySQL. The
 query thread is stopped and made inactive, except in the case where
 it was put to the lock wait state in lock0lock.cc, but the lock has already
 been granted or the transaction chosen as a victim in deadlock resolution. */
void que_thr_stop_for_mysql(que_thr_t *thr) /*!< in: query thread */
{
  trx_t *trx;

  trx = thr_get_trx(thr);

  trx_mutex_enter(trx);

  if (thr->state == QUE_THR_RUNNING) {
    if (trx->error_state != DB_SUCCESS && trx->error_state != DB_LOCK_WAIT) {
      /* Error handling built for the MySQL interface */
      thr->state = QUE_THR_COMPLETED;
    } else {
      /* It must have been a lock wait but the lock was
      already released, or this transaction was chosen
      as a victim in selective deadlock resolution */

      trx_mutex_exit(trx);

      return;
    }
  }

  ut_ad(thr->is_active == true);
  ut_ad(trx->lock.n_active_thrs == 1);
  ut_ad(thr->graph->n_active_thrs == 1);

  thr->is_active = false;
  thr->graph->n_active_thrs--;

  trx->lock.n_active_thrs--;

  trx_mutex_exit(trx);
}

/** Moves a thread from another state to the QUE_THR_RUNNING state. Increments
 the n_active_thrs counters of the query graph and transaction if thr was
 not active. */
void que_thr_move_to_run_state_for_mysql(
    que_thr_t *thr, /*!< in: an query thread */
    trx_t *trx)     /*!< in: transaction */
{
  ut_a(thr->magic_n == QUE_THR_MAGIC_N);

  if (!thr->is_active) {
    thr->graph->n_active_thrs++;

    trx->lock.n_active_thrs++;

    thr->is_active = true;
  }

  thr->state = QUE_THR_RUNNING;
}

/** A patch for MySQL used to 'stop' a dummy query thread used in MySQL
 select, when there is no error or lock wait.
@param[in] thr Query thread
@param[in] trx Transaction */
void que_thr_stop_for_mysql_no_error(que_thr_t *thr, trx_t *trx) {
  ut_ad(thr->state == QUE_THR_RUNNING);
  ut_ad(thr->is_active == true);
  ut_ad(trx->lock.n_active_thrs == 1);
  ut_ad(thr->graph->n_active_thrs == 1);
  ut_a(thr->magic_n == QUE_THR_MAGIC_N);

  thr->state = QUE_THR_COMPLETED;

  thr->is_active = false;
  thr->graph->n_active_thrs--;

  trx->lock.n_active_thrs--;
}

/** Get the first containing loop node (e.g. while_node_t or for_node_t) for the
 given node, or NULL if the node is not within a loop.
 @return containing loop node, or NULL. */
que_node_t *que_node_get_containing_loop_node(que_node_t *node) /*!< in: node */
{
  ut_ad(node);

  for (;;) {
    ulint type;

    node = que_node_get_parent(node);

    if (!node) {
      break;
    }

    type = que_node_get_type(node);

    if ((type == QUE_NODE_FOR) || (type == QUE_NODE_WHILE)) {
      break;
    }
  }

  return (node);
}

#ifdef UNIV_DEBUG
/** Gets information of an SQL query graph node.
@return type description */
[[nodiscard]] static const char *que_node_type_string(
    const que_node_t *node) /*!< in: query graph node */
{
  switch (que_node_get_type(node)) {
    case QUE_NODE_SELECT:
      return ("SELECT");
    case QUE_NODE_INSERT:
      return ("INSERT");
    case QUE_NODE_UPDATE:
      return ("UPDATE");
    case QUE_NODE_WHILE:
      return ("WHILE");
    case QUE_NODE_ASSIGNMENT:
      return ("ASSIGNMENT");
    case QUE_NODE_IF:
      return ("IF");
    case QUE_NODE_FETCH:
      return ("FETCH");
    case QUE_NODE_OPEN:
      return ("OPEN");
    case QUE_NODE_PROC:
      return ("STORED PROCEDURE");
    case QUE_NODE_FUNC:
      return ("FUNCTION");
    case QUE_NODE_LOCK:
      return ("LOCK");
    case QUE_NODE_THR:
      return ("QUERY THREAD");
    case QUE_NODE_COMMIT:
      return ("COMMIT");
    case QUE_NODE_UNDO:
      return ("UNDO ROW");
    case QUE_NODE_PURGE:
      return ("PURGE ROW");
    case QUE_NODE_ROLLBACK:
      return ("ROLLBACK");
    case QUE_NODE_FOR:
      return ("FOR LOOP");
    case QUE_NODE_RETURN:
      return ("RETURN");
    case QUE_NODE_EXIT:
      return ("EXIT");
    default:
      ut_d(ut_error);
      ut_o(return ("UNKNOWN NODE TYPE"));
  }
}
#endif /* UNIV_DEBUG */

/**
 * @brief 在查询线程上执行一个步骤。
 * @brief Performs an execution step on a query thread.
 *
 * @return 下一个要运行的查询线程：它可能与输入参数不同，例如，如果进行了子过程调用
 * @return query thread to run next: it may differ from the input
 *         parameter if, e.g., a subprocedure call is made
 */
static inline que_thr_t *que_thr_step(que_thr_t *thr) /*!< in: 查询线程 */ /*!< in: query thread */
{
  que_node_t *node;  // 当前执行的节点
  que_thr_t *old_thr;  // 旧的查询线程
  trx_t *trx;  // 事务对象
  ulint type;  // 节点类型

  trx = thr_get_trx(thr);  // 获取查询线程所属的事务

  ut_ad(thr->state == QUE_THR_RUNNING);  // 断言：确保线程处于运行状态
  ut_a(trx->error_state == DB_SUCCESS);  // 断言：确保事务没有错误

  thr->resource++;  // 增加线程的资源计数

  node = thr->run_node;  // 获取当前运行的节点
  type = que_node_get_type(node);  // 获取节点的类型

  old_thr = thr;  // 保存当前的查询线程

  DBUG_PRINT("ib_que", ("Execute %u (%s) at %p", unsigned(type),
                        que_node_type_string(node), (const void *)node));  // 调试信息：打印当前执行的节点类型和地址

  if (type & QUE_NODE_CONTROL_STAT) {
    // 如果节点是控制语句（如 WHILE、IF 等）
    if ((thr->prev_node != que_node_get_parent(node)) &&
        que_node_get_next(thr->prev_node)) {
      /* The control statements, like WHILE, always pass the
      control to the next child statement if there is any
      child left */

      thr->run_node = que_node_get_next(thr->prev_node);  // 设置下一个要运行的节点

    } else if (type == QUE_NODE_IF) {
      if_step(thr);  // 执行 IF 语句
    } else if (type == QUE_NODE_FOR) {
      for_step(thr);  // 执行 FOR 语句
    } else if (type == QUE_NODE_PROC) {
      /* We can access trx->undo_no without reserving
      trx->undo_mutex, because there cannot be active query
      threads doing updating or inserting at the moment! */

      if (thr->prev_node == que_node_get_parent(node)) {
        trx->last_sql_stat_start.least_undo_no = trx->undo_no;  // 记录事务的 undo_no
      }

      proc_step(thr);  // 执行存储过程步骤
    } else if (type == QUE_NODE_WHILE) {
      while_step(thr);  // 执行 WHILE 语句
    } else {
      ut_error;  // 如果节点类型未知，触发错误
    }
  } else if (type == QUE_NODE_ASSIGNMENT) {
    assign_step(thr);  // 执行赋值语句
  } else if (type == QUE_NODE_SELECT) {
    thr = row_sel_step(thr);  // 执行 SELECT 语句
  } else if (type == QUE_NODE_INSERT) {
    thr = row_ins_step(thr);  // 执行 INSERT 语句
  } else if (type == QUE_NODE_UPDATE) {
    thr = row_upd_step(thr);  // 执行 UPDATE 语句
  } else if (type == QUE_NODE_FETCH) {
    thr = fetch_step(thr);  // 执行 FETCH 语句
  } else if (type == QUE_NODE_OPEN) {
    thr = open_step(thr);  // 执行 OPEN 语句
  } else if (type == QUE_NODE_FUNC) {
    proc_eval_step(thr);  // 执行函数调用

  } else if (type == QUE_NODE_LOCK) {
    ut_error;  // 如果节点类型是 LOCK，触发错误
  } else if (type == QUE_NODE_THR) {
    thr = que_thr_node_step(thr);  // 执行线程节点步骤
  } else if (type == QUE_NODE_COMMIT) {
    thr = trx_commit_step(thr);  // 执行 COMMIT 语句
  } else if (type == QUE_NODE_UNDO) {
    thr = row_undo_step(thr);  // 执行 UNDO 语句
  } else if (type == QUE_NODE_PURGE) {
    thr = row_purge_step(thr);  // 执行 PURGE 语句
  } else if (type == QUE_NODE_RETURN) {
    thr = return_step(thr);  // 执行 RETURN 语句
  } else if (type == QUE_NODE_EXIT) {
    thr = exit_step(thr);  // 执行 EXIT 语句
  } else if (type == QUE_NODE_ROLLBACK) {
    thr = trx_rollback_step(thr);  // 执行 ROLLBACK 语句
  } else {
    ut_error;  // 如果节点类型未知，触发错误
  }

  if (type == QUE_NODE_EXIT) {
    old_thr->prev_node = que_node_get_containing_loop_node(node);  // 如果节点是 EXIT，设置前一个节点为包含的循环节点
  } else {
    old_thr->prev_node = node;  // 否则，设置前一个节点为当前节点
  }

  if (thr) {
    ut_a(thr_get_trx(thr)->error_state == DB_SUCCESS);  // 断言：确保事务没有错误
  }

  return (thr);  // 返回下一个要运行的查询线程
}

/** Run a query thread until it finishes or encounters e.g. a lock wait. */
static void que_run_threads_low(que_thr_t *thr) /*!< in: query thread */
{
  trx_t *trx;  // 定义事务对象
  que_thr_t *next_thr;  // 定义下一个查询线程对象

  ut_ad(thr->state == QUE_THR_RUNNING);  // 断言查询线程状态为运行中
  ut_a(thr_get_trx(thr)->error_state == DB_SUCCESS);  // 断言事务的错误状态为成功
  ut_ad(!trx_mutex_own(thr_get_trx(thr)));  // 断言当前线程不持有事务的互斥锁

  /* cumul_resource counts how much resources the OS thread (NOT the
  query thread) has spent in this function */  // cumul_resource 计算操作系统线程（而不是查询线程）在此函数中花费了多少资源

  trx = thr_get_trx(thr);  // 获取查询线程的事务对象

  do {
    /* Check that there is enough space in the log to accommodate
    possible log entries by this query step; if the operation can
    touch more than about 4 pages, checks must be made also within
    the query step! */  // 检查日志中是否有足够的空间来容纳此查询步骤可能产生的日志条目；如果操作可能涉及超过 4 页，则还必须在查询步骤内进行检查！

    log_free_check();  // 检查日志空间

    /* Perform the actual query step: note that the query thread
    may change if, e.g., a subprocedure call is made */  // 执行实际的查询步骤：注意，如果进行了子过程调用，查询线程可能会更改

    /*-------------------------*/
    next_thr = que_thr_step(thr);  // 执行查询步骤并获取下一个查询线程
    /*-------------------------*/

    trx_mutex_enter(trx);  // 进入事务的互斥锁

    ut_a(next_thr == nullptr || trx->error_state == DB_SUCCESS);  // 断言下一个查询线程为空或事务的错误状态为成功

    if (next_thr != thr) {  // 如果下一个查询线程不是当前线程
      ut_a(next_thr == nullptr);  // 断言下一个查询线程为空

      /* This can change next_thr to a non-NULL value
      if there was a lock wait that already completed. */  // 如果有一个已经完成的锁等待，这可以将 next_thr 更改为非空值

      que_thr_dec_refer_count(thr, &next_thr);  // 减少查询线程的引用计数

      if (next_thr != nullptr) {  // 如果下一个查询线程不为空
        thr = next_thr;  // 将当前查询线程设置为下一个查询线程
      }
    }

    ut_ad(trx == thr_get_trx(thr));  // 断言事务对象与查询线程的事务对象一致

    trx_mutex_exit(trx);  // 退出事务的互斥锁

  } while (next_thr != nullptr);  // 当下一个查询线程不为空时继续循环
}

/** Run a query thread. Handles lock waits. */
void que_run_threads(que_thr_t *thr) /*!< in: query thread */
{
  ut_ad(!trx_mutex_own(thr_get_trx(thr)));  // 断言当前线程不持有事务的互斥锁

loop:
  ut_a(thr_get_trx(thr)->error_state == DB_SUCCESS);  // 断言事务的错误状态为成功

  que_run_threads_low(thr);  // 运行查询线程的低级处理逻辑

  switch (thr->state) {  // 根据线程状态进行分支处理
    case QUE_THR_RUNNING:  // 如果线程状态为运行中
      /* There probably was a lock wait, but it already ended
      before we came here: continue running thr */  // 可能存在锁等待，但在我们到达之前已经结束，继续运行线程

      goto loop;  // 跳转到 loop 标签，继续处理

    case QUE_THR_LOCK_WAIT:  // 如果线程状态为锁等待
      lock_wait_suspend_thread(thr);  // 挂起线程以等待锁

      trx_mutex_enter(thr_get_trx(thr));  // 进入事务的互斥锁

      ut_a(thr_get_trx(thr)->id != 0);  // 断言事务 ID 不为 0

      if (thr_get_trx(thr)->error_state != DB_SUCCESS) {  // 如果事务的错误状态不为成功
        /* thr was chosen as a deadlock victim or there was
        a lock wait timeout */  // 线程被选为死锁受害者或锁等待超时

        que_thr_dec_refer_count(thr, nullptr);  // 减少线程的引用计数
        trx_mutex_exit(thr_get_trx(thr));  // 退出事务的互斥锁
        break;  // 跳出 switch 语句
      }

      trx_mutex_exit(thr_get_trx(thr));  // 退出事务的互斥锁
      goto loop;  // 跳转到 loop 标签，继续处理

    case QUE_THR_COMPLETED:  // 如果线程状态为已完成
    case QUE_THR_COMMAND_WAIT:  // 如果线程状态为命令等待
      /* Do nothing */  // 不做任何处理
      break;  // 跳出 switch 语句

    default:  // 如果线程状态为其他情况
      ut_error;  // 触发错误
  }
}

dberr_t que_eval_sql(pars_info_t *info, const char *sql, trx_t *trx) {
  que_t *graph;

  DBUG_TRACE;
  DBUG_PRINT("que_eval_sql", ("query: %s", sql));

  ut_a(trx->error_state == DB_SUCCESS);

  mutex_enter(&pars_mutex);

  graph = pars_sql(info, sql);

  mutex_exit(&pars_mutex);

  graph->trx = trx;
  trx->graph = nullptr;

  graph->fork_type = QUE_FORK_MYSQL_INTERFACE;

  auto thr = que_fork_start_command(graph);
  ut_a(thr);

  que_run_threads(thr);

  que_graph_free(graph);

  ut_a(trx->error_state != 0);

  return trx->error_state;
}

/** Initialise the query sub-system. */
void que_init(void) { /* No op */
}

/** Close the query sub-system. */
void que_close(void) { /* No op */
}
