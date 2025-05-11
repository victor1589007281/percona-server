/*****************************************************************************

Copyright (c) 1997, 2022, Oracle and/or its affiliates.

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

/** @file include/ibuf0ibuf.h
 Insert buffer

 Created 7/19/1997 Heikki Tuuri
 *******************************************************/

#ifndef ibuf0ibuf_h
#define ibuf0ibuf_h

#include "univ.i"

#include "dict0mem.h"
#include "fsp0fsp.h"
#include "mtr0mtr.h"

#include "ibuf0types.h"

/** Default value for maximum on-disk size of change buffer in terms
of percentage of the buffer pool. */
/** 变更缓冲区最大磁盘空间占缓冲池的默认百分比值 */
constexpr uint32_t CHANGE_BUFFER_DEFAULT_SIZE = 25;

#ifndef UNIV_HOTBACKUP
/* Possible operations buffered in the insert/whatever buffer. See
ibuf_insert(). DO NOT CHANGE THE VALUES OF THESE, THEY ARE STORED ON DISK. */
/* 可以在插入/其他缓冲区中缓冲的操作类型。参见ibuf_insert()。
   不要修改这些值，它们会被存储在磁盘上 */
typedef enum {
  IBUF_OP_INSERT = 0,      // 插入操作
  IBUF_OP_DELETE_MARK = 1, // 删除标记操作
  IBUF_OP_DELETE = 2,      // 删除操作

  /* Number of different operation types. */
  /* 不同操作类型的数量 */
  IBUF_OP_COUNT = 3
} ibuf_op_t;

/** Combinations of operations that can be buffered.
@see innodb_change_buffering_names */
/** 可以被缓冲的操作组合 @see innodb_change_buffering_names */
enum ibuf_use_t {
  IBUF_USE_NONE = 0,                  // 不缓冲任何操作
  IBUF_USE_INSERT,                     // 仅缓冲插入操作
  IBUF_USE_DELETE_MARK,                // 仅缓冲删除标记操作
  IBUF_USE_INSERT_DELETE_MARK,         // 缓冲插入和删除标记操作
  IBUF_USE_DELETE,                     // 缓冲删除操作
  IBUF_USE_ALL                         // 缓冲所有操作
};

/** Operations that can currently be buffered. */
/** 当前可以被缓冲的操作 */
extern ulong innodb_change_buffering;

/** The insert buffer control structure */
/** 插入缓冲区控制结构 */
extern ibuf_t *ibuf;

/* The purpose of the insert buffer is to reduce random disk access.
When we wish to insert a record into a non-unique secondary index and
the B-tree leaf page where the record belongs to is not in the buffer
pool, we insert the record into the insert buffer B-tree, indexed by
(space_id, page_no).  When the page is eventually read into the buffer
pool, we look up the insert buffer B-tree for any modifications to the
page, and apply these upon the completion of the read operation.  This
is called the insert buffer merge. */
/* 插入缓冲区的目的是减少随机磁盘访问。
   当我们想向非唯一二级索引插入一条记录，且该记录所属的B树叶子页不在缓冲池中时，
   我们会将该记录插入到以(space_id, page_no)为索引的插入缓冲区B树中。
   当该页最终被读入缓冲池时，我们查找插入缓冲区B树中对该页的任何修改，
   并在读操作完成后应用这些修改。这被称为插入缓冲区合并。*/

/* The insert buffer merge must always succeed.  To guarantee this,
the insert buffer subsystem keeps track of the free space in pages for
which it can buffer operations.  Two bits per page in the insert
buffer bitmap indicate the available space in coarse increments.  The
free bits in the insert buffer bitmap must never exceed the free space
on a page.  It is safe to decrement or reset the bits in the bitmap in
a mini-transaction that is committed before the mini-transaction that
affects the free space.  It is unsafe to increment the bits in a
separately committed mini-transaction, because in crash recovery, the
free bits could momentarily be set too high. */
/* 插入缓冲区合并必须总是成功。为了保证这一点，
   插入缓冲区子系统会跟踪可以缓冲操作的页中的空闲空间。
   插入缓冲区位图中每页的两个位以粗略的增量表示可用空间。
   插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
   在影响空闲空间的小事务之前提交的小事务中减少或重置位图中的位是安全的。
   在单独提交的小事务中增加位是不安全的，因为在崩溃恢复时，
   空闲位可能会暂时设置得过高。*/

/** Creates the insert buffer data structure at a database startup. */
/** 在数据库启动时创建插入缓冲区数据结构 */
void ibuf_init_at_db_start(void);

/** Updates the max_size value for ibuf. */
/** 更新ibuf的最大大小值 */
void ibuf_max_size_update(ulint new_val); /*!< in: new value in terms of
                                          percentage of the buffer pool size */
                                          /*!< 输入: 以缓冲池大小百分比表示的新值 */

/** Reads the biggest tablespace id from the high end of the insert buffer
 tree and updates the counter in fil_system. */
/** 从插入缓冲区树的高端读取最大的表空间ID，并更新fil_system中的计数器 */
void ibuf_update_max_tablespace_id(void);

/** Starts an insert buffer mini-transaction. */
/** 开始一个插入缓冲区小事务 */
static inline void ibuf_mtr_start(mtr_t *mtr); /*!< out: mini-transaction */
                                               /*!< 输出: 小事务 */

/** Commits an insert buffer mini-transaction. */
/** 提交一个插入缓冲区小事务 */
static inline void ibuf_mtr_commit(mtr_t *mtr); /*!< in/out: mini-transaction */
                                                /*!< 输入/输出: 小事务 */

/** Initializes an ibuf bitmap page. */
/** 初始化一个ibuf位图页 */
void ibuf_bitmap_page_init(buf_block_t *block, /*!< in: bitmap page */
                           mtr_t *mtr /*!< in: mtr */);
                           /*!< 输入: 位图页 */
                           /*!< 输入: 小事务 */

/** Resets the free bits of the page in the ibuf bitmap. This is done in a
 separate mini-transaction, hence this operation does not restrict
 further work to only ibuf bitmap operations, which would result if the
 latch to the bitmap page were kept.  NOTE: The free bits in the insert
 buffer bitmap must never exceed the free space on a page.  It is safe
 to decrement or reset the bits in the bitmap in a mini-transaction
 that is committed before the mini-transaction that affects the free
 space. */
/** 重置ibuf位图中页的空闲位。此操作在一个单独的小事务中完成，
    因此不会限制后续只能进行ibuf位图操作（如果保持位图页的锁存会导致这种情况）。
    注意：插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
    在影响空闲空间的小事务之前提交的小事务中减少或重置位图中的位是安全的。 */
    void ibuf_reset_free_bits(
      buf_block_t *block); /*!< in: index page; free bits are set to 0
                           if the index is a non-clustered
                           non-unique, and page level is 0 */
                           /*!< 输入: 索引页；如果索引是非聚集非唯一的且页级别为0，
                                则空闲位被设置为0 */
  
  /** Updates the free bits of an uncompressed page in the ibuf bitmap if there
  is not enough free on the page any more.  This is done in a separate
  mini-transaction, hence this operation does not restrict further work to only
  ibuf bitmap operations, which would result if the latch to the bitmap page were
  kept.  NOTE: The free bits in the insert buffer bitmap must never exceed the
  free space on a page.  It is unsafe to increment the bits in a separately
  committed mini-transaction, because in crash recovery, the free bits could
  momentarily be set too high.  It is only safe to use this function for
  decrementing the free bits.  Should more free space become available, we must
  not update the free bits here, because that would break crash recovery.
  @param[in]      block           index page to which we have added new records;
                                  the free bits are updated if the index is
                                  non-clustered and non-unique and the page level
                                  is 0, and the page becomes fuller
  @param[in]      max_ins_size    value of maximum insert size with reorganize
                                  before the latest operation performed to the
                                  page
  @param[in]      increase        upper limit for the additional space used in
                                  the latest operation, if known, or
                                  ULINT_UNDEFINED */
  /** 如果页上不再有足够的空闲空间，则更新ibuf位图中未压缩页的空闲位。
      此操作在一个单独的小事务中完成，因此不会限制后续只能进行ibuf位图操作。
      注意：插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
      在单独提交的小事务中增加位是不安全的，因为在崩溃恢复时，空闲位可能会暂时设置得过高。
      此函数仅安全用于减少空闲位。如果有更多空闲空间可用，我们不能在此更新空闲位，
      因为这会破坏崩溃恢复。
  @param[in]      block           添加了新记录的索引页；
                                  如果索引是非聚集非唯一的且页级别为0，
                                  并且页变得更满，则更新空闲位
  @param[in]      max_ins_size    执行最新操作前，通过重组得到的最大插入大小值
  @param[in]      increase        最新操作中使用的额外空间的上限，
                                  如果已知，否则为ULINT_UNDEFINED */
  static inline void ibuf_update_free_bits_if_full(buf_block_t *block,
                                                   ulint max_ins_size,
                                                   ulint increase);
  
  /** Updates the free bits for an uncompressed page to reflect the present
   state.  Does this in the mtr given, which means that the latching
   order rules virtually prevent any further operations for this OS
   thread until mtr is committed.  NOTE: The free bits in the insert
   buffer bitmap must never exceed the free space on a page.  It is safe
   to set the free bits in the same mini-transaction that updated the
   page. */
  /** 更新未压缩页的空闲位以反映当前状态。
      在给定的小事务中执行此操作，这意味着锁存顺序规则实际上会阻止此OS线程
      执行任何进一步操作，直到小事务提交。
      注意：插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
      在更新页的同一小事务中设置空闲位是安全的。 */
  void ibuf_update_free_bits_low(const buf_block_t *block, /*!< in: index page */
                                 ulint max_ins_size,       /*!< in: value of
                                                           maximum insert size
                                                           with reorganize before
                                                           the latest operation
                                                           performed to the page */
                                 mtr_t *mtr);              /*!< in/out: mtr */
                                 /*!< 输入: 索引页 */
                                 /*!< 输入: 执行最新操作前，通过重组得到的最大插入大小值 */
                                 /*!< 输入/输出: 小事务 */
  
  /** Updates the free bits for a compressed page to reflect the present
   state.  Does this in the mtr given, which means that the latching
   order rules virtually prevent any further operations for this OS
   thread until mtr is committed.  NOTE: The free bits in the insert
   buffer bitmap must never exceed the free space on a page.  It is safe
   to set the free bits in the same mini-transaction that updated the
   page. */
  /** 更新压缩页的空闲位以反映当前状态。
      在给定的小事务中执行此操作，这意味着锁存顺序规则实际上会阻止此OS线程
      执行任何进一步操作，直到小事务提交。
      注意：插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
      在更新页的同一小事务中设置空闲位是安全的。 */
  void ibuf_update_free_bits_zip(buf_block_t *block, /*!< in/out: index page */
                                 mtr_t *mtr);        /*!< in/out: mtr */
                                 /*!< 输入/输出: 索引页 */
                                 /*!< 输入/输出: 小事务 */
  
  /** Updates the free bits for the two pages to reflect the present state.
   Does this in the mtr given, which means that the latching order rules
   virtually prevent any further operations until mtr is committed.
   NOTE: The free bits in the insert buffer bitmap must never exceed the
   free space on a page.  It is safe to set the free bits in the same
   mini-transaction that updated the pages. */
  /** 更新两个页的空闲位以反映当前状态。
      在给定的小事务中执行此操作，这意味着锁存顺序规则实际上会阻止任何进一步操作，
      直到小事务提交。
      注意：插入缓冲区位图中的空闲位绝不能超过页上的空闲空间。
      在更新页的同一小事务中设置空闲位是安全的。 */
  void ibuf_update_free_bits_for_two_pages_low(
      buf_block_t *block1, /*!< in: index page */
      buf_block_t *block2, /*!< in: index page */
      mtr_t *mtr);         /*!< in: mtr */
      /*!< 输入: 索引页 */
      /*!< 输入: 索引页 */
      /*!< 输入: 小事务 */
  
  /** A basic partial test if an insert to the insert buffer could be possible
  and recommended.
  @param[in]      index                   index where to insert
  @param[in]      ignore_sec_unique       if != 0, we should ignore UNIQUE
                                          constraint on a secondary index when
                                          we decide*/
  /** 基本部分测试，判断是否可能且建议将插入操作放入插入缓冲区。
  @param[in]      index           要插入的索引
  @param[in]      ignore_sec_unique 如果!=0，在决定时应忽略二级索引上的UNIQUE约束 */
  static inline bool ibuf_should_try(dict_index_t *index,
                                     ulint ignore_sec_unique);
  
  /** Returns true if the current OS thread is performing an insert buffer
   routine.
  
   For instance, a read-ahead of non-ibuf pages is forbidden by threads
   that are executing an insert buffer routine.
   @return true if inside an insert buffer routine */
  /** 如果当前OS线程正在执行插入缓冲区例程，则返回true。
      例如，执行插入缓冲区例程的线程禁止预读非ibuf页。
      @return 如果在插入缓冲区例程中则返回true */
  [[nodiscard]] static inline bool ibuf_inside(
      const mtr_t *mtr); /*!< in: mini-transaction */
      /*!< 输入: 小事务 */
  
  /** Checks if a page address is an ibuf bitmap page (level 3 page) address.
  @param[in]      page_id         page id
  @param[in]      page_size       page size
  @return true if a bitmap page */
  /** 检查页地址是否是ibuf位图页（第3级页）地址。
  @param[in]      page_id         页ID
  @param[in]      page_size       页大小
  @return 如果是位图页则返回true */
  static inline bool ibuf_bitmap_page(const page_id_t &page_id,
                                      const page_size_t &page_size);
  
  /** Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages.
  Must not be called when recv_no_ibuf_operations==true.
  @param[in]      page_id         page id
  @param[in]      page_size       page size
  @param[in]      x_latch         false if relaxed check (avoid latching the
  bitmap page)
  @param[in]      location Location where called
  @param[in,out]  mtr             mtr which will contain an x-latch to the
  bitmap page if the page is not one of the fixed address ibuf pages, or NULL,
  in which case a new transaction is created.
  @return true if level 2 or level 3 page */
  /** 检查页是否是ibuf页层次结构中的第2级或第3级页。
      当recv_no_ibuf_operations==true时不能调用此函数。
  @param[in]      page_id         页ID
  @param[in]      page_size       页大小
  @param[in]      x_latch         如果为false则进行宽松检查（避免锁定位图页）
  @param[in]      location        调用位置
  @param[in,out]  mtr             小事务，如果不是固定地址的ibuf页，
                                  将包含位图页的x-latch，或者为NULL，
                                  此时会创建新事务
  @return 如果是第2级或第3级页则返回true */
  [[nodiscard]] bool ibuf_page_low(const page_id_t &page_id,
                                   const page_size_t &page_size,
                                   IF_DEBUG(bool x_latch, ) ut::Location location,
                                   mtr_t *mtr) MY_ATTRIBUTE((warn_unused_result));
  
  /** Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages.
  Must not be called when recv_no_ibuf_operations==true.
  @param[in]      page_id         Tablespace/page identifier
  @param[in]      page_size       Page size
  @param[in]  location Location where requested
  @param[in,out]  mtr             Mini-transaction or NULL
  @return true if level 2 or level 3 page */
  /** 检查页是否是ibuf页层次结构中的第2级或第3级页。
      当recv_no_ibuf_operations==true时不能调用此函数。
  @param[in]      page_id         表空间/页标识符
  @param[in]      page_size       页大小
  @param[in]      location        请求位置
  @param[in,out]  mtr             小事务或NULL
  @return 如果是第2级或第3级页则返回true */
  inline bool ibuf_page(const page_id_t &page_id, const page_size_t &page_size,
                        ut::Location location, mtr_t *mtr) {
    return ibuf_page_low(page_id, page_size, IF_DEBUG(true, ) location, mtr);
  }
  
  /** Frees excess pages from the ibuf free list. This function is called when an
  OS thread calls fsp services to allocate a new file segment, or a new page to a
  file segment, and the thread did not own the fsp latch before this call. */
  /** 从ibuf空闲列表中释放多余的页。当OS线程调用fsp服务分配新文件段或文件段中的新页，
      且该线程在此调用前不拥有fsp锁存时，会调用此函数。 */
  void ibuf_free_excess_pages(void);
  
  /** Buffer an operation in the insert/delete buffer, instead of doing it
  directly to the disk page, if this is possible. Does not do it if the index
  is clustered or unique.
  @param[in]      op              operation type
  @param[in]      entry           index entry to insert
  @param[in,out]  index           index where to insert
  @param[in]      page_id         page id where to insert
  @param[in]      page_size       page size
  @param[in,out]  thr             query thread
  @return true if success */
  /** 如果可能，将操作缓冲在插入/删除缓冲区中，而不是直接对磁盘页执行。
      如果索引是聚集或唯一的，则不执行此操作。
  @param[in]      op              操作类型
  @param[in]      entry          要插入的索引条目
  @param[in,out]  index          要插入的索引
  @param[in]      page_id        要插入的页ID
  @param[in]      page_size      页大小
  @param[in,out]  thr            查询线程
  @return 如果成功则返回true */
  bool ibuf_insert(ibuf_op_t op, const dtuple_t *entry, dict_index_t *index,
                   const page_id_t &page_id, const page_size_t &page_size,
                   que_thr_t *thr);

/** When an index page is read from a disk to the buffer pool, this function
applies any buffered operations to the page and deletes the entries from the
insert buffer. If the page is not read, but created in the buffer pool, this
function deletes its buffered entries from the insert buffer; there can
exist entries for such a page if the page belonged to an index which
subsequently was dropped.
@param[in,out]  block                   if page has been read from disk,
pointer to the page x-latched, else NULL
@param[in]      page_id                 page id of the index page
@param[in]      update_ibuf_bitmap      normally this is set to true, but
if we have deleted or are deleting the tablespace, then we naturally do not
want to update a non-existent bitmap page
@param[in]      page_size               page size */
/** 当从磁盘读取索引页到缓冲池时，此函数应用所有缓冲的操作到该页，
    并从插入缓冲区删除这些条目。如果页不是从磁盘读取而是在缓冲池中创建的，
    此函数也会从插入缓冲区删除其缓冲条目；如果页属于后来被删除的索引，
    则可能存在这样的条目。
@param[in,out]  block           如果页已从磁盘读取，指向该页的x-latch指针，否则为NULL
@param[in]      page_id         索引页的页ID
@param[in]      update_ibuf_bitmap 通常设为true，但如果表空间已被删除或正在删除，
                               则不需要更新不存在的位图页
@param[in]      page_size       页大小 */
void ibuf_merge_or_delete_for_page(buf_block_t *block, const page_id_t &page_id,
  const page_size_t *page_size,
  bool update_ibuf_bitmap);

/** Deletes all entries in the insert buffer for a given space id. This is used
in DISCARD TABLESPACE and IMPORT TABLESPACE.
NOTE: this does not update the page free bitmaps in the space. The space will
become CORRUPT when you call this function! */
/** 删除插入缓冲区中给定空间ID的所有条目。用于DISCARD TABLESPACE和IMPORT TABLESPACE。
注意：这不会更新空间中的页空闲位图。调用此函数后空间将变为CORRUPT状态！ */
void ibuf_delete_for_discarded_space(space_id_t space); /*!< in: space id */
                      /*!< 输入: 空间ID */

/** Contract the change buffer by reading pages to the buffer pool.
@param[in]      full            If true, do a full contraction based on
PCT_IO(100). If false, the size of contract batch is determined based on the
current size of the change buffer.
@return a lower limit for the combined size in bytes of entries which will be
merged from ibuf trees to the pages read, 0 if ibuf is empty */
/** 通过读取页到缓冲池来收缩变更缓冲区。
@param[in]      full      如果为true，基于PCT_IO(100)进行完全收缩；
如果为false，根据变更缓冲区当前大小决定收缩批次大小
@return 将从ibuf树合并到读取页的条目总大小的下限，如果ibuf为空则返回0 */
ulint ibuf_merge_in_background(bool full);

/** Contracts insert buffer trees by reading pages referring to space_id
to the buffer pool.
@returns number of pages merged.*/
/** 通过读取引用space_id的页到缓冲池来收缩插入缓冲区树。
@return 合并的页数 */
ulint ibuf_merge_space(space_id_t space); /*!< in: space id */
         /*!< 输入: 空间ID */

#endif /* !UNIV_HOTBACKUP */

/** Parses a redo log record of an ibuf bitmap page init.
@param[in] ptr Buffer.
@param[in] end_ptr Buffer end.
@param[in] block Block or nullptr.
@param[in] mtr MTR or nullptr.
@return end of log record or NULL */
/** 解析ibuf位图页初始化的redo日志记录。
@param[in] ptr      缓冲区
@param[in] end_ptr  缓冲区结束位置
@param[in] block    块或nullptr
@param[in] mtr      MTR或nullptr
@return 日志记录结束位置或NULL */
byte *ibuf_parse_bitmap_init(byte *ptr, byte *end_ptr, buf_block_t *block,
mtr_t *mtr);

#ifndef UNIV_HOTBACKUP
#ifdef UNIV_IBUF_COUNT_DEBUG

/** Gets the ibuf count for a given page.
@param[in]      page_id page id
@return number of entries in the insert buffer currently buffered for
this page */
/** 获取给定页的ibuf计数。
@param[in]      page_id 页ID
@return 当前为此页缓冲的插入缓冲区条目数 */
ulint ibuf_count_get(const page_id_t &page_id);

#endif /* UNIV_IBUF_COUNT_DEBUG */

/** Looks if the insert buffer is empty.
@return true if empty */
/** 检查插入缓冲区是否为空。
@return 如果为空返回true */
bool ibuf_is_empty(void);

/** Prints info of ibuf. */
/** 打印ibuf信息。 */
void ibuf_print(FILE *file); /*!< in: file where to print */
/*!< 输入: 要打印到的文件 */

/********************************************************************
Read the first two bytes from a record's fourth field (counter field in new
records; something else in older records).
@return "counter" field, or ULINT_UNDEFINED if for some reason it can't be read
*/
/********************************************************************
从记录的第四个字段(新记录中的counter字段；旧记录中的其他内容)读取前两个字节。
@return "counter"字段，如果无法读取则返回ULINT_UNDEFINED
*/
ulint ibuf_rec_get_counter(const rec_t *rec); /*!< in: ibuf record */
              /*!< 输入: ibuf记录 */

/** Determine if there is any multi-value field data on the change buffer
record
@param[in]      rec     ibuf record
@return true if there is any multi-value field in the record */
/** 确定变更缓冲区记录上是否有任何多值字段数据
@param[in]      rec     ibuf记录
@return 如果记录中有多值字段则返回true */
bool ibuf_rec_has_multi_value(const rec_t *rec);

/** Closes insert buffer and frees the data structures. */
/** 关闭插入缓冲区并释放数据结构。 */
void ibuf_close(void);

/** Checks the insert buffer bitmaps on IMPORT TABLESPACE.
@return DB_SUCCESS or error code */
/** 在IMPORT TABLESPACE时检查插入缓冲区位图。
@return DB_SUCCESS或错误代码 */
[[nodiscard]] dberr_t ibuf_check_bitmap_on_import(
const trx_t *trx,     /*!< in: transaction */
/*!< 输入: 事务 */
space_id_t space_id); /*!< in: tablespace identifier */
/*!< 输入: 表空间标识符 */

/** Function to pass ibuf status variables */
/** 传递ibuf状态变量的函数 */
void ibuf_export_ibuf_status(ulint *free_list, ulint *segment_size);

/** Updates free bits and buffered bits for bulk loaded page.
@param[in]      block   index page
@param[in]      reset   flag if reset free val */
/** 更新批量加载页的空闲位和缓冲位。
@param[in]      block   索引页
@param[in]      reset   是否重置空闲值的标志 */
void ibuf_set_bitmap_for_bulk_load(buf_block_t *block, bool reset);

constexpr uint32_t IBUF_HEADER_PAGE_NO = FSP_IBUF_HEADER_PAGE_NO;
constexpr uint32_t IBUF_TREE_ROOT_PAGE_NO = FSP_IBUF_TREE_ROOT_PAGE_NO;

#endif /* !UNIV_HOTBACKUP */

/* The ibuf header page currently contains only the file segment header
for the file segment from which the pages for the ibuf tree are allocated */
/* ibuf头页当前仅包含文件段头，从中分配ibuf树的页 */
constexpr uint32_t IBUF_HEADER = PAGE_DATA;
/** fseg header for ibuf tree */
/** ibuf树的文件段头 */
constexpr uint32_t IBUF_TREE_SEG_HEADER = 0;

#include "ibuf0ibuf.ic"

#endif
