/*****************************************************************************

Copyright (c) 2013, 2022, Oracle and/or its affiliates.

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

/** @file fsp/fsp0space.cc
 Multi file, shared, system tablespace implementation.

 Created 2012-11-16 by Sunny Bains as srv/srv0space.cc
 Refactored 2013-7-26 by Kevin Lewis
 *******************************************************/

#include <stdlib.h>
#include <sys/types.h>

#include "dict0load.h"
#include "fsp0sysspace.h"
#ifndef UNIV_HOTBACKUP
#include "ha_prototypes.h"
#include "mem0mem.h"

/** The server header file is included to access opt_initialize global variable.
If server passes the option for create/open DB to SE, we should remove such
direct reference to server header and global variable */
#include "mysqld.h"
#endif /* !UNIV_HOTBACKUP */
#include "os0file.h"
#ifndef UNIV_HOTBACKUP
#include "row0mysql.h"
#endif /* !UNIV_HOTBACKUP */
#include "srv0start.h"
#include "trx0sys.h"
#include "ut0new.h"

/** The control info of the system tablespace. */
SysTablespace srv_sys_space;

/** The control info of a temporary table shared tablespace. */
SysTablespace srv_tmp_space;

/** If the last data file is auto-extended, we add this many pages to it
at a time. We have to make this public because it is a config variable. */
ulong sys_tablespace_auto_extend_increment;

#ifdef UNIV_DEBUG
/** Control if extra debug checks need to be done for temporary tablespace.
Default = true that is disable such checks.
This variable is not exposed to end-user but still kept as variable for
developer to enable it during debug. */
bool srv_skip_temp_table_checks_debug = true;
#endif /* UNIV_DEBUG */

/** Put the pointer to the next byte after a valid file name. Note that we must
step over the ':' in a Windows filepath.
A Windows path normally looks like "C:\ibdata\ibdata1:1G", but a Windows raw
partition may have a specification like "\\.\C::1Gnewraw" or
"\\.\PHYSICALDRIVE2:1Gnewraw".
@param[in]      ptr             system tablespace file path spec
@return next character in string after the file name */
char *SysTablespace::parse_file_name(char *ptr) {
  const char *start = ptr;

  while ((*ptr != ':' && *ptr != '\0') ||
         (ptr != start && *ptr == ':' &&
          (*(ptr + 1) == '\\' || *(ptr + 1) == '/' || *(ptr + 1) == ':'))) {
    ptr++;
  }

  return (ptr);
}

/** Convert a numeric string representing a number of bytes
optionally ending in upper or lower case G, M, or K,
to a number of megabytes, rounding down to the nearest megabyte.
Then return the number of pages in the file.
@param[in,out]  ptr     Pointer to a numeric string
@return the number of pages in the file. */
page_no_t SysTablespace::parse_units(char *&ptr) {
  char *endp;
  ulint num = strtoul(ptr, &endp, 10);
  ulint megs;

  ptr = endp;

  switch (*ptr) {
    case 'G':
    case 'g':
      megs = num * 1024;
      ++ptr;
      break;

    case 'M':
    case 'm':
      megs = num;
      ++ptr;
      break;
    case 'K':
    case 'k':
      megs = num / 1024;
      ++ptr;
      break;
    default:
      megs = num / (1024 * 1024);
      break;
  }

  return (static_cast<page_no_t>(megs * (1024 * 1024 / UNIV_PAGE_SIZE)));
}

/** Parse the input params and populate member variables.
@param[in]      filepath_spec   path to data files
@param[in]      supports_raw    true if the tablespace supports raw devices
@return true on success parse */
bool SysTablespace::parse_params(const char *filepath_spec, bool supports_raw) {
  char *filepath;
  page_no_t size;
  ulint n_files = 0;

  ut_ad(m_last_file_size_max == 0);
  ut_ad(!m_auto_extend_last_file);

  char *input_str = mem_strdup(filepath_spec);
  char *ptr = input_str;

  /*---------------------- PASS 1 ---------------------------*/
  /* First calculate the number of data files and check syntax. */
  while (*ptr != '\0') {
    filepath = ptr;

    ptr = parse_file_name(ptr);

    if (ptr == filepath) {
      ib::error(ER_IB_MSG_431) << "File Path Specification '" << filepath_spec
                               << "' is missing a file name.";

      ut::free(input_str);
      return (false);
    }

    if (*ptr == '\0') {
      ib::error(ER_IB_MSG_432) << "File Path Specification '" << filepath_spec
                               << "' is missing a file size.";

      ut::free(input_str);
      return (false);
    }

    ptr++;

    size = parse_units(ptr);

    if (size == 0) {
    invalid_size:
      ib::error(ER_IB_MSG_433)
          << "Invalid File Path Specification: '" << filepath_spec
          << "'. An invalid file size was specified.";

      ut::free(input_str);
      return (false);
    }

    if (0 == strncmp(ptr, ":autoextend", (sizeof ":autoextend") - 1)) {
      ptr += (sizeof ":autoextend") - 1;

      if (0 == strncmp(ptr, ":max:", (sizeof ":max:") - 1)) {
        ptr += (sizeof ":max:") - 1;

        page_no_t max = parse_units(ptr);

        if (max < size) {
          goto invalid_size;
        }
      }

      if (*ptr == ';') {
        ib::error(ER_IB_MSG_434)
            << "Invalid File Path Specification: '" << filepath_spec
            << "'. Only the last"
               " file defined can be 'autoextend'.";

        ut::free(input_str);
        return (false);
      }
    }

    if (0 == strncmp(ptr, "new", (sizeof "new") - 1)) {
      ptr += (sizeof "new") - 1;
    }

    if (0 == strncmp(ptr, "raw", (sizeof "raw") - 1)) {
      if (!supports_raw) {
        ib::error(ER_IB_MSG_435)
            << "Invalid File Path Specification: '" << filepath_spec
            << "' Tablespace"
               " doesn't support raw devices";

        ut::free(input_str);
        return (false);
      }

      ptr += (sizeof "raw") - 1;
    }

    ++n_files;

    if (*ptr == ';') {
      ptr++;
    } else if (*ptr != '\0') {
      ptr[0] = '\0';
      ib::error(ER_IB_MSG_436)
          << "File Path Specification: '" << filepath_spec
          << "' has unrecognized characters after '" << input_str << "'";

      ut::free(input_str);
      return (false);
    }
  }

  if (n_files == 0) {
    ib::error(ER_IB_MSG_437) << "File Path Specification: '" << filepath_spec
                             << "' must contain"
                                " at least one data file definition";

    ut::free(input_str);
    return (false);
  }

  /*---------------------- PASS 2 ---------------------------*/
  /* Then store the actual values to our arrays */
  ptr = input_str;
  ulint order = 0;

  while (*ptr != '\0') {
    filepath = ptr;

    ptr = parse_file_name(ptr);

    if (*ptr == ':') {
      /* Make filepath a null-terminated string */
      *ptr = '\0';
      ptr++;
    }

    size = parse_units(ptr);
    ut_ad(size > 0);

    if (0 == strncmp(ptr, ":autoextend", (sizeof ":autoextend") - 1)) {
      m_auto_extend_last_file = true;

      ptr += (sizeof ":autoextend") - 1;

      if (0 == strncmp(ptr, ":max:", (sizeof ":max:") - 1)) {
        ptr += (sizeof ":max:") - 1;

        m_last_file_size_max = parse_units(ptr);
      }
    }

    m_files.push_back(Datafile(filepath, flags(), size, order));
    Datafile *datafile = &m_files.back();
    datafile->make_filepath(path(), filepath, NO_EXT);

    if (0 == strncmp(ptr, "new", (sizeof "new") - 1)) {
      ptr += (sizeof "new") - 1;
    }

    if (0 == strncmp(ptr, "raw", (sizeof "raw") - 1)) {
      ut_a(supports_raw);

      ptr += (sizeof "raw") - 1;

      /* Initialize new raw device only during initialize */
      m_files.back().m_type =
#ifndef UNIV_HOTBACKUP
          opt_initialize ? SRV_NEW_RAW : SRV_OLD_RAW;
#else  /* !UNIV_HOTBACKUP */
          SRV_OLD_RAW;
#endif /* !UNIV_HOTBACKUP */
    }

    if (*ptr == ';') {
      ++ptr;
    }
    order++;
  }

  ut_ad(n_files == ulint(m_files.size()));

  ut::free(input_str);

  return (true);
}

/** Frees the memory allocated by the parse method. */
void SysTablespace::shutdown() {
  Tablespace::shutdown();

  m_auto_extend_last_file = false;
  m_last_file_size_max = 0;
  m_created_new_raw = false;
  m_is_tablespace_full = false;
  m_sanity_checks_done = false;
}

/** Verify the size of the physical file.
@param[in]      file    data file object
@return DB_SUCCESS if OK else error code. */
dberr_t SysTablespace::check_size(Datafile &file) {
  os_offset_t size = os_file_get_size(file.m_handle);
  ut_a(size != (os_offset_t)-1);

  /* Under some error conditions like disk full scenarios
  or file size reaching filesystem limit the data file
  could contain an incomplete extent at the end. When we
  extend a data file and if some failure happens, then
  also the data file could contain an incomplete extent.
  So we need to round the size downward to a megabyte. */

  page_no_t rounded_size_pages = get_pages_from_size(size);

  /* If last file */
  if (&file == &m_files.back() && m_auto_extend_last_file) {
    if (file.m_size > rounded_size_pages ||
        (m_last_file_size_max > 0 &&
         m_last_file_size_max < rounded_size_pages)) {
      ib::error(ER_IB_MSG_438)
          << "The Auto-extending " << name() << " data file '"
          << file.filepath()
          << "' is"
             " of a different size "
          << rounded_size_pages
          << " pages (rounded down to MB) than specified"
             " in the .cnf file: initial "
          << file.m_size << " pages, max " << m_last_file_size_max
          << " (relevant if non-zero) pages!";
      return (DB_ERROR);
    }

    file.m_size = rounded_size_pages;
  }

  if (rounded_size_pages != file.m_size) {
    ib::error(ER_IB_MSG_439)
        << "The " << name() << " data file '" << file.filepath()
        << "' is of a different size " << rounded_size_pages
        << " pages (rounded down to MB)"
           " than the "
        << file.m_size
        << " pages specified in"
           " the .cnf file!";
    return (DB_ERROR);
  }

  return (DB_SUCCESS);
}

/** Set the size of the file.
@param[in,out]  file    data file object
@return DB_SUCCESS or error code */
dberr_t SysTablespace::set_size(Datafile &file) {
  ut_a(!srv_read_only_mode || m_ignore_read_only);

  /* We created the data file and now write it full of zeros */
  ib::info(ER_IB_MSG_440)
      << "Setting file '" << file.filepath() << "' size to "
      << (file.m_size >> (20 - UNIV_PAGE_SIZE_SHIFT))
      << " MB."
         " Physically writing the file full; Please wait ...";

  bool success = os_file_set_size(
      file.m_filepath, file.m_handle, 0,
      static_cast<os_offset_t>(file.m_size) << UNIV_PAGE_SIZE_SHIFT, true);

  if (success) {
    ib::info(ER_IB_MSG_441)
        << "File '" << file.filepath() << "' size is now "
        << (file.m_size >> (20 - UNIV_PAGE_SIZE_SHIFT)) << " MB.";
  } else {
    ib::error(ER_IB_MSG_442)
        << "Could not set the file size of '" << file.filepath()
        << "'. Probably out of disk space";

    return (DB_ERROR);
  }

  return (DB_SUCCESS);
}

/** Create a data file.
@param[in,out]  file    data file object
@return DB_SUCCESS or error code */
dberr_t SysTablespace::create_file(Datafile &file) {
  dberr_t err = DB_SUCCESS;

  ut_a(!file.m_exists);
  ut_a(!srv_read_only_mode || m_ignore_read_only);

  switch (file.m_type) {
    case SRV_NEW_RAW:

      /* The partition is opened, not created; then it is
      written over */
      m_created_new_raw = true;

      [[fallthrough]];

    case SRV_OLD_RAW:

      srv_start_raw_disk_in_use = true;

      [[fallthrough]];

    case SRV_NOT_RAW:
      err =
          file.open_or_create(m_ignore_read_only ? false : srv_read_only_mode);
      break;
  }

  if (err == DB_SUCCESS && file.m_type != SRV_OLD_RAW) {
    err = set_size(file);
  }

  return (err);
}

/** Open a data file.
@param[in,out]  file    data file object
@return DB_SUCCESS or error code */
dberr_t SysTablespace::open_file(Datafile &file) {
  dberr_t err = DB_SUCCESS;

  ut_a(file.m_exists);

  switch (file.m_type) {
    case SRV_NEW_RAW:
      /* The partition is opened, not created; then it is
      written over */
      m_created_new_raw = true;

      [[fallthrough]];

    case SRV_OLD_RAW:
      srv_start_raw_disk_in_use = true;

      if (srv_read_only_mode && !m_ignore_read_only) {
        ib::error(ER_IB_MSG_443)
            << "Can't open a raw device '" << file.m_filepath
            << "' when"
               " --innodb-read-only is set";

        return (DB_ERROR);
      }

      [[fallthrough]];

    case SRV_NOT_RAW:
      err =
          file.open_or_create(m_ignore_read_only ? false : srv_read_only_mode);

      if (err != DB_SUCCESS) {
        return (err);
      }
      break;
  }

  switch (file.m_type) {
    case SRV_NEW_RAW:
      /* Set file size for new raw device. */
      err = set_size(file);
      break;

    case SRV_NOT_RAW:
      /* Check file size for existing file. */
      err = check_size(file);
      break;

    case SRV_OLD_RAW:
      err = DB_SUCCESS;
      break;
  }

  if (err != DB_SUCCESS) {
    file.close();
  }

  return (err);
}

#ifndef UNIV_HOTBACKUP

dberr_t SysTablespace::read_lsn_and_check_flags(lsn_t *flushed_lsn) {
  /* Only relevant for the system tablespace. */
  /* 仅对系统表空间相关。 */
  ut_ad(space_id() == TRX_SYS_SPACE); // 断言表空间 ID 为系统表空间 ID

  files_t::iterator it = m_files.begin(); // 获取文件列表的起始迭代器

  ut_a(it->m_exists); // 断言文件存在
  ut_ad(it->m_handle.m_file != OS_FILE_CLOSED); // 断言文件句柄未关闭

  dberr_t err =
      it->read_first_page(m_ignore_read_only ? false : srv_read_only_mode); // 读取文件的第一页

  if (err != DB_SUCCESS) { // 如果读取失败
    return (err); // 返回错误码
  }

  ut_a(it->order() == 0); // 断言文件顺序为0

  err = recv_sys->dblwr->load(); // 加载双写缓冲区

  if (err != DB_SUCCESS) { // 如果加载失败
    return (err); // 返回错误码
  }

  err = recv_sys->dblwr->reduced_load(); // 加载简化的双写缓冲区

  if (err != DB_SUCCESS) { // 如果加载失败
    return (err); // 返回错误码
  }

  /* Check the contents of the first page of the first datafile. */
  /* 检查第一个数据文件的第一页内容。 */
  for (int retry = 0; retry < 2; ++retry) { // 重试两次
    err = it->validate_first_page(it->m_space_id, flushed_lsn, false); // 验证第一页

    if (err != DB_SUCCESS &&
        (retry == 1 || it->open_or_create(srv_read_only_mode) != DB_SUCCESS ||
         it->restore_from_doublewrite(0) != DB_SUCCESS)) { // 如果验证失败且重试次数为1或打开/创建文件失败或从双写缓冲区恢复失败
      it->close(); // 关闭文件

      return (err); // 返回错误码
    }
  }

  /* Make sure the tablespace space ID matches the
  space ID on the first page of the first datafile. */
  /* 确保表空间 ID 与第一个数据文件第一页上的空间 ID 匹配。 */
  if (space_id() != it->m_space_id) { // 如果表空间 ID 不匹配
    ib::error(ER_IB_MSG_444)
        << "The " << name() << " data file '" << it->name()
        << "' has the wrong space ID. It should be " << space_id() << ", but "
        << it->m_space_id << " was found"; // 打印错误信息

    it->close(); // 关闭文件

    return (err); // 返回错误码
  }

  /* The flags of srv_sys_space do not have SDI Flag set.
  Update the flags of system tablespace to indicate the presence
  of SDI */
  /* srv_sys_space 的标志没有设置 SDI 标志。
  更新系统表空间的标志以指示存在 SDI */
  set_flags(it->flags()); // 设置标志

  it->close(); // 关闭文件

  return (DB_SUCCESS); // 返回成功
}

/** Check if a file can be opened in the correct mode.
@param[in,out]  file    data file object
@param[out]     reason  exact reason if file_status check failed.
@return DB_SUCCESS or error code. */
dberr_t SysTablespace::check_file_status(const Datafile &file,
                                         file_status_t &reason) {
  os_file_stat_t stat;

  memset(&stat, 0x0, sizeof(stat));

  dberr_t err =
      os_file_get_status(file.m_filepath, &stat, true,
                         m_ignore_read_only ? false : srv_read_only_mode);

  reason = FILE_STATUS_VOID;
  /* File exists but we can't read the rw-permission settings. */
  switch (err) {
    case DB_FAIL:
      ib::error(ER_IB_MSG_445)
          << "os_file_get_status() failed on '" << file.filepath()
          << "'. Can't determine file permissions";
      err = DB_ERROR;
      reason = FILE_STATUS_RW_PERMISSION_ERROR;
      break;

    case DB_SUCCESS:

      /* Note: stat.rw_perm is only valid for "regular" files */

      if (stat.type == OS_FILE_TYPE_FILE) {
        if (!stat.rw_perm) {
          const char *p = (!srv_read_only_mode || m_ignore_read_only)
                              ? "writable"
                              : "readable";

          ib::error(ER_IB_MSG_446) << "The " << name() << " data file"
                                   << " '" << file.name() << "' must be " << p;

          err = DB_ERROR;
          reason = FILE_STATUS_READ_WRITE_ERROR;
        }

      } else {
        /* Not a regular file, bail out. */
        ib::error(ER_IB_MSG_447)
            << "The " << name() << " data file '" << file.name()
            << "' is not a regular"
               " InnoDB data file.";

        err = DB_ERROR;
        reason = FILE_STATUS_NOT_REGULAR_FILE_ERROR;
      }
      break;

    case DB_NOT_FOUND:
      break;

    default:
      ut_d(ut_error);
  }

  return (err);
}

/** Note that the data file was not found.
@param[in]      file            data file object
@param[in]      create_new_db   true if a new instance to be created
@return DB_SUCCESS or error code */
dberr_t SysTablespace::file_not_found(Datafile &file, bool create_new_db) {
  file.m_exists = false;

  if (srv_read_only_mode && !m_ignore_read_only) {
    ib::error(ER_IB_MSG_448) << "Can't create file '" << file.filepath()
                             << "' when --innodb-read-only is set";

    return (DB_ERROR);

  } else if (&file == &m_files.front()) {
    /* First data file. */

    if (space_id() == TRX_SYS_SPACE && create_new_db) {
      ib::info(ER_IB_MSG_449)
          << "The first " << name() << " data file '" << file.name()
          << "' did not exist."
             " A new tablespace will be created!";
    }

  } else {
    ib::info(ER_IB_MSG_450) << "Need to create a new " << name()
                            << " data file '" << file.name() << "'.";
  }

  /* We allow add new files at end even if dict_init_mode is
  not creating files. */
  if (!create_new_db && (&file == &m_files.front())) {
    return (DB_SUCCESS);
  }

  /* Set the file create mode. */
  switch (file.m_type) {
    case SRV_NOT_RAW:
      file.set_open_flags(OS_FILE_CREATE);
      break;

    case SRV_NEW_RAW:
    case SRV_OLD_RAW:
      file.set_open_flags(OS_FILE_OPEN_RAW);
      break;
  }

  return (DB_SUCCESS);
}

/** Note that the data file was found.
@param[in,out]  file    data file object */
void SysTablespace::file_found(Datafile &file) {
  /* Note that the file exists and can be opened
  in the appropriate mode. */
  file.m_exists = true;

  /* Set the file open mode */
  switch (file.m_type) {
    case SRV_NOT_RAW:
      file.set_open_flags(&file == &m_files.front() ? OS_FILE_OPEN_RETRY
                                                    : OS_FILE_OPEN);
      break;

    case SRV_NEW_RAW:
    case SRV_OLD_RAW:
      file.set_open_flags(OS_FILE_OPEN_RAW);
      break;
  }
}

/** Check the data file specification.
@param[in]  create_new_db     True if a new database is to be created
@param[in]  min_expected_size Minimum expected tablespace size in bytes
@return DB_SUCCESS if all OK else error code */
dberr_t SysTablespace::check_file_spec(bool create_new_db,
                                       ulint min_expected_size) {
  if (m_files.size() >= 1000) {
    ib::error(ER_IB_MSG_451) << "There must be < 1000 data files in " << name()
                             << " but " << m_files.size()
                             << " have been"
                                " defined.";

    return (DB_ERROR);
  }

  if (get_sum_of_sizes() < min_expected_size / UNIV_PAGE_SIZE) {
    ib::error(ER_IB_MSG_452) << "Tablespace size must be at least "
                             << min_expected_size / (1024 * 1024) << " MB";

    return (DB_ERROR);
  }

  dberr_t err = DB_SUCCESS;

  ut_a(!m_files.empty());

  /* If there is more than one data file and the last data file
  doesn't exist, that is OK. We allow adding of new data files. */

  files_t::iterator begin = m_files.begin();
  files_t::iterator end = m_files.end();

  for (files_t::iterator it = begin; it != end; ++it) {
    file_status_t reason_if_failed;
    err = check_file_status(*it, reason_if_failed);

    if (err == DB_NOT_FOUND) {
      err = file_not_found(*it, create_new_db);

      if (err != DB_SUCCESS) {
        break;
      }

    } else if (err != DB_SUCCESS) {
      if (reason_if_failed == FILE_STATUS_READ_WRITE_ERROR) {
        const char *p = (!srv_read_only_mode || m_ignore_read_only)
                            ? "writable"
                            : "readable";
        ib::error(ER_IB_MSG_453) << "The " << name() << " data file"
                                 << " '" << it->name() << "' must be " << p;
      }

      ut_a(err != DB_FAIL);
      break;

    } else if (create_new_db && !(*it).is_raw_type()) {
      ib::error(ER_IB_MSG_454)
          << "The " << name() << " data file '" << begin->m_name
          << "' was not found but"
             " one of the other data files '"
          << it->m_name << "' exists.";

      err = DB_ERROR;
      break;

    } else {
      ut_ad(err == DB_SUCCESS);
      file_found(*it);
    }
  }

  /* We assume doublewirte blocks in the first data file. */
  if (err == DB_SUCCESS && begin->m_size < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 3) {
    ib::error(ER_IB_MSG_455)
        << "The " << name() << " data file "
        << "'" << begin->name() << "' must be at least "
        << TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 3 * UNIV_PAGE_SIZE / (1024 * 1024)
        << " MB";

    err = DB_ERROR;
  }

  return (err);
}

dberr_t SysTablespace::open_or_create(bool is_temp, bool create_new_db,
                                      page_no_t *sum_new_sizes,
                                      lsn_t *flush_lsn) {
  dberr_t err = DB_SUCCESS; // 初始化错误码为成功
  fil_space_t *space = nullptr; // 初始化表空间指针为空

  ut_ad(!m_files.empty()); // 断言文件列表不为空

  if (sum_new_sizes) {
    *sum_new_sizes = 0; // 初始化新文件大小总和为0
  }

  files_t::iterator begin = m_files.begin(); // 获取文件列表的起始迭代器
  files_t::iterator end = m_files.end(); // 获取文件列表的结束迭代器

  ut_ad(begin->order() == 0); // 断言第一个文件的顺序为0

  for (files_t::iterator it = begin; it != end; ++it) { // 遍历文件列表
    if (it->m_exists) { // 如果文件已存在
      err = open_file(*it); // 打开文件

      /* For new raw device increment new size. */
      /* 对于新的原始设备，增加新大小。 */
      if (sum_new_sizes && it->m_type == SRV_NEW_RAW) {
        *sum_new_sizes += it->m_size; // 增加新文件大小
      }

    } else { // 如果文件不存在
      err = create_file(*it); // 创建文件

      if (sum_new_sizes) {
        *sum_new_sizes += it->m_size; // 增加新文件大小
      }

      /* Set the correct open flags now that we have
      successfully created the file. */
      /* 现在我们已经成功创建了文件，设置正确的打开标志。 */
      if (err == DB_SUCCESS) {
        /* We ignore new_db OUT parameter here
        as the information is known at this stage */
        /* 我们在这里忽略 new_db OUT 参数，因为此阶段已知信息。 */
        file_found(*it); // 标记文件已找到
      }
    }

    if (err != DB_SUCCESS) { // 如果发生错误
      return (err); // 返回错误码
    }

#if !defined(NO_FALLOCATE) && defined(UNIV_LINUX)
    /* Note: This should really be per node and not per
    tablespace because a tablespace can contain multiple
    files (nodes). The implication is that all files of
    the tablespace should be on the same medium. */
    /* 注意：这实际上应该是每个节点而不是每个表空间，因为一个表空间可以包含多个文件（节点）。
    这意味着表空间的所有文件应该在同一介质上。 */

    if (fil_fusionio_enable_atomic_write(it->m_handle)) { // 如果启用了 FusionIO 原子写入
      if (dblwr::is_enabled()) { // 如果启用了双写缓冲
        ib::info(ER_IB_MSG_456) << "FusionIO atomic IO enabled,"
                                   " disabling the double write buffer";
        // 打印信息：启用了 FusionIO 原子 IO，禁用双写缓冲

        dblwr::g_mode = dblwr::Mode::OFF; // 禁用双写缓冲
      }

      it->m_atomic_write = true; // 设置文件为原子写入
    } else {
      it->m_atomic_write = false; // 设置文件为非原子写入
    }
#else
    it->m_atomic_write = false; // 设置文件为非原子写入
#endif /* !NO_FALLOCATE && UNIV_LINUX*/
  }

  if (flush_lsn != nullptr) {
    if (create_new_db) { // 如果是创建新数据库
      /* There are no data files, so we assign the initial value
      to flush_lsn instead of reading it from disk. */
      /* 没有数据文件，所以我们将初始值分配给 flush_lsn，而不是从磁盘读取。 */
      *flush_lsn = LOG_START_LSN + LOG_BLOCK_HDR_SIZE; // 设置初始 flush_lsn
    } else {
      /* Validate the header page in the first datafile in the
      system tablespace and read flush_lsn from the validated
      header page. */
      /* 验证系统表空间中第一个数据文件的头页，并从验证的头页读取 flush_lsn。 */
      err = read_lsn_and_check_flags(flush_lsn); // 读取 LSN 并检查标志
      if (err != DB_SUCCESS) {
        return (err); // 返回错误码
      }
    }
  }

  /* Close the current handles, add space and file info to the
  fil_system cache and the Data Dictionary, and re-open them
  in file_system cache so that they stay open until shutdown. */
  /* 关闭当前句柄，将空间和文件信息添加到 fil_system 缓存和数据字典中，并在 file_system 缓存中重新打开它们，以便它们在关闭之前保持打开状态。 */
  ulint node_counter = 0; // 初始化节点计数器
  for (files_t::iterator it = begin; it != end; ++it) { // 遍历文件列表
    it->close(); // 关闭文件
    it->m_exists = true; // 标记文件已存在

    if (it == begin) { // 如果是第一个文件
      /* First data file. */
      /* 第一个数据文件。 */

      /* Create the tablespace entry for the multi-file
      tablespace in the tablespace manager. */
      /* 在表空间管理器中为多文件表空间创建表空间条目。 */
      space =
          fil_space_create(name(), space_id(), flags(),
                           is_temp ? FIL_TYPE_TEMPORARY : FIL_TYPE_TABLESPACE); // 创建表空间
    }

    ut_ad(fil_validate()); // 断言文件系统验证通过

    page_no_t max_size =
        (++node_counter == m_files.size()
             ? (m_last_file_size_max == 0 ? PAGE_NO_MAX : m_last_file_size_max)
             : it->m_size); // 计算最大页面大小

    /* Add the datafile to the fil_system cache. */
    /* 将数据文件添加到 fil_system 缓存中。 */
    if (!fil_node_create(it->m_filepath, it->m_size, space,
                         it->m_type != SRV_NOT_RAW, it->m_atomic_write,
                         max_size)) {
      err = DB_ERROR; // 设置错误码
      break; // 跳出循环
    }
  }

  return (err); // 返回错误码
}
#endif /* !UNIV_HOTBACKUP */

/**
@return next increment size */
page_no_t SysTablespace::get_increment() const {
  page_no_t increment;

  if (m_last_file_size_max == 0) {
    increment = get_autoextend_increment();
  } else {
    increment = m_last_file_size_max - last_file_size();
  }

  if (increment > get_autoextend_increment()) {
    increment = get_autoextend_increment();
  }

  return (increment);
}
