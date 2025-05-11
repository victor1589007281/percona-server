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

/** @file ibuf/ibuf0ibuf.cc
 Insert buffer

 Created 7/19/1997 Heikki Tuuri
 *******************************************************/

#include <sys/types.h>

#include "btr0sea.h"
#include "ha_prototypes.h"
#include "ibuf0ibuf.h"
#include "sync0sync.h"

#include "my_dbug.h"

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
bool srv_ibuf_disable_background_merge;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** Number of bits describing a single page */
constexpr size_t IBUF_BITS_PER_PAGE = 4;
static_assert(IBUF_BITS_PER_PAGE % 2 == 0,
              "IBUF_BITS_PER_PAGE must be an even number!");
/** The start address for an insert buffer bitmap page bitmap */
constexpr uint32_t IBUF_BITMAP = PAGE_DATA;

#ifndef UNIV_HOTBACKUP

#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "buf0buf.h"
#include "buf0rea.h"
#include "dict0boot.h"
#include "fil0fil.h"
#include "fsp0fsp.h"
#include "fsp0sysspace.h"
#include "fut0lst.h"
#include "lock0lock.h"
#include "log0buf.h"
#include "log0chkp.h"
#include "log0recv.h"
#include "que0que.h"
#include "rem0cmp.h"
#include "rem0rec.h"
#include "row0upd.h"
#include "srv0start.h"
#include "trx0sys.h"

/*      STRUCTURE OF AN INSERT BUFFER RECORD

In versions < 4.1.x:

1. The first field is the page number.
2. The second field is an array which stores type info for each subsequent
   field. We store the information which affects the ordering of records, and
   also the physical storage size of an SQL NULL value. E.g., for CHAR(10) it
   is 10 bytes.
3. Next we have the fields of the actual index record.

In versions >= 4.1.x:

Note that contrary to what we planned in the 1990's, there will only be one
insert buffer tree, and that is in the system tablespace of InnoDB.

1. The first field is the space id.
2. The second field is a one-byte marker (0) which differentiates records from
   the < 4.1.x storage format.
3. The third field is the page number.
4. The fourth field contains the type info, where we have also added 2 bytes to
   store the charset. In the compressed table format of 5.0.x we must add more
   information here so that we can build a dummy 'index' struct which 5.0.x
   can use in the binary search on the index page in the ibuf merge phase.
5. The rest of the fields contain the fields of the actual index record.

In versions >= 5.0.3:

The first byte of the fourth field is an additional marker (0) if the record
is in the compact format.  The presence of this marker can be detected by
looking at the length of the field modulo DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE.

The high-order bit of the character set field in the type info is the
"nullable" flag for the field.

In versions >= 5.5:

The optional marker byte at the start of the fourth field is replaced by
mandatory 3 fields, totaling 4 bytes:

 1. 2 bytes: Counter field, used to sort records within a (space id, page
    no) in the order they were added. This is needed so that for example the
    sequence of operations "INSERT x, DEL MARK x, INSERT x" is handled
    correctly.

 2. 1 byte: Operation type (see ibuf_op_t).

 3. 1 byte: Flags. Currently only one flag exists, IBUF_REC_COMPACT.

To ensure older records, which do not have counters to enforce correct
sorting, are merged before any new records, ibuf_insert checks if we're
trying to insert to a position that contains old-style records, and if so,
refuses the insert. Thus, ibuf pages are gradually converted to the new
format as their corresponding buffer pool pages are read into memory.
*/

/*      PREVENTING DEADLOCKS IN THE INSERT BUFFER SYSTEM

If an OS thread performs any operation that brings in disk pages from
non-system tablespaces into the buffer pool, or creates such a page there,
then the operation may have as a side effect an insert buffer index tree
compression. Thus, the tree latch of the insert buffer tree may be acquired
in the x-mode, and also the file space latch of the system tablespace may
be acquired in the x-mode.

Also, an insert to an index in a non-system tablespace can have the same
effect. How do we know this cannot lead to a deadlock of OS threads? There
is a problem with the i\o-handler threads: they break the latching order
because they own x-latches to pages which are on a lower level than the
insert buffer tree latch, its page latches, and the tablespace latch an
insert buffer operation can reserve.

The solution is the following: Let all the tree and page latches connected
with the insert buffer be later in the latching order than the fsp latch and
fsp page latches.

Insert buffer pages must be such that the insert buffer is never invoked
when these pages are accessed as this would result in a recursion violating
the latching order. We let a special i/o-handler thread take care of i/o to
the insert buffer pages and the ibuf bitmap pages, as well as the fsp bitmap
pages and the first inode page, which contains the inode of the ibuf tree: let
us call all these ibuf pages. To prevent deadlocks, we do not let a read-ahead
access both non-ibuf and ibuf pages.

Then an i/o-handler for the insert buffer never needs to access recursively the
insert buffer tree and thus obeys the latching order. On the other hand, other
i/o-handlers for other tablespaces may require access to the insert buffer,
but because all kinds of latches they need to access there are later in the
latching order, no violation of the latching order occurs in this case,
either.

A problem is how to grow and contract an insert buffer tree. As it is later
in the latching order than the fsp management, we have to reserve the fsp
latch first, before adding or removing pages from the insert buffer tree.
We let the insert buffer tree have its own file space management: a free
list of pages linked to the tree root. To prevent recursive using of the
insert buffer when adding pages to the tree, we must first load these pages
to memory, obtaining a latch on them, and only after that add them to the
free list of the insert buffer tree. More difficult is removing of pages
from the free list. If there is an excess of pages in the free list of the
ibuf tree, they might be needed if some thread reserves the fsp latch,
intending to allocate more file space. So we do the following: if a thread
reserves the fsp latch, we check the writer count field of the latch. If
this field has value 1, it means that the thread did not own the latch
before entering the fsp system, and the mtr of the thread contains no
modifications to the fsp pages. Now we are free to reserve the ibuf latch,
and check if there is an excess of pages in the free list. We can then, in a
separate mini-transaction, take them out of the free list and free them to
the fsp system.

To avoid deadlocks in the ibuf system, we divide file pages into three levels:

(1) non-ibuf pages,
(2) ibuf tree pages and the pages in the ibuf tree free list, and
(3) ibuf bitmap pages.

No OS thread is allowed to access higher level pages if it has latches to
lower level pages; even if the thread owns a B-tree latch it must not access
the B-tree non-leaf pages if it has latches on lower level pages. Read-ahead
is only allowed for level 1 and 2 pages. Dedicated i/o-handler threads handle
exclusively level 1 i/o. A dedicated i/o handler thread handles exclusively
level 2 i/o. However, if an OS thread does the i/o handling for itself, i.e.,
it uses synchronous aio, it can access any pages, as long as it obeys the
access order rules. */

/** Operations that can currently be buffered. */
ulong innodb_change_buffering = IBUF_USE_ALL;

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
/** Flag to control insert buffer debugging. */
uint ibuf_debug;
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

/** The insert buffer control structure */
ibuf_t *ibuf = nullptr;

#ifdef UNIV_IBUF_COUNT_DEBUG
/** Number of tablespaces in the ibuf_counts array */
constexpr uint32_t IBUF_COUNT_N_SPACES = 4;
/** Number of pages within each tablespace in the ibuf_counts array */
constexpr uint32_t IBUF_COUNT_N_PAGES = 130000;

/** Buffered entry counts for file pages, used in debugging */
static ulint ibuf_counts[IBUF_COUNT_N_SPACES][IBUF_COUNT_N_PAGES];

/** Checks that the indexes to ibuf_counts[][] are within limits.
@param[in]      page_id page id */
static inline void ibuf_count_check(const page_id_t &page_id) {
  if (page_id.space() < IBUF_COUNT_N_SPACES &&
      page_id.page_no() < IBUF_COUNT_N_PAGES) {
    return;
  }

  ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_605)
      << "UNIV_IBUF_COUNT_DEBUG limits space_id and page_no"
         " and breaks crash recovery. space_id="
      << page_id.space() << ", should be 0<=space_id<" << IBUF_COUNT_N_SPACES
      << ". page_no=" << page_id.page_no() << ", should be 0<=page_no<"
      << IBUF_COUNT_N_PAGES;
}
#endif

/** @name Offsets to the per-page bits in the insert buffer bitmap */
/** @{ */
/** Bits indicating the amount of free space */
constexpr uint32_t IBUF_BITMAP_FREE = 0;
/** true if there are buffered changes for the page */
constexpr uint32_t IBUF_BITMAP_BUFFERED = 2;
/** true if page is a part of  the ibuf tree, excluding the root page, or is in
 the free list of the ibuf */
constexpr uint32_t IBUF_BITMAP_IBUF = 3;
/** @} */

/** in the pre-4.1 format, the page number. later, the space_id */
constexpr uint32_t IBUF_REC_FIELD_SPACE = 0;
/** starting with 4.1, a marker consisting of 1 byte that is 0 */
constexpr uint32_t IBUF_REC_FIELD_MARKER = 1;
/** starting with 4.1, the page number */
constexpr uint32_t IBUF_REC_FIELD_PAGE = 2;
/** the metadata field */
constexpr uint32_t IBUF_REC_FIELD_METADATA = 3;
/** first user field */
constexpr uint32_t IBUF_REC_FIELD_USER = 4;

/* Various constants for checking the type of an ibuf record and extracting
data from it. For details, see the description of the record format at the
top of this file. */

/** @name Format of the IBUF_REC_FIELD_METADATA of an insert buffer record
The fourth column in the MySQL 5.5 format contains an operation
type, counter, and some flags. */
/** Combined size of info fields at the beginning of the fourth field */
constexpr uint32_t IBUF_REC_INFO_SIZE = 4;
static_assert(IBUF_REC_INFO_SIZE < DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE,
              "IBUF_REC_INFO_SIZE >= DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE");

/* Offsets for the fields at the beginning of the fourth field */
/** Operation counter */
constexpr uint32_t IBUF_REC_OFFSET_COUNTER = 0;
/** Type of operation */
constexpr uint32_t IBUF_REC_OFFSET_TYPE = 2;
/** Additional flags */
constexpr uint32_t IBUF_REC_OFFSET_FLAGS = 3;

/* Record flag masks */
/** Set in IBUF_REC_OFFSET_FLAGS if the user index is in COMPACT format or later
 */
constexpr uint32_t IBUF_REC_COMPACT = 0x1;
/** The mutex used to block pessimistic inserts to ibuf trees */
static ib_mutex_t ibuf_pessimistic_insert_mutex;

/** The mutex protecting the insert buffer structs */
static ib_mutex_t ibuf_mutex;

/** The mutex protecting the insert buffer bitmaps */
static ib_mutex_t ibuf_bitmap_mutex;

/** The area in pages from which contract looks for page numbers for merge */
const ulint IBUF_MERGE_AREA = 8;

/** Inside the merge area, pages which have at most 1 per this number less
buffered entries compared to maximum volume that can buffered for a single
page are merged along with the page whose buffer became full */
const ulint IBUF_MERGE_THRESHOLD = 4;

/** In ibuf_contract at most this number of pages is read to memory in one
batch, in order to merge the entries for them in the insert buffer */
const ulint IBUF_MAX_N_PAGES_MERGED = IBUF_MERGE_AREA;

/** If the combined size of the ibuf trees exceeds ibuf->max_size by this
many pages, we start to contract it in connection to inserts there, using
non-synchronous contract */
const ulint IBUF_CONTRACT_ON_INSERT_NON_SYNC = 0;

/** If the combined size of the ibuf trees exceeds ibuf->max_size by this
many pages, we start to contract it in connection to inserts there, using
synchronous contract */
const ulint IBUF_CONTRACT_ON_INSERT_SYNC = 5;

/** If the combined size of the ibuf trees exceeds ibuf->max_size by
this many pages, we start to contract it synchronous contract, but do
not insert */
const ulint IBUF_CONTRACT_DO_NOT_INSERT = 10;

/* TODO: how to cope with drop table if there are records in the insert
buffer for the indexes of the table? Is there actually any problem,
because ibuf merge is done to a page when it is read in, and it is
still physically like the index page even if the index would have been
dropped! So, there seems to be no problem. */

/** Sets the flag in the current mini-transaction record indicating we're
 inside an insert buffer routine. */
static inline void ibuf_enter(mtr_t *mtr) /*!< in/out: mini-transaction */
{
  ut_ad(!mtr->is_inside_ibuf());
  mtr->enter_ibuf();
}

/** Sets the flag in the current mini-transaction record indicating we're
 exiting an insert buffer routine. */
static inline void ibuf_exit(mtr_t *mtr) /*!< in/out: mini-transaction */
{
  ut_ad(mtr->is_inside_ibuf());
  mtr->exit_ibuf();
}

/** Commits an insert buffer mini-transaction and sets the persistent
 cursor latch mode to BTR_NO_LATCHES, that is, detaches the cursor. */
static inline void ibuf_btr_pcur_commit_specify_mtr(
    btr_pcur_t *pcur, /*!< in/out: persistent cursor */
    mtr_t *mtr)       /*!< in/out: mini-transaction */
{
  ut_d(ibuf_exit(mtr));
  pcur->commit_specify_mtr(mtr);
}

/** Gets the ibuf header page and x-latches it.
 @return insert buffer header page */
static page_t *ibuf_header_page_get(mtr_t *mtr) /*!< in/out: mini-transaction */
{
  buf_block_t *block;

  ut_ad(!ibuf_inside(mtr));

  block = buf_page_get(page_id_t(IBUF_SPACE_ID, FSP_IBUF_HEADER_PAGE_NO),
                       univ_page_size, RW_X_LATCH, UT_LOCATION_HERE, mtr);

  buf_block_dbg_add_level(block, SYNC_IBUF_HEADER);

  return (buf_block_get_frame(block));
}

/** Gets the root page and sx-latches it.
 @return insert buffer tree root page */
static page_t *ibuf_tree_root_get(mtr_t *mtr) /*!< in: mtr */
{
  buf_block_t *block;
  page_t *root;

  ut_ad(ibuf_inside(mtr));
  ut_ad(mutex_own(&ibuf_mutex));

  mtr_sx_lock(dict_index_get_lock(ibuf->index), mtr, UT_LOCATION_HERE);

  /* only segment list access is exclusive each other */
  block = buf_page_get(page_id_t(IBUF_SPACE_ID, FSP_IBUF_TREE_ROOT_PAGE_NO),
                       univ_page_size, RW_SX_LATCH, UT_LOCATION_HERE, mtr);

  buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE_NEW);

  root = buf_block_get_frame(block);

  ut_ad(page_get_space_id(root) == IBUF_SPACE_ID);
  ut_ad(page_get_page_no(root) == FSP_IBUF_TREE_ROOT_PAGE_NO);
  ut_ad(ibuf->empty == page_is_empty(root));

  return (root);
}

#ifdef UNIV_IBUF_COUNT_DEBUG

/** Gets the ibuf count for a given page.
@param[in]      page_id page id
@return number of entries in the insert buffer currently buffered for
this page */
ulint ibuf_count_get(const page_id_t &page_id) {
  ibuf_count_check(page_id);

  return (ibuf_counts[page_id.space()][page_id.page_no()]);
}

/** Sets the ibuf count for a given page.
@param[in]      page_id page id
@param[in]      val     value to set */
static void ibuf_count_set(const page_id_t &page_id, ulint val) {
  ibuf_count_check(page_id);
  ut_a(val < UNIV_PAGE_SIZE);

  ibuf_counts[page_id.space()][page_id.page_no()] = val;
}
#endif

/** Closes insert buffer and frees the data structures. */
void ibuf_close(void) {
  if (ibuf == nullptr) {
    return;
  }

  mutex_free(&ibuf_pessimistic_insert_mutex);

  mutex_free(&ibuf_mutex);

  mutex_free(&ibuf_bitmap_mutex);

  dict_table_t *ibuf_table = ibuf->index->table;
  rw_lock_free(&ibuf->index->lock);
  dict_mem_index_free(ibuf->index);
  dict_mem_table_free(ibuf_table);

  ut::free(ibuf);
  ibuf = nullptr;
}

/** Function to pass ibuf status variables */
void ibuf_export_ibuf_status(ulint *free_list, ulint *segment_size) {
  *free_list = ibuf->free_list_len;
  *segment_size = ibuf->seg_size;
}

/** Updates the size information of the ibuf, assuming the segment size has not
 changed. */
/** 更新插入缓冲的大小信息，假设段大小没有改变 */
static void ibuf_size_update(const page_t *root) /*!< in: ibuf tree root */
{
  // 断言：必须持有ibuf互斥锁
  ut_ad(mutex_own(&ibuf_mutex));

  // 从根页获取空闲列表长度
  ibuf->free_list_len =
      flst_get_len(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST);

  // 计算B+树高度（根页层级+1）
  ibuf->height = 1 + btr_page_get_level(root);

  /* the '1 +' is the ibuf header page */
  /* '1 +' 表示ibuf头页 */
  // 计算实际使用大小 = 段大小 - (头页 + 空闲页数)
  ibuf->size = ibuf->seg_size - (1 + ibuf->free_list_len);
}

/** Creates the insert buffer data structure at a database startup and
 initializes the data structures for the insert buffer. */
/** 在数据库启动时创建插入缓冲数据结构并初始化插入缓冲的数据结构 */
void ibuf_init_at_db_start(void) {
  // 定义变量：B+树根页指针、mini-transaction、已使用页数、头页指针
  page_t *root;
  mtr_t mtr;
  ulint n_used;
  page_t *header_page;

  // 为ibuf分配内存空间
  ibuf = static_cast<ibuf_t *>(
      ut::zalloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, sizeof(ibuf_t)));

  /* At startup we initialize ibuf to have a maximum of
  CHANGE_BUFFER_DEFAULT_SIZE in terms of percentage of the
  buffer pool size. Once ibuf struct is initialized this
  value is updated with the user supplied size by calling
  ibuf_max_size_update(). */
  /* 启动时我们将ibuf初始化为最大CHANGE_BUFFER_DEFAULT_SIZE，
  以缓冲池大小的百分比表示。一旦ibuf结构初始化后，
  这个值会通过调用ibuf_max_size_update()更新为用户指定的大小 */
  ibuf->max_size = ((buf_pool_get_curr_size() / UNIV_PAGE_SIZE) *
                    CHANGE_BUFFER_DEFAULT_SIZE) /
                   100;

  // 创建ibuf互斥量
  mutex_create(LATCH_ID_IBUF, &ibuf_mutex);

  // 创建ibuf位图互斥量
  mutex_create(LATCH_ID_IBUF_BITMAP, &ibuf_bitmap_mutex);

  // 创建ibuf悲观插入互斥量
  mutex_create(LATCH_ID_IBUF_PESSIMISTIC_INSERT,
               &ibuf_pessimistic_insert_mutex);

  // 开始一个mini-transaction
  mtr_start(&mtr);

  // 获取系统表空间的X锁
  mtr_x_lock_space(fil_space_get_sys_space(), &mtr);

  // 获取ibuf互斥量
  mutex_enter(&ibuf_mutex);

  // 获取ibuf头页
  header_page = ibuf_header_page_get(&mtr);

  // 计算保留页数
  fseg_n_reserved_pages(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER,
                        &n_used, &mtr);
  // 进入ibuf操作模式
  ibuf_enter(&mtr);

  // 断言：至少使用了2个页
  ut_ad(n_used >= 2);

  // 设置段大小
  ibuf->seg_size = n_used;

  {
    // 获取ibuf B+树根页
    buf_block_t *block;

    block = buf_page_get(page_id_t(IBUF_SPACE_ID, FSP_IBUF_TREE_ROOT_PAGE_NO),
                         univ_page_size, RW_X_LATCH, UT_LOCATION_HERE, &mtr);

    // 添加调试信息
    buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);

    // 获取页帧
    root = buf_block_get_frame(block);
  }

  // 更新ibuf大小信息
  ibuf_size_update(root);
  // 释放ibuf互斥量
  mutex_exit(&ibuf_mutex);

  // 设置ibuf是否为空标志
  ibuf->empty = page_is_empty(root);
  // 提交ibuf相关的mini-transaction
  ibuf_mtr_commit(&mtr);

  // 创建ibuf索引结构
  ibuf->index =
      dict_mem_index_create("innodb_change_buffer", "CLUST_IND", IBUF_SPACE_ID,
                            DICT_CLUSTERED | DICT_IBUF, 1);
  // 设置索引ID
  ibuf->index->id = DICT_IBUF_ID_MIN + IBUF_SPACE_ID;
  // 创建ibuf表结构
  ibuf->index->table = dict_mem_table_create("innodb_change_buffer",
                                             IBUF_SPACE_ID, 1, 0, 0, 0, 0);
  // 设置唯一键数量
  ibuf->index->n_uniq = REC_MAX_N_FIELDS;
  // 创建索引树读写锁
  rw_lock_create(index_tree_rw_lock_key, &ibuf->index->lock,
                 LATCH_ID_IBUF_INDEX_TREE);
  // 创建搜索信息结构
  ibuf->index->search_info = btr_search_info_create(ibuf->index->heap);
  // 设置根页号
  ibuf->index->page = FSP_IBUF_TREE_ROOT_PAGE_NO;
  // 调试模式下设置缓存标志
  ut_d(ibuf->index->cached = true);
}

/** Updates the max_size value for ibuf. */
void ibuf_max_size_update(ulint new_val) /*!< in: new value in terms of
                                         percentage of the buffer pool size */
{
  ulint new_size =
      ((buf_pool_get_curr_size() / UNIV_PAGE_SIZE) * new_val) / 100;
  mutex_enter(&ibuf_mutex);
  ibuf->max_size = new_size;
  mutex_exit(&ibuf_mutex);
}

#endif /* !UNIV_HOTBACKUP */
/** Initializes an ibuf bitmap page. */
void ibuf_bitmap_page_init(buf_block_t *block, /*!< in: bitmap page */
                           mtr_t *mtr)         /*!< in: mtr */
{
  page_t *page;
  ulint byte_offset;

  page = buf_block_get_frame(block);
  fil_page_set_type(page, FIL_PAGE_IBUF_BITMAP);

  /* Write all zeros to the bitmap */

  byte_offset =
      UT_BITS_IN_BYTES(block->page.size.physical() * IBUF_BITS_PER_PAGE);

  memset(page + IBUF_BITMAP, 0, byte_offset);

  /* The remaining area (up to the page trailer) is uninitialized. */

#ifndef UNIV_HOTBACKUP
  mlog_write_initial_log_record(page, MLOG_IBUF_BITMAP_INIT, mtr);
#endif /* !UNIV_HOTBACKUP */
}

byte *ibuf_parse_bitmap_init(byte *ptr, byte *end_ptr [[maybe_unused]],
                             buf_block_t *block, mtr_t *mtr) {
  if (block) {
    ibuf_bitmap_page_init(block, mtr);
  }

  return (ptr);
}
#ifndef UNIV_HOTBACKUP
/** Gets the desired bits for a given page from a bitmap page.
@param[in]      page            Bitmap page
@param[in]      page_id         Page id whose bits to get
@param[in]      page_size       Page size
@param[in]      latch_type      MTR_MEMO_PAGE_X_FIX, MTR_MEMO_BUF_FIX, ...
@param[in,out]  mtr             Mini-transaction holding latch_type on the
bitmap page
@param[in]      bit             IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
@return value of bits */
static inline ulint ibuf_bitmap_page_get_bits_low(
    const page_t *page, const page_id_t &page_id, const page_size_t &page_size,
    IF_DEBUG(ulint latch_type, mtr_t *mtr, ) ulint bit) {
  ulint byte_offset;
  ulint bit_offset;
  ulint map_byte;
  ulint value;

  ut_ad(bit < IBUF_BITS_PER_PAGE);
  static_assert(IBUF_BITS_PER_PAGE % 2 == 0,
                "IBUF_BITS_PER_PAGE must be an even number!");
  ut_ad(mtr_memo_contains_page(mtr, page, latch_type));

  bit_offset =
      (page_id.page_no() % page_size.physical()) * IBUF_BITS_PER_PAGE + bit;

  byte_offset = bit_offset / 8;
  bit_offset = bit_offset % 8;

  ut_ad(byte_offset + IBUF_BITMAP < UNIV_PAGE_SIZE);

  map_byte = mach_read_from_1(page + IBUF_BITMAP + byte_offset);

  value = ut_bit_get_nth(map_byte, bit_offset);

  if (bit == IBUF_BITMAP_FREE) {
    ut_ad(bit_offset + 1 < 8);

    value = value * 2 + ut_bit_get_nth(map_byte, bit_offset + 1);
  }

  return (value);
}

/**
 * 从位图页获取指定页面的位信息
 * Gets the desired bits for a given page from a bitmap page.
 * 
 * @param[in]      page            位图页
 *                               Bitmap page
 * @param[in]      page_id         要获取位信息的页面ID
 *                               Page id whose bits to get
 * @param[in]      page_size       页面大小
 *                               Page size
 * @param[in]      bit             要获取的位类型(IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED等)
 *                               IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
 * @param[in,out]  mtr             持有位图页X锁的迷你事务
 *                               Mini-transaction holding an x-latch on the bitmap page
 * @return 位的值
 *         value of bits 
 */
inline ulint ibuf_bitmap_page_get_bits(const page_t *page,
                                       const page_id_t &page_id,
                                       const page_size_t &page_size, ulint bit,
                                       mtr_t *mtr [[maybe_unused]]) {
  // 调用底层函数获取位信息，调试模式下会检查锁类型
  return ibuf_bitmap_page_get_bits_low(
      page, page_id, page_size, IF_DEBUG(MTR_MEMO_PAGE_X_FIX, mtr, ) bit);
}

/** 
 * 在bitmap页中为指定页面设置所需的位
 * Sets the desired bit for a given page in a bitmap page.
 * @param[in,out]  page            bitmap页
 *                                bitmap page
 * @param[in]      page_id         要设置位的页面ID
 *                                page id whose bits to set
 * @param[in]      page_size       页面大小
 *                                page size
 * @param[in]      bit             位类型(IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED等)
 *                                IBUF_BITMAP_FREE, IBUF_BITMAP_BUFFERED, ...
 * @param[in]      val             要设置的值
 *                                value to set
 * @param[in,out]  mtr             持有bitmap页X锁的mini-transaction
 *                                mtr containing an x-latch to the bitmap page 
 */
static void ibuf_bitmap_page_set_bits(page_t *page, const page_id_t &page_id,
                                      const page_size_t &page_size, ulint bit,
                                      ulint val, mtr_t *mtr) {
  // 定义变量：字节偏移量、位偏移量、位图字节值
  ulint byte_offset;
  ulint bit_offset;
  ulint map_byte;

  // 断言：位类型必须小于每页位数
  ut_ad(bit < IBUF_BITS_PER_PAGE);
  // 静态断言：每页位数必须是偶数
  static_assert(IBUF_BITS_PER_PAGE % 2 == 0,
                "IBUF_BITS_PER_PAGE must be an even number!");
  // 断言：mtr必须持有bitmap页的X锁
  ut_ad(mtr_memo_contains_page(mtr, page, MTR_MEMO_PAGE_X_FIX));
#ifdef UNIV_IBUF_COUNT_DEBUG
  // 调试模式下检查：如果要清除BUFFERED位，则计数必须为0
  ut_a((bit != IBUF_BITMAP_BUFFERED) || (val != 0) ||
       (0 == ibuf_count_get(page_id)));
#endif

  // 计算位偏移量：(页号 % 物理页大小) * 每页位数 + 位类型
  // 每个物理页在位图中占用4个bit（ IBUF_BITS_PER_PAGE=4 ）
  bit_offset =
      (page_id.page_no() % page_size.physical()) * IBUF_BITS_PER_PAGE + bit;

  // 计算字节偏移量和位偏移量
  byte_offset = bit_offset / 8;
  bit_offset = bit_offset % 8;

  // 断言：字节偏移量必须在页大小范围内
  ut_ad(byte_offset + IBUF_BITMAP < UNIV_PAGE_SIZE);

  // 读取当前位图字节值
  map_byte = mach_read_from_1(page + IBUF_BITMAP + byte_offset);

  // 如果是空闲位设置
  if (bit == IBUF_BITMAP_FREE) {
    // 断言：必须能容纳两个连续位(空闲位占2位)
    ut_ad(bit_offset + 1 < 8);
    // 断言：值必须<=3(2位最大值)
    ut_ad(val <= 3);

    // 设置高位和低位
    map_byte = ut_bit_set_nth(map_byte, bit_offset, val / 2);
    map_byte = ut_bit_set_nth(map_byte, bit_offset + 1, val % 2);
  } else {
    // 其他位类型(1位)
    // 断言：值必须<=1
    ut_ad(val <= 1);
    // 设置单个位
    map_byte = ut_bit_set_nth(map_byte, bit_offset, val);
  }

  // 将修改后的位图字节写回磁盘
  mlog_write_ulint(page + IBUF_BITMAP + byte_offset, map_byte, MLOG_1BYTE, mtr);
}

/** Calculates the bitmap page number for a given page number.
@param[in]      page_id         page id
@param[in]      page_size       page size
@return the bitmap page id where the file page is mapped */
static inline const page_id_t ibuf_bitmap_page_no_calc(
    const page_id_t &page_id, const page_size_t &page_size) {
  page_no_t bitmap_page_no;

  bitmap_page_no = FSP_IBUF_BITMAP_OFFSET +
                   (page_id.page_no() & ~(page_size.physical() - 1));

  return (page_id_t(page_id.space(), bitmap_page_no));
}

/** Gets the ibuf bitmap page where the bits describing a given file page are
stored.
@param[in]      page_id         Page id of the file page
@param[in]      page_size       Page size of the file page
@param[in]      location                Location where called
@param[in,out]  mtr             Mini-transaction
@return bitmap page where the file page is mapped, that is, the bitmap
page containing the descriptor bits for the file page; the bitmap page
is x-latched */
static page_t *ibuf_bitmap_get_map_page(const page_id_t &page_id,
                                        const page_size_t &page_size,
                                        ut::Location location, mtr_t *mtr) {
  buf_block_t *block;

  block =
      buf_page_get_gen(ibuf_bitmap_page_no_calc(page_id, page_size), page_size,
                       RW_X_LATCH, nullptr, Page_fetch::NORMAL, location, mtr);

  buf_block_dbg_add_level(block, SYNC_IBUF_BITMAP);

  return (buf_block_get_frame(block));
}

/** Sets the free bits of the page in the ibuf bitmap. This is done in a
 separate mini-transaction, hence this operation does not restrict further work
 to only ibuf bitmap operations, which would result if the latch to the bitmap
 page were kept. */
static inline void ibuf_set_free_bits_low(
    const buf_block_t *block, /*!< in: index page; free bits are set if
                              the index is non-clustered and page
                              level is 0 */
    ulint val,                /*!< in: value to set: < 4 */
    mtr_t *mtr)               /*!< in/out: mtr */
{
  page_t *bitmap_page;

  if (!page_is_leaf(buf_block_get_frame(block))) {
    return;
  }

  bitmap_page = ibuf_bitmap_get_map_page(block->page.id, block->page.size,
                                         UT_LOCATION_HERE, mtr);

#ifdef UNIV_IBUF_DEBUG
  ut_a(val <= ibuf_index_page_calc_free(block));
#endif /* UNIV_IBUF_DEBUG */

  ibuf_bitmap_page_set_bits(bitmap_page, block->page.id, block->page.size,
                            IBUF_BITMAP_FREE, val, mtr);
}

/** Sets the free bit of the page in the ibuf bitmap. This is done in a separate
 mini-transaction, hence this operation does not restrict further work to only
 ibuf bitmap operations, which would result if the latch to the bitmap page
 were kept. */
void ibuf_set_free_bits_func(
    buf_block_t *block, /*!< in: index page of a non-clustered index;
                        free bit is reset if page level is 0 */
#ifdef UNIV_IBUF_DEBUG
    ulint max_val, /*!< in: ULINT_UNDEFINED or a maximum
                   value which the bits must have before
                   setting; this is for debugging */
#endif             /* UNIV_IBUF_DEBUG */
    ulint val)     /*!< in: value to set: < 4 */
{
  mtr_t mtr;
  page_t *page;
  page_t *bitmap_page;

  page = buf_block_get_frame(block);

  if (!page_is_leaf(page)) {
    return;
  }

  mtr_start(&mtr);

  const fil_space_t *space = fil_space_get(block->page.id.space());

  bitmap_page = ibuf_bitmap_get_map_page(block->page.id, block->page.size,
                                         UT_LOCATION_HERE, &mtr);

  switch (space->purpose) {
    case FIL_TYPE_TABLESPACE:
      break;
    case FIL_TYPE_TEMPORARY:
    case FIL_TYPE_IMPORT:
      mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);
  }

#ifdef UNIV_IBUF_DEBUG
  if (max_val != ULINT_UNDEFINED) {
    ulint old_val;

    old_val = ibuf_bitmap_page_get_bits(bitmap_page, block->page.id,
                                        IBUF_BITMAP_FREE, &mtr);
#if 0
    if (old_val != max_val) {
      fprintf(stderr,
        "Ibuf: page %lu old val %lu max val %lu\n",
        page_get_page_no(page),
        old_val, max_val);
    }
#endif

    ut_a(old_val <= max_val);
  }
#if 0
  fprintf(stderr, "Setting page no %lu free bits to %lu should be %lu\n",
    page_get_page_no(page), val,
    ibuf_index_page_calc_free(block));
#endif

  ut_a(val <= ibuf_index_page_calc_free(block));
#endif /* UNIV_IBUF_DEBUG */

  ibuf_bitmap_page_set_bits(bitmap_page, block->page.id, block->page.size,
                            IBUF_BITMAP_FREE, val, &mtr);

  mtr_commit(&mtr);
}

/** Resets the free bits of the page in the ibuf bitmap. This is done in a
 separate mini-transaction, hence this operation does not restrict
 further work to only ibuf bitmap operations, which would result if the
 latch to the bitmap page were kept.  NOTE: The free bits in the insert
 buffer bitmap must never exceed the free space on a page.  It is safe
 to decrement or reset the bits in the bitmap in a mini-transaction
 that is committed before the mini-transaction that affects the free
 space. */
void ibuf_reset_free_bits(
    buf_block_t *block) /*!< in: index page; free bits are set to 0
                        if the index is a non-clustered
                        non-unique, and page level is 0 */
{
  ibuf_set_free_bits(block, 0, ULINT_UNDEFINED);
}

/** Updates the free bits for an uncompressed page to reflect the present
 state.  Does this in the mtr given, which means that the latching
 order rules virtually prevent any further operations for this OS
 thread until mtr is committed.  NOTE: The free bits in the insert
 buffer bitmap must never exceed the free space on a page.  It is safe
 to set the free bits in the same mini-transaction that updated the
 page. */
void ibuf_update_free_bits_low(const buf_block_t *block, /*!< in: index page */
                               ulint max_ins_size,       /*!< in: value of
                                                         maximum insert size
                                                         with reorganize before
                                                         the latest operation
                                                         performed to the page */
                               mtr_t *mtr)               /*!< in/out: mtr */
{
  ulint before;
  ulint after;

  ut_a(!buf_block_get_page_zip(block));

  before =
      ibuf_index_page_calc_free_bits(block->page.size.logical(), max_ins_size);

  after = ibuf_index_page_calc_free(block);

  /* This approach cannot be used on compressed pages, since the
  computed value of "before" often does not match the current
  state of the bitmap.  This is because the free space may
  increase or decrease when a compressed page is reorganized. */
  if (before != after) {
    ibuf_set_free_bits_low(block, after, mtr);
  }
}

/** Updates the free bits for a compressed page to reflect the present
 state.  Does this in the mtr given, which means that the latching
 order rules virtually prevent any further operations for this OS
 thread until mtr is committed.  NOTE: The free bits in the insert
 buffer bitmap must never exceed the free space on a page.  It is safe
 to set the free bits in the same mini-transaction that updated the
 page. */
void ibuf_update_free_bits_zip(buf_block_t *block, /*!< in/out: index page */
                               mtr_t *mtr)         /*!< in/out: mtr */
{
  page_t *bitmap_page;
  ulint after;

  ut_a(page_is_leaf(buf_block_get_frame(block)));
  ut_a(block->page.size.is_compressed());

  bitmap_page = ibuf_bitmap_get_map_page(block->page.id, block->page.size,
                                         UT_LOCATION_HERE, mtr);

  after = ibuf_index_page_calc_free_zip(block);

  if (after == 0) {
    /* We move the page to the front of the buffer pool LRU list:
    the purpose of this is to prevent those pages to which we
    cannot make inserts using the insert buffer from slipping
    out of the buffer pool */

    buf_page_make_young(&block->page);
  }

  ibuf_bitmap_page_set_bits(bitmap_page, block->page.id, block->page.size,
                            IBUF_BITMAP_FREE, after, mtr);
}

/** Updates the free bits for the two pages to reflect the present state.
 Does this in the mtr given, which means that the latching order rules
 virtually prevent any further operations until mtr is committed.
 NOTE: The free bits in the insert buffer bitmap must never exceed the
 free space on a page.  It is safe to set the free bits in the same
 mini-transaction that updated the pages. */
void ibuf_update_free_bits_for_two_pages_low(
    buf_block_t *block1, /*!< in: index page */
    buf_block_t *block2, /*!< in: index page */
    mtr_t *mtr)          /*!< in: mtr */
{
  ulint state;

  ut_ad(block1->page.id.space() == block2->page.id.space());

  /* As we have to x-latch two random bitmap pages, we have to acquire
  the bitmap mutex to prevent a deadlock with a similar operation
  performed by another OS thread. */

  mutex_enter(&ibuf_bitmap_mutex);

  state = ibuf_index_page_calc_free(block1);

  ibuf_set_free_bits_low(block1, state, mtr);

  state = ibuf_index_page_calc_free(block2);

  ibuf_set_free_bits_low(block2, state, mtr);

  mutex_exit(&ibuf_bitmap_mutex);
}

/** Returns true if the page is one of the fixed address ibuf pages.
@param[in]      page_id         page id
@param[in]      page_size       page size
@return true if a fixed address ibuf i/o page */
static inline bool ibuf_fixed_addr_page(const page_id_t &page_id,
                                        const page_size_t &page_size) {
  return ((page_id.space() == IBUF_SPACE_ID &&
           page_id.page_no() == IBUF_TREE_ROOT_PAGE_NO) ||
          ibuf_bitmap_page(page_id, page_size));
}

/** Checks if a page is a level 2 or 3 page in the ibuf hierarchy of pages.
Must not be called when recv_no_ibuf_operations==true.
@param[in]      page_id         page id
@param[in]      page_size       page size
@param[in]      x_latch         false if relaxed check (avoid latching the
bitmap page)
@param[in] location Location where called
@param[in,out]  mtr             mtr which will contain an x-latch to the
bitmap page if the page is not one of the fixed address ibuf pages, or NULL,
in which case a new transaction is created.
@return true if level 2 or level 3 page */
bool ibuf_page_low(const page_id_t &page_id, const page_size_t &page_size,
                   IF_DEBUG(bool x_latch, ) ut::Location location, mtr_t *mtr) {
  ulint ret;
  mtr_t local_mtr;
  page_t *bitmap_page;

  ut_ad(!recv_no_ibuf_operations);
  ut_ad(x_latch || mtr == nullptr);

  if (ibuf_fixed_addr_page(page_id, page_size)) {
    return true;
  } else if (page_id.space() != IBUF_SPACE_ID) {
    return false;
  }

  ut_ad(fil_space_get_type(IBUF_SPACE_ID) == FIL_TYPE_TABLESPACE);

#ifdef UNIV_DEBUG
  if (!x_latch) {
    mtr_start(&local_mtr);

    /* Get the bitmap page without a page latch, so that
    we will not be violating the latching order when
    another bitmap page has already been latched by this
    thread. The page will be buffer-fixed, and thus it
    cannot be removed or relocated while we are looking at
    it. The contents of the page could change, but the
    IBUF_BITMAP_IBUF bit that we are interested in should
    not be modified by any other thread. Nobody should be
    calling ibuf_add_free_page() or ibuf_remove_free_page()
    while the page is linked to the insert buffer b-tree. */

    bitmap_page = buf_block_get_frame(buf_page_get_gen(
        ibuf_bitmap_page_no_calc(page_id, page_size), page_size, RW_NO_LATCH,
        nullptr, Page_fetch::NO_LATCH, location, &local_mtr));

    ret = ibuf_bitmap_page_get_bits_low(bitmap_page, page_id, page_size,
                                        MTR_MEMO_BUF_FIX, &local_mtr,
                                        IBUF_BITMAP_IBUF);

    mtr_commit(&local_mtr);
    return (ret);
  }
#endif /* UNIV_DEBUG */

  if (mtr == nullptr) {
    mtr = &local_mtr;
    mtr_start(mtr);
  }

  bitmap_page = ibuf_bitmap_get_map_page(page_id, page_size, location, mtr);

  ret = ibuf_bitmap_page_get_bits(bitmap_page, page_id, page_size,
                                  IBUF_BITMAP_IBUF, mtr);

  if (mtr == &local_mtr) {
    mtr_commit(mtr);
  }

  return ret != 0;
}

/** Returns the page number field of an ibuf record.
 @param[in] mtr mini-transaction owning rec
 @param[in] rec ibuf record
 @return page number */
static page_no_t ibuf_rec_get_page_no_func(IF_DEBUG(mtr_t *mtr, )
                                               const rec_t *rec) {
  const byte *field;
  ulint len;

  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(ibuf_inside(mtr));
  ut_ad(rec_get_n_fields_old_raw(rec) > 2);

  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_MARKER, &len);

  ut_a(len == 1);

  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_PAGE, &len);

  ut_a(len == 4);

  return (mach_read_from_4(field));
}

static inline page_no_t ibuf_rec_get_page_no(mtr_t *mtr [[maybe_unused]],
                                             const rec_t *rec) {
  return ibuf_rec_get_page_no_func(IF_DEBUG(mtr, ) rec);
}

/** Returns the space id field of an ibuf record. For < 4.1.x format records
 returns 0.
 @param[in] mtr mini-transaction owning rec
 @param[in] rec ibuf record
 @return space id */
static space_id_t ibuf_rec_get_space_func(IF_DEBUG(mtr_t *mtr, )
                                              const rec_t *rec) {
  const byte *field;
  ulint len;

  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(ibuf_inside(mtr));
  ut_ad(rec_get_n_fields_old_raw(rec) > 2);

  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_MARKER, &len);

  ut_a(len == 1);

  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_SPACE, &len);

  ut_a(len == 4);

  return (mach_read_from_4(field));
}

static inline space_id_t ibuf_rec_get_space(mtr_t *mtr [[maybe_unused]],
                                            const rec_t *rec) {
  return ibuf_rec_get_space_func(IF_DEBUG(mtr, ) rec);
}

/**
 * 从ibuf记录中获取各种信息（4.1.x及以上版本格式）
 * Get various information about an ibuf record in >= 4.1.x format.
 * @param[in]     mtr             拥有记录的mini-transaction，或者nullptr（当从ibuf_rec_has_multi_value()调用时）
 *                                Mini-transaction owning rec, or nullptr if this
 *                                is called from ibuf_rec_has_multi_value().
 *                                Because it's from page_validate() which doesn't
 *                                have mtr at hand
 * @param[in]     rec             ibuf记录
 *                                Ibuf record
 * @param[in,out] op              操作类型，或NULL
 *                                Operation type, or NULL
 * @param[in,out] comp            是否紧凑格式标志，或NULL
 *                                Compact flag, or NULL
 * @param[in,out] info_len        第四个字段开头信息字段的长度，或NULL
 *                                Length of info fields at the start of the fourth
 *                                field, or NULL
 * @param[in]     counter         计数器值，或NULL
 *                                Counter value, or NULL
 */
static void ibuf_rec_get_info_func(IF_DEBUG(mtr_t *mtr, ) const rec_t *rec,
                                   ibuf_op_t *op, bool *comp, ulint *info_len,
                                   ulint *counter) {
  // 类型信息指针
  const byte *types;
  // 字段数量
  ulint fields;
  // 长度
  ulint len;

  /* 局部变量用于暂存参数值 */
  /* Local variables to shadow arguments. */
  // 操作类型
  ibuf_op_t op_local;
  // 是否紧凑格式
  bool comp_local;
  // 信息长度
  ulint info_len_local;
  // 计数器值
  ulint counter_local;

  // 断言：mtr为null或者mtr持有记录的X或S锁
  ut_ad(mtr == nullptr ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  // 断言：mtr为null或者处于ibuf操作中
  ut_ad(mtr == nullptr || ibuf_inside(mtr));
  // 获取记录的字段数量
  fields = rec_get_n_fields_old_raw(rec);
  // 断言：字段数量必须大于用户字段起始位置
  ut_a(fields > IBUF_REC_FIELD_USER);

  // 获取元数据字段
  types = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_METADATA, &len);

  // 计算信息长度（取模）
  info_len_local = len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;

  // 根据信息长度处理不同情况
  switch (info_len_local) {
    case 0:
    case 1:
      // 旧格式记录，只能是插入操作
      op_local = IBUF_OP_INSERT;
      // 紧凑格式标志
      comp_local = info_len_local;
      // 断言：旧格式没有计数器
      ut_ad(!counter);
      // 计数器未定义
      counter_local = ULINT_UNDEFINED;
      break;

    case IBUF_REC_INFO_SIZE:
      // 新格式记录，读取操作类型
      op_local = (ibuf_op_t)types[IBUF_REC_OFFSET_TYPE];
      // 读取紧凑格式标志
      comp_local = types[IBUF_REC_OFFSET_FLAGS] & IBUF_REC_COMPACT;
      // 读取计数器值
      counter_local = mach_read_from_2(types + IBUF_REC_OFFSET_COUNTER);
      break;

    default:
      // 不支持的格式，报错
      ut_error;
  }

  // 断言：操作类型必须有效
  ut_a(op_local < IBUF_OP_COUNT);
  // 断言：剩余长度必须等于字段数量乘以类型缓冲区大小
  ut_a((len - info_len_local) ==
       (fields - IBUF_REC_FIELD_USER) * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

  // 如果op参数不为null，设置操作类型
  if (op) {
    *op = op_local;
  }

  // 如果comp参数不为null，设置紧凑格式标志
  if (comp) {
    *comp = comp_local;
  }

  // 如果info_len参数不为null，设置信息长度
  if (info_len) {
    *info_len = info_len_local;
  }

  // 如果counter参数不为null，设置计数器值
  if (counter) {
    *counter = counter_local;
  }
}

inline void ibuf_rec_get_info(mtr_t *mtr [[maybe_unused]], const rec_t *rec,
                              ibuf_op_t *op, bool *comp, ulint *info_len,
                              ulint *counter) {
  ibuf_rec_get_info_func(IF_DEBUG(mtr, ) rec, op, comp, info_len, counter);
}

/**
 * 从ibuf记录中获取操作类型字段
 * Returns the operation type field of an ibuf record.
 * @param[in] mtr 拥有记录的小事务
 *               mini-transaction owning rec
 * @param[in] rec ibuf记录
 *               ibuf record
 * @return 操作类型
 *         operation type 
 */
static ibuf_op_t ibuf_rec_get_op_type_func(IF_DEBUG(mtr_t *mtr, )
                                               const rec_t *rec) {
  // 字段长度变量
  ulint len;

  // 断言：确保mtr持有记录的X或S锁
  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  // 断言：确保在ibuf操作中
  ut_ad(ibuf_inside(mtr));
  // 断言：记录字段数必须大于2
  ut_ad(rec_get_n_fields_old_raw(rec) > 2);

  // 获取标记字段长度(忽略返回值)
  (void)rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_MARKER, &len);

  // 如果标记字段长度大于1，说明是旧格式记录(4.1.x之前)
  if (len > 1) {
    /* This is a < 4.1.x format record */
    // 旧格式只支持插入操作
    return (IBUF_OP_INSERT);
  } else {
    // 新格式记录，需要获取操作类型信息
    ibuf_op_t op;

    // 调用ibuf_rec_get_info_func获取操作类型
    ibuf_rec_get_info_func(IF_DEBUG(mtr, ) rec, &op, nullptr, nullptr, nullptr);

    return (op);
  }
}

inline ibuf_op_t ibuf_rec_get_op_type(mtr_t *mtr [[maybe_unused]],
                                      const rec_t *rec) {
  return ibuf_rec_get_op_type_func(IF_DEBUG(mtr, ) rec);
}

/** Read the first two bytes from a record's fourth field (counter field in
 new records; something else in older records).
 @return "counter" field, or ULINT_UNDEFINED if for some reason it
 can't be read */
ulint ibuf_rec_get_counter(const rec_t *rec) /*!< in: ibuf record */
{
  const byte *ptr;
  ulint len;

  if (rec_get_n_fields_old_raw(rec) <= IBUF_REC_FIELD_METADATA) {
    return (ULINT_UNDEFINED);
  }

  /* nullptr for index as it can't be clustered index */
  ptr = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_METADATA, &len);

  if (len >= 2) {
    return (mach_read_from_2(ptr));
  } else {
    return (ULINT_UNDEFINED);
  }
}

bool ibuf_rec_has_multi_value(const rec_t *rec) {
  ulint len;
  ulint info_len;
  uint32_t n_fields = rec_get_n_fields_old_raw(rec) - IBUF_REC_FIELD_USER;
  /* nullptr for index as it can't be clustered index */
  const byte *types =
      rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_METADATA, &len);

  ibuf_rec_get_info(nullptr, rec, nullptr, nullptr, &info_len, nullptr);
  types += info_len;

  for (uint32_t i = 0; i < n_fields; ++i) {
    dtype_t dtype;

    dtype_new_read_for_order_and_null_size(&dtype, types);

    if ((dtype.prtype & DATA_MULTI_VALUE) != 0) {
      return (true);
    }

    types += DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;
  }

  return (false);
}

/** Add accumulated operation counts to a permanent array. Both arrays must be
 of size IBUF_OP_COUNT. */
static void ibuf_add_ops(
    std::atomic<ulint> *arr, /*!< in/out: array to modify */
    const ulint *ops)        /*!< in: operation counts */

{
  ulint i;

  for (i = 0; i < IBUF_OP_COUNT; i++) {
    arr[i].fetch_add(ops[i]);
  }
}

/** Print operation counts. The array must be of size IBUF_OP_COUNT. */
static void ibuf_print_ops(
    const std::atomic<ulint> *ops, /*!< in: operation counts */
    FILE *file)                    /*!< in: file where to print */
{
  static const char *op_names[] = {"insert", "delete mark", "delete"};
  ulint i;

  static_assert(UT_ARR_SIZE(op_names) == IBUF_OP_COUNT);

  for (i = 0; i < IBUF_OP_COUNT; i++) {
    fprintf(file, "%s %lu%s", op_names[i], (ulong)ops[i].load(),
            (i < (IBUF_OP_COUNT - 1)) ? ", " : "");
  }

  putc('\n', file);
}

/** Creates a dummy index for inserting a record to a non-clustered index.
 @return dummy index */
static dict_index_t *ibuf_dummy_index_create(
    ulint n,   /*!< in: number of fields */
    bool comp) /*!< in: true=use compact record format */
{
  dict_table_t *table;
  dict_index_t *index;

  table = dict_mem_table_create("IBUF_DUMMY", DICT_HDR_SPACE, n, 0, 0,
                                comp ? DICT_TF_COMPACT : 0, 0);

  index =
      dict_mem_index_create("IBUF_DUMMY", "IBUF_DUMMY", DICT_HDR_SPACE, 0, n);

  index->table = table;

  /* avoid ut_ad(index->cached) in dict_index_get_n_unique_in_tree */
  index->cached = true;

  return (index);
}
/** Add a column to the dummy index */
static void ibuf_dummy_index_add_col(
    dict_index_t *index, /*!< in: dummy index */
    const dtype_t *type, /*!< in: the data type of the column */
    ulint len)           /*!< in: length of the column */
{
  ulint i = index->table->n_def;
  dict_mem_table_add_col(index->table, nullptr, nullptr, dtype_get_mtype(type),
                         dtype_get_prtype(type), dtype_get_len(type), true);
  dict_index_add_col(index, index->table, index->table->get_col(i), len, true);
}
/** Deallocates a dummy index for inserting a record to a non-clustered index.
 */
static void ibuf_dummy_index_free(
    dict_index_t *index) /*!< in, own: dummy index */
{
  dict_table_t *table = index->table;

  dict_mem_index_free(index);
  dict_mem_table_free(table);
}

/** Builds the entry used to
 1) IBUF_OP_INSERT: insert into a non-clustered index
 2) IBUF_OP_DELETE_MARK: find the record whose delete-mark flag we need to
    activate
 3) IBUF_OP_DELETE: find the record we need to delete
 when we have the corresponding record in an ibuf index.
 NOTE that as we copy pointers to fields in ibuf_rec, the caller must
 hold a latch to the ibuf_rec page as long as the entry is used!
 @param[in] mtr mini-transaction owning rec
 @param[in] ibuf_rec record in an insert buffer
 @param[in] heap heap where built
 @param[out] pindex own: dummy index that describes the entry
 @return own: entry to insert to a non-clustered index */
/* 构建用于以下操作的条目：
   1) IBUF_OP_INSERT: 插入到非聚集索引
   2) IBUF_OP_DELETE_MARK: 找到需要激活删除标记的记录
   3) IBUF_OP_DELETE: 找到需要删除的记录
   当我们在插入缓冲区索引中有相应记录时使用。
   注意：由于我们复制了指向ibuf_rec字段的指针，调用者必须保持对ibuf_rec页的锁定，只要条目在使用中！
   @param[in] mtr 拥有rec的迷你事务
   @param[in] ibuf_rec 插入缓冲区中的记录
   @param[in] heap 构建时使用的堆
   @param[out] pindex 拥有：描述条目的虚拟索引
   @return 拥有：要插入到非聚集索引的条目 */
static dtuple_t *ibuf_build_entry_from_ibuf_rec_func(IF_DEBUG(mtr_t *mtr, )
                                                         const rec_t *ibuf_rec,
                                                     mem_heap_t *heap,
                                                     dict_index_t **pindex) {
  // 声明变量：元组、字段、字段数量等
  dtuple_t *tuple;
  dfield_t *field;
  ulint n_fields;
  const byte *types;
  const byte *data;
  ulint len;
  ulint info_len;
  ulint i;
  bool comp;
  dict_index_t *index;

  // 断言：确保mtr持有对ibuf_rec页的X或S锁
  ut_ad(mtr_memo_contains_page(mtr, ibuf_rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, ibuf_rec, MTR_MEMO_PAGE_S_FIX));
  // 断言：确保在插入缓冲区操作中
  ut_ad(ibuf_inside(mtr));

  // 获取标记字段的数据和长度
  data = rec_get_nth_field_old(nullptr, ibuf_rec, IBUF_REC_FIELD_MARKER, &len);

  // 验证标记字段的长度和值
  ut_a(len == 1);
  ut_a(*data == 0);
  // 验证记录字段数大于用户字段起始位置
  ut_a(rec_get_n_fields_old_raw(ibuf_rec) > IBUF_REC_FIELD_USER);

  // 计算实际用户字段数量
  n_fields = rec_get_n_fields_old_raw(ibuf_rec) - IBUF_REC_FIELD_USER;

  // 在堆上创建元组
  tuple = dtuple_create(heap, n_fields);

  // 获取元数据字段
  types =
      rec_get_nth_field_old(nullptr, ibuf_rec, IBUF_REC_FIELD_METADATA, &len);

  // 从记录中获取信息：压缩标志、信息长度等
  ibuf_rec_get_info_func(IF_DEBUG(mtr, ) ibuf_rec, nullptr, &comp, &info_len,
                         nullptr);

  // 创建虚拟索引
  index = ibuf_dummy_index_create(n_fields, comp);

  // 调整类型指针和长度
  len -= info_len;
  types += info_len;

  // 验证类型信息长度
  ut_a(len == n_fields * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

  // 遍历所有字段
  for (i = 0; i < n_fields; i++) {
    // 获取当前字段
    field = dtuple_get_nth_field(tuple, i);

    // 获取字段数据
    data =
        rec_get_nth_field_old(nullptr, ibuf_rec, i + IBUF_REC_FIELD_USER, &len);

    // 设置字段数据
    dfield_set_data(field, data, len);

    // 从类型信息中读取字段类型
    dtype_new_read_for_order_and_null_size(
        dfield_get_type(field), types + i * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE);

    // 向虚拟索引添加列
    ibuf_dummy_index_add_col(index, dfield_get_type(field), len);
  }

  /* Prevent an ut_ad() failure in page_zip_write_rec() by
  adding system columns to the dummy table pointed to by the
  dummy secondary index.  The insert buffer is only used for
  secondary indexes, whose records never contain any system
  columns, such as DB_TRX_ID. */
  /* 通过向虚拟二级索引指向的虚拟表添加系统列，
     防止page_zip_write_rec()中的ut_ad()失败。
     插入缓冲区仅用于二级索引，其记录从不包含任何系统列，如DB_TRX_ID。 */
  ut_d(dict_table_add_system_columns(index->table, index->table->heap));

  // 设置输出参数
  *pindex = index;

  // 返回构建的元组
  return (tuple);
}

inline dtuple_t *ibuf_build_entry_from_ibuf_rec(mtr_t *mtr [[maybe_unused]],
                                                const rec_t *ibuf_rec,
                                                mem_heap_t *heap,
                                                dict_index_t **pindex) {
  return ibuf_build_entry_from_ibuf_rec_func(IF_DEBUG(mtr, ) ibuf_rec, heap,
                                             pindex);
}
/** Get the data size.
 @return size of fields */
static inline ulint ibuf_rec_get_size(
    const rec_t *rec,  /*!< in: ibuf record */
    const byte *types, /*!< in: fields */
    ulint n_fields,    /*!< in: number of fields */
    bool comp)         /*!< in: 0=ROW_FORMAT=REDUNDANT,
                        nonzero=ROW_FORMAT=COMPACT */
{
  ulint i;
  ulint field_offset;
  ulint types_offset;
  ulint size = 0;

  field_offset = IBUF_REC_FIELD_USER;
  types_offset = DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;

  for (i = 0; i < n_fields; i++) {
    ulint len;
    dtype_t dtype;

    /* nullptr for index as it can't be clustered index */
    rec_get_nth_field_offs_old(nullptr, rec, i + field_offset, &len);

    if (len != UNIV_SQL_NULL) {
      size += len;
    } else {
      dtype_new_read_for_order_and_null_size(&dtype, types);

      size += dtype_get_sql_null_size(&dtype, comp);
    }

    types += types_offset;
  }

  return (size);
}

/** Returns the space taken by a stored non-clustered index entry if converted
 to an index record.
 @param[in] mtr mini-transaction owning rec
 @param[in] ibuf_rec ibuf record
 @return size of index record in bytes + an upper limit of the space
 taken in the page directory */
static ulint ibuf_rec_get_volume_func(IF_DEBUG(mtr_t *mtr, )
                                          const rec_t *ibuf_rec) {
  ulint len;
  const byte *data;
  const byte *types;
  ulint n_fields;
  ulint data_size;
  bool comp;
  ibuf_op_t op;
  ulint info_len;

  ut_ad(mtr_memo_contains_page(mtr, ibuf_rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, ibuf_rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(ibuf_inside(mtr));
  ut_ad(rec_get_n_fields_old_raw(ibuf_rec) > 2);

  data = rec_get_nth_field_old(nullptr, ibuf_rec, IBUF_REC_FIELD_MARKER, &len);
  ut_a(len == 1);
  ut_a(*data == 0);

  types =
      rec_get_nth_field_old(nullptr, ibuf_rec, IBUF_REC_FIELD_METADATA, &len);

  ibuf_rec_get_info_func(IF_DEBUG(mtr, ) ibuf_rec, &op, &comp, &info_len,
                         nullptr);

  if (op == IBUF_OP_DELETE_MARK || op == IBUF_OP_DELETE) {
    /* Delete-marking a record doesn't take any
    additional space, and while deleting a record
    actually frees up space, we have to play it safe and
    pretend it takes no additional space (the record
    might not exist, etc.).  */

    return (0);
  } else if (comp) {
    dtuple_t *entry;
    ulint volume;
    dict_index_t *dummy_index;
    mem_heap_t *heap = mem_heap_create(500, UT_LOCATION_HERE);

    entry = ibuf_build_entry_from_ibuf_rec_func(IF_DEBUG(mtr, ) ibuf_rec, heap,
                                                &dummy_index);

    volume = rec_get_converted_size(dummy_index, entry);

    ibuf_dummy_index_free(dummy_index);
    mem_heap_free(heap);

    return (volume + page_dir_calc_reserved_space(1));
  }

  types += info_len;
  n_fields = rec_get_n_fields_old_raw(ibuf_rec) - IBUF_REC_FIELD_USER;

  data_size = ibuf_rec_get_size(ibuf_rec, types, n_fields, comp);

  return (data_size + rec_get_converted_extra_size(data_size, n_fields, false) +
          page_dir_calc_reserved_space(1));
}

inline ulint ibuf_rec_get_volume(mtr_t *mtr [[maybe_unused]],
                                 const rec_t *rec) {
  return ibuf_rec_get_volume_func(IF_DEBUG(mtr, ) rec);
}

/** 
 * 构建要插入到ibuf树中的元组，当有一个非聚集索引的条目时
 * Builds the tuple to insert to an ibuf tree when we have an entry for a
 * non-clustered index.
 *
 * 注意：原始条目必须保留，因为我们复制了指向其字段的指针
 * NOTE that the original entry must be kept because we copy pointers to
 * its fields.
 *
 * @return 拥有：要插入到ibuf索引树的条目
 * @return own: entry to insert into an ibuf index tree 
 */
static dtuple_t *ibuf_entry_build(
    ibuf_op_t op,          /*!< in: 操作类型 */
    dict_index_t *index,   /*!< in: 非聚集索引 */
    const dtuple_t *entry, /*!< in: 非聚集索引的条目 */
    space_id_t space,      /*!< in: 表空间ID */
    page_no_t page_no,     /*!< in: 条目应该插入的索引页号 */
    ulint counter,         /*!< in: 计数器值；
                           ULINT_UNDEFINED=未使用 */
    mem_heap_t *heap)      /*!< in: 用于构建的内存堆 */
{
  // 声明变量：元组、字段、字段数量等
  dtuple_t *tuple;
  dfield_t *field;
  const dfield_t *entry_field;
  dtype_t fake_type;
  ulint n_fields;
  byte *buf;
  byte *ti;
  byte *type_info;
  ulint i;

  // 断言：如果不是未定义计数器，操作必须是INSERT
  ut_ad(counter != ULINT_UNDEFINED || op == IBUF_OP_INSERT);
  // 断言：计数器值必须小于等于0xFFFF
  ut_ad(counter == ULINT_UNDEFINED || counter <= 0xFFFF);
  // 断言：操作类型必须有效
  ut_ad(op < IBUF_OP_COUNT);

  // 初始化一个假的类型结构
  memset(&fake_type, 0, sizeof(dtype_t));

  /* 我们必须构建一个包含以下字段的元组：
     1-4) 这些在本文件顶部有描述
     5) 其余字段从条目中复制
     元组中的所有字段都按照我们插入缓冲区树中的类型二进制顺序排列 */
  /* We have to build a tuple with the following fields:
     1-4) These are described at the top of this file.
     5) The rest of the fields are copied from the entry.
     All fields in the tuple are ordered like the type binary in our
     insert buffer tree. */

  // 获取条目中的字段数量
  n_fields = dtuple_get_n_fields(entry);

  // 创建元组，包含用户字段和IBUF元数据字段
  tuple = dtuple_create(heap, n_fields + IBUF_REC_FIELD_USER);

  /* 1) 表空间ID */
  /* 1) Space Id */

  // 获取元组中的空间ID字段
  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_SPACE);

  // 分配4字节缓冲区
  buf = static_cast<byte *>(mem_heap_alloc(heap, 4));

  // 将空间ID写入缓冲区
  mach_write_to_4(buf, space);

  // 设置字段数据和类型
  dfield_set_data(field, buf, 4);
  dfield_set_type(field, &fake_type);

  /* 2) 标记字节 */
  /* 2) Marker byte */

  // 获取元组中的标记字段
  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_MARKER);

  // 分配1字节缓冲区
  buf = static_cast<byte *>(mem_heap_alloc(heap, 1));

  /* 我们将标记字节设置为零 */
  /* We set the marker byte zero */

  // 写入标记字节
  mach_write_to_1(buf, 0);

  // 设置字段数据和类型
  dfield_set_data(field, buf, 1);
  dfield_set_type(field, &fake_type);

  /* 3) 页号 */
  /* 3) Page number */

  // 获取元组中的页号字段
  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_PAGE);

  // 分配4字节缓冲区
  buf = static_cast<byte *>(mem_heap_alloc(heap, 4));

  // 将页号写入缓冲区
  mach_write_to_4(buf, page_no);

  // 设置字段数据和类型
  dfield_set_data(field, buf, 4);
  dfield_set_type(field, &fake_type);

  /* 4) 类型信息，第一部分 */
  /* 4) Type info, part #1 */

  // 根据计数器是否定义决定类型信息长度
  if (counter == ULINT_UNDEFINED) {
    // 旧格式：紧凑标志（1字节）或无（0字节）
    i = dict_table_is_comp(index->table) ? 1 : 0;
  } else {
    // 新格式：固定长度信息
    ut_ad(counter <= 0xFFFF);
    i = IBUF_REC_INFO_SIZE;
  }

  // 分配类型信息缓冲区
  ti = type_info = static_cast<byte *>(
      mem_heap_alloc(heap, i + n_fields * DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE));

  // 根据信息长度处理不同类型格式
  switch (i) {
    default:
      ut_error;
      break;
    case 1:
      /* 设置ROW_FORMAT=COMPACT的标志 */
      /* set the flag for ROW_FORMAT=COMPACT */
      *ti++ = 0;
      [[fallthrough]];
    case 0:
      /* 旧格式不允许删除缓冲 */
      /* the old format does not allow delete buffering */
      ut_ad(op == IBUF_OP_INSERT);
      break;
    case IBUF_REC_INFO_SIZE:
      // 写入计数器值
      mach_write_to_2(ti + IBUF_REC_OFFSET_COUNTER, counter);

      // 写入操作类型和标志
      ti[IBUF_REC_OFFSET_TYPE] = (byte)op;
      ti[IBUF_REC_OFFSET_FLAGS] =
          dict_table_is_comp(index->table) ? IBUF_REC_COMPACT : 0;
      ti += IBUF_REC_INFO_SIZE;
      break;
  }

  /* 5+) 从条目中复制字段 */
  /* 5+) Fields from the entry */

  // 遍历所有字段
  for (i = 0; i < n_fields; i++) {
    ulint fixed_len;
    const dict_field_t *ifield;

    // 获取元组中的当前字段
    field = dtuple_get_nth_field(tuple, i + IBUF_REC_FIELD_USER);
    // 获取条目中的对应字段
    entry_field = dtuple_get_nth_field(entry, i);
    // 复制字段数据
    dfield_copy(field, entry_field);

    // 获取索引字段信息
    ifield = index->get_field(i);
    /* 固定长度列的前缀索引列是固定长度的。
        然而，在下面的函数调用中，
        dfield_get_type(entry_field)包含聚集索引中列的固定长度。
        用二级索引列的固定长度替换它。 */
    /* Prefix index columns of fixed-length columns are of
    fixed length.  However, in the function call below,
    dfield_get_type(entry_field) contains the fixed length
    of the column in the clustered index.  Replace it with
    the fixed length of the secondary index column. */
    fixed_len = ifield->fixed_len;

#ifdef UNIV_DEBUG
    // 调试断言：检查固定长度是否有效
    if (fixed_len) {
      /* dict_index_add_col()应该保证这些 */
      /* dict_index_add_col() should guarantee these */
      ut_ad(fixed_len <= (ulint)dfield_get_type(entry_field)->len);
      if (ifield->prefix_len) {
        ut_ad(ifield->prefix_len == fixed_len);
      } else {
        ut_ad(fixed_len == (ulint)dfield_get_type(entry_field)->len);
      }
    }
#endif /* UNIV_DEBUG */

    // 存储字段类型信息
    dtype_new_store_for_order_and_null_size(ti, dfield_get_type(entry_field),
                                            fixed_len);
    ti += DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE;
  }

  /* 4) 类型信息，第二部分 */
  /* 4) Type info, part #2 */

  // 设置元数据字段
  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_METADATA);

  dfield_set_data(field, type_info, ti - type_info);
  dfield_set_type(field, &fake_type);

  /* 设置新元组二进制中的所有类型 */
  /* Set all the types in the new tuple binary */
  dtuple_set_types_binary(tuple, n_fields + IBUF_REC_FIELD_USER);

  // 返回构建的元组
  return (tuple);
}

/** Builds a search tuple used to search buffered inserts for an index page.
 This is for >= 4.1.x format records.
 @return own: search tuple */
static dtuple_t *ibuf_search_tuple_build(
    space_id_t space,  /*!< in: space id */
    page_no_t page_no, /*!< in: index page number */
    mem_heap_t *heap)  /*!< in: heap into which to build */
{
  dtuple_t *tuple;
  dfield_t *field;
  dtype_t fake_type;
  byte *buf;

  memset(&fake_type, 0, sizeof(dtype_t));

  tuple = dtuple_create(heap, IBUF_REC_FIELD_METADATA);

  /* Store the space id in tuple */

  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_SPACE);

  buf = static_cast<byte *>(mem_heap_alloc(heap, 4));

  mach_write_to_4(buf, space);

  dfield_set_data(field, buf, 4);

  dfield_set_type(field, &fake_type);

  /* Store the new format record marker byte */

  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_MARKER);

  buf = static_cast<byte *>(mem_heap_alloc(heap, 1));

  mach_write_to_1(buf, 0);

  dfield_set_data(field, buf, 1);

  dfield_set_type(field, &fake_type);

  /* Store the page number in tuple */

  field = dtuple_get_nth_field(tuple, IBUF_REC_FIELD_PAGE);

  buf = static_cast<byte *>(mem_heap_alloc(heap, 4));

  mach_write_to_4(buf, page_no);

  dfield_set_data(field, buf, 4);

  dfield_set_type(field, &fake_type);

  dtuple_set_types_binary(tuple, IBUF_REC_FIELD_METADATA);

  return (tuple);
}

/** Checks if there are enough pages in the free list of the ibuf tree that we
 dare to start a pessimistic insert to the insert buffer.
 @return true if enough free pages in list */
static inline bool ibuf_data_enough_free_for_insert(void) {
  ut_ad(mutex_own(&ibuf_mutex));

  /* We want a big margin of free pages, because a B-tree can sometimes
  grow in size also if records are deleted from it, as the node pointers
  can change, and we must make sure that we are able to delete the
  inserts buffered for pages that we read to the buffer pool, without
  any risk of running out of free space in the insert buffer. */

  return (ibuf->free_list_len >= (ibuf->size / 2) + 3 * ibuf->height);
}

/** Checks if there are enough pages in the free list of the ibuf tree that we
 should remove them and free to the file space management.
 @return true if enough free pages in list */
static inline bool ibuf_data_too_much_free(void) {
  ut_ad(mutex_own(&ibuf_mutex));

  return (ibuf->free_list_len >= 3 + (ibuf->size / 2) + 3 * ibuf->height);
}

/** Allocates a new page from the ibuf file segment and adds it to the free
 list.
 @return true on success, false if no space left */
static bool ibuf_add_free_page(void) {
  mtr_t mtr;
  page_t *header_page;
  buf_block_t *block;
  page_t *page;
  page_t *root;
  page_t *bitmap_page;

  fil_space_t *space = fil_space_get_sys_space();

  mtr_start(&mtr);

  /* Acquire the fsp latch before the ibuf header, obeying the latching
  order */
  mtr_x_lock(&space->latch, &mtr, UT_LOCATION_HERE);
  header_page = ibuf_header_page_get(&mtr);

  /* Allocate a new page: NOTE that if the page has been a part of a
  non-clustered index which has subsequently been dropped, then the
  page may have buffered inserts in the insert buffer, and these
  should be deleted from there. These get deleted when the page
  allocation creates the page in buffer. Thus the call below may end
  up calling the insert buffer routines and, as we yet have no latches
  to insert buffer tree pages, these routines can run without a risk
  of a deadlock. This is the reason why we created a special ibuf
  header page apart from the ibuf tree. */

  block = fseg_alloc_free_page(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER,
                               0, FSP_UP, &mtr);

  if (block == nullptr) {
    mtr_commit(&mtr);

    return false;
  }

  ut_ad(rw_lock_get_x_lock_count(&block->lock) == 1);
  ibuf_enter(&mtr);
  mutex_enter(&ibuf_mutex);
  root = ibuf_tree_root_get(&mtr);

  buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE_NEW);
  page = buf_block_get_frame(block);

  /* Add the page to the free list and update the ibuf size data */

  flst_add_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
                page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, &mtr);

  mlog_write_ulint(page + FIL_PAGE_TYPE, FIL_PAGE_IBUF_FREE_LIST, MLOG_2BYTES,
                   &mtr);

  ibuf->seg_size++;
  ibuf->free_list_len++;

  /* Set the bit indicating that this page is now an ibuf tree page
  (level 2 page) */

  const page_id_t page_id(IBUF_SPACE_ID, block->page.id.page_no());
  const page_size_t page_size(space->flags);

  bitmap_page =
      ibuf_bitmap_get_map_page(page_id, page_size, UT_LOCATION_HERE, &mtr);

  mutex_exit(&ibuf_mutex);

  ibuf_bitmap_page_set_bits(bitmap_page, page_id, page_size, IBUF_BITMAP_IBUF,
                            true, &mtr);

  ibuf_mtr_commit(&mtr);

  return true;
}

/** Removes a page from the free list and frees it to the fsp system. */
static void ibuf_remove_free_page(void) {
  mtr_t mtr;
  mtr_t mtr2;
  page_t *header_page;
  page_no_t page_no;
  page_t *page;
  page_t *root;
  page_t *bitmap_page;

  fil_space_t *space = fil_space_get_sys_space();

  mtr_start(&mtr);

  const page_size_t page_size(space->flags);

  /* Acquire the fsp latch before the ibuf header, obeying the latching
  order */

  mtr_x_lock(&space->latch, &mtr, UT_LOCATION_HERE);
  header_page = ibuf_header_page_get(&mtr);

  /* Prevent pessimistic inserts to insert buffer trees for a while */
  ibuf_enter(&mtr);
  mutex_enter(&ibuf_pessimistic_insert_mutex);
  mutex_enter(&ibuf_mutex);

  if (!ibuf_data_too_much_free()) {
    mutex_exit(&ibuf_mutex);
    mutex_exit(&ibuf_pessimistic_insert_mutex);

    ibuf_mtr_commit(&mtr);

    return;
  }

  ibuf_mtr_start(&mtr2);

  root = ibuf_tree_root_get(&mtr2);

  mutex_exit(&ibuf_mutex);

  page_no =
      flst_get_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, &mtr2).page;

  /* NOTE that we must release the latch on the ibuf tree root
  because in fseg_free_page we access level 1 pages, and the root
  is a level 2 page. */

  ibuf_mtr_commit(&mtr2);
  ibuf_exit(&mtr);

  /* Since pessimistic inserts were prevented, we know that the
  page is still in the free list. NOTE that also deletes may take
  pages from the free list, but they take them from the start, and
  the free list was so long that they cannot have taken the last
  page from it. */

  fseg_free_page(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER,
                 IBUF_SPACE_ID, page_no, false, &mtr);

  const page_id_t page_id(IBUF_SPACE_ID, page_no);

  ut_d(buf_page_reset_file_page_was_freed(page_id));

  ibuf_enter(&mtr);

  mutex_enter(&ibuf_mutex);

  root = ibuf_tree_root_get(&mtr);

  ut_ad(page_no ==
        flst_get_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, &mtr).page);

  {
    buf_block_t *block;

    block = buf_page_get(page_id, univ_page_size, RW_X_LATCH, UT_LOCATION_HERE,
                         &mtr);

    buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);

    page = buf_block_get_frame(block);
  }

  /* Remove the page from the free list and update the ibuf size data */

  flst_remove(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST,
              page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, &mtr);

  mutex_exit(&ibuf_pessimistic_insert_mutex);

  ibuf->seg_size--;
  ibuf->free_list_len--;

  /* Set the bit indicating that this page is no more an ibuf tree page
  (level 2 page) */

  bitmap_page =
      ibuf_bitmap_get_map_page(page_id, page_size, UT_LOCATION_HERE, &mtr);

  mutex_exit(&ibuf_mutex);

  ibuf_bitmap_page_set_bits(bitmap_page, page_id, page_size, IBUF_BITMAP_IBUF,
                            false, &mtr);

  ut_d(buf_page_set_file_page_was_freed(page_id));

  ibuf_mtr_commit(&mtr);
}

/** Frees excess pages from the ibuf free list. This function is called when an
 OS thread calls fsp services to allocate a new file segment, or a new page to a
 file segment, and the thread did not own the fsp latch before this call. */
void ibuf_free_excess_pages(void) {
  ut_ad(rw_lock_own(fil_space_get_latch(IBUF_SPACE_ID), RW_LOCK_X));

  ut_ad(rw_lock_get_x_lock_count(fil_space_get_latch(IBUF_SPACE_ID)) == 1);

  /* NOTE: We require that the thread did not own the latch before,
  because then we know that we can obey the correct latching order
  for ibuf latches */

  if (!ibuf) {
    /* Not yet initialized; not sure if this is possible, but
    does no harm to check for it. */

    return;
  }

  /* Free at most a few pages at a time, so that we do not delay the
  requested service too much */

  for (ulint i = 0; i < 4; i++) {
    mutex_enter(&ibuf_mutex);
    auto too_much_free = ibuf_data_too_much_free();
    mutex_exit(&ibuf_mutex);

    if (!too_much_free) {
      return;
    }

    ibuf_remove_free_page();
  }
}

/** Reads page numbers from a leaf in an ibuf tree.
 @param[in] contract true if this function is called to contract the tree, false
 if this is called when a single page becomes full and we look if it pays to
 read also nearby pages
 @param[in] rec insert buffer record
 @param[in] mtr mini-transaction holding rec
 @param[in,out] space_ids space id's of the pages
 @param[in,out] page_nos buffer for at least IBUF_MAX_N_PAGES_MERGED many page
 numbers; the page numbers are in an ascending order
 @param[out] n_stored number of page numbers stored to page_nos in this function
 @return a lower limit for the combined volume of records which will be
 merged */
static ulint ibuf_get_merge_page_nos_func(bool contract, const rec_t *rec,
                                          IF_DEBUG(mtr_t *mtr, )
                                              space_id_t *space_ids,
                                          page_no_t *page_nos,
                                          ulint *n_stored) {
  page_no_t prev_page_no;
  space_id_t prev_space_id;
  page_no_t first_page_no;
  space_id_t first_space_id;
  page_no_t rec_page_no;
  space_id_t rec_space_id;
  ulint sum_volumes;
  ulint volume_for_page;
  ulint rec_volume;
  ulint limit;
  ulint n_pages;
#ifndef UNIV_DEBUG
  mtr_t *mtr = nullptr;
#endif

  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(ibuf_inside(mtr));

  *n_stored = 0;

  limit = std::min(IBUF_MAX_N_PAGES_MERGED, buf_pool_get_curr_size() / 4);

  if (page_rec_is_supremum(rec)) {
    rec = page_rec_get_prev_const(rec);
  }

  if (page_rec_is_infimum(rec)) {
    rec = page_rec_get_next_const(rec);
  }

  if (page_rec_is_supremum(rec)) {
    return (0);
  }

  first_page_no = ibuf_rec_get_page_no(mtr, rec);
  first_space_id = ibuf_rec_get_space(mtr, rec);
  n_pages = 0;
  prev_page_no = 0;
  prev_space_id = 0;

  /* Go backwards from the first rec until we reach the border of the
  'merge area', or the page start or the limit of storable pages is
  reached */

  while (!page_rec_is_infimum(rec) && UNIV_LIKELY(n_pages < limit)) {
    rec_page_no = ibuf_rec_get_page_no(mtr, rec);
    rec_space_id = ibuf_rec_get_space(mtr, rec);

    if (rec_space_id != first_space_id ||
        (rec_page_no / IBUF_MERGE_AREA) != (first_page_no / IBUF_MERGE_AREA)) {
      break;
    }

    if (rec_page_no != prev_page_no || rec_space_id != prev_space_id) {
      n_pages++;
    }

    prev_page_no = rec_page_no;
    prev_space_id = rec_space_id;

    rec = page_rec_get_prev_const(rec);
  }

  rec = page_rec_get_next_const(rec);

  /* At the loop start there is no prev page; we mark this with a pair
  of space id, page no (0, 0) for which there can never be entries in
  the insert buffer */

  prev_page_no = 0;
  prev_space_id = 0;
  sum_volumes = 0;
  volume_for_page = 0;

  while (*n_stored < limit) {
    if (page_rec_is_supremum(rec)) {
      /* When no more records available, mark this with
      another 'impossible' pair of space id, page no */
      rec_page_no = 1;
      rec_space_id = 0;
    } else {
      rec_page_no = ibuf_rec_get_page_no(mtr, rec);
      rec_space_id = ibuf_rec_get_space(mtr, rec);
      /* In the system tablespace the smallest
      possible secondary index leaf page number is
      bigger than FSP_DICT_HDR_PAGE_NO (7).
      In all tablespaces, pages 0 and 1 are reserved
      for the allocation bitmap and the change
      buffer bitmap. In file-per-table tablespaces,
      a file segment inode page will be created at
      page 2 and the clustered index tree is created
      at page 3.  So for file-per-table tablespaces,
      page 4 is the smallest possible secondary
      index leaf page. CREATE TABLESPACE also initially
      uses pages 2 and 3 for the first created table,
      but that table may be dropped, allowing page 2
      to be reused for a secondary index leaf page.
      To keep this assertion simple, just
      make sure the page is >= 2. */
      ut_ad(rec_page_no >= FSP_FIRST_INODE_PAGE_NO);
    }

#ifdef UNIV_IBUF_DEBUG
    ut_a(*n_stored < IBUF_MAX_N_PAGES_MERGED);
#endif
    if ((rec_space_id != prev_space_id || rec_page_no != prev_page_no) &&
        (prev_space_id != 0 || prev_page_no != 0)) {
      if (contract ||
          (prev_page_no == first_page_no && prev_space_id == first_space_id) ||
          (volume_for_page > ((IBUF_MERGE_THRESHOLD - 1) * 4 * UNIV_PAGE_SIZE /
                              IBUF_PAGE_SIZE_PER_FREE_SPACE) /
                                 IBUF_MERGE_THRESHOLD)) {
        space_ids[*n_stored] = prev_space_id;
        page_nos[*n_stored] = prev_page_no;

        (*n_stored)++;

        sum_volumes += volume_for_page;
      }

      if (rec_space_id != first_space_id ||
          rec_page_no / IBUF_MERGE_AREA != first_page_no / IBUF_MERGE_AREA) {
        break;
      }

      volume_for_page = 0;
    }

    if (rec_page_no == 1 && rec_space_id == 0) {
      /* Supremum record */

      break;
    }

    rec_volume = ibuf_rec_get_volume(mtr, rec);

    volume_for_page += rec_volume;

    prev_page_no = rec_page_no;
    prev_space_id = rec_space_id;

    rec = page_rec_get_next_const(rec);
  }

#ifdef UNIV_IBUF_DEBUG
  ut_a(*n_stored <= IBUF_MAX_N_PAGES_MERGED);
#endif
#if 0
  fprintf(stderr, "Ibuf merge batch %lu pages %lu volume\n",
    *n_stored, sum_volumes);
#endif
  return (sum_volumes);
}

/** Get the matching records for space id.
 @return current rec or NULL */
[[nodiscard]] static const rec_t *ibuf_get_user_rec(
    btr_pcur_t *pcur, /*!< in: the current cursor */
    mtr_t *mtr)       /*!< in: mini-transaction */
{
  do {
    const rec_t *rec = pcur->get_rec();

    if (page_rec_is_user_rec(rec)) {
      return (rec);
    }
  } while (pcur->move_to_next(mtr));

  return (nullptr);
}

inline ulint ibuf_get_merge_page_nos(bool contract, const rec_t *rec,
                                     mtr_t *mtr [[maybe_unused]],
                                     space_id_t *ids, page_no_t *pages,
                                     ulint *n_stored) {
  return ibuf_get_merge_page_nos_func(contract, rec, IF_DEBUG(mtr, ) ids, pages,
                                      n_stored);
}

/** Reads page numbers for a space id from an ibuf tree.
 @return a lower limit for the combined volume of records which will be
 merged */
[[nodiscard]] static ulint ibuf_get_merge_pages(
    btr_pcur_t *pcur,   /*!< in/out: cursor */
    space_id_t space,   /*!< in: space for which to merge */
    ulint limit,        /*!< in: max page numbers to read */
    page_no_t *pages,   /*!< out: pages read */
    space_id_t *spaces, /*!< out: spaces read */
    ulint *n_pages,     /*!< out: number of pages read */
    mtr_t *mtr)         /*!< in: mini-transaction */
{
  const rec_t *rec;
  ulint volume = 0;

  ut_a(space != SPACE_UNKNOWN);

  *n_pages = 0;

  while ((rec = ibuf_get_user_rec(pcur, mtr)) != nullptr &&
         ibuf_rec_get_space(mtr, rec) == space && *n_pages < limit) {
    page_no_t page_no = ibuf_rec_get_page_no(mtr, rec);

    if (*n_pages == 0 || pages[*n_pages - 1] != page_no) {
      spaces[*n_pages] = space;
      pages[*n_pages] = page_no;
      ++*n_pages;
    }

    volume += ibuf_rec_get_volume(mtr, rec);

    pcur->move_to_next(mtr);
  }

  return (volume);
}

/** Contracts insert buffer trees by reading pages to the buffer pool.
 @return a lower limit for the combined size in bytes of entries which
 will be merged from ibuf trees to the pages read, 0 if ibuf is
 empty */
static ulint ibuf_merge_pages(
    ulint *n_pages, /*!< out: number of pages to which merged */
    bool sync)      /*!< in: true if the caller wants to wait for
                    the issued read with the highest tablespace
                    address to complete */
{
  mtr_t mtr;
  btr_pcur_t pcur;
  ulint sum_sizes;
  page_no_t page_nos[IBUF_MAX_N_PAGES_MERGED];
  space_id_t space_ids[IBUF_MAX_N_PAGES_MERGED];

  *n_pages = 0;

  /* Check if there is enough reusable space in redo log files. */
  log_free_check();

  ibuf_mtr_start(&mtr);

  /* Open a cursor to a randomly chosen leaf of the tree, at a random
  position within the leaf */
  bool available;

  available = pcur.set_random_position(ibuf->index, BTR_SEARCH_LEAF, &mtr,
                                       UT_LOCATION_HERE);
  /* No one should make this index unavailable when server is running */
  ut_a(available);

  ut_ad(page_validate(pcur.get_page(), ibuf->index));

  if (page_is_empty(pcur.get_page())) {
    /* If a B-tree page is empty, it must be the root page
    and the whole B-tree must be empty. InnoDB does not
    allow empty B-tree pages other than the root. */
    ut_ad(ibuf->empty);
    ut_ad(page_get_space_id(pcur.get_page()) == IBUF_SPACE_ID);
    ut_ad(page_get_page_no(pcur.get_page()) == FSP_IBUF_TREE_ROOT_PAGE_NO);

    ibuf_mtr_commit(&mtr);
    pcur.close();

    return (0);
  }

  sum_sizes = ibuf_get_merge_page_nos(true, pcur.get_rec(), &mtr, space_ids,
                                      page_nos, n_pages);
#if 0 /* defined UNIV_IBUF_DEBUG */
  fprintf(stderr, "Ibuf contract sync %lu pages %lu volume %lu\n",
    sync, *n_pages, sum_sizes);
#endif
  ibuf_mtr_commit(&mtr);
  pcur.close();

  buf_read_ibuf_merge_pages(sync, space_ids, page_nos, *n_pages);

  return (sum_sizes + 1);
}

/** Contracts insert buffer trees by reading pages referring to space_id
 to the buffer pool.
 @returns number of pages merged.*/
ulint ibuf_merge_space(space_id_t space) /*!< in: tablespace id to merge */
{
  mtr_t mtr;
  btr_pcur_t pcur;
  mem_heap_t *heap = mem_heap_create(512, UT_LOCATION_HERE);
  dtuple_t *tuple = ibuf_search_tuple_build(space, 0, heap);
  ulint n_pages = 0;

  ut_ad(!dict_sys_t::is_reserved(space));

  ibuf_mtr_start(&mtr);

  /* Position the cursor on the first matching record. */

  pcur.open(ibuf->index, 0, tuple, PAGE_CUR_GE, BTR_SEARCH_LEAF, &mtr,
            UT_LOCATION_HERE);

  mem_heap_free(heap);

  ut_ad(page_validate(pcur.get_page(), ibuf->index));

  ulint sum_sizes = 0;
  page_no_t pages[IBUF_MAX_N_PAGES_MERGED];
  space_id_t spaces[IBUF_MAX_N_PAGES_MERGED];

  if (page_is_empty(pcur.get_page())) {
    /* If a B-tree page is empty, it must be the root page
    and the whole B-tree must be empty. InnoDB does not
    allow empty B-tree pages other than the root. */
    ut_ad(ibuf->empty);
    ut_ad(page_get_space_id(pcur.get_page()) == IBUF_SPACE_ID);
    ut_ad(page_get_page_no(pcur.get_page()) == FSP_IBUF_TREE_ROOT_PAGE_NO);

  } else {
    sum_sizes = ibuf_get_merge_pages(&pcur, space, IBUF_MAX_N_PAGES_MERGED,
                                     &pages[0], &spaces[0], &n_pages, &mtr);
    ib::info(ER_IB_MSG_606) << "Size of pages merged " << sum_sizes;
  }

  ibuf_mtr_commit(&mtr);

  pcur.close();

  if (n_pages > 0) {
    ut_ad(n_pages <= UT_ARR_SIZE(pages));

#ifdef UNIV_DEBUG
    for (ulint i = 0; i < n_pages; ++i) {
      ut_ad(spaces[i] == space);
    }
#endif /* UNIV_DEBUG */

    buf_read_ibuf_merge_pages(true, spaces, pages, n_pages);
  }

  return (n_pages);
}

/** Contract the change buffer by reading pages to the buffer pool.
@param[out]     n_pages         number of pages merged
@param[in]      sync            whether the caller waits for
the issued reads to complete
@return a lower limit for the combined size in bytes of entries which
will be merged from ibuf trees to the pages read, 0 if ibuf is
empty */
[[nodiscard]] static ulint ibuf_merge(ulint *n_pages, bool sync) {
  *n_pages = 0;

  /* We perform a dirty read of ibuf->empty, without latching
  the insert buffer root page. We trust this dirty read except
  when a slow shutdown is being executed. During a slow
  shutdown, the insert buffer merge must be completed. */

  if (ibuf->empty && srv_shutdown_state.load() < SRV_SHUTDOWN_CLEANUP) {
    return (0);
#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
  } else if (ibuf_debug) {
    return (0);
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */
  } else {
    return (ibuf_merge_pages(n_pages, sync));
  }
}

/** Contract the change buffer by reading pages to the buffer pool.
@param[in]      sync    whether the caller waits for
the issued reads to complete
@return a lower limit for the combined size in bytes of entries which
will be merged from ibuf trees to the pages read, 0 if ibuf is empty */
static ulint ibuf_contract(bool sync) {
  ulint n_pages;

  return (ibuf_merge_pages(&n_pages, sync));
}

/** Contract the change buffer by reading pages to the buffer pool.
@param[in]      full            If true, do a full contraction based
on PCT_IO(100). If false, the size of contract batch is determined
based on the current size of the change buffer.
@return a lower limit for the combined size in bytes of entries which
will be merged from ibuf trees to the pages read, 0 if ibuf is
empty */
ulint ibuf_merge_in_background(bool full) {
  ulint sum_bytes = 0;
  ulint sum_pages = 0;
  ulint n_pag2;
  ulint n_pages;

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
  if (srv_ibuf_disable_background_merge) {
    return (0);
  }
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

  if (full) {
    /* Caller has requested a full batch */
    n_pages = PCT_IO(100);
  } else {
    /* By default we do a batch of 5% of the io_capacity */
    n_pages = PCT_IO(5);

    mutex_enter(&ibuf_mutex);

    /* If the ibuf->size is more than half the max_size
    then we make more aggressive contraction.
    +1 is to avoid division by zero. */
    if (ibuf->size > ibuf->max_size / 2) {
      ulint diff = ibuf->size - ibuf->max_size / 2;
      /* limits to around 100% value, for shrinking max_size case */
      diff = std::min(diff, ibuf->max_size);
      n_pages += PCT_IO((diff * 100) / (ibuf->max_size + 1));
    }

    mutex_exit(&ibuf_mutex);
  }

  while (sum_pages < n_pages) {
    ulint n_bytes;

    n_bytes = ibuf_merge(&n_pag2, false);

    if (n_bytes == 0) {
      return (sum_bytes);
    }

    sum_bytes += n_bytes;
    sum_pages += n_pag2;

    srv_inc_activity_count(true);
  }

  return (sum_bytes);
}

/** Contract insert buffer trees after insert if they are too big. */
static inline void ibuf_contract_after_insert(
    ulint entry_size) /*!< in: size of a record which was inserted
                      into an ibuf tree */
{
  /* Perform dirty reads of ibuf->size and ibuf->max_size, to
  reduce ibuf_mutex contention. ibuf->max_size remains constant
  after ibuf_init_at_db_start(), but ibuf->size should be
  protected by ibuf_mutex. Given that ibuf->size fits in a
  machine word, this should be OK; at worst we are doing some
  excessive ibuf_contract() or occasionally skipping a
  ibuf_contract(). */
  auto size = ibuf->size;
  auto max_size = ibuf->max_size;

  if (size < max_size + IBUF_CONTRACT_ON_INSERT_NON_SYNC) {
    return;
  }

  auto sync = (size >= max_size + IBUF_CONTRACT_ON_INSERT_SYNC);

  /* Contract at least entry_size many bytes */
  ulint sum_sizes = 0;
  size = 1;

  do {
    size = ibuf_contract(sync);
    sum_sizes += size;
  } while (size > 0 && sum_sizes < entry_size);
}

/** Determine if an insert buffer record has been encountered already.
 @return true if a new record, false if possible duplicate */
static bool ibuf_get_volume_buffered_hash(
    const rec_t *rec,  /*!< in: ibuf record in post-4.1 format */
    const byte *types, /*!< in: fields */
    const byte *data,  /*!< in: start of user record data */
    ulint comp,        /*!< in: 0=ROW_FORMAT=REDUNDANT,
                       nonzero=ROW_FORMAT=COMPACT */
    ulint *hash,       /*!< in/out: hash array */
    ulint size)        /*!< in: number of elements in hash array */
{
  ulint len;
  ulint bitmask;

  len = ibuf_rec_get_size(
      rec, types, rec_get_n_fields_old_raw(rec) - IBUF_REC_FIELD_USER, comp);
  const auto hash_value = ut::hash_binary(data, len);

  hash += (hash_value / (CHAR_BIT * sizeof *hash)) % size;
  bitmask = static_cast<ulint>(1) << (hash_value % (CHAR_BIT * sizeof(*hash)));

  if (*hash & bitmask) {
    return false;
  }

  /* We have not seen this record yet.  Insert it. */
  *hash |= bitmask;

  return true;
}

/** Update the estimate of the number of records on a page, and
 get the space taken by merging the buffered record to the index page.
 @param[in] mtr mini-transaction owning rec
 @param[in] rec insert buffer record
 @param[in,out] hash hash array
 @param[in] size number of elements in hash array
 @param[in,out] n_recs estimated number of records on the page that rec points
 to
 @return size of index record in bytes + an upper limit of the space
 taken in the page directory */
static ulint ibuf_get_volume_buffered_count_func(IF_DEBUG(mtr_t *mtr, )
                                                     const rec_t *rec,
                                                 ulint *hash, ulint size,
                                                 lint *n_recs) {
  ulint len;
  ibuf_op_t ibuf_op;
  const byte *types;
  ulint n_fields;

  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(ibuf_inside(mtr));

  n_fields = rec_get_n_fields_old_raw(rec);
  ut_ad(n_fields > IBUF_REC_FIELD_USER);
  n_fields -= IBUF_REC_FIELD_USER;

  /* nullptr for index as it can't be clustered index */
  rec_get_nth_field_offs_old(nullptr, rec, 1, &len);
  /* This function is only invoked when buffering new
  operations.  All pre-4.1 records should have been merged
  when the database was started up. */
  ut_a(len == 1);

  if (rec_get_deleted_flag(rec, 0)) {
    /* This record has been merged already,
    but apparently the system crashed before
    the change was discarded from the buffer.
    Pretend that the record does not exist. */
    return (0);
  }

  types = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_METADATA, &len);

  switch (UNIV_EXPECT(len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE,
                      IBUF_REC_INFO_SIZE)) {
    default:
      ut_error;
    case 0:
      /* This ROW_TYPE=REDUNDANT record does not include an
      operation counter.  Exclude it from the *n_recs,
      because deletes cannot be buffered if there are
      old-style inserts buffered for the page. */

      len = ibuf_rec_get_size(rec, types, n_fields, 0);

      return (len + rec_get_converted_extra_size(len, n_fields, false) +
              page_dir_calc_reserved_space(1));
    case 1:
      /* This ROW_TYPE=COMPACT record does not include an
      operation counter.  Exclude it from the *n_recs,
      because deletes cannot be buffered if there are
      old-style inserts buffered for the page. */
      goto get_volume_comp;

    case IBUF_REC_INFO_SIZE:
      ibuf_op = (ibuf_op_t)types[IBUF_REC_OFFSET_TYPE];
      break;
  }

  switch (ibuf_op) {
    case IBUF_OP_INSERT:
      /* Inserts can be done by updating a delete-marked record.
      Because delete-mark and insert operations can be pointing to
      the same records, we must not count duplicates. */
    case IBUF_OP_DELETE_MARK:
      /* There must be a record to delete-mark.
      See if this record has been already buffered. */
      if (n_recs &&
          ibuf_get_volume_buffered_hash(
              rec, types + IBUF_REC_INFO_SIZE, types + len,
              types[IBUF_REC_OFFSET_FLAGS] & IBUF_REC_COMPACT, hash, size)) {
        (*n_recs)++;
      }

      if (ibuf_op == IBUF_OP_DELETE_MARK) {
        /* Setting the delete-mark flag does not
        affect the available space on the page. */
        return (0);
      }
      break;
    case IBUF_OP_DELETE:
      /* A record will be removed from the page. */
      if (n_recs) {
        (*n_recs)--;
      }
      /* While deleting a record actually frees up space,
      we have to play it safe and pretend that it takes no
      additional space (the record might not exist, etc.). */
      return (0);
    default:
      ut_error;
  }

  ut_ad(ibuf_op == IBUF_OP_INSERT);

get_volume_comp : {
  dtuple_t *entry;
  ulint volume;
  dict_index_t *dummy_index;
  mem_heap_t *heap = mem_heap_create(500, UT_LOCATION_HERE);

  entry = ibuf_build_entry_from_ibuf_rec_func(IF_DEBUG(mtr, ) rec, heap,
                                              &dummy_index);

  volume = rec_get_converted_size(dummy_index, entry);

  ibuf_dummy_index_free(dummy_index);
  mem_heap_free(heap);

  return (volume + page_dir_calc_reserved_space(1));
}
}

inline static ulint ibuf_get_volume_buffered_count(mtr_t *mtr [[maybe_unused]],
                                                   const rec_t *rec,
                                                   ulint *hash, ulint size,
                                                   lint *n_recs) {
  return ibuf_get_volume_buffered_count_func(IF_DEBUG(mtr, ) rec, hash, size,
                                             n_recs);
}

/** Gets an upper limit for the combined size of entries buffered in the
 insert buffer for a given page.
 @return upper limit for the volume of buffered inserts for the index
 page, in bytes; UNIV_PAGE_SIZE, if the entries for the index page span
 several pages in the insert buffer */
static ulint ibuf_get_volume_buffered(
    const btr_pcur_t *pcur, /*!< in: pcur positioned at a place in an
                            insert buffer tree where we would insert an
                            entry for the index page whose number is
                            page_no, latch mode has to be BTR_MODIFY_PREV
                            or BTR_MODIFY_TREE */
    space_id_t space,       /*!< in: space id */
    page_no_t page_no,      /*!< in: page number of an index page */
    lint *n_recs,           /*!< in/out: minimum number of records on the
                            page after the buffered changes have been
                            applied, or NULL to disable the counting */
    mtr_t *mtr)             /*!< in: mini-transaction of pcur */
{
  ulint volume;
  const rec_t *rec;
  const page_t *page;
  page_no_t prev_page_no;
  const page_t *prev_page;
  page_no_t next_page_no;
  const page_t *next_page;
  /* bitmap of buffered recs */
  ulint hash_bitmap[128 / sizeof(ulint)];

  ut_ad((pcur->m_latch_mode == BTR_MODIFY_PREV) ||
        (pcur->m_latch_mode == BTR_MODIFY_TREE));

  /* Count the volume of inserts earlier in the alphabetical order than
  pcur */

  volume = 0;

  if (n_recs) {
    memset(hash_bitmap, 0, sizeof hash_bitmap);
  }

  rec = pcur->get_rec();
  page = page_align(rec);
  ut_ad(page_validate(page, ibuf->index));

  if (page_rec_is_supremum(rec)) {
    rec = page_rec_get_prev_const(rec);
  }

  for (; !page_rec_is_infimum(rec); rec = page_rec_get_prev_const(rec)) {
    ut_ad(page_align(rec) == page);

    if (page_no != ibuf_rec_get_page_no(mtr, rec) ||
        space != ibuf_rec_get_space(mtr, rec)) {
      goto count_later;
    }

    volume += ibuf_get_volume_buffered_count(mtr, rec, hash_bitmap,
                                             UT_ARR_SIZE(hash_bitmap), n_recs);
  }

  /* Look at the previous page */

  prev_page_no = btr_page_get_prev(page, mtr);

  if (prev_page_no == FIL_NULL) {
    goto count_later;
  }

  {
    buf_block_t *block;

    block = buf_page_get(page_id_t(IBUF_SPACE_ID, prev_page_no), univ_page_size,
                         RW_X_LATCH, UT_LOCATION_HERE, mtr);

    buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);

    prev_page = buf_block_get_frame(block);
    ut_ad(page_validate(prev_page, ibuf->index));
  }

#ifdef UNIV_BTR_DEBUG
  ut_a(btr_page_get_next(prev_page, mtr) == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

  rec = page_get_supremum_rec(prev_page);
  rec = page_rec_get_prev_const(rec);

  for (;; rec = page_rec_get_prev_const(rec)) {
    ut_ad(page_align(rec) == prev_page);

    if (page_rec_is_infimum(rec)) {
      /* We cannot go to yet a previous page, because we
      do not have the x-latch on it, and cannot acquire one
      because of the latching order: we have to give up */

      return (UNIV_PAGE_SIZE);
    }

    if (page_no != ibuf_rec_get_page_no(mtr, rec) ||
        space != ibuf_rec_get_space(mtr, rec)) {
      goto count_later;
    }

    volume += ibuf_get_volume_buffered_count(mtr, rec, hash_bitmap,
                                             UT_ARR_SIZE(hash_bitmap), n_recs);
  }

count_later:
  rec = pcur->get_rec();

  if (!page_rec_is_supremum(rec)) {
    rec = page_rec_get_next_const(rec);
  }

  for (; !page_rec_is_supremum(rec); rec = page_rec_get_next_const(rec)) {
    if (page_no != ibuf_rec_get_page_no(mtr, rec) ||
        space != ibuf_rec_get_space(mtr, rec)) {
      return (volume);
    }

    volume += ibuf_get_volume_buffered_count(mtr, rec, hash_bitmap,
                                             UT_ARR_SIZE(hash_bitmap), n_recs);
  }

  /* Look at the next page */

  next_page_no = btr_page_get_next(page, mtr);

  if (next_page_no == FIL_NULL) {
    return (volume);
  }

  {
    buf_block_t *block;

    block = buf_page_get(page_id_t(IBUF_SPACE_ID, next_page_no), univ_page_size,
                         RW_X_LATCH, UT_LOCATION_HERE, mtr);

    buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);

    next_page = buf_block_get_frame(block);
    ut_ad(page_validate(next_page, ibuf->index));
  }

#ifdef UNIV_BTR_DEBUG
  ut_a(btr_page_get_prev(next_page, mtr) == page_get_page_no(page));
#endif /* UNIV_BTR_DEBUG */

  rec = page_get_infimum_rec(next_page);
  rec = page_rec_get_next_const(rec);

  for (;; rec = page_rec_get_next_const(rec)) {
    ut_ad(page_align(rec) == next_page);

    if (page_rec_is_supremum(rec)) {
      /* We give up */

      return (UNIV_PAGE_SIZE);
    }

    if (page_no != ibuf_rec_get_page_no(mtr, rec) ||
        space != ibuf_rec_get_space(mtr, rec)) {
      return (volume);
    }

    volume += ibuf_get_volume_buffered_count(mtr, rec, hash_bitmap,
                                             UT_ARR_SIZE(hash_bitmap), n_recs);
  }
}

/** Reads the biggest tablespace id from the high end of the insert buffer
 tree and updates the counter in fil_system. */
void ibuf_update_max_tablespace_id(void) {
  space_id_t max_space_id;
  const rec_t *rec;
  const byte *field;
  ulint len;
  btr_pcur_t pcur;
  mtr_t mtr;

  ut_a(!dict_table_is_comp(ibuf->index->table));

  ibuf_mtr_start(&mtr);

  pcur.open_at_side(false, ibuf->index, BTR_SEARCH_LEAF, true, 0, &mtr);

  ut_ad(page_validate(pcur.get_page(), ibuf->index));

  pcur.move_to_prev(&mtr);

  if (pcur.is_before_first_on_page()) {
    /* The tree is empty */

    max_space_id = 0;
  } else {
    rec = pcur.get_rec();

    field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_SPACE, &len);

    ut_a(len == 4);

    max_space_id = mach_read_from_4(field);
  }

  ibuf_mtr_commit(&mtr);

  /* printf("Maximum space id in insert buffer %lu\n", max_space_id); */

  fil_set_max_space_id_if_bigger(max_space_id);
}

/** Helper function for ibuf_get_entry_counter_func. Checks if rec is for
 (space, page_no), and if so, reads counter value from it and returns
 that + 1.
 @param[in] mtr mini-transaction of rec
 @param[in] rec insert buffer record
 @param[in] space space id
 @param[in] page_no page number
 @retval ULINT_UNDEFINED if the record does not contain any counter
 @retval 0 if the record is not for (space, page_no)
 @retval 1 + previous counter value, otherwise */
static ulint ibuf_get_entry_counter_low_func(IF_DEBUG(mtr_t *mtr, )
                                                 const rec_t *rec,
                                             space_id_t space,
                                             page_no_t page_no) {
  ulint counter;
  const byte *field;
  ulint len;

  ut_ad(ibuf_inside(mtr));
  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX) ||
        mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_S_FIX));
  ut_ad(rec_get_n_fields_old_raw(rec) > 2);

  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_MARKER, &len);

  ut_a(len == 1);

  /* Check the tablespace identifier. */
  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_SPACE, &len);

  ut_a(len == 4);

  if (mach_read_from_4(field) != space) {
    return (0);
  }

  /* Check the page offset. */
  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_PAGE, &len);
  ut_a(len == 4);

  if (mach_read_from_4(field) != page_no) {
    return (0);
  }

  /* Check if the record contains a counter field. */
  field = rec_get_nth_field_old(nullptr, rec, IBUF_REC_FIELD_METADATA, &len);

  switch (len % DATA_NEW_ORDER_NULL_TYPE_BUF_SIZE) {
    default:
      ut_error;
    case 0: /* ROW_FORMAT=REDUNDANT */
    case 1: /* ROW_FORMAT=COMPACT */
      return (ULINT_UNDEFINED);

    case IBUF_REC_INFO_SIZE:
      counter = mach_read_from_2(field + IBUF_REC_OFFSET_COUNTER);
      ut_a(counter < 0xFFFF);
      return (counter + 1);
  }
}

/** Calculate the counter field for an entry based on the current
 last record in ibuf for (space, page_no).
 @return the counter field, or ULINT_UNDEFINED
 if we should abort this insertion to ibuf
 @param[in] space space id of entry
 @param[in] page_no page number of entry
 @param[in] rec the record preceding the insertion point
 @param mtr mini-transaction
 @param[in] only_leaf true if this is the only leaf page that can contain
 entries for (space,page_no), that is, there was no exact match for
 (space,page_no) in the node pointer */
static ulint ibuf_get_entry_counter_func(
    space_id_t space, page_no_t page_no, const rec_t *rec,
    IF_DEBUG(mtr_t *mtr, ) bool only_leaf) {
  ut_ad(ibuf_inside(mtr));
  ut_ad(mtr_memo_contains_page(mtr, rec, MTR_MEMO_PAGE_X_FIX));
  ut_ad(page_validate(page_align(rec), ibuf->index));

  if (page_rec_is_supremum(rec)) {
    /* This is just for safety. The record should be a
    page infimum or a user record. */
    ut_d(ut_error);
    ut_o(return (ULINT_UNDEFINED));
  } else if (!page_rec_is_infimum(rec)) {
    return (
        ibuf_get_entry_counter_low_func(IF_DEBUG(mtr, ) rec, space, page_no));
  } else if (only_leaf || fil_page_get_prev(page_align(rec)) == FIL_NULL) {
    /* The parent node pointer did not contain the
    searched for (space, page_no), which means that the
    search ended on the correct page regardless of the
    counter value, and since we're at the infimum record,
    there are no existing records. */
    return (0);
  } else {
    /* We used to read the previous page here. It would
    break the latching order, because the caller has
    buffer-fixed an insert buffer bitmap page. */
    return (ULINT_UNDEFINED);
  }
}

inline ulint ibuf_get_entry_counter(space_id_t space, page_no_t page_no,
                                    const rec_t *rec,
                                    mtr_t *mtr [[maybe_unused]],
                                    bool exact_leaf) {
  return ibuf_get_entry_counter_func(space, page_no, rec,
                                     IF_DEBUG(mtr, ) exact_leaf);
}

/**
 * 将操作缓冲到插入/删除缓冲区，而不是直接执行到磁盘页（如果可能的话）
 * Buffer an operation in the insert/delete buffer, instead of doing it
 * directly to the disk page, if this is possible.
 * 
 * @param[in] mode BTR_MODIFY_PREV或BTR_MODIFY_TREE
 * @param[in] op 操作类型
 * @param[in] no_counter true=使用5.0.3格式；false=允许删除缓冲
 * @param[in] entry 要插入的索引条目
 * @param[in] entry_size rec_get_converted_size(index, entry)
 * @param[in,out] index 要插入的索引；不能是唯一或聚集索引
 * @param[in] page_id 要插入的页ID
 * @param[in] page_size 页大小
 * @param[in,out] thr 查询线程
 * @return DB_SUCCESS, DB_STRONG_FAIL或其他错误
 */
[[nodiscard]] static dberr_t ibuf_insert_low(
  ulint mode, ibuf_op_t op, bool no_counter, const dtuple_t *entry,
  ulint entry_size, dict_index_t *index, const page_id_t &page_id,
  const page_size_t &page_size, que_thr_t *thr) {
// 大记录指针，用于处理大记录情况
big_rec_t *dummy_big_rec;
// B树游标
btr_pcur_t pcur;
// B树游标指针
btr_cur_t *cursor;
// 要插入到插入缓冲区的条目
dtuple_t *ibuf_entry;
// 偏移量堆，初始为nullptr
mem_heap_t *offsets_heap = nullptr;
// 内存堆
mem_heap_t *heap;
// 记录偏移量数组
ulint *offsets = nullptr;
// 已缓冲的大小
ulint buffered;
// 最小记录数
lint min_n_recs;
// 插入的记录指针
rec_t *ins_rec;
// 旧的位图位值
bool old_bit_value;
// 位图页指针
page_t *bitmap_page;
// 块指针
buf_block_t *block;
// 根页指针
page_t *root;
// 错误码
dberr_t err;
// 空间ID数组，用于合并
space_id_t space_ids[IBUF_MAX_N_PAGES_MERGED];
// 页号数组，用于合并
page_no_t page_nos[IBUF_MAX_N_PAGES_MERGED];
// 存储的数量
ulint n_stored = 0;
// 主迷你事务
mtr_t mtr;
// 位图迷你事务
mtr_t bitmap_mtr;

// 断言：索引不能是聚集索引
ut_a(!index->is_clustered());
// 断言：索引不能是空间索引
ut_ad(!dict_index_is_spatial(index));
// 断言：检查条目类型是否正确
ut_ad(dtuple_check_typed(entry));
// 断言：如果是no_counter模式，操作必须是INSERT
ut_ad(!no_counter || op == IBUF_OP_INSERT);
// 断言：操作类型必须小于IBUF_OP_COUNT
ut_a(op < IBUF_OP_COUNT);

// 是否执行合并的标志
auto do_merge = false;

/* 对ibuf->size和ibuf->max_size执行脏读，
   以减少ibuf_mutex争用。考虑到ibuf->max_size和
   ibuf->size适合机器字大小，这应该是可以的；最坏情况下
   我们会执行一些多余的ibuf_contract()或偶尔跳过ibuf_contract() */
/* Perform dirty reads of ibuf->size and ibuf->max_size, to
reduce ibuf_mutex contention. Given that ibuf->max_size and
ibuf->size fit in a machine word, this should be OK; at worst
we are doing some excessive ibuf_contract() or occasionally
skipping an ibuf_contract(). */
if (ibuf->max_size == 0) {
  return (DB_STRONG_FAIL);
}

// 如果插入缓冲区太大，则收缩它但不尝试插入
if (ibuf->size >= ibuf->max_size + IBUF_CONTRACT_DO_NOT_INSERT) {
  /* 插入缓冲区现在太大，收缩它但不尝试插入 */
  /* Insert buffer is now too big, contract it but do not try
  to insert */

#ifdef UNIV_IBUF_DEBUG
  fputs("Ibuf too big\n", stderr);
#endif
  // 执行收缩操作
  ibuf_contract(true);

  return (DB_STRONG_FAIL);
}

// 创建内存堆
heap = mem_heap_create(1024, UT_LOCATION_HERE);

/* 构建包含空间ID和页号作为前几个字段的条目，
   以及其他字段的类型信息，该条目将被插入到插入缓冲区。
   使用计数器值0xFFFF我们可以找到(space, page_no)的最后一条记录，
   从中我们可以读取计数器值N并在我们插入的记录中使用N+1 */
/* Build the entry which contains the space id and the page number
as the first fields and the type information for other fields, and
which will be inserted to the insert buffer. Using a counter value
of 0xFFFF we find the last record for (space, page_no), from which
we can then read the counter value N and use N + 1 in the record we
insert. (We patch the ibuf_entry's counter field to the correct
value just before actually inserting the entry.) */
ibuf_entry =
    ibuf_entry_build(op, index, entry, page_id.space(), page_id.page_no(),
                     no_counter ? ULINT_UNDEFINED : 0xFFFF, heap);

/* 打开插入缓冲区树的游标，以计算是否可以在不超出页空闲空间限制的情况下
   将新条目添加到其中 */
/* Open a cursor to the insert buffer tree to calculate if we can add
the new entry to it without exceeding the free space limit for the
page. */

// 如果是BTR_MODIFY_TREE模式，需要获取悲观插入互斥锁
if (BTR_LATCH_MODE_WITHOUT_INTENTION(mode) == BTR_MODIFY_TREE) {
  for (;;) {
    // 获取悲观插入互斥锁
    mutex_enter(&ibuf_pessimistic_insert_mutex);
    // 获取插入缓冲区互斥锁
    mutex_enter(&ibuf_mutex);

    // 检查是否有足够的空闲空间
    if (UNIV_LIKELY(ibuf_data_enough_free_for_insert())) {
      break;
    }

    // 释放锁
    mutex_exit(&ibuf_mutex);
    mutex_exit(&ibuf_pessimistic_insert_mutex);

    // 如果空间不足，尝试添加空闲页
    if (!ibuf_add_free_page()) {
      // 释放内存堆
      mem_heap_free(heap);
      return (DB_STRONG_FAIL);
    }
  }
}

// 开始主迷你事务
ibuf_mtr_start(&mtr);

// 打开游标定位到插入位置
pcur.open(ibuf->index, 0, ibuf_entry, PAGE_CUR_LE, mode, &mtr,
          UT_LOCATION_HERE);
// 验证页有效性
ut_ad(page_validate(pcur.get_page(), ibuf->index));

/* 找出同一索引页已缓冲的插入量 */
/* Find out the volume of already buffered inserts for the same index
page */
min_n_recs = 0;
// 获取已缓冲的量
buffered = ibuf_get_volume_buffered(
    &pcur, page_id.space(), page_id.page_no(),
    op == IBUF_OP_DELETE ? &min_n_recs : nullptr, &mtr);

// 如果是删除操作且记录数小于2或页面已被读取到缓冲池
if (op == IBUF_OP_DELETE &&
    (min_n_recs < 2 || buf_pool_watch_occurred(page_id))) {
  /* 删除记录后页面可能变为空，或者页面已被读入缓冲池。
      拒绝缓冲该操作 */
  /* The page could become empty after the record is
  deleted, or the page has been read in to the buffer
  pool.  Refuse to buffer the operation. */

  /* 对于IBUF_OP_DELETE需要缓冲池监视，因为锁顺序考虑。
      只有在锁定了包含页面缓冲更改的插入缓冲区B树页之后，
      才能检查buf_pool_watch_occurred()。除非之前已经为页面缓冲了
      一些IBUF_OP_INSERT或IBUF_OP_DELETE_MARK，否则我们从不缓冲
      IBUF_OP_DELETE。因为页面的缓冲操作，mtr持有的插入缓冲区B树页
      锁将保证在mtr_commit(&mtr)之前不会合并用户页面的更改。在缓冲
      IBUF_OP_DELETE之前，我们不得mtr_commit(&mtr) */
  /* The buffer pool watch is needed for IBUF_OP_DELETE
  because of latching order considerations.  We can
  check buf_pool_watch_occurred() only after latching
  the insert buffer B-tree pages that contain buffered
  changes for the page.  We never buffer IBUF_OP_DELETE,
  unless some IBUF_OP_INSERT or IBUF_OP_DELETE_MARK have
  been previously buffered for the page.  Because there
  are buffered operations for the page, the insert
  buffer B-tree page latches held by mtr will guarantee
  that no changes for the user page will be merged
  before mtr_commit(&mtr).  We must not mtr_commit(&mtr)
  until after the IBUF_OP_DELETE has been buffered. */

// 失败退出标签
fail_exit:
  // 如果是BTR_MODIFY_TREE模式，释放互斥锁
  if (BTR_LATCH_MODE_WITHOUT_INTENTION(mode) == BTR_MODIFY_TREE) {
    mutex_exit(&ibuf_mutex);
    mutex_exit(&ibuf_pessimistic_insert_mutex);
  }

  // 设置错误码
  err = DB_STRONG_FAIL;
  // 跳转到函数退出处理
  goto func_exit;
}

/* 在此点之后，页面可能仍被加载到缓冲池，但我们不必关心，
   因为我们持有包含(space, page_no)缓冲更改的插入缓冲区叶子页的锁。
   如果页面进入缓冲池，(space, page_no)的buf_page_io_complete()
   将必须获取相同的插入缓冲区叶子页锁，在我们缓冲IBUF_OP_DELETE并
   执行mtr_commit(&mtr)释放锁之前，它无法做到这一点 */
/* After this point, the page could still be loaded to the
buffer pool, but we do not have to care about it, since we are
holding a latch on the insert buffer leaf page that contains
buffered changes for (space, page_no).  If the page enters the
buffer pool, buf_page_io_complete() for (space, page_no) will
have to acquire a latch on the same insert buffer leaf page,
which it cannot do until we have buffered the IBUF_OP_DELETE
and done mtr_commit(&mtr) to release the latch. */

#ifdef UNIV_IBUF_COUNT_DEBUG
ut_a((buffered == 0) || ibuf_count_get(page_id));
#endif
// 开始位图迷你事务
ibuf_mtr_start(&bitmap_mtr);

// 获取位图页
bitmap_page = ibuf_bitmap_get_map_page(page_id, page_size, UT_LOCATION_HERE,
                                       &bitmap_mtr);

/* 我们检查索引页是否适合缓冲条目 */
/* We check if the index page is suitable for buffered entries */

// 如果页面在缓冲池中或存在显式锁
if (buf_page_peek(page_id) || lock_rec_expl_exist_on_page(page_id)) {
  // 提交位图事务
  ibuf_mtr_commit(&bitmap_mtr);
  // 跳转到失败处理
  goto fail_exit;
}

// 如果是插入操作
if (op == IBUF_OP_INSERT) {
  // 获取位图中的空闲空间信息
  ulint bits = ibuf_bitmap_page_get_bits(bitmap_page, page_id, page_size,
                                         IBUF_BITMAP_FREE, &bitmap_mtr);

  // 检查是否有足够的空间
  if (buffered + entry_size + page_dir_calc_reserved_space(1) >
      ibuf_index_page_calc_free_from_bits(page_size, bits)) {
    /* 释放位图页锁 */
    /* Release the bitmap page latch early. */
    ibuf_mtr_commit(&bitmap_mtr);

    /* 它可能不适合 */
    /* It may not fit */
    do_merge = true;

    // 获取需要合并的页面列表
    ibuf_get_merge_page_nos(false, pcur.get_rec(), &mtr, space_ids, page_nos,
                            &n_stored);

    // 跳转到失败处理
    goto fail_exit;
  }
}

// 如果不是no_counter模式
if (!no_counter) {
  /* 为要插入的条目修补正确的计数器值。
      这可以改变插入位置，可能导致在某些情况下需要中止 */
  /* Patch correct counter value to the entry to
  insert. This can change the insert position, which can
  result in the need to abort in some cases. */
  // 获取计数器值
  ulint counter = ibuf_get_entry_counter(
      page_id.space(), page_id.page_no(), pcur.get_rec(), &mtr,
      pcur.get_btr_cur()->low_match < IBUF_REC_FIELD_METADATA);
  // 获取元数据字段
  dfield_t *field;

  // 如果计数器无效
  if (counter == ULINT_UNDEFINED) {
    // 提交位图事务
    ibuf_mtr_commit(&bitmap_mtr);
    // 跳转到失败处理
    goto fail_exit;
  }

  // 获取元数据字段并写入计数器值
  field = dtuple_get_nth_field(ibuf_entry, IBUF_REC_FIELD_METADATA);
  mach_write_to_2((byte *)dfield_get_data(field) + IBUF_REC_OFFSET_COUNTER,
                  counter);
}

/* 设置位图位，表示插入缓冲区包含此索引页的缓冲条目，
   如果该位尚未设置 */
/* Set the bitmap bit denoting that the insert buffer contains
buffered entries for this index page, if the bit is not set yet */

// 获取旧的位图位值
old_bit_value = ibuf_bitmap_page_get_bits(bitmap_page, page_id, page_size,
                                          IBUF_BITMAP_BUFFERED, &bitmap_mtr);

// 如果位图位未设置，则设置它
if (!old_bit_value) {
  ibuf_bitmap_page_set_bits(bitmap_page, page_id, page_size,
                            IBUF_BITMAP_BUFFERED, true, &bitmap_mtr);
}

// 提交位图事务
ibuf_mtr_commit(&bitmap_mtr);

// 获取B树游标
cursor = pcur.get_btr_cur();

// 如果是BTR_MODIFY_PREV模式
if (mode == BTR_MODIFY_PREV) {
  // 尝试乐观插入
  err = btr_cur_optimistic_insert(BTR_NO_LOCKING_FLAG, cursor, &offsets,
                                  &offsets_heap, ibuf_entry, &ins_rec,
                                  &dummy_big_rec, thr, &mtr);
  // 获取块
  block = btr_cur_get_block(cursor);
  // 断言：块必须在系统表空间
  ut_ad(block->page.id.space() == IBUF_SPACE_ID);

  /* 如果是根页，更新ibuf->empty */
  /* If this is the root page, update ibuf->empty. */
  if (block->page.id.page_no() == FSP_IBUF_TREE_ROOT_PAGE_NO) {
    const page_t *root = buf_block_get_frame(block);

    ut_ad(page_get_space_id(root) == IBUF_SPACE_ID);
    ut_ad(page_get_page_no(root) == FSP_IBUF_TREE_ROOT_PAGE_NO);

    ibuf->empty = page_is_empty(root);
  }
} else {
  // 断言：必须是BTR_MODIFY_TREE模式
  ut_ad(BTR_LATCH_MODE_WITHOUT_INTENTION(mode) == BTR_MODIFY_TREE);

  /* 在插入之前获取根页的sx锁，因为悲观插入会释放树x锁，
      这将导致在此之后对根的sx锁定破坏锁定顺序 */
  /* We acquire an sx-latch to the root page before the insert,
  because a pessimistic insert releases the tree x-latch,
  which would cause the sx-latching of the root after that to
  break the latching order. */

  // 获取根页
  root = ibuf_tree_root_get(&mtr);

  // 尝试乐观插入
  err = btr_cur_optimistic_insert(BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG,
                                  cursor, &offsets, &offsets_heap, ibuf_entry,
                                  &ins_rec, &dummy_big_rec, thr, &mtr);

  // 如果乐观插入失败，尝试悲观插入
    if (err == DB_FAIL) {
      // 悲观插入失败后，尝试悲观插入
      err = btr_cur_pessimistic_insert(
          BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG, cursor, &offsets,
          &offsets_heap, ibuf_entry, &ins_rec, &dummy_big_rec, thr, &mtr);
    }

    // 释放悲观插入互斥锁
    mutex_exit(&ibuf_pessimistic_insert_mutex);
    // 更新ibuf大小信息
    ibuf_size_update(root);
    // 释放ibuf互斥锁
    mutex_exit(&ibuf_mutex);
    // 更新ibuf空状态
    ibuf->empty = page_is_empty(root);

    // 获取当前块
    block = btr_cur_get_block(cursor);
    // 断言：块必须在系统表空间
    ut_ad(block->page.id.space() == IBUF_SPACE_ID);
  }

  // 如果有偏移量堆，释放它
  if (offsets_heap) {
    mem_heap_free(offsets_heap);
  }

  // 如果插入成功且不是删除操作
  if (err == DB_SUCCESS && op != IBUF_OP_DELETE) {
    /* 更新页面最大事务ID字段 */
    /* Update the page max trx id field */
    page_update_max_trx_id(block, nullptr, thr_get_trx(thr)->id, &mtr);
  }

func_exit:
#ifdef UNIV_IBUF_COUNT_DEBUG
  // 调试模式下更新ibuf计数
  if (err == DB_SUCCESS) {
    ib::info(ER_IB_MSG_607)
        << "Incrementing ibuf count of page " << page_id << " from "
        << ibuf_count_get(space, page_no) << " by 1";

    ibuf_count_set(page_id, ibuf_count_get(page_id) + 1);
  }
#endif

  // 提交迷你事务
  ibuf_mtr_commit(&mtr);
  // 关闭游标
  pcur.close();

  // 释放内存堆
  mem_heap_free(heap);

  // 如果是BTR_MODIFY_TREE模式且插入成功，执行收缩操作
  if (err == DB_SUCCESS &&
      BTR_LATCH_MODE_WITHOUT_INTENTION(mode) == BTR_MODIFY_TREE) {
    ibuf_contract_after_insert(entry_size);
  }

  // 如果需要合并页面
  if (do_merge) {
#ifdef UNIV_IBUF_DEBUG
    // 调试断言：存储的页面数不超过最大值
    ut_a(n_stored <= IBUF_MAX_N_PAGES_MERGED);
#endif
    // 读取并合并页面
    buf_read_ibuf_merge_pages(false, space_ids, page_nos, n_stored);
  }

  // 返回错误码
  return (err);
}

/** 在插入/删除缓冲区中缓冲操作，而不是直接执行到磁盘页（如果可能的话）。
    如果索引是聚集或唯一的，则不执行此操作。
    Buffer an operation in the insert/delete buffer, instead of doing it
    directly to the disk page, if this is possible. Does not do it if the index
    is clustered or unique. */
bool ibuf_insert(ibuf_op_t op,          /*!< in: 操作类型 */
                 const dtuple_t *entry, /*!< in: 要插入的索引条目 */
                 dict_index_t *index,   /*!< in/out: 要插入的索引 */
                 const page_id_t &page_id, /*!< in: 要插入的页ID */
                 const page_size_t &page_size, /*!< in: 页大小 */
                 que_thr_t *thr)       /*!< in/out: 查询线程 */
{
  dberr_t err;
  ulint entry_size;
  /* 读取可设置的全局变量ibuf_use，确保在此函数中一致 */
  assert(innodb_change_buffering <= IBUF_USE_ALL);
  ibuf_use_t use = static_cast<ibuf_use_t>(innodb_change_buffering);

  DBUG_TRACE;

  DBUG_PRINT("ibuf", ("op: %d, space: " UINT32PF ", page_no: " UINT32PF, op,
                      page_id.space(), page_id.page_no()));

  // 断言：检查条目类型是否正确
  ut_ad(dtuple_check_typed(entry));
  // 断言：不能是系统临时表空间
  ut_ad(!fsp_is_system_temporary(page_id.space()));

  // 断言：索引不能是聚集索引
  ut_a(!index->is_clustered());

  // 根据使用模式决定是否使用计数器
  auto no_counter = use <= IBUF_USE_INSERT;

  // 根据操作类型和使用模式决定是否缓冲
  switch (op) {
    case IBUF_OP_INSERT:
      switch (use) {
        case IBUF_USE_NONE:
        case IBUF_USE_DELETE:
        case IBUF_USE_DELETE_MARK:
          return false;
        case IBUF_USE_INSERT:
        case IBUF_USE_INSERT_DELETE_MARK:
        case IBUF_USE_ALL:
          goto check_watch;
      }
      break;
    case IBUF_OP_DELETE_MARK:
      switch (use) {
        case IBUF_USE_NONE:
        case IBUF_USE_INSERT:
          return false;
        case IBUF_USE_DELETE_MARK:
        case IBUF_USE_DELETE:
        case IBUF_USE_INSERT_DELETE_MARK:
        case IBUF_USE_ALL:
          ut_ad(!no_counter);
          goto check_watch;
      }
      break;
    case IBUF_OP_DELETE:
      switch (use) {
        case IBUF_USE_NONE:
        case IBUF_USE_INSERT:
        case IBUF_USE_INSERT_DELETE_MARK:
          return false;
        case IBUF_USE_DELETE_MARK:
        case IBUF_USE_DELETE:
        case IBUF_USE_ALL:
          ut_ad(!no_counter);
          goto skip_watch;
      }
      break;
    case IBUF_OP_COUNT:
      break;
  }

  /* 未知操作或使用模式 */
  ut_error;

check_watch:
  /* If a thread attempts to buffer an insert on a page while a
  purge is in progress on the same page, the purge must not be
  buffered, because it could remove a record that was
  re-inserted later.  For simplicity, we block the buffering of
  all operations on a page that has a purge pending.

  We do not check this in the IBUF_OP_DELETE case, because that
  would always trigger the buffer pool watch during purge and
  thus prevent the buffering of delete operations.  We assume
  that the issuer of IBUF_OP_DELETE has called
  buf_pool_watch_set(space, page_no). */
  /* 如果一个线程尝试在页面有purge操作时缓冲插入，
     则purge不能被缓冲，因为它可能会删除后来重新插入的记录。
     为简单起见，我们阻止对有purge挂起的页面进行所有操作的缓冲。

     在IBUF_OP_DELETE情况下我们不检查这个，因为那会总是触发
     缓冲池监视，从而阻止删除操作的缓冲。我们假设
     IBUF_OP_DELETE的调用者已经调用了buf_pool_watch_set(space, page_no)。 */

  {
    // 获取缓冲池实例
    buf_pool_t *buf_pool = buf_pool_get(page_id);
    // 获取页面（包括监视页面）
    buf_page_t *bpage = buf_page_get_also_watch(buf_pool, page_id);

    if (bpage != nullptr) {
      /* A buffer pool watch has been set or the
      page has been read into the buffer pool.
      Do not buffer the request.  If a purge operation
      is being buffered, have this request executed
      directly on the page in the buffer pool after the
      buffered entries for this page have been merged. */      
      /* 已设置缓冲池监视或页面已读入缓冲池。
         不缓冲请求。如果有purge操作正在缓冲，
         在合并此页面的缓冲条目后，直接在缓冲池中的页面上执行此请求。 */
      return false;
    }
  }

skip_watch:
  // 计算转换后的记录大小
  entry_size = rec_get_converted_size(index, entry);

  // 如果记录大小超过空页一半空间，不缓冲
  if (entry_size >=
      page_get_free_space_of_empty(dict_table_is_comp(index->table)) / 2) {
    return false;
  }

  // 先尝试乐观插入
  err = ibuf_insert_low(BTR_MODIFY_PREV, op, no_counter, entry, entry_size,
                        index, page_id, page_size, thr);
  // 如果乐观插入失败，尝试悲观插入
  if (err == DB_FAIL) {
    err =
        ibuf_insert_low(BTR_MODIFY_TREE | BTR_LATCH_FOR_INSERT, op, no_counter,
                        entry, entry_size, index, page_id, page_size, thr);
  }

  // 返回操作结果
  if (err == DB_SUCCESS) {
    return true;
  } else {
    ut_a(err == DB_STRONG_FAIL || err == DB_TOO_BIG_RECORD);
    return false;
  }
}

/** During merge, inserts to an index page a secondary index entry extracted
 from the insert buffer.
 @return        newly inserted record */
static rec_t *ibuf_insert_to_index_page_low(
    const dtuple_t *entry, /*!< in: buffered entry to insert */
    buf_block_t *block,    /*!< in/out: index page where the buffered
                           entry should be placed */
    dict_index_t *index,   /*!< in: record descriptor */
    ulint **offsets,       /*!< out: offsets on *rec */
    mem_heap_t *heap,      /*!< in/out: memory heap */
    mtr_t *mtr,            /*!< in/out: mtr */
    page_cur_t *page_cur)  /*!< in/out: cursor positioned on the record
                          after which to insert the buffered entry */
{
  const page_t *page;
  const page_t *bitmap_page;
  ulint old_bits;
  rec_t *rec;
  DBUG_TRACE;

  rec = page_cur_tuple_insert(page_cur, entry, index, offsets, &heap, mtr);
  if (rec != nullptr) {
    return rec;
  }

  /* Page reorganization or recompression should already have
  been attempted by page_cur_tuple_insert(). Besides, per
  ibuf_index_page_calc_free_zip() the page should not have been
  recompressed or reorganized. */
  ut_ad(!buf_block_get_page_zip(block));

  /* If the record did not fit, reorganize */

  btr_page_reorganize(page_cur, index, mtr);

  /* This time the record must fit */

  rec = page_cur_tuple_insert(page_cur, entry, index, offsets, &heap, mtr);
  if (rec != nullptr) {
    return rec;
  }

  page = buf_block_get_frame(block);

  ib::error(ER_IB_MSG_608) << "Insert buffer insert fails; page free "
                           << page_get_max_insert_size(page, 1)
                           << ", dtuple size "
                           << rec_get_converted_size(index, entry);

  fputs("InnoDB: Cannot insert index record ", stderr);
  dtuple_print(stderr, entry);
  fputs(
      "\nInnoDB: The table where this index record belongs\n"
      "InnoDB: is now probably corrupt. Please run CHECK TABLE on\n"
      "InnoDB: that table.\n",
      stderr);

  bitmap_page = ibuf_bitmap_get_map_page(block->page.id, block->page.size,
                                         UT_LOCATION_HERE, mtr);
  old_bits = ibuf_bitmap_page_get_bits(bitmap_page, block->page.id,
                                       block->page.size, IBUF_BITMAP_FREE, mtr);

  ib::error(ER_IB_MSG_609) << "page " << block->page.id << ", size "
                           << block->page.size.physical() << ", bitmap bits "
                           << old_bits;

  ib::error(ER_IB_MSG_610) << BUG_REPORT_MSG;

  ut_d(ut_error);
  ut_o(return nullptr);
}

/************************************************************************
/************************************************************************
During merge, inserts to an index page a secondary index entry extracted
from the insert buffer. */
/* 在合并过程中，将插入缓冲区中的二级索引条目插入到索引页 */
static void ibuf_insert_to_index_page(
    const dtuple_t *entry, /*!< in: buffered entry to insert */
    /* 输入参数：要插入的缓冲条目 */
    buf_block_t *block,    /*!< in/out: index page where the buffered entry
                           should be placed */
    /* 输入输出参数：要插入缓冲条目的索引页 */
    dict_index_t *index,   /*!< in: record descriptor */
    /* 输入参数：记录描述符 */
    mtr_t *mtr)            /*!< in: mtr */
    /* 输入参数：迷你事务 */
{
  // 页面游标，用于定位插入位置
  page_cur_t page_cur;
  // 最低匹配字段数
  ulint low_match;
  // 获取页面帧指针
  page_t *page = buf_block_get_frame(block);
  // 记录指针
  rec_t *rec;
  // 记录偏移量数组
  ulint *offsets;
  // 内存堆，用于临时分配
  mem_heap_t *heap;

  // 调试跟踪
  DBUG_TRACE;

  // 打印调试信息：页面空间ID和页号
  DBUG_PRINT("ibuf", ("page " UINT32PF ":" UINT32PF, block->page.id.space(),
                      block->page.id.page_no()));

  // 断言：索引不能是online DDL索引（这是一个ibuf_dummy索引）
  ut_ad(!dict_index_is_online_ddl(index));  // this is an ibuf_dummy index
  // 断言：必须在ibuf操作中
  ut_ad(ibuf_inside(mtr));
  // 断言：检查条目类型是否正确
  ut_ad(dtuple_check_typed(entry));
  /* A change buffer merge must occur before users are granted
  any access to the page. No adaptive hash index entries may
  point to a freshly read page. */
  /* 变更缓冲区合并必须在用户获得页面访问权限之前完成。
     自适应哈希索引条目不能指向刚读取的页面 */
  // 断言：块不能有自适应哈希索引
  ut_ad(!block->ahi.index);
  // 断言：自适应哈希索引必须为空
  block->ahi.assert_empty();

  // 检查表的压缩标志是否与页面压缩标志匹配
  if (UNIV_UNLIKELY(dict_table_is_comp(index->table) != page_is_comp(page))) {
    ib::warn(ER_IB_MSG_611)
        << "Trying to insert a record from the insert"
           " buffer to an index page but the 'compact' flag does"
           " not match!";
    /* 尝试将记录从插入缓冲区插入到索引页，但'compact'标志不匹配 */
    goto dump;
  }

  // 获取页面的第一条用户记录
  rec = page_rec_get_next(page_get_infimum_rec(page));

  // 检查页面是否为空（只有supremum记录）
  if (page_rec_is_supremum(rec)) {
    ib::warn(ER_IB_MSG_612) << "Trying to insert a record from the insert"
                               " buffer to an index page but the index page"
                               " is empty!";
    /* 尝试将记录从插入缓冲区插入到索引页，但索引页为空 */
    goto dump;
  }

  // 检查记录字段数是否合理
  if (!rec_n_fields_is_sane(index, rec, entry)) {
    ib::warn(ER_IB_MSG_613)
        << "Trying to insert a record from the insert"
           " buffer to an index page but the number of fields"
           " does not match!";
    /* 尝试将记录从插入缓冲区插入到索引页，但字段数不匹配 */
    // 打印错误记录
    rec_print(stderr, rec, index);
  dump:
    // 打印错误条目
    dtuple_print(stderr, entry);
    ib::warn(ER_IB_MSG_614)
        << "The table where this index record belongs"
           " is now probably corrupt. Please run CHECK TABLE on"
           " your tables. "
        << BUG_REPORT_MSG;
    /* 该索引记录所属的表现在可能已损坏。请在您的表上运行CHECK TABLE */

    // 调试模式下触发错误
    ut_d(ut_error);

    // 非调试模式下直接返回
    ut_o(return );
  }

  // 在页面中搜索条目位置，获取最低匹配字段数
  low_match = page_cur_search(block, index, entry, &page_cur);

  // 创建内存堆，用于存储更新向量和偏移量
  heap = mem_heap_create(
      sizeof(upd_t) + REC_OFFS_HEADER_SIZE * sizeof(*offsets) +
          dtuple_get_n_fields(entry) * (sizeof(upd_field_t) + sizeof *offsets),
      UT_LOCATION_HERE);

// 如果所有字段都匹配（记录已存在）
  if (UNIV_UNLIKELY(low_match == dtuple_get_n_fields(entry))) {
    upd_t *update;           // 更新向量
    page_zip_des_t *page_zip; // 页面压缩描述符

    // 获取当前游标指向的记录
    rec = page_cur_get_rec(&page_cur);

    /* This is based on
    row_ins_sec_index_entry_by_modify(BTR_MODIFY_LEAF). */
    /* 这部分代码基于row_ins_sec_index_entry_by_modify(BTR_MODIFY_LEAF)实现 */
    // 断言：记录必须有删除标记
    ut_ad(rec_get_deleted_flag(rec, page_is_comp(page)));

    // 获取记录的偏移量
    offsets = rec_get_offsets(rec, index, nullptr, ULINT_UNDEFINED,
                             UT_LOCATION_HERE, &heap);
    // 构建二进制差异更新向量
    update = row_upd_build_sec_rec_difference_binary(rec, index, offsets, entry,
                                                    heap);

    // 获取页面的压缩描述符
    page_zip = buf_block_get_page_zip(block);

    // 如果更新向量中没有字段需要更新（只有删除标记不同）
    if (update->n_fields == 0) {
      /* The records only differ in the delete-mark.
      Clear the delete-mark, like we did before
      Bug #56680 was fixed. */
      /* 记录只在删除标记上有不同，清除删除标记，就像修复Bug #56680前那样 */
      btr_cur_set_deleted_flag_for_ibuf(rec, page_zip, false, mtr);
      goto updated_in_place; // 跳转到更新完成处理
    }

    /* Copy the info bits. Clear the delete-mark. */
    /* 复制信息位，清除删除标记 */
    update->info_bits = rec_get_info_bits(rec, page_is_comp(page));
    update->info_bits &= ~REC_INFO_DELETED_FLAG;

    /* We cannot invoke btr_cur_optimistic_update() here,
    because we do not have a btr_cur_t or que_thr_t,
    as the insert buffer merge occurs at a very low level. */
    /* 这里不能调用btr_cur_optimistic_update()，因为插入缓冲区合并发生在很低层级，
       我们没有btr_cur_t或que_thr_t对象 */
    // 检查是否可以直接原地更新
    if (!row_upd_changes_field_size_or_external(index, offsets, update) &&
        (!page_zip ||
         btr_cur_update_alloc_zip(page_zip, &page_cur, index, offsets,
                                 rec_offs_size(offsets), false, mtr))) {
      /* This is the easy case. Do something similar
      to btr_cur_update_in_place(). */
      /* 这是简单情况，执行类似btr_cur_update_in_place()的操作 */
      rec = page_cur_get_rec(&page_cur);
      // 原地更新记录
      row_upd_rec_in_place(rec, index, offsets, update, page_zip);

      /* Log the update in place operation. During recovery
      MLOG_COMP_REC_UPDATE_IN_PLACE/MLOG_REC_UPDATE_IN_PLACE
      expects trx_id, roll_ptr for secondary indexes. So we
      just write dummy trx_id(0), roll_ptr(0) */
      /* 记录原地更新操作。在恢复期间，MLOG_COMP_REC_UPDATE_IN_PLACE/
         MLOG_REC_UPDATE_IN_PLACE期望二级索引有trx_id和roll_ptr，
         所以我们只写入虚拟的trx_id(0)和roll_ptr(0) */
      btr_cur_update_in_place_log(BTR_KEEP_SYS_FLAG, rec, index, update, 0, 0,
                                 mtr);

      // 调试代码：在记录更新日志后模拟崩溃
      DBUG_EXECUTE_IF("crash_after_log_ibuf_upd_inplace",
                     log_buffer_flush_to_disk();
                     ib::info(ER_IB_MSG_615) << "Wrote log record for ibuf"
                                                " update in place operation";
                     DBUG_SUICIDE(););

      goto updated_in_place; // 跳转到更新完成处理
    }

    /* btr_cur_update_alloc_zip() may have changed this */
    /* btr_cur_update_alloc_zip()可能已经改变了当前记录 */
    rec = page_cur_get_rec(&page_cur);

    /* A collation may identify values that differ in
    storage length.
    Some examples (1 or 2 bytes):
    utf8mb3_turkish_ci: I = U+0131 LATIN SMALL LETTER DOTLESS I
    utf8mb3_general_ci: S = U+00DF LATIN SMALL LETTER SHARP S
    utf8mb3_general_ci: A = U+00E4 LATIN SMALL LETTER A WITH DIAERESIS

    latin1_german2_ci: SS = U+00DF LATIN SMALL LETTER SHARP S

    Examples of a character (3-byte UTF-8 sequence)
    identified with 2 or 4 characters (1-byte UTF-8 sequences):

    utf8mb3_unicode_ci: 'II' = U+2171 SMALL ROMAN NUMERAL TWO
    utf8mb3_unicode_ci: '(10)' = U+247D PARENTHESIZED NUMBER TEN
    */
    /* 排序规则可能导致值在存储长度上不同。
       一些例子（1或2字节差异）：
       utf8mb3_turkish_ci: I = U+0131 拉丁文小写无点I
       utf8mb3_general_ci: S = U+00DF 拉丁文小写SHARP S
       utf8mb3_general_ci: A = U+00E4 拉丁文小写带分音符A

       latin1_german2_ci: SS = U+00DF 拉丁文小写SHARP S

       一个字符（3字节UTF-8序列）被识别为2或4个字符（1字节UTF-8序列）的例子：
       utf8mb3_unicode_ci: 'II' = U+2171 小写罗马数字二
       utf8mb3_unicode_ci: '(10)' = U+247D 带括号数字十 */

    /* Delete the different-length record, and insert the
    buffered one. */
    /* 删除长度不同的记录，并插入缓冲的记录 */

    // 在页面infimum记录上存储锁信息
    lock_rec_store_on_page_infimum(block, rec);
    // 删除当前游标指向的记录
    page_cur_delete_rec(&page_cur, index, offsets, mtr);
    // 将游标移动到前一条记录
    page_cur_move_to_prev(&page_cur);
    // 插入缓冲条目到索引页
    rec = ibuf_insert_to_index_page_low(entry, block, index, &offsets, heap,
                                        mtr, &page_cur);

    // 断言：确保插入的记录与条目匹配
    ut_ad(!cmp_dtuple_rec(entry, rec, index, offsets));
    // 从页面infimum记录恢复锁信息
    lock_rec_restore_from_page_infimum(block, rec, block);
  } else {
    // 如果没有完全匹配，偏移量为空
    offsets = nullptr;
    // 直接插入缓冲条目到索引页
    ibuf_insert_to_index_page_low(entry, block, index, &offsets, heap, mtr,
                                  &page_cur);
  }
updated_in_place:
  // 释放内存堆
  mem_heap_free(heap);
}

/** During merge, sets the delete mark on a record for a secondary index
 entry. */
/* 在合并过程中，为二级索引条目设置删除标记 */
static void ibuf_set_del_mark(
    const dtuple_t *entry,     /*!< in: entry */
    /* 输入参数：要处理的条目 */
    buf_block_t *block,        /*!< in/out: block */
    /* 输入输出参数：数据块 */
    const dict_index_t *index, /*!< in: record descriptor */
    /* 输入参数：记录描述符 */
    mtr_t *mtr)                /*!< in: mtr */
    /* 输入参数：迷你事务 */
{
  // 页面游标，用于定位记录
  page_cur_t page_cur;
  // 最低匹配字段数
  ulint low_match;

  // 断言：必须在插入缓冲区操作中
  ut_ad(ibuf_inside(mtr));
  // 断言：检查条目类型是否正确
  ut_ad(dtuple_check_typed(entry));

  // 在页面中搜索条目位置，获取最低匹配字段数
  low_match = page_cur_search(block, index, entry, &page_cur);

  // 如果所有字段都匹配（找到对应记录）
  if (low_match == dtuple_get_n_fields(entry)) {
    rec_t *rec;                // 记录指针
    page_zip_des_t *page_zip;  // 页面压缩描述符

    // 获取当前游标指向的记录
    rec = page_cur_get_rec(&page_cur);
    // 获取页面的压缩描述符
    page_zip = page_cur_get_page_zip(&page_cur);

    /* Delete mark the old index record. According to a
    comment in row_upd_sec_index_entry(), it can already
    have been delete marked if a lock wait occurred in
    row_ins_sec_index_entry() in a previous invocation of
    row_upd_sec_index_entry(). */
    /* 为旧索引记录设置删除标记。根据row_upd_sec_index_entry()中的注释，
       如果在前一次调用row_upd_sec_index_entry()时row_ins_sec_index_entry()
       发生了锁等待，记录可能已经被标记为删除 */

    // 如果记录未被标记为删除，则设置删除标记
    if (UNIV_LIKELY(
            !rec_get_deleted_flag(rec, dict_table_is_comp(index->table)))) {
      btr_cur_set_deleted_flag_for_ibuf(rec, page_zip, true, mtr);
    }
  } else {
    // 获取页面和块信息用于错误报告
    const page_t *page = page_cur_get_page(&page_cur);
    const buf_block_t *block = page_cur_get_block(&page_cur);

    // 输出错误信息：无法找到要删除标记的记录
    ib::error(ER_IB_MSG_616) << "Unable to find a record to delete-mark";
    fputs("InnoDB: tuple ", stderr);
    // 打印错误条目
    dtuple_print(stderr, entry);
    fputs(
        "\n"
        "InnoDB: record ",
        stderr);
    // 打印错误记录
    rec_print(stderr, page_cur_get_rec(&page_cur), index);

    // 输出页面详细信息
    ib::error(ER_IB_MSG_617)
        << "page " << block->page.id << " (" << page_get_n_recs(page)
        << " records, index id " << btr_page_get_index_id(page) << ").";

    // 输出错误报告信息
    ib::error(ER_IB_MSG_618) << BUG_REPORT_MSG;
    // 调试模式下触发错误
    ut_d(ut_error);
  }
}

/** During merge, delete a record for a secondary index entry. */
/* 在合并过程中，删除二级索引条目对应的记录 */
static void ibuf_delete(const dtuple_t *entry, /*!< in: entry */
                        /* 输入参数：要删除的条目 */
                        buf_block_t *block,    /*!< in/out: block */
                        /* 输入输出参数：数据块 */
                        dict_index_t *index,   /*!< in: record descriptor */
                        /* 输入参数：记录描述符 */
                        mtr_t *mtr) /*!< in/out: mtr; must be committed
                                    before latching any further pages */
                        /* 输入输出参数：迷你事务，在锁定更多页之前必须提交 */
{
  // 页面游标，用于定位记录
  page_cur_t page_cur;
  // 最低匹配字段数
  ulint low_match;

  // 断言：必须在插入缓冲区操作中
  ut_ad(ibuf_inside(mtr));
  // 断言：检查条目类型是否正确
  ut_ad(dtuple_check_typed(entry));
  // 断言：索引不能是空间索引
  ut_ad(!dict_index_is_spatial(index));

  // 在页面中搜索条目位置，获取最低匹配字段数
  low_match = page_cur_search(block, index, entry, &page_cur);

  // 如果所有字段都匹配（找到对应记录）
  if (low_match == dtuple_get_n_fields(entry)) {
    // 获取页面压缩描述符
    page_zip_des_t *page_zip = buf_block_get_page_zip(block);
    // 获取页面帧指针
    page_t *page = buf_block_get_frame(block);
    // 获取当前游标指向的记录
    rec_t *rec = page_cur_get_rec(&page_cur);

    /* TODO: the below should probably be a separate function,
    it's a bastardized version of btr_cur_optimistic_delete. */
    /* TODO: 这部分代码应该单独提取为一个函数，
       它是btr_cur_optimistic_delete的简化版本 */

    // 初始化记录偏移量数组
    ulint offsets_[REC_OFFS_NORMAL_SIZE];
    ulint *offsets = offsets_;
    // 内存堆，用于临时分配
    mem_heap_t *heap = nullptr;
    // 最大插入大小
    ulint max_ins_size = 0;

    // 初始化偏移量数组
    rec_offs_init(offsets_);

    // 获取记录的偏移量
    offsets = rec_get_offsets(rec, index, offsets, ULINT_UNDEFINED,
                             UT_LOCATION_HERE, &heap);

    // 如果页中只有一条记录或记录未被标记为删除
    if (page_get_n_recs(page) <= 1 ||
        !(REC_INFO_DELETED_FLAG & rec_get_info_bits(rec, page_is_comp(page)))) {
      /* Refuse to purge the last record or a
      record that has not been marked for deletion. */
      /* 拒绝清除最后一条记录或未被标记为删除的记录 */
      ib::error(ER_IB_MSG_619) << "Unable to purge a record";
      fputs("InnoDB: tuple ", stderr);
      // 打印错误条目
      dtuple_print(stderr, entry);
      fputs(
          "\n"
          "InnoDB: record ",
          stderr);
      // 打印错误记录
      rec_print_new(stderr, rec, offsets);
      fprintf(stderr,
              "\nspace " UINT32PF " offset " UINT32PF
              " (%u records, index id %llu)\n"
              "InnoDB: Submit a detailed bug report"
              " to http://bugs.mysql.com\n",
              block->page.id.space(), block->page.id.page_no(),
              (unsigned)page_get_n_recs(page),
              (ulonglong)btr_page_get_index_id(page));

      // 调试模式下触发错误
      ut_d(ut_error);
      // 非调试模式下直接返回
      ut_o(return );
    }

    // 更新锁信息
    lock_update_delete(block, rec);

    // 如果不是压缩页，计算重组后的最大插入大小
    if (!page_zip) {
      max_ins_size = page_get_max_insert_size_after_reorganize(page, 1);
    }
#ifdef UNIV_ZIP_DEBUG
    // 调试模式下验证压缩页
    ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */
    // 删除当前游标指向的记录
    page_cur_delete_rec(&page_cur, index, offsets, mtr);
#ifdef UNIV_ZIP_DEBUG
    // 调试模式下再次验证压缩页
    ut_a(!page_zip || page_zip_validate(page_zip, page, index));
#endif /* UNIV_ZIP_DEBUG */

    if (page_zip) {
      // 如果是压缩页，更新空闲位
      ibuf_update_free_bits_zip(block, mtr);
    } else {
      // 如果是非压缩页，更新空闲位
      ibuf_update_free_bits_low(block, max_ins_size, mtr);
    }

    if (UNIV_LIKELY_NULL(heap)) {
      mem_heap_free(heap);
    }
  } else {
    /* The record must have been purged already. */
  }
}

/** Restores insert buffer tree cursor position
 @return true if the position was restored; false if not */
static bool ibuf_restore_pos(
    space_id_t space,  /*!< in: space id */
    page_no_t page_no, /*!< in: index page number where the record
                       should belong */
    const dtuple_t *search_tuple,
    /*!< in: search tuple for entries of page_no */
    ulint mode,       /*!< in: BTR_MODIFY_LEAF or BTR_MODIFY_TREE */
    btr_pcur_t *pcur, /*!< in/out: persistent cursor whose
                      position is to be restored */
    mtr_t *mtr)       /*!< in/out: mini-transaction */
{
  ut_ad(mode == BTR_MODIFY_LEAF ||
        BTR_LATCH_MODE_WITHOUT_INTENTION(mode) == BTR_MODIFY_TREE);

  if (pcur->restore_position(mode, mtr, UT_LOCATION_HERE)) {
    return true;
  }

  if (fil_space_get_flags(space) == UINT32_UNDEFINED) {
    /* The tablespace has been dropped.  It is possible
    that another thread has deleted the insert buffer
    entry.  Do not complain. */
    ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);
  } else {
    ib::error(ER_IB_MSG_620) << "ibuf cursor restoration fails!."
                                " ibuf record inserted to page "
                             << space << ":" << page_no;

    ib::error(ER_IB_MSG_621) << BUG_REPORT_MSG;

    rec_print_old(stderr, pcur->get_rec());
    rec_print_old(stderr, pcur->m_old_rec);
    dtuple_print(stderr, search_tuple);

    rec_print_old(stderr, page_rec_get_next(pcur->get_rec()));

    ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_622)
        << "Failed to restore ibuf position.";
  }

  return false;
}

/** 
 * 从插入缓冲区删除pcur定位的记录。如果必须使用悲观删除，
 * 此函数会提交mtr并关闭游标。
 * Deletes from ibuf the record on which pcur is positioned. If we have to
 * resort to a pessimistic delete, this function commits mtr and closes
 * the cursor.
 *
 * @return 如果在此操作中提交了mtr并关闭了pcur，则返回true
 * @return true if mtr was committed and pcur closed in this operation 
 */
[[nodiscard]] static bool ibuf_delete_rec(
    space_id_t space,  /*!< in: 表空间ID */
    page_no_t page_no, /*!< in: 记录所属的索引页号 */
    btr_pcur_t *pcur,  /*!< in: 定位在要删除记录上的pcur，
                       具有BTR_MODIFY_LEAF锁模式 */
    const dtuple_t *search_tuple,
    /*!< in: 用于page_no条目的搜索元组 */
    mtr_t *mtr)        /*!< in: 迷你事务 */
{
  // 声明变量：根页指针和错误码
  page_t *root;
  dberr_t err;

  // 断言：必须在插入缓冲区操作中
  ut_ad(ibuf_inside(mtr));
  // 断言：当前记录必须是用户记录
  ut_ad(page_rec_is_user_rec(pcur->get_rec()));
  // 断言：记录中的页号必须匹配参数
  ut_ad(ibuf_rec_get_page_no(mtr, pcur->get_rec()) == page_no);
  // 断言：记录中的表空间ID必须匹配参数
  ut_ad(ibuf_rec_get_space(mtr, pcur->get_rec()) == space);

#if defined UNIV_DEBUG || defined UNIV_IBUF_DEBUG
  // 调试模式下注入故障（崩溃）
  if (ibuf_debug == 2) {
    /* 在尝试乐观删除前注入故障（崩溃），因为在变更缓冲区中进行悲观删除
       需要更大的测试用例 */
    /* Inject a fault (crash). We do this before trying
    optimistic delete, because a pessimistic delete in the
    change buffer would require a larger test case. */

    /* 标记缓冲记录为已处理，避免崩溃恢复后断言失败 */
    /* Flag the buffered record as processed, to avoid
    an assertion failure after crash recovery. */
    btr_cur_set_deleted_flag_for_ibuf(pcur->get_rec(), nullptr, true, mtr);

    // 提交迷你事务并刷新日志
    ibuf_mtr_commit(mtr);
    log_buffer_flush_to_disk();
    DBUG_SUICIDE();
  }
#endif /* UNIV_DEBUG || UNIV_IBUF_DEBUG */

  // 尝试乐观删除记录
  auto success = btr_cur_optimistic_delete(pcur->get_btr_cur(), 0, mtr);

  // 构造页ID对象
  const page_id_t page_id(space, page_no);

  // 如果乐观删除成功
  if (success) {
    // 如果页面变为空
    if (page_is_empty(pcur->get_page())) {
      /* 如果B树页为空，它必须是根页且整个B树必须为空。
         InnoDB不允许根页以外的空B树页 */
      /* If a B-tree page is empty, it must be the root page
      and the whole B-tree must be empty. InnoDB does not
      allow empty B-tree pages other than the root. */
      root = pcur->get_page();

      // 断言：根页必须在系统表空间
      ut_ad(page_get_space_id(root) == IBUF_SPACE_ID);
      // 断言：根页必须是插入缓冲区树的根页
      ut_ad(page_get_page_no(root) == FSP_IBUF_TREE_ROOT_PAGE_NO);

      /* ibuf->empty受根页锁保护。在删除前，它必须是false */
      /* ibuf->empty is protected by the root page latch.
      Before the deletion, it had to be false. */
      ut_ad(!ibuf->empty);
      // 设置插入缓冲区为空标志
      ibuf->empty = true;
    }

#ifdef UNIV_IBUF_COUNT_DEBUG
    // 调试模式下更新插入缓冲区计数
    ib::info(ER_IB_MSG_623)
        << "Decrementing ibuf count of space " << space << " page " << page_no
        << " from " << ibuf_count_get(page_id) << " by 1";

    ibuf_count_set(page_id, ibuf_count_get(page_id) - 1);
#endif /* UNIV_IBUF_COUNT_DEBUG */

    // 返回false表示没有提交mtr或关闭pcur
    return false;
  }

  // 断言：当前记录必须是用户记录
  ut_ad(page_rec_is_user_rec(pcur->get_rec()));
  // 断言：记录中的页号必须匹配参数
  ut_ad(ibuf_rec_get_page_no(mtr, pcur->get_rec()) == page_no);
  // 断言：记录中的表空间ID必须匹配参数
  ut_ad(ibuf_rec_get_space(mtr, pcur->get_rec()) == space);

  /* 我们必须使用悲观删除从插入缓冲区删除记录。
     删除标记记录，这样在服务器崩溃前不会再次应用它，
     如果悲观删除还没有持久化 */
  /* We have to resort to a pessimistic delete from ibuf.
  Delete-mark the record so that it will not be applied again,
  in case the server crashes before the pessimistic delete is
  made persistent. */
  // 设置记录的删除标记
  btr_cur_set_deleted_flag_for_ibuf(pcur->get_rec(), nullptr, true, mtr);

  // 存储游标位置
  pcur->store_position(mtr);
  // 提交迷你事务并关闭游标
  ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);

  // 开始新的迷你事务
  ibuf_mtr_start(mtr);

  // 获取插入缓冲区互斥锁
  mutex_enter(&ibuf_mutex);

  // 尝试恢复游标位置
  if (!ibuf_restore_pos(space, page_no, search_tuple,
                        BTR_MODIFY_TREE | BTR_LATCH_FOR_DELETE, pcur, mtr)) {
    // 如果恢复失败，释放互斥锁
    mutex_exit(&ibuf_mutex);
    // 断言：mtr必须已提交
    ut_ad(mtr->has_committed());
    // 跳转到函数退出处理
    goto func_exit;
  }

  // 获取插入缓冲区树的根页
  root = ibuf_tree_root_get(mtr);

  // 执行悲观删除
  btr_cur_pessimistic_delete(&err, true, pcur->get_btr_cur(), 0, false, 0, 0, 0,
                             mtr, nullptr, nullptr);
  // 断言：删除必须成功
  ut_a(err == DB_SUCCESS);

#ifdef UNIV_IBUF_COUNT_DEBUG
  // 调试模式下更新插入缓冲区计数
  ibuf_count_set(page_id, ibuf_count_get(page_id) - 1);
#endif /* UNIV_IBUF_COUNT_DEBUG */

  // 更新插入缓冲区大小
  ibuf_size_update(root);
  // 释放插入缓冲区互斥锁
  mutex_exit(&ibuf_mutex);

  // 更新插入缓冲区空状态
  ibuf->empty = page_is_empty(root);
  // 提交迷你事务并关闭游标
  ibuf_btr_pcur_commit_specify_mtr(pcur, mtr);

func_exit:
  // 断言：mtr必须已提交
  ut_ad(mtr->has_committed());
  // 关闭游标
  pcur->close();

  // 返回true表示已提交mtr并关闭pcur
  return true;
}

/**
 * 当索引页从磁盘读入缓冲池时，此函数会应用所有缓冲操作到该页，
 * 并从插入缓冲区删除相关条目。如果页不是从磁盘读取而是在缓冲池中创建的，
 * 此函数会从插入缓冲区删除其缓冲条目；如果页属于后来被删除的索引，
 * 则可能存在这样的条目。
 * 
 * When an index page is read from a disk to the buffer pool, this function
 * applies any buffered operations to the page and deletes the entries from the
 * insert buffer. If the page is not read, but created in the buffer pool, this
 * function deletes its buffered entries from the insert buffer; there can
 * exist entries for such a page if the page belonged to an index which
 * subsequently was dropped.
 * 
 * @param[in,out]  block                   如果页已从磁盘读取，则指向该页的x-latch指针，否则为NULL
 *                                          if page has been read from disk,
 *                                          pointer to the page x-latched, else NULL
 * @param[in]      page_id                 索引页的页ID
 *                                          page id of the index page
 * @param[in]      update_ibuf_bitmap      通常设置为true，但如果表空间已被删除或正在删除，
 *                                          则不需要更新不存在的位图页
 *                                          normally this is set to true, but
 *                                          if we have deleted or are deleting the tablespace, 
 *                                          then we naturally do not want to update a 
 *                                          non-existent bitmap page
 * @param[in]      page_size               页大小
 *                                          page size 
 */
void ibuf_merge_or_delete_for_page(buf_block_t *block, const page_id_t &page_id,
                                   const page_size_t *page_size,
                                   bool update_ibuf_bitmap) {
  // 内存堆，用于临时分配
  mem_heap_t *heap;
  // 持久游标，用于遍历插入缓冲区
  btr_pcur_t pcur;
  // 搜索元组，用于定位插入缓冲区中的记录
  dtuple_t *search_tuple;
#ifdef UNIV_IBUF_DEBUG
  // 调试用，记录操作量
  ulint volume = 0;
#endif /* UNIV_IBUF_DEBUG */
  // 页压缩描述符
  page_zip_des_t *page_zip = nullptr;
  // 表空间对象
  fil_space_t *space = nullptr;
  // 标记是否已发现损坏
  bool corruption_noticed = false;
  // 操作是否成功
  bool success;
  // 迷你事务
  mtr_t mtr;

  // 记录合并和丢弃的操作计数
  /* Counts for merged & discarded operations. */
  ulint mops[IBUF_OP_COUNT];
  ulint dops[IBUF_OP_COUNT];

  // 断言：如果block不为空，则其页ID必须与参数一致
  ut_ad(block == nullptr || page_id == block->page.id);
  // 断言：如果block不为空，则必须处于IO_FIX_READ状态
  ut_ad(block == nullptr || block->page.is_io_fix_read());

  // 如果强制恢复级别禁止插入缓冲区合并，或是系统表空间页，或是临时表空间，则直接返回
  if (srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE ||
      trx_sys_hdr_page(page_id) || fsp_is_system_temporary(page_id.space())) {
    return;
  }

  /* 我们不能在以下代码中引用page_size，因为当buf_read_ibuf_merge_pages()合并
     (丢弃)已删除表空间的更改时，它会被传递为NULL(未知)。当block != NULL或
     update_ibuf_bitmap被指定时，page_size必须是已知的。
     这就是为什么我们会在下面用page_size替换univ_page_size重复检查。
     传递univ_page_size假设未压缩页大小总是压缩页大小的2的幂次方倍数。 */
  /* We cannot refer to page_size in the following, because it is passed
  as NULL (it is unknown) when buf_read_ibuf_merge_pages() is merging
  (discarding) changes for a dropped tablespace. When block != NULL or
  update_ibuf_bitmap is specified, then page_size must be known.
  That is why we will repeat the check below, with page_size in
  place of univ_page_size. Passing univ_page_size assumes that the
  uncompressed page size always is a power-of-2 multiple of the
  compressed page size. */

  // 如果是固定地址页或表空间描述页，则直接返回
  if (ibuf_fixed_addr_page(page_id, univ_page_size) ||
      fsp_descr_page(page_id, univ_page_size)) {
    return;
  }

  // 如果需要更新插入缓冲区位图
  if (update_ibuf_bitmap) {
    // 断言：page_size不能为空
    ut_ad(page_size != nullptr);

    // 再次检查是否是固定地址页或表空间描述页
    if (ibuf_fixed_addr_page(page_id, *page_size) ||
        fsp_descr_page(page_id, *page_size)) {
      return;
    }

    // 尝试静默获取表空间
    space = fil_space_acquire_silent(page_id.space());

    if (space == nullptr) {
      /* 不要尝试从表空间读取位图页；
         只需删除该页的插入缓冲区记录 */
      /* Do not try to read the bitmap page from space;
      just delete the ibuf records for the page */

      block = nullptr;
      update_ibuf_bitmap = false;
    } else {
      page_t *bitmap_page;
      ulint bitmap_bits;

      // 启动迷你事务
      ibuf_mtr_start(&mtr);

      // 获取位图页
      bitmap_page =
          ibuf_bitmap_get_map_page(page_id, *page_size, UT_LOCATION_HERE, &mtr);

      // 获取位图中的缓冲位
      bitmap_bits = ibuf_bitmap_page_get_bits(bitmap_page, page_id, *page_size,
                                              IBUF_BITMAP_BUFFERED, &mtr);

      // 提交迷你事务
      ibuf_mtr_commit(&mtr);

      // 如果没有缓冲的操作，则释放表空间并返回
      if (!bitmap_bits) {
        /* No inserts buffered for this page */

        fil_space_release(space);
        return;
      }
    }
  } else if (block != nullptr && (ibuf_fixed_addr_page(page_id, *page_size) ||
                                  fsp_descr_page(page_id, *page_size))) {
    // 如果block不为空且是固定地址页或表空间描述页，则直接返回
    return;
  }

  // 创建内存堆
  heap = mem_heap_create(512, UT_LOCATION_HERE);

  // 构建搜索元组
  search_tuple =
      ibuf_search_tuple_build(page_id.space(), page_id.page_no(), heap);

  if (block != nullptr) {
    /* Move the ownership of the x-latch on the page to this OS
    thread, so that we can acquire a second x-latch on it. This
    is needed for the insert operations to the index page to pass
    the debug checks. */

    rw_lock_x_lock_move_ownership(&(block->lock));
    page_zip = buf_block_get_page_zip(block);


    if (!fil_page_index_page_check(block->frame) ||
        !page_is_leaf(block->frame)) {
      corruption_noticed = true;

      ib::error(ER_IB_MSG_624) << "Corruption in the tablespace. Bitmap"
                                  " shows insert buffer records to page "
                               << page_id << " though the page type is "
                               << fil_page_get_type(block->frame)
                               << ", which is not an index leaf page. We try"
                                  " to resolve the problem by skipping the"
                                  " insert buffer merge for this page. Please"
                                  " run CHECK TABLE on your tables to determine"
                                  " if they are corrupt after this.";

      ib::error(ER_IB_MSG_625) << "Please submit a detailed bug"
                                  " report to http://bugs.mysql.com";
      ut_d(ut_error);
    }
  }

  // 初始化操作计数器数组
  memset(mops, 0, sizeof(mops));
  memset(dops, 0, sizeof(dops));

loop:
  // 开始一个新的迷你事务
  ibuf_mtr_start(&mtr);

  /* 在插入缓冲区中定位到该索引页的第一个条目 */
  /* Position pcur in the insert buffer at the first entry for this index page */
  pcur.open_on_user_rec(ibuf->index, search_tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF,
                        &mtr, UT_LOCATION_HERE);

  // 如果block不为空，获取页的X锁
  if (block != nullptr) {
    auto success = buf_page_get_known_nowait(
        RW_X_LATCH, block, Cache_hint::KEEP_OLD, __FILE__, __LINE__, &mtr);

    ut_a(success);

    /* 这是一个用户页(二级索引叶子页)，但我们假装它是变更缓冲区页，
       以遵守锁顺序。这应该是OK的，因为缓冲的变更会在block处于io-fixed状态时立即应用。
       其他线程不能尝试锁定一个io-fixed的block。 */
    /* This is a user page (secondary index leaf page),
    but we pretend that it is a change buffer page in
    order to obey the latching order. This should be OK,
    because buffered changes are applied immediately while
    the block is io-fixed. Other threads must not try to
    latch an io-fixed block. */
    buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);
  }

  // 如果没有定位到用户记录，跳转到reset_bit标签
  if (!pcur.is_on_user_rec()) {
    ut_ad(pcur.is_after_last_in_tree(&mtr));

    goto reset_bit;
  }

  // 循环处理所有记录
  for (;;) {
    rec_t *rec;

    ut_ad(pcur.is_on_user_rec());

    rec = pcur.get_rec();

    /* 检查条目是否属于这个索引页 */
    /* Check if the entry is for this index page */
    if (ibuf_rec_get_page_no(&mtr, rec) != page_id.page_no() ||
        ibuf_rec_get_space(&mtr, rec) != page_id.space()) {
      if (block != nullptr) {
        page_header_reset_last_insert(block->frame, page_zip, &mtr);
      }

      goto reset_bit;
    }

    // 如果已发现损坏，打印丢弃记录的信息
    if (corruption_noticed) {
      fputs("InnoDB: Discarding record\n ", stderr);
      rec_print_old(stderr, rec);
      fputs("\nInnoDB: from the insert buffer!\n\n", stderr);
    } else if (block != nullptr && !rec_get_deleted_flag(rec, 0)) {
      /* 现在我们有一个应该应用到索引页的记录 */
      /* Now we have at pcur a record which should be applied on the index page */
      dtuple_t *entry;
      trx_id_t max_trx_id;
      dict_index_t *dummy_index;
      ibuf_op_t op = ibuf_rec_get_op_type(&mtr, rec);

      // 更新页的最大事务ID
      max_trx_id = page_get_max_trx_id(page_align(rec));
      page_update_max_trx_id(block, page_zip, max_trx_id, &mtr);

      ut_ad(page_validate(page_align(rec), ibuf->index));

      // 从插入缓冲区记录构建条目
      entry = ibuf_build_entry_from_ibuf_rec(&mtr, rec, heap, &dummy_index);

      ut_ad(page_validate(block->frame, dummy_index));

      // 根据操作类型执行不同的处理
      switch (op) {
        case IBUF_OP_INSERT:
#ifdef UNIV_IBUF_DEBUG
          volume += rec_get_converted_size(dummy_index, entry);
          volume += page_dir_calc_reserved_space(1);
          ut_a(volume <= 4 * UNIV_PAGE_SIZE / IBUF_PAGE_SIZE_PER_FREE_SPACE);
#endif
          // 执行插入操作
          ibuf_insert_to_index_page(entry, block, dummy_index, &mtr);
          break;

        case IBUF_OP_DELETE_MARK:
          // 执行删除标记操作
          ibuf_set_del_mark(entry, block, dummy_index, &mtr);
          break;

        case IBUF_OP_DELETE:
          // 执行删除操作
          ibuf_delete(entry, block, dummy_index, &mtr);
          /* 因为ibuf_delete()会锁定插入缓冲区位图页，
             所以在锁定更多页之前提交mtr */
          /* Because ibuf_delete() will latch an insert buffer bitmap page, 
             commit mtr before latching any further pages */
          ut_ad(rec == pcur.get_rec());
          ut_ad(page_rec_is_user_rec(rec));
          ut_ad(ibuf_rec_get_page_no(&mtr, rec) == page_id.page_no());
          ut_ad(ibuf_rec_get_space(&mtr, rec) == page_id.space());

          /* 标记变更缓冲区记录已处理，防止在服务器崩溃后再次合并 */
          /* Mark the change buffer record processed,
          so that it will not be merged again in case
          the server crashes between the following
          mtr_commit() and the subsequent mtr_commit()
          of deleting the change buffer record. */
          
          btr_cur_set_deleted_flag_for_ibuf(pcur.get_rec(), nullptr, true, &mtr);

          pcur.store_position(&mtr);
          ibuf_btr_pcur_commit_specify_mtr(&pcur, &mtr);

          ibuf_mtr_start(&mtr);

          success = buf_page_get_known_nowait(RW_X_LATCH, block, 
                    Cache_hint::KEEP_OLD, __FILE__, __LINE__, &mtr);
          ut_a(success);

          /* 这是一个用户页，但因为block是io-fixed的，使用较低的锁顺序是OK的 */
          /* This is a user page, but it should be OK to use too low latching 
             order for it, as the block is io-fixed. */
          buf_block_dbg_add_level(block, SYNC_IBUF_TREE_NODE);

          if (!ibuf_restore_pos(page_id.space(), page_id.page_no(),
                              search_tuple, BTR_MODIFY_LEAF, &pcur, &mtr)) {
            ut_ad(mtr.has_committed());
            mops[op]++;
            ibuf_dummy_index_free(dummy_index);
            goto loop;
          }

          break;
        default:
          ut_error;
      }

      mops[op]++;
      ibuf_dummy_index_free(dummy_index);
    } else {
      dops[ibuf_rec_get_op_type(&mtr, rec)]++;
    }

    /* 从插入缓冲区删除记录 */
    /* Delete the record from ibuf */
    if (ibuf_delete_rec(page_id.space(), page_id.page_no(), &pcur, search_tuple,
                        &mtr)) {
      /* 如果是悲观删除且mtr已提交，则从头开始 */
      /* Deletion was pessimistic and mtr was committed: we start from the beginning */
      ut_ad(mtr.has_committed());
      goto loop;
    } else if (pcur.is_after_last_on_page()) {
      ibuf_mtr_commit(&mtr);
      pcur.close();
      goto loop;
    }
  }

reset_bit:
  // 如果需要更新位图
  if (update_ibuf_bitmap) {
    page_t *bitmap_page;

    bitmap_page = ibuf_bitmap_get_map_page(page_id, *page_size, 
                                          UT_LOCATION_HERE, &mtr);

    // 清除缓冲位
    ibuf_bitmap_page_set_bits(bitmap_page, page_id, *page_size,
                              IBUF_BITMAP_BUFFERED, false, &mtr);

    if (block != nullptr) {
      // 获取并更新空闲位
      ulint old_bits = ibuf_bitmap_page_get_bits(bitmap_page, page_id, 
                                                *page_size, IBUF_BITMAP_FREE, &mtr);
      ulint new_bits = ibuf_index_page_calc_free(block);

      if (old_bits != new_bits) {
        ibuf_bitmap_page_set_bits(bitmap_page, page_id, *page_size,
                                IBUF_BITMAP_FREE, new_bits, &mtr);
      }
    }
  }

  // 提交事务并清理资源
  ibuf_mtr_commit(&mtr);
  pcur.close();
  mem_heap_free(heap);

  // 更新统计信息
  ibuf->n_merges.fetch_add(1);
  ibuf_add_ops(ibuf->n_merged_ops, mops);
  ibuf_add_ops(ibuf->n_discarded_ops, dops);

  if (space != nullptr) {
    fil_space_release(space);
  }

#ifdef UNIV_IBUF_COUNT_DEBUG
  ut_a(ibuf_count_get(page_id) == 0);
#endif
}

/** Deletes all entries in the insert buffer for a given space id. This is used
in DISCARD TABLESPACE and IMPORT TABLESPACE.
NOTE: this does not update the page free bitmaps in the space. The space will
become CORRUPT when you call this function! */
void ibuf_delete_for_discarded_space(space_id_t space) /*!< in: space id */
{
  mem_heap_t *heap;
  btr_pcur_t pcur;
  dtuple_t *search_tuple;
  const rec_t *ibuf_rec;
  page_no_t page_no;
  mtr_t mtr;

  /* Counts for discarded operations. */
  ulint dops[IBUF_OP_COUNT];

  heap = mem_heap_create(512, UT_LOCATION_HERE);

  /* Use page number 0 to build the search tuple so that we get the
  cursor positioned at the first entry for this space id */

  search_tuple = ibuf_search_tuple_build(space, 0, heap);

  memset(dops, 0, sizeof(dops));
loop:
  ibuf_mtr_start(&mtr);

  /* Position pcur in the insert buffer at the first entry for the
  space */
  pcur.open_on_user_rec(ibuf->index, search_tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF,
                        &mtr, UT_LOCATION_HERE);

  if (!pcur.is_on_user_rec()) {
    ut_ad(pcur.is_after_last_in_tree(&mtr));

    goto leave_loop;
  }

  for (;;) {
    ut_ad(pcur.is_on_user_rec());

    ibuf_rec = pcur.get_rec();

    /* Check if the entry is for this space */
    if (ibuf_rec_get_space(&mtr, ibuf_rec) != space) {
      goto leave_loop;
    }

    page_no = ibuf_rec_get_page_no(&mtr, ibuf_rec);

    dops[ibuf_rec_get_op_type(&mtr, ibuf_rec)]++;

    /* Delete the record from ibuf */
    if (ibuf_delete_rec(space, page_no, &pcur, search_tuple, &mtr)) {
      /* Deletion was pessimistic and mtr was committed:
      we start from the beginning again */

      ut_ad(mtr.has_committed());
      goto loop;
    }

    if (pcur.is_after_last_on_page()) {
      ibuf_mtr_commit(&mtr);
      pcur.close();

      goto loop;
    }
  }

leave_loop:
  ibuf_mtr_commit(&mtr);
  pcur.close();

  ibuf_add_ops(ibuf->n_discarded_ops, dops);

  mem_heap_free(heap);
}

/** Looks if the insert buffer is empty.
 @return true if empty */
bool ibuf_is_empty(void) {
  bool is_empty;
  const page_t *root;
  mtr_t mtr;

  ibuf_mtr_start(&mtr);

  mutex_enter(&ibuf_mutex);
  root = ibuf_tree_root_get(&mtr);
  mutex_exit(&ibuf_mutex);

  is_empty = page_is_empty(root);
  ut_a(is_empty == ibuf->empty);
  ibuf_mtr_commit(&mtr);

  return (is_empty);
}

/** Prints info of ibuf. */
void ibuf_print(FILE *file) /*!< in: file where to print */
{
#ifdef UNIV_IBUF_COUNT_DEBUG
  space_id_t i;
  page_no_t j;
#endif

  mutex_enter(&ibuf_mutex);

  fprintf(file,
          "Ibuf: size %lu, free list len %lu,"
          " seg size %lu, %lu merges\n",
          (ulong)ibuf->size, (ulong)ibuf->free_list_len, (ulong)ibuf->seg_size,
          (ulong)ibuf->n_merges);

  fputs("merged operations:\n ", file);
  ibuf_print_ops(ibuf->n_merged_ops, file);

  fputs("discarded operations:\n ", file);
  ibuf_print_ops(ibuf->n_discarded_ops, file);

#ifdef UNIV_IBUF_COUNT_DEBUG
  for (i = 0; i < IBUF_COUNT_N_SPACES; i++) {
    for (j = 0; j < IBUF_COUNT_N_PAGES; j++) {
      ulint count = ibuf_count_get(page_id_t(i, j, 0));

      if (count > 0) {
        fprintf(stderr,
                "Ibuf count for space/page %lu/%lu"
                " is %lu\n",
                (ulong)i, (ulong)j, (ulong)count);
      }
    }
  }
#endif /* UNIV_IBUF_COUNT_DEBUG */

  mutex_exit(&ibuf_mutex);
}

/** Checks the insert buffer bitmaps on IMPORT TABLESPACE.
 @return DB_SUCCESS or error code */
dberr_t ibuf_check_bitmap_on_import(
    const trx_t *trx,    /*!< in: transaction */
    space_id_t space_id) /*!< in: tablespace identifier */
{
  page_no_t size;
  page_no_t page_no;

  ut_ad(space_id);
  ut_ad(trx->mysql_thd);

  bool found;

  const page_size_t &page_size = fil_space_get_page_size(space_id, &found);

  if (!found) {
    return (DB_TABLE_NOT_FOUND);
  }

  size = fil_space_get_size(space_id);

  if (size == 0) {
    return (DB_TABLE_NOT_FOUND);
  }

  mutex_enter(&ibuf_mutex);

  /* The two bitmap pages (allocation bitmap and ibuf bitmap) repeat
  every page_size pages. For example if page_size is 16 KiB, then the
  two bitmap pages repeat every 16 KiB * 16384 = 256 MiB. In the loop
  below page_no is measured in number of pages since the beginning of
  the space, as usual. */

  for (page_no = 0; page_no < size;
       page_no += static_cast<page_no_t>(page_size.physical())) {
    mtr_t mtr;
    page_t *bitmap_page;
    page_no_t i;

    if (trx_is_interrupted(trx)) {
      mutex_exit(&ibuf_mutex);
      return (DB_INTERRUPTED);
    }

    mtr_start(&mtr);

    mtr_set_log_mode(&mtr, MTR_LOG_NO_REDO);

    ibuf_enter(&mtr);

    bitmap_page = ibuf_bitmap_get_map_page(page_id_t(space_id, page_no),
                                           page_size, UT_LOCATION_HERE, &mtr);

    if (buf_page_is_zeroes(bitmap_page, page_size)) {
      /* This means we got all-zero page instead of
      ibuf bitmap page. The subsequent page should be
      all-zero pages. */
#ifdef UNIV_DEBUG
      for (page_no_t curr_page = page_no + 1; curr_page < page_size.physical();
           curr_page++) {
        buf_block_t *block =
            buf_page_get(page_id_t(space_id, curr_page), page_size, RW_S_LATCH,
                         UT_LOCATION_HERE, &mtr);
        page_t *page = buf_block_get_frame(block);
        ut_ad(buf_page_is_zeroes(page, page_size));
      }
#endif /* UNIV_DEBUG */
      ibuf_exit(&mtr);
      mtr_commit(&mtr);
      continue;
    }

    for (i = FSP_IBUF_BITMAP_OFFSET + 1;
         i < static_cast<page_no_t>(page_size.physical()); i++) {
      const page_no_t offset = page_no + i;

      const page_id_t cur_page_id(space_id, offset);

      if (ibuf_bitmap_page_get_bits(bitmap_page, cur_page_id, page_size,
                                    IBUF_BITMAP_IBUF, &mtr)) {
        mutex_exit(&ibuf_mutex);
        ibuf_exit(&mtr);
        mtr_commit(&mtr);

        ib_errf(trx->mysql_thd, IB_LOG_LEVEL_ERROR, ER_INNODB_INDEX_CORRUPT,
                "Space %u page %u"
                " is wrongly flagged to belong to the"
                " insert buffer",
                (unsigned)space_id, (unsigned)offset);

        return (DB_CORRUPTION);
      }

      if (ibuf_bitmap_page_get_bits(bitmap_page, cur_page_id, page_size,
                                    IBUF_BITMAP_BUFFERED, &mtr)) {
        ib_errf(trx->mysql_thd, IB_LOG_LEVEL_WARN, ER_INNODB_INDEX_CORRUPT,
                "Buffered changes"
                " for space %u page %u are lost",
                (unsigned)space_id, (unsigned)offset);

        /* Tolerate this error, so that
        slightly corrupted tables can be
        imported and dumped.  Clear the bit. */
        ibuf_bitmap_page_set_bits(bitmap_page, cur_page_id, page_size,
                                  IBUF_BITMAP_BUFFERED, false, &mtr);
      }
    }

    ibuf_exit(&mtr);
    mtr_commit(&mtr);
  }

  mutex_exit(&ibuf_mutex);
  return (DB_SUCCESS);
}

/** Updates free bits and buffered bits for bulk loaded page.
@param[in]      block   index page
@param[in]      reset   flag if reset free val */
void ibuf_set_bitmap_for_bulk_load(buf_block_t *block, bool reset) {
  page_t *bitmap_page;
  mtr_t mtr;
  ulint free_val;

  ut_a(page_is_leaf(buf_block_get_frame(block)));

  free_val = ibuf_index_page_calc_free(block);

  mtr_start(&mtr);

  bitmap_page = ibuf_bitmap_get_map_page(block->page.id, block->page.size,
                                         UT_LOCATION_HERE, &mtr);

  free_val = reset ? 0 : ibuf_index_page_calc_free(block);
  ibuf_bitmap_page_set_bits(bitmap_page, block->page.id, block->page.size,
                            IBUF_BITMAP_FREE, free_val, &mtr);

  ibuf_bitmap_page_set_bits(bitmap_page, block->page.id, block->page.size,
                            IBUF_BITMAP_BUFFERED, false, &mtr);

  mtr_commit(&mtr);
}

#endif /* !UNIV_HOTBACKUP */
