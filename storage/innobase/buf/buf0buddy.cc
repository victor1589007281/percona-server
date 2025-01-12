/*****************************************************************************

Copyright (c) 2006, 2022, Oracle and/or its affiliates.

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

/** @file buf/buf0buddy.cc
 Binary buddy allocator for compressed pages

 Created December 2006 by Marko Makela
 *******************************************************/

#include "buf0buddy.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "dict0dict.h"

#include "page0zip.h"

/** When freeing a buf we attempt to coalesce by looking at its buddy
and deciding whether it is free or not. To ascertain if the buddy is
free we look for BUF_BUDDY_STAMP_FREE at BUF_BUDDY_STAMP_OFFSET
within the buddy. The question is how we can be sure that it is
safe to look at BUF_BUDDY_STAMP_OFFSET.
The answer lies in following invariants:
* All blocks allocated by buddy allocator are used for compressed
page frame.
* A compressed table always have space_id < dict_sys_t::s_log_space_id
* BUF_BUDDY_STAMP_OFFSET always points to the space_id field in
a frame.
  -- The above is true because we look at these fields when the
     corresponding buddy block is free which implies that:
     - The block we are looking at must have an address aligned at
       the same size that its free buddy has. For example, if we have
       a free block of 8K then its buddy's address must be aligned at
       8K as well.
     - It is possible that the block we are looking at may have been
       further divided into smaller sized blocks but its starting
       address must still remain the start of a page frame i.e.: it
       cannot be middle of a block. For example, if we have a free
       block of size 8K then its buddy may be divided into blocks
       of, say, 1K, 1K, 2K, 4K but the buddy's address will still be
       the starting address of first 1K compressed page.
     - What is important to note is that for any given block, the
       buddy's address cannot be in the middle of a larger block i.e.:
       in above example, our 8K block cannot have a buddy whose address
       is aligned on 8K but it is part of a larger 16K block.
*/

/** Offset within buf_buddy_free_t where free or non_free stamps
are written.*/
constexpr uint32_t BUF_BUDDY_STAMP_OFFSET = FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID;

/** Value that we stamp on all buffers that are currently on the zip_free
list. This value is stamped at BUF_BUDDY_STAMP_OFFSET offset */
constexpr uint64_t BUF_BUDDY_STAMP_FREE = dict_sys_t::s_log_space_id;

/** Stamp value for non-free buffers. Will be overwritten by a non-zero
value by the consumer of the block */
[[maybe_unused]] constexpr uint64_t BUF_BUDDY_STAMP_NONFREE = 0XFFFFFFFFUL;

/** Return type of buf_buddy_is_free() */
enum buf_buddy_state_t {
  BUF_BUDDY_STATE_FREE,          /*!< If the buddy to completely free */
  BUF_BUDDY_STATE_USED,          /*!< Buddy currently in used */
  BUF_BUDDY_STATE_PARTIALLY_USED /*!< Some sub-blocks in the buddy
                           are in use */
};

#ifdef UNIV_DEBUG_VALGRIND
/** Invalidate memory area that we won't access while page is free */
static inline void buf_buddy_mem_invalid(
    buf_buddy_free_t *buf, /*!< in: block to check */
    ulint i)               /*!< in: index of zip_free[] */
{
  const size_t size = BUF_BUDDY_LOW << i;
  ut_ad(i <= BUF_BUDDY_SIZES);

  UNIV_MEM_ASSERT_W(buf, size);
  UNIV_MEM_INVALID(buf, size);
}
#else  /* UNIV_DEBUG_VALGRIND */
static inline void buf_buddy_mem_invalid(buf_buddy_free_t *, ulint i) {
  ut_ad(i <= BUF_BUDDY_SIZES);
}
#endif /* UNIV_DEBUG_VALGRIND */

/** Check if a buddy is stamped free.
 @return whether the buddy is free */
[[nodiscard]] static inline bool buf_buddy_stamp_is_free(
    const buf_buddy_free_t *buf) /*!< in: block to check */
{
  return (mach_read_from_4(buf->stamp.bytes + BUF_BUDDY_STAMP_OFFSET) ==
          BUF_BUDDY_STAMP_FREE);
}

/** Stamps a buddy free. */
static inline void buf_buddy_stamp_free(
    buf_buddy_free_t *buf, /*!< in/out: block to stamp */
    ulint i)               /*!< in: block size */
{
  ut_d(memset(&buf->stamp, static_cast<int>(i), BUF_BUDDY_LOW << i));
  buf_buddy_mem_invalid(buf, i);
  mach_write_to_4(buf->stamp.bytes + BUF_BUDDY_STAMP_OFFSET,
                  BUF_BUDDY_STAMP_FREE);
  buf->stamp.size = i;
}

/** Stamps a buddy nonfree.
 @param[in,out] buf     block to stamp
 @param[in]     i       block size */
static inline void buf_buddy_stamp_nonfree(buf_buddy_free_t *buf, ulint i) {
  buf_buddy_mem_invalid(buf, i);
  memset(buf->stamp.bytes + BUF_BUDDY_STAMP_OFFSET, 0xff, 4);
}

/** Get the offset of the buddy of a compressed page frame.
 @return the buddy relative of page */
static inline void *buf_buddy_get(byte *page, /*!< in: compressed page */
                                  ulint size) /*!< in: page size in bytes */
{
  ut_ad(ut_is_2pow(size));
  ut_ad(size >= BUF_BUDDY_LOW);
  static_assert(BUF_BUDDY_LOW <= UNIV_ZIP_SIZE_MIN);
  ut_ad(size < BUF_BUDDY_HIGH);
  ut_ad(BUF_BUDDY_HIGH == UNIV_PAGE_SIZE);
  ut_ad(!ut_align_offset(page, size));

  if (((ulint)page) & size) {
    return (page - size);
  } else {
    return (page + size);
  }
}

#ifdef UNIV_DEBUG
/** Validate a given zip_free list. */
struct CheckZipFree {
  CheckZipFree(ulint i) : m_i(i) {}

  void operator()(const buf_buddy_free_t *elem) const {
    ut_a(buf_buddy_stamp_is_free(elem));
    ut_a(elem->stamp.size <= m_i);
  }

  ulint m_i;
};

/** Validate a buddy list.
@param[in]      buf_pool        buffer pool instance
@param[in]      i               buddy size to validate */
static void buf_buddy_list_validate(const buf_pool_t *buf_pool, ulint i) {
  CheckZipFree check(i);
  ut_ad(mutex_own(&buf_pool->zip_free_mutex));
  ut_list_validate(buf_pool->zip_free[i], check);
}

/** Debug function to validate that a buffer is indeed free i.e.: in the
zip_free[].
@param[in]      buf_pool        buffer pool instance
@param[in]      buf             block to check
@param[in]      i               index of buf_pool->zip_free[]
@return true if free */
static inline bool buf_buddy_check_free(buf_pool_t *buf_pool,
                                        const buf_buddy_free_t *buf, ulint i) {
  const ulint size = BUF_BUDDY_LOW << i;

  ut_ad(mutex_own(&buf_pool->zip_free_mutex));
  ut_ad(!ut_align_offset(buf, size));
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN));

  for (auto itr : buf_pool->zip_free[i]) {
    if (itr == buf) return true;
  }

  return false;
}
#endif /* UNIV_DEBUG */

/** Checks if a buf is free i.e.: in the zip_free[].
 @retval BUF_BUDDY_STATE_FREE if fully free
 @retval BUF_BUDDY_STATE_USED if currently in use
 @retval BUF_BUDDY_STATE_PARTIALLY_USED if partially in use. */
[[nodiscard]] static buf_buddy_state_t buf_buddy_is_free(
    buf_buddy_free_t *buf, /*!< in: block to check */
    ulint i)               /*!< in: index of
                           buf_pool->zip_free[] */
{
#ifdef UNIV_DEBUG
  const ulint size = BUF_BUDDY_LOW << i;
  ut_ad(!ut_align_offset(buf, size));
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN));
#endif /* UNIV_DEBUG */

  /* We assume that all memory from buf_buddy_alloc()
  is used for compressed page frames. */

  /* We look inside the allocated objects returned by
  buf_buddy_alloc() and assume that each block is a compressed
  page that contains one of the following in space_id.
  * BUF_BUDDY_STAMP_FREE if the block is in a zip_free list or
  * BUF_BUDDY_STAMP_NONFREE if the block has been allocated but
  not initialized yet or
  * A valid space_id of a compressed tablespace

  The call below attempts to read from free memory.  The memory
  is "owned" by the buddy allocator (and it has been allocated
  from the buffer pool), so there is nothing wrong about this. */
  if (!buf_buddy_stamp_is_free(buf)) {
    return (BUF_BUDDY_STATE_USED);
  }

  /* A block may be free but a fragment of it may still be in use.
  To guard against that we write the free block size in terms of
  zip_free index at start of stamped block. Note that we can
  safely rely on this value only if the buf is free. */
  ut_ad(buf->stamp.size <= i);
  return (buf->stamp.size == i ? BUF_BUDDY_STATE_FREE
                               : BUF_BUDDY_STATE_PARTIALLY_USED);
}

/** Add a block to the head of the appropriate buddy free list.
@param[in]      buf_pool        buffer pool instance
@param[in,out]  buf             block to be freed
@param[in]      i               index of buf_pool->zip_free[] */
static inline void buf_buddy_add_to_free(buf_pool_t *buf_pool,
                                         buf_buddy_free_t *buf, ulint i) {
  ut_ad(mutex_own(&buf_pool->zip_free_mutex));
  ut_ad(buf_pool->zip_free[i].first_element != buf);

  buf_buddy_stamp_free(buf, i);
  UT_LIST_ADD_FIRST(buf_pool->zip_free[i], buf);
  ut_d(buf_buddy_list_validate(buf_pool, i));
}

/** Remove a block from the appropriate buddy free list.
@param[in]      buf_pool        buffer pool instance
@param[in,out]  buf             block to be freed
@param[in]      i               index of buf_pool->zip_free[] */
// 从适当的伙伴空闲列表中移除一个块。
// @param[in]      buf_pool        缓冲池实例
// @param[in,out]  buf             要释放的块
// @param[in]      i               buf_pool->zip_free[] 的索引
static inline void buf_buddy_remove_from_free(buf_pool_t *buf_pool,
                                              buf_buddy_free_t *buf, ulint i) {
  ut_ad(mutex_own(&buf_pool->zip_free_mutex)); // 断言当前线程拥有 zip_free_mutex
  ut_ad(buf_buddy_check_free(buf_pool, buf, i)); // 断言 buf 在空闲列表中

  UT_LIST_REMOVE(buf_pool->zip_free[i], buf); // 从 zip_free[i] 列表中移除 buf
  buf_buddy_stamp_nonfree(buf, i); // 标记 buf 为非空闲状态
}

/** Try to allocate a block from buf_pool->zip_free[].
@param[in]      buf_pool        buffer pool instance
@param[in]      i               index of buf_pool->zip_free[]
@return allocated block, or NULL if buf_pool->zip_free[] was empty */
static buf_buddy_free_t *buf_buddy_alloc_zip(buf_pool_t *buf_pool, ulint i) {
  buf_buddy_free_t *buf;

  ut_a(i < BUF_BUDDY_SIZES);
  ut_a(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN));

  mutex_enter(&buf_pool->zip_free_mutex);
  ut_d(buf_buddy_list_validate(buf_pool, i));

  buf = UT_LIST_GET_FIRST(buf_pool->zip_free[i]);

  if (buf_get_withdraw_depth(buf_pool)) {
    while (buf != nullptr &&
           buf_frame_will_withdrawn(buf_pool, reinterpret_cast<byte *>(buf))) {
      /* This should be withdrawn, not to be allocated */
      buf = UT_LIST_GET_NEXT(list, buf);
    }
  }

  if (buf) {
    buf_buddy_remove_from_free(buf_pool, buf, i);
    mutex_exit(&buf_pool->zip_free_mutex);

  } else if (i + 1 < BUF_BUDDY_SIZES) {
    mutex_exit(&buf_pool->zip_free_mutex);
    /* Attempt to split. */
    buf = buf_buddy_alloc_zip(buf_pool, i + 1);

    if (buf) {
      byte *allocated_block = buf->stamp.bytes;
      buf_buddy_free_t *buddy = reinterpret_cast<buf_buddy_free_t *>(
          allocated_block + (BUF_BUDDY_LOW << i));

      mutex_enter(&buf_pool->zip_free_mutex);
      ut_ad(!buf_pool_contains_zip(buf_pool, buddy));
      buf_buddy_add_to_free(buf_pool, buddy, i);
      mutex_exit(&buf_pool->zip_free_mutex);
    }
  } else {
    mutex_exit(&buf_pool->zip_free_mutex);
  }

  if (buf) {
    /* Trash the page other than the BUF_BUDDY_STAMP_NONFREE. */
    UNIV_MEM_TRASH(buf->stamp.bytes, ~i, BUF_BUDDY_STAMP_OFFSET);
    UNIV_MEM_TRASH(BUF_BUDDY_STAMP_OFFSET + 4 + buf->stamp.bytes, ~i,
                   (BUF_BUDDY_LOW << i) - (BUF_BUDDY_STAMP_OFFSET + 4));
    ut_ad(mach_read_from_4(buf->stamp.bytes + BUF_BUDDY_STAMP_OFFSET) ==
          BUF_BUDDY_STAMP_NONFREE);
  }

  return (buf);
}

/** Deallocate a buffer frame of UNIV_PAGE_SIZE.
@param[in]      buf_pool        buffer pool instance
@param[in]      buf             buffer frame to deallocate */
static void buf_buddy_block_free(buf_pool_t *buf_pool, void *buf) {
  const auto hash_value = buf_pool_hash_zip_frame(buf);
  buf_page_t *bpage;

  ut_ad(!mutex_own(&buf_pool->zip_mutex));
  ut_a(!ut_align_offset(buf, UNIV_PAGE_SIZE));

  mutex_enter(&buf_pool->zip_hash_mutex);

  HASH_SEARCH(hash, buf_pool->zip_hash, hash_value, buf_page_t *, bpage,
              ut_ad(buf_page_get_state(bpage) == BUF_BLOCK_MEMORY &&
                    bpage->in_zip_hash && !bpage->in_page_hash),
              ((buf_block_t *)bpage)->frame == buf);
  ut_a(bpage);
  ut_a(buf_page_get_state(bpage) == BUF_BLOCK_MEMORY);
  ut_ad(!bpage->in_page_hash);
  ut_ad(bpage->in_zip_hash);
  ut_d(bpage->in_zip_hash = false);
  HASH_DELETE(buf_page_t, hash, buf_pool->zip_hash, hash_value, bpage);

  ut_ad(buf_pool->buddy_n_frames > 0);
  ut_d(buf_pool->buddy_n_frames--);

  mutex_exit(&buf_pool->zip_hash_mutex);

  ut_d(memset(buf, 0, UNIV_PAGE_SIZE));
  UNIV_MEM_INVALID(buf, UNIV_PAGE_SIZE);

  buf_LRU_block_free_non_file_page(reinterpret_cast<buf_block_t *>(bpage));
}

/** Allocate a buffer block to the buddy allocator.
@param[in]      block   buffer frame to allocate */
static void buf_buddy_block_register(buf_block_t *block) {
  buf_pool_t *buf_pool = buf_pool_from_block(block);
  const auto hash_value = buf_pool_hash_zip(block);
  ut_ad(!mutex_own(&buf_pool->zip_mutex));
  ut_ad(buf_block_get_state(block) == BUF_BLOCK_READY_FOR_USE);

  buf_block_set_state(block, BUF_BLOCK_MEMORY);

  ut_a(block->frame);
  ut_a(!ut_align_offset(block->frame, UNIV_PAGE_SIZE));

  ut_ad(!block->page.in_page_hash);
  ut_ad(!block->page.in_zip_hash);
  ut_d(block->page.in_zip_hash = true);

  mutex_enter(&buf_pool->zip_hash_mutex);
  HASH_INSERT(buf_page_t, hash, buf_pool->zip_hash, hash_value, &block->page);

  ut_d(buf_pool->buddy_n_frames++);
  mutex_exit(&buf_pool->zip_hash_mutex);
}

/** Allocate a block from a bigger object.
@param[in]      buf_pool        buffer pool instance
@param[in]      buf             a block that is free to use
@param[in]      i               index of buf_pool->zip_free[]
@param[in]      j               size of buf as an index of buf_pool->zip_free[]
@return allocated block */
static void *buf_buddy_alloc_from(buf_pool_t *buf_pool, void *buf, ulint i,
                                  ulint j) {
  ulint offs = BUF_BUDDY_LOW << j;
  ut_ad(mutex_own(&buf_pool->zip_free_mutex));
  ut_ad(j <= BUF_BUDDY_SIZES);
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN));
  ut_ad(j >= i);
  ut_ad(!ut_align_offset(buf, offs));

  /* Add the unused parts of the block to the free lists. */
  while (j > i) {
    buf_buddy_free_t *zip_buf;

    offs >>= 1;
    j--;

    zip_buf = reinterpret_cast<buf_buddy_free_t *>(
        reinterpret_cast<byte *>(buf) + offs);
    buf_buddy_add_to_free(buf_pool, zip_buf, j);
  }

  buf_buddy_stamp_nonfree(reinterpret_cast<buf_buddy_free_t *>(buf), i);
  return (buf);
}

/** Allocate a block.
@param[in,out]  buf_pool        buffer pool instance
@param[in]      i               index of buf_pool->zip_free[]
                                or BUF_BUDDY_SIZES
@return allocated block, never NULL */
void *buf_buddy_alloc_low(buf_pool_t *buf_pool, ulint i) {
  buf_block_t *block;

  ut_ad(!mutex_own(&buf_pool->zip_mutex));
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN));

  if (i < BUF_BUDDY_SIZES) {
    /* Try to allocate from the buddy system. */
    block = (buf_block_t *)buf_buddy_alloc_zip(buf_pool, i);

    if (block) {
      goto func_exit;
    }
  }

  /* Try allocating from the buf_pool->free list. */
  block = buf_LRU_get_free_only(buf_pool);

  if (block) {
    goto alloc_big;
  }

  /* Try replacing an uncompressed page in the buffer pool. */
  block = buf_LRU_get_free_block(buf_pool);

alloc_big:
  buf_buddy_block_register(block);

  mutex_enter(&buf_pool->zip_free_mutex);
  block = (buf_block_t *)buf_buddy_alloc_from(buf_pool, block->frame, i,
                                              BUF_BUDDY_SIZES);
  mutex_exit(&buf_pool->zip_free_mutex);

func_exit:
  buf_pool->buddy_stat[i].used.fetch_add(1);
  return (block);
}

/** Try to relocate a block. The caller must hold zip_free_mutex, and this
function will release and lock it again.
@param[in]      buf_pool        buffer pool instance
@param[in]      src             block to relocate
@param[in]      dst             free block to relocated to
@param[in]      i               index of buf_pool->zip_free[]
@param[in]      force           true if we must relocated always
@return true if relocated */
// 尝试重新定位一个块。调用者必须持有 zip_free_mutex，本函数会释放并重新锁定它。
// @param[in]      buf_pool        缓冲池实例
// @param[in]      src             要重新定位的块
// @param[in]      dst             要重新定位到的空闲块
// @param[in]      i               buf_pool->zip_free[] 的索引
// @param[in]      force           如果必须总是重新定位，则为 true
// @return 如果重新定位成功，则为 true
static bool buf_buddy_relocate(buf_pool_t *buf_pool, void *src, void *dst,
                               ulint i, bool force) {
  buf_page_t *bpage; // 定义缓冲页面指针
  const ulint size = BUF_BUDDY_LOW << i; // 计算块大小
  space_id_t space; // 定义空间 ID
  page_no_t offset; // 定义页偏移量

  ut_ad(mutex_own(&buf_pool->zip_free_mutex)); // 断言当前线程拥有 zip_free_mutex
  ut_ad(!mutex_own(&buf_pool->zip_mutex)); // 断言当前线程不拥有 zip_mutex
  ut_ad(!ut_align_offset(src, size)); // 断言 src 地址对齐
  ut_ad(!ut_align_offset(dst, size)); // 断言 dst 地址对齐
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN)); // 断言索引大于等于最小压缩大小的插槽索引
  UNIV_MEM_ASSERT_W(dst, size); // 断言 dst 内存有效

  space =
      mach_read_from_4((const byte *)src + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID); // 从 src 读取空间 ID
  offset = mach_read_from_4((const byte *)src + FIL_PAGE_OFFSET); // 从 src 读取页偏移量

  /* Suppress Valgrind warnings about conditional jump
  on uninitialized value. */
  // 抑制 Valgrind 关于条件跳转未初始化值的警告
  UNIV_MEM_VALID(&space, sizeof space); // 断言 space 内存有效
  UNIV_MEM_VALID(&offset, sizeof offset); // 断言 offset 内存有效

  ut_ad(space != BUF_BUDDY_STAMP_FREE); // 断言空间 ID 不等于 BUF_BUDDY_STAMP_FREE

  const page_id_t page_id(space, offset); // 创建页面 ID

  /* If space,offset is bogus, then we know that the
  buf_page_hash_get_low() call below will return NULL. */
  // 如果空间 ID 和偏移量无效，则 buf_page_hash_get_low() 调用将返回 NULL
  if (!force && buf_pool != buf_pool_get(page_id)) {
    return (false); // 返回 false
  }

  mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free_mutex

  rw_lock_t *hash_lock = buf_page_hash_lock_get(buf_pool, page_id); // 获取哈希锁

  rw_lock_x_lock(hash_lock, UT_LOCATION_HERE); // 加锁

  /* page_hash can be changed. */
  // page_hash 可能已更改
  hash_lock = buf_page_hash_lock_x_confirm(hash_lock, buf_pool, page_id); // 确认哈希锁

  bpage = buf_page_hash_get_low(buf_pool, page_id); // 获取缓冲页面

  if (!bpage || bpage->zip.data != src) {
    /* The block has probably been freshly
    allocated by buf_LRU_get_free_block() but not
    added to buf_pool->page_hash yet.  Obviously,
    it cannot be relocated. */
    // 该块可能是由 buf_LRU_get_free_block() 新分配的，但尚未添加到 buf_pool->page_hash 中。
    // 显然，它不能被重新定位。

    rw_lock_x_unlock(hash_lock); // 解锁

    if (!force || space != 0 || offset != 0) {
      mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free_mutex
      return (false); // 返回 false
    }

    /* It might be just uninitialized page.
    We should search from LRU list also. */
    // 它可能只是未初始化的页面。我们还应该从 LRU 列表中搜索。

    /* force is true only when buffer pool resizing,
    in which we hold LRU_list_mutex already, see
    buf_pool_withdraw_blocks(). */
    // force 仅在缓冲池调整大小时为 true，此时我们已经持有 LRU_list_mutex，参见 buf_pool_withdraw_blocks()。
    ut_ad(force);
    ut_ad(mutex_own(&buf_pool->LRU_list_mutex)); // 断言当前线程拥有 LRU_list_mutex

    bpage = UT_LIST_GET_FIRST(buf_pool->LRU); // 获取 LRU 列表中的第一个页面
    while (bpage != nullptr) {
      if (bpage->zip.data == src) {
        hash_lock = buf_page_hash_lock_get(buf_pool, bpage->id); // 获取哈希锁
        rw_lock_x_lock(hash_lock, UT_LOCATION_HERE); // 加锁
        break;
      }
      bpage = UT_LIST_GET_NEXT(LRU, bpage); // 获取下一个页面
    }

    if (bpage == nullptr) {
      mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free_mutex
      return (false); // 返回 false
    }
  }

  if (page_zip_get_size(&bpage->zip) != size) {
    /* The block is of different size.  We would
    have to relocate all blocks covered by src.
    For the sake of simplicity, give up. */
    // 该块大小不同。我们将不得不重新定位 src 覆盖的所有块。为了简单起见，放弃。
    ut_ad(page_zip_get_size(&bpage->zip) < size);

    rw_lock_x_unlock(hash_lock); // 解锁

    mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free_mutex
    return (false); // 返回 false
  }

  /* The block must have been allocated, but it may
  contain uninitialized data. */
  // 该块必须已分配，但可能包含未初始化的数据。
  UNIV_MEM_ASSERT_W(src, size); // 断言 src 内存有效

  BPageMutex *block_mutex = buf_page_get_mutex(bpage); // 获取页面互斥锁

  mutex_enter(block_mutex); // 进入页面互斥锁

  mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free_mutex

  if (buf_page_can_relocate(bpage)) {
    /* Relocate the compressed page. */
    // 重新定位压缩页面
    const auto start_time = std::chrono::steady_clock::now(); // 获取当前时间

    ut_a(bpage->zip.data == src); // 断言页面数据等于 src

    memcpy(dst, src, size); // 复制数据到 dst
    bpage->zip.data = reinterpret_cast<page_zip_t *>(dst); // 更新页面数据指针

    rw_lock_x_unlock(hash_lock); // 解锁

    mutex_exit(block_mutex); // 退出页面互斥锁

    buf_buddy_mem_invalid(reinterpret_cast<buf_buddy_free_t *>(src), i); // 标记 src 内存无效

    buf_buddy_stat_t *buddy_stat = &buf_pool->buddy_stat[i]; // 获取伙伴统计信息
    buddy_stat->relocated++; // 增加重新定位计数
    buddy_stat->relocated_duration +=
        std::chrono::steady_clock::now() - start_time; // 增加重新定位持续时间
    return (true); // 返回 true
  }

  rw_lock_x_unlock(hash_lock); // 解锁

  mutex_exit(block_mutex); // 退出页面互斥锁
  return (false); // 返回 false
}


/** Deallocate a block.
@param[in]      buf_pool        buffer pool instance
@param[in]      buf             block to be freed, must not be pointed to
                                by the buffer pool
@param[in]      i               index of buf_pool->zip_free[],
                                or BUF_BUDDY_SIZES
@param[in]      has_zip_free    whether has zip_free_mutex */
// 释放一个块。
// @param[in]      buf_pool        缓冲池实例
// @param[in]      buf             要释放的块，不能被缓冲池指向
// @param[in]      i               buf_pool->zip_free[] 的索引，或 BUF_BUDDY_SIZES
// @param[in]      has_zip_free    是否持有 zip_free_mutex
void buf_buddy_free_low(buf_pool_t *buf_pool, void *buf, ulint i,
                        bool has_zip_free) {
  buf_buddy_free_t *buddy;

  ut_ad(!mutex_own(&buf_pool->zip_mutex)); // 断言当前线程不拥有 zip_mutex
  ut_ad(i <= BUF_BUDDY_SIZES); // 断言索引小于等于 BUF_BUDDY_SIZES
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN)); // 断言索引大于等于最小压缩大小的插槽索引

  if (!has_zip_free) {
    mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free 互斥锁
  }

  ut_ad(mutex_own(&buf_pool->zip_free_mutex)); // 断言当前线程拥有 zip_free_mutex
  ut_ad(buf_pool->buddy_stat[i].used > 0); // 断言使用计数大于 0
  buf_pool->buddy_stat[i].used.fetch_sub(1); // 使用计数减一
recombine:
  UNIV_MEM_ASSERT_AND_ALLOC(buf, BUF_BUDDY_LOW << i); // 断言并分配内存

  if (i == BUF_BUDDY_SIZES) {
    if (!has_zip_free) {
      mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free 互斥锁
    }
    buf_buddy_block_free(buf_pool, buf); // 释放块
    return;
  }

  ut_ad(i < BUF_BUDDY_SIZES); // 断言索引小于 BUF_BUDDY_SIZES
  ut_ad(buf == ut_align_down(buf, BUF_BUDDY_LOW << i)); // 断言 buf 地址对齐
  ut_ad(!buf_pool_contains_zip(buf_pool, buf)); // 断言缓冲池不包含 buf

  /* Do not recombine blocks if there are few free blocks.
  We may waste up to 15360*max_len bytes to free blocks
  (1024 + 2048 + 4096 + 8192 = 15360) */
  // 如果空闲块很少，不要重新组合块。我们可能会浪费最多 15360*max_len 字节的空闲块（1024 + 2048 + 4096 + 8192 = 15360）
  if (UT_LIST_GET_LEN(buf_pool->zip_free[i]) < 16 &&
      buf_pool->curr_size >= buf_pool->old_size) {
    goto func_exit;
  }

  /* Try to combine adjacent blocks. */
  // 尝试合并相邻的块
  buddy = reinterpret_cast<buf_buddy_free_t *>(
      buf_buddy_get(reinterpret_cast<byte *>(buf), BUF_BUDDY_LOW << i)); // 获取伙伴块

  switch (buf_buddy_is_free(buddy, i)) {
    case BUF_BUDDY_STATE_FREE:
      /* The buddy is free: recombine */
      // 伙伴块是空闲的：重新组合
      buf_buddy_remove_from_free(buf_pool, buddy, i); // 从空闲列表中移除伙伴块
    buddy_is_free:
      ut_ad(!buf_pool_contains_zip(buf_pool, buddy)); // 断言缓冲池不包含伙伴块
      i++;
      buf = ut_align_down(buf, BUF_BUDDY_LOW << i); // 地址对齐

      goto recombine;

    case BUF_BUDDY_STATE_USED:
      ut_d(buf_buddy_list_validate(buf_pool, i)); // 验证伙伴列表

      /* The buddy is not free. Is there a free block of
      this size? */
      // 伙伴块不是空闲的。是否有这个大小的空闲块？
      if (buf_buddy_free_t *zip_buf =
              UT_LIST_GET_FIRST(buf_pool->zip_free[i])) {
        /* Remove the block from the free list, because
        a successful buf_buddy_relocate() will overwrite
        zip_free->list. */
        // 从空闲列表中移除块，因为成功的 buf_buddy_relocate() 会覆盖 zip_free->list。
        buf_buddy_remove_from_free(buf_pool, zip_buf, i);

        /* Try to relocate the buddy of buf to the free
        block. */
        // 尝试将 buf 的伙伴重新定位到空闲块。
        if (buf_buddy_relocate(buf_pool, buddy, zip_buf, i, false)) {
          goto buddy_is_free;
        }

        buf_buddy_add_to_free(buf_pool, zip_buf, i); // 将块添加到空闲列表
      }

      break;
    case BUF_BUDDY_STATE_PARTIALLY_USED:
      /* Some sub-blocks in the buddy are still in use.
      Relocation will fail. No need to try. */
      // 伙伴中的一些子块仍在使用中。重新定位将失败。无需尝试。
      break;
  }

func_exit:
  /* Free the block to the buddy list. */
  // 将块释放到伙伴列表。
  buf_buddy_add_to_free(buf_pool, reinterpret_cast<buf_buddy_free_t *>(buf), i);
  if (!has_zip_free) {
    mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free 互斥锁
  }
}

/** Try to reallocate a block.
@param[in]      buf_pool        buffer pool instance
@param[in]      buf             block to be reallocated, must be pointed
to by the buffer pool
@param[in]      size            block size, up to UNIV_PAGE_SIZE
@retval true    if succeeded or if failed because the block was fixed
@retval false   if failed because of no free blocks. */
// 尝试重新分配一个块。
// @param[in]      buf_pool        缓冲池实例
// @param[in]      buf             要重新分配的块，必须由缓冲池指向
// @param[in]      size            块大小，最大为 UNIV_PAGE_SIZE
// @retval true    如果成功或因块被固定而失败
// @retval false   如果因没有空闲块而失败
bool buf_buddy_realloc(buf_pool_t *buf_pool, void *buf, ulint size) {
  buf_block_t *block = nullptr; // 定义缓冲块指针并初始化为 nullptr
  ulint i = buf_buddy_get_slot(size); // 获取块大小对应的插槽索引

  ut_ad(!mutex_own(&buf_pool->zip_mutex)); // 断言当前线程不拥有 zip_mutex
  ut_ad(i <= BUF_BUDDY_SIZES); // 断言插槽索引小于等于 BUF_BUDDY_SIZES
  ut_ad(i >= buf_buddy_get_slot(UNIV_ZIP_SIZE_MIN)); // 断言插槽索引大于等于最小压缩大小的插槽索引

  if (i < BUF_BUDDY_SIZES) {
    /* Try to allocate from the buddy system. */
    // 尝试从伙伴系统分配
    block = reinterpret_cast<buf_block_t *>(buf_buddy_alloc_zip(buf_pool, i)); // 从伙伴系统分配块
  }

  if (block == nullptr) {
    /* Try allocating from the buf_pool->free list if it is not empty. This
    method is executed during withdrawing phase of BufferPool resize only. It is
    better to not block other user threads as much as possible. So, the main
    strategy is to passively reserve and use blocks that are already on the free
    list. Otherwise, if we were to call `buf_LRU_get_free_block` instead of
    `buf_LRU_get_free_only`, we would have to release the LRU mutex before the
    call and this would cause a need to break the reallocation loop in
    `buf_pool_withdraw_blocks`, which would render withdrawing even more
    inefficient. */
    // 尝试从 buf_pool->free 列表分配块（如果不为空）。此方法仅在缓冲池调整大小的撤回阶段执行。
    // 最好尽量不要阻塞其他用户线程。因此，主要策略是被动地保留和使用已经在 free 列表中的块。
    // 否则，如果我们调用 `buf_LRU_get_free_block` 而不是 `buf_LRU_get_free_only`，我们将不得不在调用前释放 LRU 互斥锁，
    // 这将导致需要中断 `buf_pool_withdraw_blocks` 中的重新分配循环，从而使撤回效率更低。
    block = buf_LRU_get_free_only(buf_pool); // 从 LRU 列表中获取空闲块

    if (block == nullptr) {
      return (false); /* free_list was not enough */
      // free_list 不够
    }

    buf_buddy_block_register(block); // 注册块

    mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free 互斥锁
    block = reinterpret_cast<buf_block_t *>(
        buf_buddy_alloc_from(buf_pool, block->frame, i, BUF_BUDDY_SIZES)); // 从伙伴系统分配块
  } else {
    mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free 互斥锁
  }

  buf_pool->buddy_stat[i].used.fetch_add(1); // 增加使用计数

  /* Try to relocate the buddy of buf to the free block. */
  // 尝试将 buf 的伙伴重新定位到空闲块
  if (buf_buddy_relocate(buf_pool, buf, block, i, true)) {
    mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free 互斥锁
    /* succeeded */
    // 成功
    buf_buddy_free_low(buf_pool, buf, i, false); // 释放低级伙伴块
    return (true);
  }

  /* failed */
  // 失败
  mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free 互斥锁
  buf_buddy_free_low(buf_pool, block, i, false); // 释放低级伙伴块

  return (false);
}


/*
在 InnoDB 的缓冲池管理中，zip_free 是一个数组，用于管理压缩页面的空闲块。每个元素都是一个链表，存储了相同大小的空闲块。
成对的空闲伙伴块（free buddies）是指两个相邻的、大小相同的空闲块，它们可以合并成一个更大的空闲块。

伙伴系统（Buddy System）
伙伴系统是一种内存分配算法，用于减少内存碎片。它将内存划分为大小为 2 的幂次方的块，并通过合并相邻的空闲块来形成更大的块。
这样可以有效地管理内存，减少内存碎片。

zip_free 数组
zip_free 数组中的每个元素都是一个链表，存储了相同大小的空闲块。数组的索引表示块的大小。
例如，索引 0 表示大小为 BUF_BUDDY_LOW 的块，索引 1 表示大小为 BUF_BUDDY_LOW * 2 的块，依此类推。

合并空闲伙伴块的原因
合并空闲伙伴块的主要目的是减少内存碎片，提高内存使用效率。
通过合并相邻的空闲块，可以形成更大的连续内存块，从而更有效地利用内存资源。

合并相邻的空闲块并不会直接减少内存的总量，但它可以显著提高内存的使用效率，减少内存碎片，从而间接地优化内存的利用。

内存碎片
内存碎片是指内存中分散的小块空闲区域，这些区域可能无法被有效利用。内存碎片会导致内存的浪费，因为虽然总的空闲内存量可能足够，但由于这些空闲内存是分散的，无法满足较大块内存的分配需求。

合并空闲块的好处
减少内存碎片: 通过合并相邻的空闲块，可以将分散的小块内存合并成较大的连续内存块，从而减少内存碎片。
提高内存利用率: 合并后的较大内存块可以更好地满足较大内存分配的需求，提高内存的利用率。
优化内存分配: 合并空闲块可以使内存分配更加高效，减少内存分配和释放的开销。
*/
/** Combine all pairs of free buddies.
@param[in]      buf_pool        buffer pool instance */
// 合并所有成对的空闲伙伴块。
// @param[in]      buf_pool        缓冲池实例
void buf_buddy_condense_free(buf_pool_t *buf_pool) {
  mutex_enter(&buf_pool->zip_free_mutex); // 进入 zip_free 互斥锁
  ut_ad(buf_pool->curr_size < buf_pool->old_size); // 断言当前大小小于旧大小

  for (ulint i = 0; i < UT_ARR_SIZE(buf_pool->zip_free); ++i) { // 遍历 zip_free 数组
    buf_buddy_free_t *buf = UT_LIST_GET_FIRST(buf_pool->zip_free[i]); // 获取 zip_free 列表中的第一个元素

    /* seek to withdraw target */
    // 寻找撤回目标
    while (buf != nullptr &&
           !buf_frame_will_withdrawn(buf_pool, reinterpret_cast<byte *>(buf))) { // 如果 buf 不为空且不会被撤回
      buf = UT_LIST_GET_NEXT(list, buf); // 获取下一个元素
    }

    while (buf != nullptr) { // 如果 buf 不为空
      buf_buddy_free_t *next = UT_LIST_GET_NEXT(list, buf); // 获取下一个元素

      buf_buddy_free_t *buddy = reinterpret_cast<buf_buddy_free_t *>(
          buf_buddy_get(reinterpret_cast<byte *>(buf), BUF_BUDDY_LOW << i)); // 获取伙伴块

      /* seek to the next withdraw target */
      // 寻找下一个撤回目标
      while (true) {
        while (next != nullptr &&
               !buf_frame_will_withdrawn(buf_pool,
                                         reinterpret_cast<byte *>(next))) { // 如果 next 不为空且不会被撤回
          next = UT_LIST_GET_NEXT(list, next); // 获取下一个元素
        }

        if (buddy != next) { // 如果伙伴块不是下一个元素
          break; // 跳出循环
        }

        next = UT_LIST_GET_NEXT(list, next); // 获取下一个元素
      }

      if (buf_buddy_is_free(buddy, i) == BUF_BUDDY_STATE_FREE) { // 如果 buf 和 buddy 都是空闲的
        /* Both buf and buddy are free.
        Try to combine them. */
        // 尝试合并它们
        buf_buddy_remove_from_free(buf_pool, buf, i); // 从空闲列表中移除 buf
        buf_pool->buddy_stat[i].used.fetch_add(1); // 增加使用计数

        buf_buddy_free_low(buf_pool, buf, i, true); // 释放低级伙伴块
      }

      buf = next; // 移动到下一个元素
    }
  }
  mutex_exit(&buf_pool->zip_free_mutex); // 退出 zip_free 互斥锁
}