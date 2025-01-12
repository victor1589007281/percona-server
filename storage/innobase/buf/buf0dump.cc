/*****************************************************************************

Copyright (c) 2011, 2022, Oracle and/or its affiliates.

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

/** @file buf/buf0dump.cc
 Implements a buffer pool dump/load.

 Created April 08, 2011 Vasil Dimov
 *******************************************************/

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>

#include "buf0buf.h"
#include "buf0dump.h"
#include "dict0dict.h"

#include "my_io.h"
#include "my_psi_config.h"
#include "my_sys.h"
#include "my_thread.h"
#include "mysql/psi/mysql_stage.h"
#include "os0file.h"
#include "os0thread-create.h"
#include "os0thread.h"
#include "srv0srv.h"
#include "srv0start.h"
#include "sync0rw.h"
#include "univ.i"
#include "ut0byte.h"

enum status_severity { STATUS_VERBOSE, STATUS_INFO, STATUS_ERR };

static inline bool SHUTTING_DOWN() {
  return srv_shutdown_state.load() >= SRV_SHUTDOWN_CLEANUP;
}

/* Flags that tell the buffer pool dump/load thread which action should it
take after being waked up. */
static bool buf_dump_should_start = false;
static bool buf_load_should_start = false;

static bool buf_load_abort_flag = false;

/* Used to temporary store dump info in order to avoid IO while holding
buffer pool LRU list mutex during dump and also to sort the contents of the
dump before reading the pages from disk during load.
We store the space id in the high 32 bits and page no in low 32 bits. */
typedef uint64_t buf_dump_t;

/* Aux macros to create buf_dump_t and to extract space and page from it */
inline uint64_t BUF_DUMP_CREATE(space_id_t space, page_no_t page) {
  return ut_ull_create(space, page);
}
constexpr space_id_t BUF_DUMP_SPACE(uint64_t a) {
  return static_cast<space_id_t>((a) >> 32);
}
constexpr page_no_t BUF_DUMP_PAGE(uint64_t a) {
  return static_cast<page_no_t>((a)&0xFFFFFFFFUL);
}
/** Wakes up the buffer pool dump/load thread and instructs it to start
 a dump. This function is called by MySQL code via buffer_pool_dump_now()
 and it should return immediately because the whole MySQL is frozen during
 its execution. */
void buf_dump_start() {
  buf_dump_should_start = true;
  os_event_set(srv_buf_dump_event);
}

/** Wakes up the buffer pool dump/load thread and instructs it to start
 a load. This function is called by MySQL code via buffer_pool_load_now()
 and it should return immediately because the whole MySQL is frozen during
 its execution. */
void buf_load_start() {
  buf_load_should_start = true;
  os_event_set(srv_buf_dump_event);
}

/** Sets the global variable that feeds MySQL's innodb_buffer_pool_dump_status
 to the specified string. The format and the following parameters are the
 same as the ones used for printf(3). The value of this variable can be
 retrieved by:
 SELECT variable_value FROM performance_schema.global_status WHERE
 variable_name = 'INNODB_BUFFER_POOL_DUMP_STATUS';
 or by:
 SHOW STATUS LIKE 'innodb_buffer_pool_dump_status'; */
static MY_ATTRIBUTE((format(printf, 2, 3))) void buf_dump_status(
    enum status_severity severity, /*!< in: status severity */
    const char *fmt,               /*!< in: format */
    ...)                           /*!< in: extra parameters according
                                   to fmt */
{
  va_list ap;

  va_start(ap, fmt);

  ut_vsnprintf(export_vars.innodb_buffer_pool_dump_status,
               sizeof(export_vars.innodb_buffer_pool_dump_status), fmt, ap);

  switch (severity) {
    case STATUS_INFO:
      ib::info(ER_IB_MSG_119) << export_vars.innodb_buffer_pool_dump_status;
      break;

    case STATUS_ERR:
      ib::error(ER_IB_MSG_120) << export_vars.innodb_buffer_pool_dump_status;
      break;

    case STATUS_VERBOSE:
      break;
  }

  va_end(ap);
}

/** Sets the global variable that feeds MySQL's innodb_buffer_pool_load_status
 to the specified string. The format and the following parameters are the
 same as the ones used for printf(3). The value of this variable can be
 retrieved by:
 SELECT variable_value FROM performance_schema.global_status WHERE
 variable_name = 'INNODB_BUFFER_POOL_LOAD_STATUS';
 or by:
 SHOW STATUS LIKE 'innodb_buffer_pool_load_status'; */
static MY_ATTRIBUTE((format(printf, 2, 3))) void buf_load_status(
    enum status_severity severity, /*!< in: status severity */
    const char *fmt,               /*!< in: format */
    ...)                           /*!< in: extra parameters according to fmt */
{
  va_list ap;

  va_start(ap, fmt);

  ut_vsnprintf(export_vars.innodb_buffer_pool_load_status,
               sizeof(export_vars.innodb_buffer_pool_load_status), fmt, ap);

  switch (severity) {
    case STATUS_INFO:
      ib::info(ER_IB_MSG_121) << export_vars.innodb_buffer_pool_load_status;
      break;

    case STATUS_ERR:
      ib::error(ER_IB_MSG_122) << export_vars.innodb_buffer_pool_load_status;
      break;

    case STATUS_VERBOSE:
      break;
  }

  va_end(ap);
}

/** Returns the directory path where the buffer pool dump file will be created.
@return directory path */
static const char *get_buf_dump_dir() {
  const char *dump_dir;

  /* The dump file should be created in the default data directory if
  innodb_data_home_dir is set as an empty string. */
  if (strcmp(srv_data_home, "") == 0) {
    dump_dir = MySQL_datadir_path;
  } else {
    dump_dir = srv_data_home;
  }

  return (dump_dir);
}

/** Generate the path to the buffer pool dump/load file.
@param[out]     path            generated path
@param[in]      path_size       size of 'path', used as in snprintf(3). */
void buf_dump_generate_path(char *path, size_t path_size) {
  char buf[FN_REFLEN];

  snprintf(buf, sizeof(buf), "%s%c%s", get_buf_dump_dir(), OS_PATH_SEPARATOR,
           srv_buf_dump_filename);

  /* Use this file if it exists. */
  if (os_file_exists(buf)) {
    /* my_realpath() assumes the destination buffer is big enough
    to hold FN_REFLEN bytes. */
    ut_a(path_size >= FN_REFLEN);

    my_realpath(path, buf, 0);
  } else {
    /* If it does not exist, then resolve only srv_data_home
    and append srv_buf_dump_filename to it. */
    char srv_data_home_full[FN_REFLEN];

    my_realpath(srv_data_home_full, get_buf_dump_dir(), 0);

    if (srv_data_home_full[strlen(srv_data_home_full) - 1] ==
        OS_PATH_SEPARATOR) {
      snprintf(path, path_size, "%s%s", srv_data_home_full,
               srv_buf_dump_filename);
    } else {
      snprintf(path, path_size, "%s%c%s", srv_data_home_full, OS_PATH_SEPARATOR,
               srv_buf_dump_filename);
    }
  }
}

/** Perform a buffer pool dump into the file specified by
innodb_buffer_pool_filename. If any errors occur then the value of
innodb_buffer_pool_dump_status will be set accordingly, see buf_dump_status().
The dump filename can be specified by (relative to srv_data_home):
SET GLOBAL innodb_buffer_pool_filename='filename';
@param[in]      obey_shutdown   quit if we are in a shutting down state */
static void buf_dump(bool obey_shutdown) {
#define SHOULD_QUIT() (SHUTTING_DOWN() && obey_shutdown)

  char full_filename[OS_FILE_MAX_PATH];
  char tmp_filename[OS_FILE_MAX_PATH + 11];
  char now[32];
  FILE *f;
  ulint i;
  int ret;

  buf_dump_generate_path(full_filename, sizeof(full_filename));

  snprintf(tmp_filename, sizeof(tmp_filename), "%s.incomplete", full_filename);

  buf_dump_status(STATUS_INFO, "Dumping buffer pool(s) to %s", full_filename);

  f = fopen(tmp_filename, "w");
  if (f == nullptr) {
    buf_dump_status(STATUS_ERR, "Cannot open '%s' for writing: %s",
                    tmp_filename, strerror(errno));
    return;
  }
  /* else */

  /* walk through each buffer pool */
  for (i = 0; i < srv_buf_pool_instances && !SHOULD_QUIT(); i++) {
    buf_pool_t *buf_pool;
    buf_dump_t *dump;

    buf_pool = buf_pool_from_array(i);

    /* obtain buf_pool LRU list mutex before allocate, since
    UT_LIST_GET_LEN(buf_pool->LRU) could change */
    mutex_enter(&buf_pool->LRU_list_mutex);

    size_t n_pages = UT_LIST_GET_LEN(buf_pool->LRU);

    /* skip empty buffer pools */
    if (n_pages == 0) {
      mutex_exit(&buf_pool->LRU_list_mutex);
      continue;
    }

    if (srv_buf_pool_dump_pct != 100) {
      ut_ad(srv_buf_pool_dump_pct < 100);

      n_pages = n_pages * srv_buf_pool_dump_pct / 100;

      if (n_pages == 0) {
        n_pages = 1;
      }
    }

    dump = static_cast<buf_dump_t *>(
        ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, n_pages * sizeof(*dump)));

    if (dump == nullptr) {
      mutex_exit(&buf_pool->LRU_list_mutex);
      fclose(f);
      buf_dump_status(STATUS_ERR, "Cannot allocate %zu bytes: %s",
                      n_pages * sizeof(*dump), strerror(errno));
      /* leave tmp_filename to exist */
      return;
    }
    {
      size_t j{0};
      for (auto bpage : buf_pool->LRU) {
        if (n_pages <= j) break;
        ut_a(buf_page_in_file(bpage));

        dump[j++] = BUF_DUMP_CREATE(bpage->id.space(), bpage->id.page_no());
      }

      ut_a(j == n_pages);
    }

    mutex_exit(&buf_pool->LRU_list_mutex);

    for (size_t j = 0; j < n_pages && !SHOULD_QUIT(); j++) {
      ret = fprintf(f, SPACE_ID_PF "," PAGE_NO_PF "\n", BUF_DUMP_SPACE(dump[j]),
                    BUF_DUMP_PAGE(dump[j]));
      if (ret < 0) {
        ut::free(dump);
        fclose(f);
        buf_dump_status(STATUS_ERR, "Cannot write to '%s': %s", tmp_filename,
                        strerror(errno));
        /* leave tmp_filename to exist */
        return;
      }

      if (j % 128 == 0) {
        buf_dump_status(
            STATUS_VERBOSE,
            "Dumping buffer pool " ULINTPF "/" ULINTPF ", page %zu/%zu", i + 1,
            static_cast<ulint>(srv_buf_pool_instances), j + 1, n_pages);
      }
    }

    ut::free(dump);
  }

  ret = fclose(f);
  if (ret != 0) {
    buf_dump_status(STATUS_ERR, "Cannot close '%s': %s", tmp_filename,
                    strerror(errno));
    return;
  }
  /* else */

  ret = unlink(full_filename);
  if (ret != 0 && errno != ENOENT) {
    buf_dump_status(STATUS_ERR, "Cannot delete '%s': %s", full_filename,
                    strerror(errno));
    /* leave tmp_filename to exist */
    return;
  }
  /* else */

  ret = rename(tmp_filename, full_filename);
  if (ret != 0) {
    buf_dump_status(STATUS_ERR, "Cannot rename '%s' to '%s': %s", tmp_filename,
                    full_filename, strerror(errno));
    /* leave tmp_filename to exist */
    return;
  }
  /* else */

  /* success */

  ut_sprintf_timestamp(now);

  buf_dump_status(STATUS_INFO, "Buffer pool(s) dump completed at %s", now);
}

/** Artificially delay the buffer pool loading if necessary. The idea of this
function is to prevent hogging the server with IO and slowing down too much
normal client queries.
@param[in,out]  last_check_time         milliseconds since epoch of the last
                                        time we did check if throttling is
                                        needed, we do the check every
                                        srv_io_capacity IO ops.
@param[in]      last_activity_count     activity count
@param[in]      n_io                    number of IO ops done since buffer
                                        pool load has started */
static inline void buf_load_throttle_if_needed(
    std::chrono::steady_clock::time_point *last_check_time,
    ulint *last_activity_count, ulint n_io) {
  if (n_io % srv_io_capacity < srv_io_capacity - 1) {
    return;
  }

  if (*last_check_time == std::chrono::steady_clock::time_point{} ||
      *last_activity_count == 0) {
    *last_check_time = std::chrono::steady_clock::now();
    *last_activity_count = srv_get_activity_count();
    return;
  }

  /* srv_io_capacity IO operations have been performed by buffer pool
  load since the last time we were here. */

  /* If no other activity, then keep going without any delay. */
  if (srv_get_activity_count() == *last_activity_count) {
    return;
  }

  /* There has been other activity, throttle. */

  const auto elapsed_time = std::chrono::steady_clock::now() - *last_check_time;

  /* Notice that elapsed_time is not the time for the last
  srv_io_capacity IO operations performed by BP load. It is the
  time elapsed since the last time we detected that there has been
  other activity. This has a small and acceptable deficiency, e.g.:
  1. BP load runs and there is no other activity.
  2. Other activity occurs, we run N IO operations after that and
     enter here (where 0 <= N < srv_io_capacity).
  3. last_check_time is very old and we do not sleep at this time, but
     only update last_check_time and last_activity_count.
  4. We run srv_io_capacity more IO operations and call this function
     again.
  5. There has been more other activity and thus we enter here.
  6. Now last_check_time is recent and we sleep if necessary to prevent
     more than srv_io_capacity IO operations per second.
  The deficiency is that we could have slept at 3., but for this we
  would have to update last_check_time before the
  "cur_activity_count == *last_activity_count" check and calling
  ut_time_monotonic_ms() that often may turn out to be too expensive. */

  if (elapsed_time < std::chrono::seconds{1}) {
    std::this_thread::sleep_for(std::chrono::seconds{1} - elapsed_time);
  }

  *last_check_time = std::chrono::steady_clock::now();
  *last_activity_count = srv_get_activity_count();
}

/** Perform a buffer pool load from the file specified by
 innodb_buffer_pool_filename. If any errors occur then the value of
 innodb_buffer_pool_load_status will be set accordingly, see buf_load_status().
 The dump filename can be specified by (relative to srv_data_home):
 SET GLOBAL innodb_buffer_pool_filename='filename'; */
// 从由 innodb_buffer_pool_filename 指定的文件执行缓冲池加载。
// 如果发生任何错误，则会相应地设置 innodb_buffer_pool_load_status 的值，参见 buf_load_status()。
// 转储文件名可以通过以下方式指定（相对于 srv_data_home）：
// SET GLOBAL innodb_buffer_pool_filename='filename';
static void buf_load() {
  char full_filename[OS_FILE_MAX_PATH]; // 定义完整文件名的字符数组
  char now[32]; // 定义当前时间的字符数组
  FILE *f; // 定义文件指针
  buf_dump_t *dump; // 定义缓冲区转储指针
  ulint dump_n; // 定义转储条目数
  ulint total_buffer_pools_pages; // 定义缓冲池总页数
  ulint i; // 定义循环变量
  ulint space_id; // 定义空间 ID
  ulint page_no; // 定义页号
  int fscanf_ret; // 定义 fscanf 返回值

  /* Ignore any leftovers from before */
  // 忽略之前的任何残留
  buf_load_abort_flag = false; // 设置缓冲区加载中止标志为 false

  buf_dump_generate_path(full_filename, sizeof(full_filename)); // 生成完整文件路径

  buf_load_status(STATUS_INFO, "Loading buffer pool(s) from %s", full_filename); // 输出加载缓冲池的信息

  f = fopen(full_filename, "r"); // 打开文件进行读取
  if (f == nullptr) { // 如果文件打开失败
    buf_load_status(STATUS_ERR, "Cannot open '%s' for reading: %s",
                    full_filename, strerror(errno)); // 输出错误信息，无法打开文件进行读取
    return; // 返回
  }
  /* else */

  /* First scan the file to estimate how many entries are in it.
  This file is tiny (approx 500KB per 1GB buffer pool), reading it
  two times is fine. */
  // 首先扫描文件以估计其中有多少条目。
  // 这个文件很小（每 1GB 缓冲池大约 500KB），读取两次是可以的。
  dump_n = 0; // 初始化转储条目数为 0
  while (fscanf(f, ULINTPF "," ULINTPF, &space_id, &page_no) == 2 &&
         !SHUTTING_DOWN()) { // 读取文件中的空间 ID 和页号，并检查是否正在关闭
    dump_n++; // 转储条目数加 1
  }

  if (!SHUTTING_DOWN() && !feof(f)) { // 如果没有关闭且未到达文件末尾
    /* fscanf() returned != 2 */
    // fscanf() 返回值不等于 2
    const char *what; // 定义错误类型指针
    if (ferror(f)) { // 如果文件读取错误
      what = "reading"; // 错误类型为读取
    } else {
      what = "parsing"; // 否则错误类型为解析
    }
    fclose(f); // 关闭文件
    buf_load_status(STATUS_ERR,
                    "Error %s '%s',"
                    " unable to load buffer pool (stage 1)",
                    what, full_filename); // 输出错误信息，无法加载缓冲池（阶段 1）
    return; // 返回
  }

  /* If dump is larger than the buffer pool(s), then we ignore the
  extra trailing. This could happen if a dump is made, then buffer
  pool is shrunk and then load is attempted. */
  // 如果转储大于缓冲池，则忽略多余的尾部。
  // 这种情况可能发生在进行转储后，缓冲池缩小，然后尝试加载。
  total_buffer_pools_pages = buf_pool_get_n_pages() * srv_buf_pool_instances; // 获取缓冲池总页数
  if (dump_n > total_buffer_pools_pages) { // 如果转储条目数大于缓冲池总页数
    dump_n = total_buffer_pools_pages; // 将转储条目数设置为缓冲池总页数
  }

  if (dump_n != 0) { // 如果转储条目数不为 0
    dump = static_cast<buf_dump_t *>(
        ut::malloc_withkey(UT_NEW_THIS_FILE_PSI_KEY, dump_n * sizeof(*dump))); // 分配转储内存
  } else {
    fclose(f); // 关闭文件
    ut_sprintf_timestamp(now); // 获取当前时间戳
    buf_load_status(STATUS_INFO,
                    "Buffer pool(s) load completed at %s"
                    " (%s was empty)",
                    now, full_filename); // 输出缓冲池加载完成信息（文件为空）
    return; // 返回
  }

  if (dump == nullptr) { // 如果转储内存分配失败
    fclose(f); // 关闭文件
    buf_load_status(STATUS_ERR, "Cannot allocate " ULINTPF " bytes: %s",
                    (ulint)(dump_n * sizeof(*dump)), strerror(errno)); // 输出错误信息，无法分配内存
    return; // 返回
  }

  rewind(f); // 重置文件指针

  for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) { // 遍历转储条目并检查是否正在关闭
    fscanf_ret = fscanf(f, ULINTPF "," ULINTPF, &space_id, &page_no); // 读取文件中的空间 ID 和页号

    if (fscanf_ret != 2) { // 如果 fscanf 返回值不等于 2
      if (feof(f)) { // 如果到达文件末尾
        break; // 跳出循环
      }
      /* else */

      ut::free(dump); // 释放转储内存
      fclose(f); // 关闭文件
      buf_load_status(STATUS_ERR,
                      "Error parsing '%s', unable"
                      " to load buffer pool (stage 2)",
                      full_filename); // 输出错误信息，无法加载缓冲池（阶段 2）
      return; // 返回
    }

    if (space_id > UINT32_MASK || page_no > UINT32_MASK) { // 如果空间 ID 或页号超出范围
      ut::free(dump); // 释放转储内存
      fclose(f); // 关闭文件
      buf_load_status(STATUS_ERR,
                      "Error parsing '%s': bogus"
                      " space,page " ULINTPF "," ULINTPF " at line " ULINTPF
                      ","
                      " unable to load buffer pool",
                      full_filename, space_id, page_no, i); // 输出错误信息，无法加载缓冲池
      return; // 返回
    }

    dump[i] = BUF_DUMP_CREATE(space_id, page_no); // 创建转储条目
  }

  /* Set dump_n to the actual number of initialized elements,
  i could be smaller than dump_n here if the file got truncated after
  we read it the first time. */
  // 将 dump_n 设置为实际初始化的元素数，
  // 如果文件在第一次读取后被截断，i 可能小于 dump_n。
  dump_n = i; // 更新转储条目数

  fclose(f); // 关闭文件

  if (dump_n == 0) { // 如果转储条目数为 0
    ut::free(dump); // 释放转储内存
    ut_sprintf_timestamp(now); // 获取当前时间戳
    buf_load_status(STATUS_INFO,
                    "Buffer pool(s) load completed at %s"
                    " (%s was empty)",
                    now, full_filename); // 输出缓冲池加载完成信息（文件为空）
    return; // 返回
  }

  if (!SHUTTING_DOWN()) { // 如果没有关闭
    std::sort(dump, dump + dump_n); // 对转储条目进行排序
  }

  std::chrono::steady_clock::time_point last_check_time; // 定义上次检查时间
  ulint last_activity_cnt = 0; // 定义上次活动计数

  /* Avoid calling the expensive fil_space_acquire_silent() for each
  page within the same tablespace. dump[] is sorted by (space, page),
  so all pages from a given tablespace are consecutive. */
  // 避免为同一表空间内的每个页面调用昂贵的 fil_space_acquire_silent()。
  // dump[] 按（空间，页面）排序，因此给定表空间的所有页面都是连续的。
  space_id_t cur_space_id = BUF_DUMP_SPACE(dump[0]); // 获取当前空间 ID
  fil_space_t *space = fil_space_acquire_silent(cur_space_id); // 获取表空间
  page_size_t page_size(space ? space->flags : 0); // 获取页面大小

#ifdef HAVE_PSI_STAGE_INTERFACE
  PSI_stage_progress *pfs_stage_progress =
      mysql_set_stage(srv_stage_buffer_pool_load.m_key); // 设置阶段进度
#endif /* HAVE_PSI_STAGE_INTERFACE */

  mysql_stage_set_work_estimated(pfs_stage_progress, dump_n); // 设置预估工作量
  mysql_stage_set_work_completed(pfs_stage_progress, 0); // 设置已完成工作量

  for (i = 0; i < dump_n && !SHUTTING_DOWN(); i++) { // 遍历转储条目并检查是否正在关闭
    /* space_id for this iteration of the loop */
    // 此循环迭代的空间 ID
    const space_id_t this_space_id = BUF_DUMP_SPACE(dump[i]); // 获取当前空间 ID

    if (this_space_id != cur_space_id) { // 如果当前空间 ID 不等于之前的空间 ID
      if (space != nullptr) { // 如果表空间不为空
        fil_space_release(space); // 释放表空间
      }

      cur_space_id = this_space_id; // 更新当前空间 ID
      space = fil_space_acquire_silent(cur_space_id); // 获取新的表空间

      if (space != nullptr) { // 如果表空间不为空
        const page_size_t cur_page_size(space->flags); // 获取当前页面大小
        page_size.copy_from(cur_page_size); // 复制页面大小
      }
    }

    if (space == nullptr) { // 如果表空间为空
      continue; // 跳过当前迭代
    }

    buf_read_page_background(page_id_t(this_space_id, BUF_DUMP_PAGE(dump[i])),
                             page_size, true); // 在后台读取页面

    if (i % 64 == 63) { // 每 64 次迭代唤醒一次模拟的 AIO 处理程序线程
      os_aio_simulated_wake_handler_threads(); // 唤醒模拟的 AIO 处理程序线程
    }

    /* Update the progress every 32 MiB, which is every Nth page,
    where N = 32*1024^2 / page_size. */
    // 每 32 MiB 更新一次进度，即每 N 页，
    // 其中 N = 32*1024^2 / 页面大小。
    static const ulint update_status_every_n_mb = 32; // 定义每 32 MiB 更新一次状态
    static const ulint update_status_every_n_pages =
        update_status_every_n_mb * 1024 * 1024 / page_size.physical(); // 计算每 N 页更新一次状态

    if (i % update_status_every_n_pages == 0) { // 如果当前迭代数是更新状态的倍数
      buf_load_status(STATUS_VERBOSE, "Loaded " ULINTPF "/" ULINTPF " pages",
                      i + 1, dump_n); // 输出加载进度
      mysql_stage_set_work_completed(pfs_stage_progress, i); // 设置已完成工作量
    }

    if (buf_load_abort_flag) { // 如果缓冲区加载中止标志为 true
      if (space != nullptr) { // 如果表空间不为空
        fil_space_release(space); // 释放表空间
      }
      buf_load_abort_flag = false; // 重置缓冲区加载中止标志
      ut::free(dump); // 释放转储内存
      buf_load_status(STATUS_INFO, "Buffer pool(s) load aborted on request"); // 输出缓冲池加载中止信息
      /* Premature end, set estimated = completed = i and
      end the current stage event. */
      // 提前结束，设置预估工作量和已完成工作量为 i，并结束当前阶段事件。
      mysql_stage_set_work_estimated(pfs_stage_progress, i); // 设置预估工作量
      mysql_stage_set_work_completed(pfs_stage_progress, i); // 设置已完成工作量
#ifdef HAVE_PSI_STAGE_INTERFACE
      mysql_end_stage(); // 结束阶段进度事件
#endif /* HAVE_PSI_STAGE_INTERFACE */
      return; // 返回
    }

    buf_load_throttle_if_needed(&last_check_time, &last_activity_cnt, i); // 如果需要，限制缓冲区加载速度
  }

  if (space != nullptr) { // 如果表空间不为空
    fil_space_release(space); // 释放表空间
  }

  ut::free(dump); // 释放转储内存

  ut_sprintf_timestamp(now); // 获取当前时间戳

  buf_load_status(STATUS_INFO, "Buffer pool(s) load completed at %s", now); // 输出缓冲池加载完成信息

  /* Make sure that estimated = completed when we end. */
  // 确保在结束时预估工作量等于已完成工作量。
  mysql_stage_set_work_completed(pfs_stage_progress, dump_n); // 设置已完成工作量
  /* End the stage progress event. */
  // 结束阶段进度事件。
#ifdef HAVE_PSI_STAGE_INTERFACE
  mysql_end_stage(); // 结束阶段进度事件
#endif /* HAVE_PSI_STAGE_INTERFACE */
}


/** Aborts a currently running buffer pool load. This function is called by
 MySQL code via buffer_pool_load_abort() and it should return immediately
 because the whole MySQL is frozen during its execution. */
// 中止当前正在运行的缓冲池加载。此函数由 MySQL 代码通过 buffer_pool_load_abort() 调用，
// 并且应该立即返回，因为在执行期间整个 MySQL 都被冻结。
void buf_load_abort() { 
    buf_load_abort_flag = true; // 设置中止标志为 true
}

/** This is the main thread for buffer pool dump/load. It waits for an
event and when waked up either performs a dump or load and sleeps
again. */
void buf_dump_thread() {
  ut_ad(!srv_read_only_mode);

  buf_dump_status(STATUS_VERBOSE, "Dumping of buffer pool not started");
  buf_load_status(STATUS_VERBOSE, "Loading of buffer pool not started");

  if (srv_buffer_pool_load_at_startup) {
    buf_load();
  }

  while (!SHUTTING_DOWN()) {
    os_event_wait(srv_buf_dump_event);

    if (buf_dump_should_start) {
      buf_dump_should_start = false;
      buf_dump(true /* quit on shutdown */);
    }

    if (buf_load_should_start) {
      buf_load_should_start = false;
      buf_load();
    }

    os_event_reset(srv_buf_dump_event);
  }

  if (srv_buffer_pool_dump_at_shutdown && srv_fast_shutdown != 2) {
    buf_dump(false /* ignore shutdown down flag,
                keep going even if we are in a shutdown state */);
  }
}
