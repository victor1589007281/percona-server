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

#ifndef SEMISYNC_SOURCE_H
#define SEMISYNC_SOURCE_H

#include <assert.h>
#include <sys/types.h>

#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "plugin/semisync/semisync.h"

extern PSI_memory_key key_ss_memory_TranxNodeAllocator_block;

#ifdef HAVE_PSI_INTERFACE
extern PSI_mutex_key key_ss_mutex_LOCK_binlog_;
extern PSI_cond_key key_ss_cond_COND_binlog_send_;
#endif

extern PSI_stage_info stage_waiting_for_semi_sync_ack_from_replica;
extern PSI_stage_info stage_waiting_for_semi_sync_replica;
extern PSI_stage_info stage_reading_semi_sync_ack;

extern unsigned int rpl_semi_sync_source_wait_for_replica_count;

/**
 * @struct TranxNode
 * 
 * Represents a node in the active transaction list for semi-synchronous replication.
 * 用于表示半同步复制中活跃事务列表的节点。
 */
struct TranxNode {
  char log_name_[FN_REFLEN];  // The binlog file name associated with the transaction.
                              // 与事务关联的 binlog 文件名。
  my_off_t log_pos_;          // The binlog file position associated with the transaction.
                              // 与事务关联的 binlog 文件位置。
  mysql_cond_t cond;          // Condition variable used for threads waiting on this transaction.
                              // 用于线程等待该事务的条件变量。
  int n_waiters;              // Number of threads currently waiting on this transaction.
                              // 当前等待该事务的线程数。
  struct TranxNode *next_;    // Pointer to the next node in the sorted transaction list.
                              // 指向排序事务列表中下一个节点的指针。
  struct TranxNode *hash_next_; // Pointer to the next node in case of hash collision.
                                // 在哈希冲突情况下指向下一个节点的指针。
};

/**
  @class TranxNodeAllocator

  This class provides memory allocating and freeing methods for
  TranxNode. The main target is performance.

  @section ALLOCATE How to allocate a node
    The pointer of the first node after 'last_node' in current_block is
    returned. current_block will move to the next free Block when all nodes of
    it are in use. A new Block is allocated and is put into the rear of the
    Block link table if no Block is free.

    The list starts up empty (ie, there is no allocated Block).

    After some nodes are freed, there probably are some free nodes before
    the sequence of the allocated nodes, but we do not reuse it. It is better
    to keep the allocated nodes are in the sequence, for it is more efficient
    for allocating and freeing TranxNode.

  @section FREENODE How to free nodes
    There are two methods for freeing nodes. They are free_all_nodes and
    free_nodes_before.

    'A Block is free' means all of its nodes are free.
    @subsection free_nodes_before
    As all allocated nodes are in the sequence, 'Before one node' means all
    nodes before given node in the same Block and all Blocks before the Block
    which containing the given node. As such, all Blocks before the given one
    ('node') are free Block and moved into the rear of the Block link table.
    The Block containing the given 'node', however, is not. For at least the
    given 'node' is still in use. This will waste at most one Block, but it is
    more efficient.
 */
#define BLOCK_TRANX_NODES 16
class TranxNodeAllocator {
 public:
  /**
    @param reserved_nodes
      The number of reserved TranxNodes. It is used to set 'reserved_blocks'
      which can contain at least 'reserved_nodes' number of TranxNodes.  When
      freeing memory, we will reserve at least reserved_blocks of Blocks not
      freed.
   */
  TranxNodeAllocator(uint reserved_nodes)
      : reserved_blocks(reserved_nodes / BLOCK_TRANX_NODES +
                        (reserved_nodes % BLOCK_TRANX_NODES > 1 ? 2 : 1)),
        first_block(nullptr),
        last_block(nullptr),
        current_block(nullptr),
        last_node(-1),
        block_num(0) {}

  ~TranxNodeAllocator() {
    Block *block = first_block;
    while (block != nullptr) {
      Block *next = block->next;
      free_block(block);
      block = next;
    }
  }

  /**
    The pointer of the first node after 'last_node' in current_block is
    returned. current_block will move to the next free Block when all nodes of
    it are in use. A new Block is allocated and is put into the rear of the
    Block link table if no Block is free.

    @return Return a TranxNode *, or NULL if an error occurred.
   */
  TranxNode *allocate_node() {
    TranxNode *trx_node;
    Block *block = current_block;

    if (last_node == BLOCK_TRANX_NODES - 1) {
      current_block = current_block->next;
      last_node = -1;
    }

    if (current_block == nullptr && allocate_block()) {
      current_block = block;
      if (current_block) last_node = BLOCK_TRANX_NODES - 1;
      return nullptr;
    }

    trx_node = &(current_block->nodes[++last_node]);
    trx_node->log_name_[0] = '\0';
    trx_node->log_pos_ = 0;
    trx_node->next_ = nullptr;
    trx_node->hash_next_ = nullptr;
    trx_node->n_waiters = 0;
    return trx_node;
  }

  /**
    All nodes are freed.

    @return Return 0, or 1 if an error occurred.
   */
  int free_all_nodes() {
    current_block = first_block;
    last_node = -1;
    free_blocks();
    return 0;
  }

  /**
    All Blocks before the given 'node' are free Block and moved into the rear
    of the Block link table.

    @param node All nodes before 'node' will be freed

    @return Return 0, or 1 if an error occurred.
   */
  int free_nodes_before(TranxNode *node) {
    Block *block;
    Block *prev_block = nullptr;

    block = first_block;
    while (block != current_block->next) {
      /* Find the Block containing the given node */
      if (&(block->nodes[0]) <= node &&
          &(block->nodes[BLOCK_TRANX_NODES]) >= node) {
        /* All Blocks before the given node are put into the rear */
        if (first_block != block) {
          last_block->next = first_block;
          first_block = block;
          last_block = prev_block;
          last_block->next = nullptr;
          free_blocks();
        }
        return 0;
      }
      prev_block = block;
      block = block->next;
    }

    /* Node does not find should never happen */
    assert(0);
    return 1;
  }

 private:
  uint reserved_blocks;

  /**
    A sequence memory which contains BLOCK_TRANX_NODES TranxNodes.

    BLOCK_TRANX_NODES The number of TranxNodes which are in a Block.

    next Every Block has a 'next' pointer which points to the next Block.
         These linking Blocks constitute a Block link table.
   */
  struct Block {
    Block *next;
    TranxNode nodes[BLOCK_TRANX_NODES];
  };

  /**
    The 'first_block' is the head of the Block link table;
   */
  Block *first_block;
  /**
    The 'last_block' is the rear of the Block link table;
   */
  Block *last_block;

  /**
    current_block always points the Block in the Block link table in
    which the last allocated node is. The Blocks before it are all in use
    and the Blocks after it are all free.
   */
  Block *current_block;

  /**
    It always points to the last node which has been allocated in the
    current_block.
   */
  int last_node;

  /**
    How many Blocks are in the Block link table.
   */
  uint block_num;

  /**
    Allocate a block and then assign it to current_block.
  */
  int allocate_block() {
    Block *block = (Block *)my_malloc(key_ss_memory_TranxNodeAllocator_block,
                                      sizeof(Block), MYF(0));
    if (block) {
      block->next = nullptr;

      if (first_block == nullptr)
        first_block = block;
      else
        last_block->next = block;

      /* New Block is always put into the rear */
      last_block = block;
      /* New Block is always the current_block */
      current_block = block;
      ++block_num;

      for (int i = 0; i < BLOCK_TRANX_NODES; i++)
        mysql_cond_init(key_ss_cond_COND_binlog_send_,
                        &current_block->nodes[i].cond);

      return 0;
    }
    return 1;
  }

  /**
    Free a given Block.
    @param block The Block will be freed.
   */
  void free_block(Block *block) {
    for (int i = 0; i < BLOCK_TRANX_NODES; i++)
      mysql_cond_destroy(&block->nodes[i].cond);
    my_free(block);
    --block_num;
  }

  /**
    If there are some free Blocks and the total number of the Blocks in the
    Block link table is larger than the 'reserved_blocks', Some free Blocks
    will be freed until the total number of the Blocks is equal to the
    'reserved_blocks' or there is only one free Block behind the
    'current_block'.
   */
  void free_blocks() {
    if (current_block == nullptr || current_block->next == nullptr) return;

    /* One free Block is always kept behind the current block */
    Block *block = current_block->next->next;
    while (block_num > reserved_blocks && block != nullptr) {
      Block *next = block->next;
      free_block(block);
      block = next;
    }
    current_block->next->next = block;
    if (block == nullptr) last_block = current_block->next;
  }
};

/**
 * @class ActiveTranx
 * 
 * This class manages memory for the active transaction list.
 * 用于管理半同步复制中活跃事务列表的类。
 * 
 * Each active transaction is recorded with a `TranxNode`. Each session
 * can have only one open transaction. However, due to events like
 * replication, the total number of active transaction nodes can exceed
 * the maximum allowed connections.
 * 每个活跃事务通过一个 `TranxNode` 记录。每个会话只能有一个打开的事务。
 * 但由于事件（如复制）的存在，活跃事务节点的总数可能超过允许的最大连接数。
 */
class ActiveTranx : public Trace {
  private:
   TranxNodeAllocator allocator_;  // Allocator for managing memory of TranxNodes.
                                   // 用于管理 TranxNode 内存的分配器。
 
   /* These two record the active transaction list in sort order. */
   TranxNode *trx_front_;  // Pointer to the front of the sorted transaction list.
                           // 指向排序事务列表头部的指针。
   TranxNode *trx_rear_;   // Pointer to the rear of the sorted transaction list.
                           // 指向排序事务列表尾部的指针。
 
   TranxNode **trx_htb_;   // A hash table for active transactions.
                           // 活跃事务的哈希表。
 
   int num_entries_;       // Maximum number of hash table entries.
                           // 哈希表的最大条目数。
   mysql_mutex_t *lock_;   // Mutex lock for thread safety.
                           // 用于线程安全的互斥锁。
 
   inline void assert_lock_owner();  // Ensures the current thread owns the lock.
                                     // 确保当前线程拥有锁。
 
   inline unsigned int calc_hash(const unsigned char *key, unsigned int length);
   // Calculates a hash value for a given key.
   // 为给定的键计算哈希值。
 
   unsigned int get_hash_value(const char *log_file_name, my_off_t log_file_pos);
   // Generates a hash value based on the binlog file name and position.
   // 根据 binlog 文件名和位置生成哈希值。
 
   /* Compare functions for sorting and searching transactions. */
   int compare(const char *log_file_name1, my_off_t log_file_pos1,
               const TranxNode *node2) {
     return compare(log_file_name1, log_file_pos1, node2->log_name_,
                    node2->log_pos_);
   }
   int compare(const TranxNode *node1, const char *log_file_name2,
               my_off_t log_file_pos2) {
     return compare(node1->log_name_, node1->log_pos_, log_file_name2,
                    log_file_pos2);
   }
   int compare(const TranxNode *node1, const TranxNode *node2) {
     return compare(node1->log_name_, node1->log_pos_, node2->log_name_,
                    node2->log_pos_);
   }
 
  public:
   /**
    * Signals all waiting sessions to wake up.
    * 唤醒所有等待的会话。
    */
   int signal_waiting_sessions_all();
 
   /**
    * Signals waiting sessions up to a specific binlog position.
    * 唤醒等待到指定 binlog 位置的会话。
    */
   int signal_waiting_sessions_up_to(const char *log_file_name,
                                     my_off_t log_file_pos);
 
   /**
    * Finds an active transaction node based on the binlog file name and position.
    * 根据 binlog 文件名和位置查找活跃事务节点。
    */
   TranxNode *find_active_tranx_node(const char *log_file_name,
                                     my_off_t log_file_pos);
 
   /**
    * Constructor for initializing the ActiveTranx object.
    * 初始化 ActiveTranx 对象的构造函数。
    */
   ActiveTranx(mysql_mutex_t *lock, unsigned long trace_level);
 
   /**
    * Destructor for cleaning up resources.
    * 清理资源的析构函数。
    */
   ~ActiveTranx();
 
   /**
    * Inserts an active transaction node with the specified binlog position.
    * 插入一个具有指定 binlog 位置的活跃事务节点。
    * 
    * @return 0 on success, non-zero on error.
    *         成功返回 0，错误返回非零值。
    */
   int insert_tranx_node(const char *log_file_name, my_off_t log_file_pos);
 
   /**
    * Clears active transaction nodes up to (and including) the specified position.
    * 清理直到（包括）指定位置的活跃事务节点。
    * 
    * If `log_file_name` is NULL, all nodes are cleared, and the list and hash table
    * are reset to empty.
    * 如果 `log_file_name` 为 NULL，则清理所有节点，并将列表和哈希表重置为空。
    * 
    * @return 0 on success, non-zero on error.
    *         成功返回 0，错误返回非零值。
    */
   int clear_active_tranx_nodes(const char *log_file_name,
                                my_off_t log_file_pos);
 
   /**
    * Checks if a given binlog position is the ending position of an active transaction.
    * 检查给定的 binlog 位置是否为活跃事务的结束位置。
    */
   bool is_tranx_end_pos(const char *log_file_name, my_off_t log_file_pos);
 
   /**
    * Compares two binlog positions to determine which one is larger.
    * 比较两个 binlog 位置以确定哪个更大。
    */
   static int compare(const char *log_file_name1, my_off_t log_file_pos1,
                      const char *log_file_name2, my_off_t log_file_pos2);
 
   /**
    * Checks if the active transaction node list is empty.
    * 检查活跃事务节点列表是否为空。
    * 
    * @return True if the list is empty, False otherwise.
    *         如果列表为空返回 True，否则返回 False。
    */
   bool is_empty() { return (trx_front_ == nullptr); }
};

/**
   AckInfo is a POD. It defines a structure including information related to an
   ack: server_id   - which slave the ack comes from. binlog_name - the binlog
   file name included in the ack. binlog_pos  - the binlog file position
   included in the ack.
*/
struct AckInfo {
  int server_id;
  char binlog_name[FN_REFLEN];
  unsigned long long binlog_pos = 0;

  AckInfo() { clear(); }

  void clear() { binlog_name[0] = '\0'; }
  bool empty() const { return binlog_name[0] == '\0'; }
  bool is_server(int server_id) const { return this->server_id == server_id; }

  bool equal_to(const char *log_file_name, my_off_t log_file_pos) const {
    return (ActiveTranx::compare(binlog_name, binlog_pos, log_file_name,
                                 log_file_pos) == 0);
  }
  bool less_than(const char *log_file_name, my_off_t log_file_pos) const {
    return (ActiveTranx::compare(binlog_name, binlog_pos, log_file_name,
                                 log_file_pos) < 0);
  }

  void set(int server_id, const char *log_file_name, my_off_t log_file_pos) {
    this->server_id = server_id;
    update(log_file_name, log_file_pos);
  }
  void update(const char *log_file_name, my_off_t log_file_pos) {
    strcpy(binlog_name, log_file_name);
    binlog_pos = log_file_pos;
  }
};

/**
   AckContainer stores received acks internally and tell the caller the
   ack's position when a transaction is fully acknowledged, so it can wake
   up the waiting transactions.
 */
class AckContainer : public Trace {
 public:
  AckContainer() : m_ack_array(nullptr), m_size(0), m_empty_slot(0) {}
  ~AckContainer() {
    if (m_ack_array) my_free(m_ack_array);
  }

  /** Clear the content of the ack array */
  void clear() {
    if (m_ack_array) {
      for (unsigned i = 0; i < m_size; ++i) {
        m_ack_array[i].clear();
        m_ack_array[i].server_id = 0;
        m_ack_array[i].binlog_pos = 0;
      }
      m_empty_slot = m_size;
    }
    m_greatest_ack.clear();
  }

  /**
     Adjust capacity for the container and report the ack to semisync master,
     if it is full.

     @param[in] size size of the container.
     @param ackinfo Acknowledgement information

     @return 0 if succeeds, otherwise fails.
  */
  int resize(unsigned int size, const AckInfo **ackinfo);

  /**
     Insert an ack's information into the container and report the minimum
     ack to semisync master if it is full.

     @param[in] server_id  slave server_id of the ack
     @param[in] log_file_name  binlog file name of the ack
     @param[in] log_file_pos   binlog file position of the ack

     @return Pointer of an ack if the ack should be reported to semisync master.
             Otherwise, NULL is returned.
  */
  const AckInfo *insert(int server_id, const char *log_file_name,
                        my_off_t log_file_pos);
  const AckInfo *insert(const AckInfo &ackinfo) {
    return insert(ackinfo.server_id, ackinfo.binlog_name, ackinfo.binlog_pos);
  }

 private:
  /* The greatest ack of the acks already reported to semisync master. */
  AckInfo m_greatest_ack;

  AckInfo *m_ack_array;
  /* size of the array */
  unsigned int m_size;
  /* index of an empty slot, it helps improving insert speed. */
  unsigned int m_empty_slot;

  /* Prohibit to copy AckContainer objects */
  AckContainer(AckContainer &container);
  AckContainer &operator=(const AckContainer &container);

  bool full() { return m_empty_slot == m_size; }
  unsigned int size() { return m_size; }

  /**
     Remove all acks which equal to the given position.

     @param[in] log_file_name  binlog name of the ack that should be removed
     @param[in] log_file_pos   binlog position of the ack that should removed
  */
  void remove_all(const char *log_file_name, my_off_t log_file_pos) {
    unsigned int i = m_size;
    for (i = 0; i < m_size; i++) {
      if (m_ack_array[i].equal_to(log_file_name, log_file_pos)) {
        m_ack_array[i].clear();
        m_empty_slot = i;
      }
    }
  }

  /**
     Update a slave's ack into the container if another ack of the
     slave is already in it.

     @param[in] server_id      server_id of the ack
     @param[in] log_file_name  binlog file name of the ack
     @param[in] log_file_pos   binlog file position of the ack

     @return index of the slot that is updated. if it equals to
             the size of container, then no slot is updated.
  */
  unsigned int updateIfExist(int server_id, const char *log_file_name,
                             my_off_t log_file_pos) {
    unsigned int i;

    m_empty_slot = m_size;
    for (i = 0; i < m_size; i++) {
      if (m_ack_array[i].empty())
        m_empty_slot = i;
      else if (m_ack_array[i].is_server(server_id)) {
        m_ack_array[i].update(log_file_name, log_file_pos);
        if (trace_level_ & kTraceDetail)
          LogErr(INFORMATION_LEVEL, ER_SEMISYNC_UPDATE_EXISTING_SLAVE_ACK, i);
        break;
      }
    }
    return i;
  }

  /**
     Find the minimum ack which is smaller than given position. When more than
     one slots are minimum acks, it returns the one has smallest index.

     @param[in] log_file_name  binlog file name
     @param[in] log_file_pos   binlog file position

     @return NULL if no ack is smaller than given position, otherwise
              return its pointer.
  */
  AckInfo *minAck(const char *log_file_name, my_off_t log_file_pos) {
    unsigned int i;
    AckInfo *ackinfo = nullptr;

    for (i = 0; i < m_size; i++) {
      if (m_ack_array[i].less_than(log_file_name, log_file_pos))
        ackinfo = m_ack_array + i;
    }

    return ackinfo;
  }
};

/**
   The extension class for the master of semi-synchronous replication
   半同步复制主库的扩展类
*/
class ReplSemiSyncMaster : public ReplSemiSyncBase {
  private:
   ActiveTranx *active_tranxs_ = nullptr;
   /* 
      active transaction list: the list will
      be cleared when semi-sync switches off.
      活跃事务列表：当半同步复制关闭时，该列表将被清空。
   */
 
   /* 
      True when initObject has been called 
      当 `initObject` 被调用时设置为 true。
   */
   bool init_done_ = false;
 
   /* 
      Mutex that protects the following state variables and the active
      transaction list.
      Under no circumstances we can acquire mysql_bin_log.LOCK_log if we are
      already holding LOCK_binlog_ because it can cause deadlocks.
      用于保护以下状态变量和活跃事务列表的互斥锁。
      在任何情况下，如果已经持有 `LOCK_binlog_`，都不能获取 `mysql_bin_log.LOCK_log`，
      否则可能导致死锁。
   */
   mysql_mutex_t LOCK_binlog_;
 
   /* 
      This is set to true when reply_file_name_ contains meaningful data.
      当 `reply_file_name_` 包含有效数据时设置为 true。
   */
   bool reply_file_name_inited_ = false;
 
   /* 
      The binlog name up to which we have received replies from any slaves.
      我们从任意从库接收到确认的 binlog 文件名。
   */
   char reply_file_name_[FN_REFLEN];
 
   /* 
      The position in that file up to which we have the reply from any slaves.
      我们从任意从库接收到确认的 binlog 文件中的位置。
   */
   my_off_t reply_file_pos_ = 0;
 
   /* 
      This is set to true when we know the 'smallest' wait position.
      当我们知道最小的等待位置时设置为 true。
   */
   bool wait_file_name_inited_ = false;
 
   /* 
      NULL, or the 'smallest' filename that a transaction is waiting for
      slave replies.
      NULL 或事务等待从库确认的最小文件名。
   */
   char wait_file_name_[FN_REFLEN];

  /* 
   * The smallest position in that file that a trx is waiting for: the trx
   * can proceed and send an 'ok' to the client when the master has got the
   * reply from the slave indicating that it already got the binlog events.
   * 
   * 表示当前事务等待从库确认的最小 binlog 位置。当主库收到从库的确认（从库已接收到 binlog 事件）后，
   * 事务可以继续执行并向客户端发送 'ok'。
   */
  my_off_t wait_file_pos_ = 0;
  
  /* 
   * This is set to true when we know the 'largest' transaction commit
   * position in the binlog file.
   * We always maintain the position no matter whether semi-sync is switched
   * on or switched off. When a transaction wait timeout occurs, semi-sync will
   * switch off. Binlog-dump thread can use the three fields to detect when
   * slaves catch up on replication so that semi-sync can switch on again.
   * 
   * 标记是否已初始化最大事务提交位置。即使半同步复制被关闭，也会维护这些位置。
   * 当事务等待超时时，半同步复制会关闭。Binlog-dump 线程可以使用这些字段检测从库何时追上主库，
   * 从而重新启用半同步复制。
   */
  bool commit_file_name_inited_ = false;
  
  /* 
   * The 'largest' binlog filename that a commit transaction is seeing.
   * 
   * 当前事务看到的 binlog 文件中最大的提交位置的文件名。
   */
  char commit_file_name_[FN_REFLEN];
  
  /* 
   * The 'largest' position in that file that a commit transaction is seeing.
   * 
   * 当前事务看到的 binlog 文件中最大的提交位置的偏移量。
   */
  my_off_t commit_file_pos_ = 0;
  
  /* 
   * All global variables which can be set by parameters.
   * 
   * 所有可以通过参数设置的全局变量。
   */
  
  /* 
   * semi-sync is enabled on the master.
   * 
   * 表示主库是否启用了半同步复制。使用 `volatile` 确保多线程环境下的可见性。
   */
  volatile bool master_enabled_ = false;
  
  /* 
   * timeout period(ms) during tranx wait.
   * 
   * 事务等待从库确认的超时时间（以毫秒为单位）。如果超时，事务将不再等待。
   */
  unsigned long wait_timeout_ = 0;
  
  /* 
   * whether semi-sync is switched.
   * 
   * 当前半同步复制的状态。`true` 表示半同步复制已开启，`false` 表示已关闭。
   */
  bool state_ = false;

  AckContainer ack_container_;

  void lock();
  void unlock();

  /* Is semi-sync replication on? */
  bool is_on() { return (state_); }

  void set_master_enabled(bool enabled) { master_enabled_ = enabled; }

  /* Switch semi-sync off because of timeout in transaction waiting. */
  int switch_off();

  void force_switch_on();

  /* Switch semi-sync on when slaves catch up. */
  int try_switch_on(const char *log_file_name, my_off_t log_file_pos);

 public:
  ReplSemiSyncMaster();
  ~ReplSemiSyncMaster();

  bool getMasterEnabled() { return master_enabled_; }
  void setTraceLevel(unsigned long trace_level) {
    trace_level_ = trace_level;
    ack_container_.trace_level_ = trace_level;
    if (active_tranxs_) active_tranxs_->trace_level_ = trace_level;
  }

  /* Set if the master has to wait for an ack from the salve or not. */
  void set_wait_no_replica(const void *val);

  /* Set the transaction wait timeout period, in milliseconds. */
  void setWaitTimeout(unsigned long wait_timeout) {
    wait_timeout_ = wait_timeout;
  }

  /* Initialize this class after MySQL parameters are initialized. this
   * function should be called once at bootstrap time.
   */
  int initObject();

  /* Enable the object to enable semi-sync replication inside the master. */
  int enableMaster();

  /* Enable the object to enable semi-sync replication inside the master. */
  int disableMaster();

  /* Add a semi-sync replication slave */
  void add_slave();

  /* Remove a semi-sync replication slave */
  void remove_slave();

  /* Is the slave servered by the thread requested semi-sync */
  bool is_semi_sync_slave();

  /* It parses a reply packet and call reportReplyBinlog to handle it. */
  int reportReplyPacket(uint32 server_id, const uchar *packet,
                        ulong packet_len);

  /* In semi-sync replication, reports up to which binlog position we have
   * received replies from the slave indicating that it already get the events
   * or that was skipped in the master.
   *
   * Input:
   *  log_file_name - (IN)  binlog file name
   *  end_offset    - (IN)  the offset in the binlog file up to which we have
   *                        the replies from the slave or that was skipped
   */
  void reportReplyBinlog(const char *log_file_name, my_off_t end_offset);

  /* Commit a transaction in the final step.  This function is called from
   * InnoDB before returning from the low commit.  If semi-sync is switch on,
   * the function will wait to see whether binlog-dump thread get the reply for
   * the events of the transaction.  Remember that this is not a direct wait,
   * instead, it waits to see whether the binlog-dump thread has reached the
   * point.  If the wait times out, semi-sync status will be switched off and
   * all other transaction would not wait either.
   *
   * Input:  (the transaction events' ending binlog position)
   *  trx_wait_binlog_name - (IN)  ending position's file name
   *  trx_wait_binlog_pos  - (IN)  ending position's file offset
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int commitTrx(const char *trx_wait_binlog_name, my_off_t trx_wait_binlog_pos);

  /* Reserve space in the replication event packet header:
   *  . slave semi-sync off: 1 byte - (0)
   *  . slave semi-sync on:  3 byte - (0, 0xef, 0/1}
   *
   * Input:
   *  header   - (IN)  the header buffer
   *  size     - (IN)  size of the header buffer
   *
   * Return:
   *  size of the bytes reserved for header
   */
  int reserveSyncHeader(unsigned char *header, unsigned long size);

  /* Update the sync bit in the packet header to indicate to the slave whether
   * the master will wait for the reply of the event.  If semi-sync is switched
   * off and we detect that the slave is catching up, we switch semi-sync on.
   *
   * Input:
   *  packet        - (IN)  the packet containing the replication event
   *  log_file_name - (IN)  the event ending position's file name
   *  log_file_pos  - (IN)  the event ending position's file offset
   *  server_id     - (IN)  master server id number
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int updateSyncHeader(unsigned char *packet, const char *log_file_name,
                       my_off_t log_file_pos, uint32 server_id);

  /* Called when a transaction finished writing binlog events.
   *  . update the 'largest' transactions' binlog event position
   *  . insert the ending position in the active transaction list if
   *    semi-sync is on
   *
   * Input:  (the transaction events' ending binlog position)
   *  log_file_name - (IN)  transaction ending position's file name
   *  log_file_pos  - (IN)  transaction ending position's file offset
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int writeTranxInBinlog(const char *log_file_name, my_off_t log_file_pos);

  /* Read the slave's reply so that we know how much progress the slave makes
   * on receive replication events.
   *
   * Input:
   *  net          - (IN)  the connection to master
   *  event_buf    - (IN)  pointer to the event packet
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int readSlaveReply(NET *net, const char *event_buf);

  /* In semi-sync replication, this method simulates the reception of
   * an reply and executes reportReplyBinlog directly when a transaction
   * is skipped in the master.
   *
   * Input:
   *  event_buf     - (IN)  pointer to the event packet
   *  server_id     - (IN)  master server id numbe
   *  log_file_name - (IN)  the event ending position's file name
   *  log_file_pos  - (IN)  the event ending position's file offset
   *
   * Return:
   *  0: success;  non-zero: error
   */
  int skipSlaveReply(const char *event_buf, uint32 server_id,
                     const char *log_file_name, my_off_t log_file_pos);

  /* Export internal statistics for semi-sync replication. */
  void setExportStats();

  /* 'reset master' command is issued from the user and semi-sync need to
   * go off for that.
   */
  int resetMaster();

  /*
    'SET rpl_semi_sync_source_wait_for_replica_count' command is issued from
    user and semi-sync need to update
    rpl_semi_sync_source_wait_for_replica_count and notify ack_container_ to
    resize itself.

    @param[in] new_value The value users want to set to.

    @return It returns 0 if succeeds, otherwise 1 is returned.
   */
  int setWaitSlaveCount(unsigned int new_value);

  /*
      Update ack_array after receiving an ack from a dump connection. If any
      binlog pos is already replied by rpl_semi_sync_source_wait_for_replica_count
      slaves, it will call reportReplyBinlog to increase received binlog
      position and wake up waiting transactions. It acquires LOCK_binlog_
      to protect the operation.
  
      在接收到来自 dump 连接的确认（ack）后更新 ack_array。如果某个 binlog 位置
      已经被 rpl_semi_sync_source_wait_for_replica_count 个从库确认，
      则调用 reportReplyBinlog 来更新接收到的 binlog 位置，并唤醒等待的事务。
      该操作会获取 LOCK_binlog_ 以保护操作的线程安全。
  
      @param[in] server_id  slave server_id of the ack
                            确认的从库 server_id。
      @param[in] log_file_name  binlog file name of the ack
                                确认的 binlog 文件名。
      @param[in] log_file_pos   binlog file position of the ack
                                确认的 binlog 文件位置。
  */
  void handleAck(int server_id, const char *log_file_name,
                 my_off_t log_file_pos) {
    lock();  // 获取互斥锁，确保线程安全。
  
    // 如果只需要一个从库的确认即可满足条件，直接调用 reportReplyBinlog。
    if (rpl_semi_sync_source_wait_for_replica_count == 1)
      reportReplyBinlog(log_file_name, log_file_pos);
    else {
      const AckInfo *ackinfo = nullptr;
  
      // 将确认信息插入到 ack_container_ 中。
      ackinfo = ack_container_.insert(server_id, log_file_name, log_file_pos);
  
      // 如果 ackinfo 不为空，说明某个 binlog 位置已经被足够数量的从库确认，
      // 调用 reportReplyBinlog 更新接收到的 binlog 位置并唤醒等待的事务。
      if (ackinfo != nullptr)
        reportReplyBinlog(ackinfo->binlog_name, ackinfo->binlog_pos);
    }
  
    unlock();  // 释放互斥锁。
  }
};

/* System and status variables for the master component */
extern bool rpl_semi_sync_source_enabled;
extern char rpl_semi_sync_source_status;
extern unsigned long rpl_semi_sync_source_clients;
extern unsigned long rpl_semi_sync_source_timeout;
extern unsigned long rpl_semi_sync_source_trace_level;
extern unsigned long rpl_semi_sync_source_yes_transactions;
extern unsigned long rpl_semi_sync_source_no_transactions;
extern unsigned long rpl_semi_sync_source_off_times;
extern unsigned long rpl_semi_sync_source_wait_timeouts;
extern unsigned long rpl_semi_sync_source_timefunc_fails;
extern unsigned long rpl_semi_sync_source_num_timeouts;
extern unsigned long rpl_semi_sync_source_wait_sessions;
extern unsigned long rpl_semi_sync_source_wait_pos_backtraverse;
extern unsigned long rpl_semi_sync_source_avg_trx_wait_time;
extern unsigned long rpl_semi_sync_source_avg_net_wait_time;
extern unsigned long long rpl_semi_sync_source_net_wait_num;
extern unsigned long long rpl_semi_sync_source_trx_wait_num;
extern unsigned long long rpl_semi_sync_source_net_wait_time;
extern unsigned long long rpl_semi_sync_source_trx_wait_time;

/*
  This indicates whether we should keep waiting if no semi-sync slave
  is available.
     0           : stop waiting if detected no available semi-sync slave.
     1 (default) : keep waiting until timeout even no available semi-sync slave.
*/
extern bool rpl_semi_sync_source_wait_no_replica;
#endif /* SEMISYNC_SOURCE_H */
