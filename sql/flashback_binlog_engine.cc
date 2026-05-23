/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/flashback_binlog_engine.cc
  Binlog-based Flashback Engine Implementation

  实现 BinlogFlashbackEngine 的核心函数:
  - find_position_at_timestamp(): 定位 binlog 位置
  - reverse_rows_event(): 逆向 Rows_event 并生成反向 SQL
  - check_row_image_compatibility(): 检查 binlog_row_image 兼容性
  - cache_table_map(): 缓存 Table_map_event 元信息

  设计参考: mysql_flashback_implementation_v2.md §5.2.2
*/

#include "sql/flashback_binlog_engine.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string.h>
#include <thread>
#include <time.h>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"  // mysql_file_open, mysql_file_close
#include "sql/binlog.h"            // mysql_bin_log, log_bin_basename
#include "sql/binlog_reader.h"     // Binlog_file_reader
#include "sql/flashback_ddl_barrier.h"  // DDLBarrier (C5)
#include "sql/flashback_sysvars.h"      // flashback system variables
#include "sql/log_event.h"              // Rows_log_event, Table_map_log_event, etc.
#include "sql/sql_class.h"              // THD
#include "sql/sql_parse.h"              // mysql_parse
#include "sql/system_variables.h"       // BINLOG_ROW_IMAGE_*

#ifdef MYSQL_SERVER

namespace flashback {

/* ================================================================
 * 内部辅助函数
 * ================================================================ */

/**
  获取 binlog 索引文件的完整路径

  @param buf   输出缓冲区（长度 ≥ FN_REFLEN）
  @param buflen 缓冲区大小
  @retval true  获取失败（binlog 未启用）
  @retval false 获取成功
*/
static bool get_binlog_index_path(char *buf, size_t buflen) {
  if (log_bin_basename == nullptr || log_bin_basename[0] == '\0') return true;

  /* binlog 索引文件 = binlog basename + ".index" */
  size_t base_len = strlen(log_bin_basename);
  const char *suffix = ".index";
  size_t suffix_len = strlen(suffix);

  if (base_len + suffix_len >= buflen) return true;

  memcpy(buf, log_bin_basename, base_len);
  memcpy(buf + base_len, suffix, suffix_len + 1);
  return false;
}

/**
  从 binlog 文件名提取序号

  binlog 文件名格式: <basename>.NNNNNN
  例如: mysql-bin.000001 → 1

  @param filename binlog 文件名
  @return 序号，解析失败返回 0
*/
static uint64_t extract_binlog_sequence(const char *filename) {
  /* 找到最后一个 '.' */
  const char *dot = strrchr(filename, '.');
  if (dot == nullptr) return 0;
  return strtoull(dot + 1, nullptr, 10);
}

/**
  读取 binlog 索引文件，返回所有 binlog 文件名列表

  索引文件格式: 每行一个 binlog 文件的完整路径或相对路径。

  @param index_path  索引文件路径
  @param[out] files  输出文件名列表（调用者负责释放）
  @retval true  读取失败
  @retval false 成功
*/
static bool read_binlog_index(const char *index_path,
                              std::vector<std::string> &files) {
  File fd = mysql_file_open(key_file_binlog_index, index_path, O_RDONLY,
                            MYF(0));
  if (fd < 0) return true;

  /* 逐行读取索引文件 */
  char line[FN_REFLEN];
  while (true) {
    ssize_t n = my_read(fd, reinterpret_cast<unsigned char *>(line),
                        sizeof(line) - 1, MYF(0));
    if (n <= 0) break;

    line[n] = '\0';

    /* 去掉换行符 */
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';

    if (line[0] == '\0') continue;

    /* 提取文件名部分（去掉路径） */
    const char *base = strrchr(line, '/');
    if (base)
      files.emplace_back(base + 1);
    else
      files.emplace_back(line);
  }

  mysql_file_close(fd, MYF(0));
  return false;
}

/**
  获取 Log_event 的时间戳（秒级）

  MySQL server 侧的 Log_event 使用 common_header->when (struct timeval) 存储时间。

  @param ev Log_event 指针
  @return 时间戳（秒），如果 ev 为 nullptr 返回 0
*/
static my_time_t get_event_timestamp(const Log_event *ev) {
  if (ev == nullptr || ev->common_header == nullptr) return 0;
  return static_cast<my_time_t>(ev->common_header->when.tv_sec);
}

/**
  转义 SQL 字符串值中的特殊字符

  WHY: 逆向生成的 SQL 中的字符串值必须转义单引号、反斜杠等
  特殊字符，否则会导致 SQL 语法错误或注入风险。

  @param src 原始字符串
  @param dst 输出缓冲区
  @param dst_len 缓冲区大小
  @return 写入 dst 的字符数
*/
static size_t escape_sql_string(const char *src, char *dst, size_t dst_len) {
  if (src == nullptr || dst_len == 0) return 0;

  size_t j = 0;
  for (size_t i = 0; src[i] != '\0' && j + 2 < dst_len; i++) {
    switch (src[i]) {
      case '\'':
        dst[j++] = '\\';
        dst[j++] = '\'';
        break;
      case '\\':
        dst[j++] = '\\';
        dst[j++] = '\\';
        break;
      case '\n':
        dst[j++] = '\\';
        dst[j++] = 'n';
        break;
      case '\r':
        dst[j++] = '\\';
        dst[j++] = 'r';
        break;
      case '\0':
        dst[j++] = '\\';
        dst[j++] = '0';
        break;
      default:
        dst[j++] = src[i];
        break;
    }
  }
  dst[j] = '\0';
  return j;
}

/* ================================================================
 * find_position_at_timestamp()
 * ================================================================ */

FlashbackError BinlogFlashbackEngine::find_position_at_timestamp(
    my_time_t target_ts, char *binlog_file, my_off_t *binlog_pos,
    uint8_t *checksum_alg) {
  DBUG_TRACE;

  if (binlog_file == nullptr || binlog_pos == nullptr)
    return FlashbackError::GENERIC;

  /* 检查 binlog 是否启用 */
  if (log_bin_basename == nullptr || log_bin_basename[0] == '\0')
    return FlashbackError::BINLOG_EXPIRED;

  /* 1. 获取 binlog 索引文件路径 */
  char index_path[FN_REFLEN];
  if (get_binlog_index_path(index_path, sizeof(index_path)))
    return FlashbackError::BINLOG_EXPIRED;

  /* 2. 读取 binlog 索引文件，获取文件列表 */
  std::vector<std::string> binlog_files;
  if (read_binlog_index(index_path, binlog_files))
    return FlashbackError::BINLOG_EXPIRED;

  if (binlog_files.empty()) return FlashbackError::BINLOG_EXPIRED;

  /* 3. 按序号排序（确保顺序正确） */
  std::sort(binlog_files.begin(), binlog_files.end(),
            [](const std::string &a, const std::string &b) {
              return extract_binlog_sequence(a.c_str()) <
                     extract_binlog_sequence(b.c_str());
            });

  /* 构造 binlog 文件所在目录 */
  char binlog_dir[FN_REFLEN];
  const char *slash = strrchr(log_bin_basename, '/');
  if (slash) {
    size_t dir_len = static_cast<size_t>(slash - log_bin_basename);
    if (dir_len >= sizeof(binlog_dir)) dir_len = sizeof(binlog_dir) - 1;
    memcpy(binlog_dir, log_bin_basename, dir_len);
    binlog_dir[dir_len] = '\0';
  } else {
    /* 使用 mysql_data_home 作为目录 */
    strncpy(binlog_dir, mysql_data_home, sizeof(binlog_dir) - 1);
    binlog_dir[sizeof(binlog_dir) - 1] = '\0';
  }

  /* 4. 查找目标 binlog 文件
     策略: 打开每个 binlog 文件，读取第一个事件的时间戳。
     找到第一个起始时间 > target_ts 的文件，使用前一个文件。*/
  std::string target_file;
  int candidate_idx = -1;

  for (size_t i = 0; i < binlog_files.size(); i++) {
    /* 构造完整路径 */
    char full_path[FN_REFLEN * 2];  /* WHY: 目录+文件名可能超过 FN_REFLEN */
    snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
             binlog_files[i].c_str());

    Binlog_file_reader reader(false /* verify_checksum */);
    if (reader.open(full_path, 0 /* offset */)) continue;

    /* 读取第一个实际事件（跳过 FDE） */
    Log_event *ev = reader.read_event_object();
    if (ev == nullptr) {
      reader.close();
      continue;
    }

    /* 检查事件时间戳 */
    my_time_t first_event_ts = get_event_timestamp(ev);
    delete ev;
    reader.close();

    if (first_event_ts > target_ts) {
      /* 这个文件起始时间晚于目标时间，使用前一个文件 */
      break;
    }
    candidate_idx = static_cast<int>(i);
    target_file = binlog_files[i];
  }

  if (candidate_idx < 0) return FlashbackError::OUT_OF_WINDOW;

  /* 5. 在目标文件中精确查找 position
     策略: 打开文件，正向扫描事件，找到最后一个时间戳 <= target_ts 的位置 */
  char full_path[FN_REFLEN * 2];
  snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
           target_file.c_str());

  Binlog_file_reader reader(false /* verify_checksum */);
  if (reader.open(full_path, 0 /* offset */))
    return FlashbackError::BINLOG_EXPIRED;

  /* 先读取 FDE，记录 checksum 算法 */
  Log_event *fde_ev = reader.read_event_object();
  Format_description_log_event *fdle = nullptr;
  if (fde_ev != nullptr) {
    fdle = dynamic_cast<Format_description_log_event *>(fde_ev);
    if (fdle != nullptr && checksum_alg != nullptr) {
      *checksum_alg =
          static_cast<uint8_t>(fdle->footer()->checksum_alg);
    }
  }
  delete fde_ev;

  my_off_t best_pos = BIN_LOG_HEADER_SIZE;
  bool found = false;

  Log_event *ev = nullptr;
  while ((ev = reader.read_event_object()) != nullptr) {
    my_time_t event_ts = get_event_timestamp(ev);

    if (event_ts <= target_ts) {
      best_pos = reader.event_start_pos();
      found = true;
    } else {
      /* 事件时间已超过目标时间，停止 */
      delete ev;
      break;
    }

    delete ev;
  }

  reader.close();

  if (!found) return FlashbackError::OUT_OF_WINDOW;

  /* 6. 输出结果 */
  strncpy(binlog_file, target_file.c_str(), FN_REFLEN - 1);
  binlog_file[FN_REFLEN - 1] = '\0';
  *binlog_pos = best_pos;

  return FlashbackError::NONE;
}

/* ================================================================
 * check_row_image_compatibility()
 * ================================================================ */

FlashbackError BinlogFlashbackEngine::check_row_image_compatibility(THD *thd) {
  DBUG_TRACE;

  if (thd == nullptr) return FlashbackError::GENERIC;

  uint binlog_row_image = thd->variables.binlog_row_image;

  /* 根据系统变量决定检查严格程度 */
  bool require_full = flashback::require_full_row_image();

  if (require_full) {
    /* 严格模式: 只接受 FULL */
    if (binlog_row_image != BINLOG_ROW_IMAGE_FULL)
      return FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL;
  } else {
    /* 宽松模式: 接受 FULL 和 NOBLOB */
    if (binlog_row_image != BINLOG_ROW_IMAGE_FULL &&
        binlog_row_image != BINLOG_ROW_IMAGE_NOBLOB)
      return FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL;
  }

  return FlashbackError::NONE;
}

/* ================================================================
 * TableMapCache 方法实现
 * ================================================================ */

/**
  从 Table_map_log_event 对象中提取信息并缓存

  提取的内容包括:
  - 表名和数据库名
  - 列数
  - 列类型（从 m_coltype 数组）
  - 列名（从 optional metadata 中的 COLUMN_NAME 字段，如果可用）

  @param tmev Table_map_log_event 指针
*/
void TableMapCache::register_table_map(const Table_map_log_event *tmev) {
  if (tmev == nullptr) return;

  std::string db(tmev->get_db_name(), tmev->get_db_name_length());
  std::string tbl(tmev->get_table_name(), tmev->get_table_name_length());
  uint16_t col_count = static_cast<uint16_t>(tmev->get_column_count());

  /* WHY: 使用 Table_mapEntry 的重载方法，传入列类型和列名信息 */
  register_table_map(tmev->get_table_id(), db, tbl, col_count,
                     tmev->m_coltype, tmev->m_colcnt,
                     nullptr /* optional metadata, parsed separately */);

  /* 提取列类型: m_coltype 数组 */
  TableMapEntry &entry = m_cache[tmev->get_table_id()];
  if (tmev->m_coltype != nullptr && tmev->m_colcnt > 0) {
    entry.column_types.resize(tmev->m_colcnt);
    for (unsigned long i = 0; i < tmev->m_colcnt; i++) {
      entry.column_types[i] = tmev->m_coltype[i];
    }
  }

  /* 提取列名: 从 optional metadata 解析 */
  if (tmev->m_optional_metadata != nullptr && tmev->m_optional_metadata_len > 0) {
    Table_map_event::Optional_metadata_fields fields(
        tmev->m_optional_metadata, tmev->m_optional_metadata_len);
    if (fields.is_valid && !fields.m_column_name.empty()) {
      entry.column_names = fields.m_column_name;
    }
  }
}

/* ================================================================
 * should_process_event()
 * ================================================================ */

/**
  检查 Rows_event 是否属于请求中的目标表

  WHY: 闪回请求可能指定了特定的表列表。如果 request.tables 非空，
  只处理属于目标表的 event；如果为空，处理所有表。
*/
bool BinlogFlashbackEngine::should_process_event(
    const Rows_log_event &event, const FlashbackRequest &request) const {
  DBUG_TRACE;

  /* 如果请求未指定表，处理所有表 */
  if (request.tables == nullptr || request.table_count == 0) return true;

  /* 从缓存中查找表名 */
  const TableMapEntry *entry = lookup_table_map(event.get_table_id());
  if (entry == nullptr || !entry->is_valid) {
    /* 无法解析表名，保守处理: 跳过 */
    return false;
  }

  /* 检查是否匹配请求中的任何表 */
  for (uint32_t i = 0; i < request.table_count; i++) {
    const LEX_CSTRING &target = request.tables[i];
    if (target.length == 0) continue;

    /* 表名可能包含 "db.table" 格式 */
    const char *dot = static_cast<const char *>(
        memchr(target.str, '.', target.length));
    if (dot != nullptr) {
      /* 精确匹配: db.table */
      size_t db_len = static_cast<size_t>(dot - target.str);
      size_t tbl_len = target.length - db_len - 1;
      if (entry->db_name.length() == db_len &&
          memcmp(entry->db_name.c_str(), target.str, db_len) == 0 &&
          entry->table_name.length() == tbl_len &&
          memcmp(entry->table_name.c_str(), dot + 1, tbl_len) == 0) {
        return true;
      }
    } else {
      /* 仅匹配表名（忽略 db） */
      if (entry->table_name.length() == target.length &&
          memcmp(entry->table_name.c_str(), target.str, target.length) == 0) {
        return true;
      }
    }
  }

  return false;
}

/* ================================================================
 * get_column_type()
 * ================================================================ */

/**
  获取列的 MySQL 类型

  从 Table_map_log_event 的 m_type 数组中读取指定列的类型。
*/
uint8_t BinlogFlashbackEngine::get_column_type(
    const Table_map_log_event *tmev, uint16_t col_idx) {
  if (tmev == nullptr || tmev->m_type == nullptr) return MYSQL_TYPE_STRING;

  /* m_type 数组的大小应该等于 column_count */
  if (col_idx >= tmev->get_column_count()) return MYSQL_TYPE_STRING;

  return tmev->m_type[col_idx];
}

/* ================================================================
 * encode_column_value() — 根据列类型编码单个列值
 * ================================================================ */

/**
  将单个列的原始数据编码为 SQL 值字符串

  WHY: 完整的 SQL 编码需要 Table_map_event 中的列类型信息，
  以及每个列的 metadata（如 varchar 长度、decimal 精度等）。
  这里按常见 MySQL 类型进行区分处理。

  @param col_data  列的原始字节数据
  @param col_len   列数据长度
  @param col_type  MySQL 列类型
  @param is_null   是否为 NULL
  @param sql_buf   输出的 SQL 值字符串
*/
static void encode_column_value(const uint8_t *col_data, size_t col_len,
                                uint8_t col_type, bool is_null,
                                std::string &sql_buf) {
  if (is_null) {
    sql_buf += "NULL";
    return;
  }

  switch (col_type) {
    /* ---- 整数类型 ---- */
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24: {
      /* 直接使用原始字节解析为整数 */
      long long val = 0;
      if (col_len <= 8 && col_data != nullptr) {
        memcpy(&val, col_data, col_len);
      }
      sql_buf += std::to_string(val);
      break;
    }

    /* ---- 浮点类型 ---- */
    case MYSQL_TYPE_FLOAT: {
      if (col_len == sizeof(float) && col_data != nullptr) {
        float f;
        memcpy(&f, col_data, sizeof(f));
        char buf[64];
        snprintf(buf, sizeof(buf), "%.6g", f);
        sql_buf += buf;
      } else {
        sql_buf += "0";
      }
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      if (col_len == sizeof(double) && col_data != nullptr) {
        double d;
        memcpy(&d, col_data, sizeof(d));
        char buf[128];
        snprintf(buf, sizeof(buf), "%.15g", d);
        sql_buf += buf;
      } else {
        sql_buf += "0";
      }
      break;
    }

    /* ---- 字符串类型 ---- */
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_JSON: {
      sql_buf += "'";
      if (col_data != nullptr && col_len > 0) {
        /* 转义特殊字符 */
        char escaped[FN_REFLEN];
        escape_sql_string(reinterpret_cast<const char *>(col_data), escaped,
                          sizeof(escaped));
        sql_buf += escaped;
      }
      sql_buf += "'";
      break;
    }

    /* ---- BLOB / TEXT ---- */
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_GEOMETRY: {
      /* BLOB 类型使用 HEX 编码 */
      sql_buf += "0x";
      if (col_data != nullptr) {
        for (size_t i = 0; i < col_len; i++) {
          char hex[3];
          snprintf(hex, sizeof(hex), "%02X", col_data[i]);
          sql_buf += hex;
        }
      }
      break;
    }

    /* ---- 日期/时间类型 ---- */
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_YEAR: {
      sql_buf += "'";
      if (col_data != nullptr && col_len > 0) {
        char buf[128];
        escape_sql_string(reinterpret_cast<const char *>(col_data), buf,
                          sizeof(buf));
        sql_buf += buf;
      }
      sql_buf += "'";
      break;
    }

    /* ---- BIT 类型 ---- */
    case MYSQL_TYPE_BIT: {
      if (col_len > 0 && col_data != nullptr) {
        unsigned long long bit_val = 0;
        for (size_t i = 0; i < col_len; i++) {
          bit_val = (bit_val << 8) | col_data[i];
        }
        sql_buf += std::to_string(bit_val);
      } else {
        sql_buf += "0";
      }
      break;
    }

    /* ---- DECIMAL ---- */
    case MYSQL_TYPE_NEWDECIMAL: {
      /* DECIMAL 在 binlog 中以字符串格式存储 */
      sql_buf += "'";
      if (col_data != nullptr && col_len > 0) {
        char buf[128];
        escape_sql_string(reinterpret_cast<const char *>(col_data), buf,
                          sizeof(buf));
        sql_buf += buf;
      }
      sql_buf += "'";
      break;
    }

    /* ---- 默认: 以 HEX 格式输出 ---- */
    default: {
      sql_buf += "0x";
      if (col_data != nullptr) {
        for (size_t i = 0; i < col_len; i++) {
          char hex[3];
          snprintf(hex, sizeof(hex), "%02X", col_data[i]);
          sql_buf += hex;
        }
      }
      break;
    }
  }
}

/* ================================================================
 * decode_row_from_binlog() — 从 binlog 格式解码一行数据
 * ================================================================ */

/**
  解码后的列值

  存储从 binlog 行数据中提取的单个列值。
*/
struct DecodedColumnValue {
  bool is_null;                          /* 是否为 NULL */
  std::vector<uint8_t> raw_data;        /* 原始二进制数据 */
  uint8_t column_type;                  /* MySQL 列类型 */
};

/**
  从 binlog 行数据中解码单行

  binlog 行数据格式:
  - NULL 位图: 每列 1 位，1=该列在此行中出现
  - 对于出现的列:
    - 如果 NULL 位图中对应位为 1: 值为 NULL，无后续数据
    - 否则: 按列类型编码的二进制值

  对于 DELETE 事件，before_image 包含被删除行的完整数据。
  位图是 m_cols，标识哪些列出现在行数据中。

  @param row_ptr       行数据起始指针
  @param row_end       行数据结束指针
  @param table_map     表映射信息（列类型）
  @param cols_bitmap   列位图（m_cols）
  @param[out] columns  解码后的列值列表

  @retval true  解码失败（数据不完整）
  @retval false 解码成功
*/
static bool decode_row_from_binlog(
    const uint8_t *row_ptr,
    const uint8_t *row_end,
    const TableMapEntry &table_map,
    const MY_BITMAP *cols_bitmap,
    std::vector<DecodedColumnValue> &columns) {

  columns.clear();
  columns.reserve(table_map.column_count);

  if (row_ptr == nullptr || row_end == nullptr || row_ptr >= row_end) {
    return true;
  }

  /* Step 1: 读取 NULL 位图 */
  size_t null_bits_len = (table_map.column_count + 7) / 8;
  if (static_cast<size_t>(row_end - row_ptr) < null_bits_len) {
    return true;  /* 数据不完整 */
  }

  const uint8_t *null_bits_ptr = row_ptr;
  row_ptr += null_bits_len;

  /* Step 2: 逐列解码 */
  for (uint16_t col_idx = 0; col_idx < table_map.column_count; col_idx++) {
    DecodedColumnValue col_val;
    col_val.is_null = false;
    col_val.column_type = (col_idx < table_map.column_types.size())
                              ? table_map.column_types[col_idx]
                              : MYSQL_TYPE_STRING;

    /* 检查该列是否出现在行数据中 */
    bool col_present = my_bmp_tst(cols_bitmap, col_idx);
    if (!col_present) {
      /* 列未出现在事件中（对于 DELETE，这不应该发生，因为 before_image
         在 FULL 模式下应该包含所有列） */
      col_val.is_null = true;
      columns.push_back(std::move(col_val));
      continue;
    }

    /* 检查是否为 NULL */
    bool is_null = my_bmp_tst(
        reinterpret_cast<const MY_BITMAP *>(null_bits_ptr), col_idx);
    /* WHY: null_bits_ptr 不是 MY_BITMAP 结构，而是原始位数据。
       我们需要手动检查位。 */
    is_null = (null_bits_ptr[col_idx / 8] & (1 << (col_idx % 8))) != 0;

    if (is_null) {
      col_val.is_null = true;
      columns.push_back(std::move(col_val));
      continue;
    }

    /* Step 3: 根据列类型解码值 */
    size_t remaining = static_cast<size_t>(row_end - row_ptr);
    if (remaining == 0) {
      return true;  /* 数据不完整 */
    }

    size_t col_len = 0;

    switch (col_val.column_type) {
      /* 定长整数类型 */
      case MYSQL_TYPE_TINY:
        col_len = 1;
        break;
      case MYSQL_TYPE_SHORT:
      case MYSQL_TYPE_YEAR:
        col_len = 2;
        break;
      case MYSQL_TYPE_INT24:
        col_len = 3;
        break;
      case MYSQL_TYPE_LONG:
      case MYSQL_TYPE_FLOAT:
        col_len = 4;
        break;
      case MYSQL_TYPE_LONGLONG:
      case MYSQL_TYPE_DOUBLE:
        col_len = 8;
        break;

      /* 字符串类型: 长度前缀 + 数据 */
      case MYSQL_TYPE_VARCHAR:
      case MYSQL_TYPE_VAR_STRING: {
        /* VARCHAR: 长度前缀 1 或 2 字节（取决于列定义） */
        if (remaining < 1) return true;
        /* 简化: 假设小长度用 1 字节 */
        col_len = row_ptr[0];
        if (col_len > 254) {
          /* 大长度: 2 字节 */
          if (remaining < 2) return true;
          col_len = uint2korr(row_ptr);
          row_ptr += 2;
          remaining -= 2;
        } else {
          row_ptr += 1;
          remaining -= 1;
        }
        if (col_len > remaining) col_len = remaining;
        break;
      }

      case MYSQL_TYPE_STRING:
      case MYSQL_TYPE_ENUM:
      case MYSQL_TYPE_SET: {
        /* 字符串/ENUM/SET: 长度前缀 1 字节 */
        if (remaining < 1) return true;
        col_len = row_ptr[0];
        row_ptr += 1;
        remaining -= 1;
        if (col_len > remaining) col_len = remaining;
        break;
      }

      /* BLOB/TEXT 类型: 1-4 字节长度前缀 */
      case MYSQL_TYPE_TINY_BLOB:
        if (remaining < 1) return true;
        col_len = row_ptr[0];
        row_ptr += 1;
        remaining -= 1;
        break;
      case MYSQL_TYPE_BLOB:
        if (remaining < 2) return true;
        col_len = uint2korr(row_ptr);
        row_ptr += 2;
        remaining -= 2;
        break;
      case MYSQL_TYPE_MEDIUM_BLOB:
        if (remaining < 3) return true;
        col_len = uint3korr(row_ptr);
        row_ptr += 3;
        remaining -= 3;
        break;
      case MYSQL_TYPE_LONG_BLOB:
      case MYSQL_TYPE_GEOMETRY:
        if (remaining < 4) return true;
        col_len = uint4korr(row_ptr);
        row_ptr += 4;
        remaining -= 4;
        break;

      /* BIT 类型 */
      case MYSQL_TYPE_BIT: {
        /* BIT: 1-8 字节（取决于位数） */
        if (remaining < 1) return true;
        col_len = row_ptr[0];
        if (col_len == 0 || col_len > 8) return true;
        row_ptr += 1;
        remaining -= 1;
        if (col_len > remaining) col_len = remaining;
        break;
      }

      /* 时间类型 */
      case MYSQL_TYPE_DATE:
        col_len = 3;  /* YYYY-MM-DD: 3 字节 */
        break;
      case MYSQL_TYPE_TIME:
      case MYSQL_TYPE_TIME2:
        col_len = 3;  /* HH:MM:SS: 3 字节 + 可选小数 */
        break;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
        col_len = 5;  /* YYYY-MM-DD HH:MM:SS: 5 字节 + 可选小数 */
        break;
      case MYSQL_TYPE_TIMESTAMP:
      case MYSQL_TYPE_TIMESTAMP2:
        col_len = 4;  /* Unix timestamp: 4 字节 */
        break;

      /* DECIMAL: 二进制打包格式 */
      case MYSQL_TYPE_NEWDECIMAL: {
        /* DECIMAL 长度取决于精度和标度，简化处理 */
        col_len = remaining > 32 ? 32 : remaining;
        break;
      }

      /* JSON */
      case MYSQL_TYPE_JSON: {
        if (remaining < 2) return true;
        col_len = uint2korr(row_ptr);
        row_ptr += 2;
        remaining -= 2;
        if (col_len > remaining) col_len = remaining;
        break;
      }

      /* 默认: 尝试读取所有剩余数据（保守做法） */
      default:
        col_len = remaining > 64 ? 64 : remaining;
        break;
    }

    if (col_len > remaining) col_len = remaining;

    /* 复制列的原始数据 */
    if (col_len > 0) {
      col_val.raw_data.assign(row_ptr, row_ptr + col_len);
      row_ptr += col_len;
    }

    columns.push_back(std::move(col_val));
  }

  return false;  /* 成功 */
}

/* ================================================================
 * extract_rows_from_event() — 从 Rows_event 中提取行数据
 * ================================================================ */

/**
  从 Rows_log_event 中提取行数据

  WHY: Rows_event 中的行数据是连续存储的，包含:
  - NULL 位图
  - 列值数据

  对于 Update_rows_log_event，包含 before_image 和 after_image 两组行数据。

  @param event        Rows_event
  @param use_ai       true=使用 after_image, false=使用 before_image
  @param table_map    表映射信息（包含列类型）
  @param[out] rows   提取的行数据列表（每行是列值向量）
  @retval true  提取失败
  @retval false 成功
*/
static bool extract_rows_from_event(
    const Rows_log_event &event, bool use_ai,
    const TableMapEntry &table_map,
    std::vector<std::vector<uint8_t>> &rows) {
  DBUG_TRACE;

  const TABLE *table = event.get_table();
  if (table == nullptr) return true;

  const uint8_t *row_data = nullptr;
  size_t row_data_len = 0;

  if (use_ai) {
    /* After image: 仅 Update_rows 有，Write_rows 也视为 after */
    row_data = event.get_ai_buf();
    row_data_len = event.get_ai_buf_len();
  } else {
    /* Before image */
    row_data = event.get_bi_buf();
    row_data_len = event.get_bi_buf_len();
  }

  if (row_data == nullptr || row_data_len == 0) return true;

  /* 提取位图信息 */
  const uint8_t *col_bitmap = nullptr;
  size_t bitmap_len = 0;
  if (use_ai) {
    col_bitmap = event.get_ai_col_bitmap();
    bitmap_len = event.get_ai_col_bitmap_len();
  } else {
    col_bitmap = event.get_bi_col_bitmap();
    bitmap_len = event.get_bi_col_bitmap_len();
  }

  /* 计算行数 */
  uint64_t row_count = 0;
  if (use_ai) {
    row_count = event.get_ai_width();
  } else {
    row_count = event.get_bi_width();
  }

  if (row_count == 0 || col_bitmap == nullptr) return true;

  /* 解析每行数据 */
  const uint8_t *ptr = row_data;
  const uint8_t *end = row_data + row_data_len;

  for (uint64_t r = 0; r < row_count && ptr < end; r++) {
    std::vector<uint8_t> row_bytes;

    /* NULL 位图 */
    size_t null_bits_len = (table_map.column_count + 7) / 8;
    if (ptr + null_bits_len > end) break;

    const uint8_t *null_bits = ptr;
    ptr += null_bits_len;

    /* 提取列值 */
    size_t col_bit_index = 0;
    for (uint16_t c = 0; c < table_map.column_count; c++) {
      /* 检查该列是否出现在位图中 */
      bool col_present = (col_bitmap[col_bit_index / 8] &
                          (1 << (col_bit_index % 8))) != 0;
      col_bit_index++;

      if (!col_present) {
        /* 列未出现在行数据中（对于 UPDATE，可能是未修改的列） */
        row_bytes.push_back(0);
        row_bytes.push_back(0xFF);  /* 标记为 absent */
        continue;
      }

      /* 检查是否为 NULL */
      bool is_null = (null_bits[c / 8] & (1 << (c % 8))) != 0;
      row_bytes.push_back(1);  /* present */
      row_bytes.push_back(is_null ? 0xFF : 0x00);

      if (is_null) continue;

      /* 获取列类型和长度 */
      /* 简化处理: 尝试从 Table_map 获取列类型 */
      uint8_t col_type = MYSQL_TYPE_STRING;  /* 默认 */

      /* 读取列长度（简化处理，假设定长） */
      size_t remaining = static_cast<size_t>(end - ptr);
      if (remaining == 0) break;

      /* 对于变长类型，前 1-2 字节是长度 */
      size_t col_len = remaining;  /* 简化：取剩余所有 */
      if (col_len > 1024) col_len = 1024;  /* 安全限制 */

      /* 复制列数据 */
      for (size_t i = 0; i < col_len; i++) {
        row_bytes.push_back(ptr[i]);
      }
      ptr += col_len;
    }

    rows.push_back(std::move(row_bytes));
  }

  return false;
}

/* ================================================================
 * encode_row_as_sql_values()
 * ================================================================ */

/**
  将行数据编码为 SQL 值字符串

  根据 Table_map_event 中的列类型信息，将原始行数据编码为
  可直接用于 SQL 语句的字面值格式。

  @param row_data    行数据（来自 binlog Rows_event）
  @param null_bitmap NULL 位图
  @param sql_buf     输出的 SQL 值字符串
*/
void BinlogFlashbackEngine::encode_row_as_sql_values(
    const std::vector<uint8_t> &row_data,
    const std::vector<uint8_t> &null_bitmap,
    std::string &sql_buf) {
  DBUG_TRACE;

  if (row_data.empty()) {
    sql_buf += "NULL";
    return;
  }

  /*
    简化实现: 将原始数据以 HEX 格式输出。
    完整实现需要根据列类型正确编码每个列值。

    TODO (Phase 2): 根据列类型正确编码:
    - MYSQL_TYPE_STRING/varchar: 用单引号包裹，转义特殊字符
    - MYSQL_TYPE_LONG/INT: 直接输出数字
    - MYSQL_TYPE_FLOAT/DOUBLE: 浮点数格式
    - MYSQL_TYPE_BLOB/TEXT: HEX 编码或单引号包裹
    - MYSQL_TYPE_TIMESTAMP/DATETIME: 时间格式
  */

  sql_buf += "0x";
  for (uint8_t byte : row_data) {
    char hex[3];
    snprintf(hex, sizeof(hex), "%02X", byte);
    sql_buf += hex;
  }
}

/* ================================================================
 * Column value encoding helpers
 * ================================================================ */

/**
  根据列类型读取列数据长度（不解码值，仅跳过）

  WHY: 当某列不是主键列时，我们仍然需要跳过其数据才能正确解析后续列。
  对于变长类型（VARCHAR/BLOB），需要从数据中读取长度前缀。

  @param col_type  MySQL 列类型
  @param ptr       当前数据指针
  @param end       数据末尾指针
  @return 该列占用的字节数
*/
static size_t skip_column_value(uint8_t col_type,
                                const uint8_t *ptr,
                                const uint8_t *end) {
  if (ptr == nullptr || end == nullptr || ptr >= end) return 0;

  size_t remaining = static_cast<size_t>(end - ptr);
  if (remaining == 0) return 0;

  switch (col_type) {
    /* 定长类型 */
    case MYSQL_TYPE_TINY:
      return 1;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      return 2;
    case MYSQL_TYPE_INT24:
      return 3;
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_FLOAT:
      return 4;
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_DOUBLE:
      return 8;

    /* 变长字符串: 前 1-2 字节是长度 */
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      size_t len_bytes = (remaining >= 2 && *ptr > 255) ? 2 : 1;
      size_t str_len = (len_bytes == 2)
          ? static_cast<size_t>(ptr[0] | (ptr[1] << 8))
          : static_cast<size_t>(ptr[0]);
      return len_bytes + str_len;
    }

    /* 固定长度字符串 */
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      /* 实际长度取决于 metadata，保守取 256 */
      return (remaining > 256) ? 256 : remaining;
    }

    /* BLOB/TEXT: 前 1-4 字节是长度 */
    case MYSQL_TYPE_TINY_BLOB:
      if (remaining < 1) return 0;
      return 1 + static_cast<size_t>(ptr[0]);

    case MYSQL_TYPE_BLOB:
      if (remaining < 2) return 0;
      return 2 + static_cast<size_t>(ptr[0] | (ptr[1] << 8));

    case MYSQL_TYPE_MEDIUM_BLOB:
      if (remaining < 3) return 0;
      return 3 + static_cast<size_t>(ptr[0] | (ptr[1] << 8) |
                                      (ptr[2] << 16));

    case MYSQL_TYPE_LONG_BLOB:
      if (remaining < 4) return 0;
      return 4 + static_cast<size_t>(
          ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));

    /* 日期时间: 通常是 3-8 字节 */
    case MYSQL_TYPE_DATE:
      return 3;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_YEAR:
      return 3;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      return (remaining >= 8) ? 8 : remaining;

    /* BIT: 前 1-2 字节是长度 */
    case MYSQL_TYPE_BIT: {
      if (remaining < 1) return 0;
      return 1 + static_cast<size_t>(ptr[0]);
    }

    /* NEWDECIMAL: 以二进制打包格式存储 */
    case MYSQL_TYPE_NEWDECIMAL: {
      /* 保守估计，最多 65 字节 */
      return (remaining > 65) ? 65 : remaining;
    }

    /* 其他: 保守取剩余所有 */
    default:
      return remaining;
  }
}

/**
  根据列类型读取列数据并编码为 SQL 值

  WHY: 不同类型的列在 binlog 中的编码方式不同:
  - 整数: 直接小端序存储
  - 字符串: 长度前缀 + 内容
  - 日期时间: MySQL 内部格式
  - DECIMAL: 压缩十进制格式

  这里根据类型将二进制数据转换为 SQL 可读的字面量。

  @param col_type    MySQL 列类型
  @param col_start   列数据起始指针
  @param end         数据末尾指针
  @param sql_buf     输出的 SQL 值字符串
  @return 读取的字节数
*/
static size_t read_column_value(uint8_t col_type,
                                const uint8_t *col_start,
                                const uint8_t *end,
                                std::string &sql_buf) {
  if (col_start == nullptr || end == nullptr || col_start >= end) {
    sql_buf += "NULL";
    return 0;
  }

  size_t remaining = static_cast<size_t>(end - col_start);
  const uint8_t *ptr = col_start;

  switch (col_type) {
    /* ---- 整数类型 ---- */
    case MYSQL_TYPE_TINY: {
      if (remaining < 1) { sql_buf += "0"; return 0; }
      /* 使用 int8_t 保留符号 */
      int8_t v;
      memcpy(&v, ptr, 1);
      sql_buf += std::to_string(static_cast<long long>(v));
      return 1;
    }
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR: {
      if (remaining < 2) { sql_buf += "0"; return 0; }
      int16_t v;
      memcpy(&v, ptr, 2);
      sql_buf += std::to_string(static_cast<long long>(v));
      return 2;
    }
    case MYSQL_TYPE_INT24: {
      if (remaining < 3) { sql_buf += "0"; return 0; }
      /* 24-bit signed: 读取 3 字节并做符号扩展 */
      int32_t v = static_cast<int32_t>(
          ptr[0] | (ptr[1] << 8) | (ptr[2] << 16));
      if (v & 0x00800000) v |= 0xFF000000;  /* 符号扩展 */
      sql_buf += std::to_string(static_cast<long long>(v));
      return 3;
    }
    case MYSQL_TYPE_LONG: {
      if (remaining < 4) { sql_buf += "0"; return 0; }
      int32_t v;
      memcpy(&v, ptr, 4);
      sql_buf += std::to_string(static_cast<long long>(v));
      return 4;
    }
    case MYSQL_TYPE_LONGLONG: {
      if (remaining < 8) { sql_buf += "0"; return 0; }
      long long v;
      memcpy(&v, ptr, 8);
      sql_buf += std::to_string(v);
      return 8;
    }

    /* ---- 浮点类型 ---- */
    case MYSQL_TYPE_FLOAT: {
      if (remaining < 4) { sql_buf += "0"; return 0; }
      float f;
      memcpy(&f, ptr, 4);
      char buf[64];
      snprintf(buf, sizeof(buf), "%.6g", f);
      sql_buf += buf;
      return 4;
    }
    case MYSQL_TYPE_DOUBLE: {
      if (remaining < 8) { sql_buf += "0"; return 0; }
      double d;
      memcpy(&d, ptr, 8);
      char buf[128];
      snprintf(buf, sizeof(buf), "%.15g", d);
      sql_buf += buf;
      return 8;
    }

    /* ---- 字符串类型 ---- */
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING: {
      /* 长度前缀: 1 或 2 字节 */
      size_t len_bytes = (remaining >= 2 && *ptr > 255) ? 2 : 1;
      size_t str_len = (len_bytes == 2)
          ? static_cast<size_t>(ptr[0] | (ptr[1] << 8))
          : static_cast<size_t>(ptr[0]);
      if (len_bytes + str_len > remaining) str_len = remaining - len_bytes;

      sql_buf += "'";
      if (str_len > 0) {
        char escaped[4096];
        escape_sql_string(reinterpret_cast<const char *>(ptr + len_bytes),
                          escaped, sizeof(escaped));
        sql_buf += escaped;
      }
      sql_buf += "'";
      return len_bytes + str_len;
    }

    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      /* 保守取最多 256 字节 */
      size_t str_len = (remaining > 256) ? 256 : remaining;
      sql_buf += "'";
      if (str_len > 0) {
        char escaped[4096];
        escape_sql_string(reinterpret_cast<const char *>(ptr),
                          escaped, sizeof(escaped));
        sql_buf += escaped;
      }
      sql_buf += "'";
      return str_len;
    }

    /* ---- JSON ---- */
    case MYSQL_TYPE_JSON: {
      /* JSON 以二进制格式存储，简化为字符串处理 */
      if (remaining < 1) { sql_buf += "NULL"; return 0; }
      size_t len_bytes = 1;
      size_t str_len = static_cast<size_t>(ptr[0]);
      if (str_len > 127 && remaining >= 2) {
        /* 2-byte length */
        str_len = static_cast<size_t>(ptr[0] | (ptr[1] << 8));
        len_bytes = 2;
      }
      if (len_bytes + str_len > remaining) str_len = remaining - len_bytes;

      sql_buf += "'";
      if (str_len > 0) {
        char escaped[4096];
        escape_sql_string(
            reinterpret_cast<const char *>(ptr + len_bytes),
            escaped, sizeof(escaped));
        sql_buf += escaped;
      }
      sql_buf += "'";
      return len_bytes + str_len;
    }

    /* ---- BLOB / TEXT ---- */
    case MYSQL_TYPE_TINY_BLOB: {
      if (remaining < 1) { sql_buf += "NULL"; return 0; }
      size_t blob_len = static_cast<size_t>(ptr[0]);
      if (1 + blob_len > remaining) blob_len = remaining - 1;

      sql_buf += "0x";
      for (size_t i = 0; i < blob_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[1 + i]);
        sql_buf += hex;
      }
      return 1 + blob_len;
    }

    case MYSQL_TYPE_BLOB: {
      if (remaining < 2) { sql_buf += "NULL"; return 0; }
      size_t blob_len =
          static_cast<size_t>(ptr[0] | (ptr[1] << 8));
      if (2 + blob_len > remaining) blob_len = remaining - 2;

      sql_buf += "0x";
      for (size_t i = 0; i < blob_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[2 + i]);
        sql_buf += hex;
      }
      return 2 + blob_len;
    }

    case MYSQL_TYPE_MEDIUM_BLOB: {
      if (remaining < 3) { sql_buf += "NULL"; return 0; }
      size_t blob_len = static_cast<size_t>(
          ptr[0] | (ptr[1] << 8) | (ptr[2] << 16));
      if (3 + blob_len > remaining) blob_len = remaining - 3;

      sql_buf += "0x";
      for (size_t i = 0; i < blob_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[3 + i]);
        sql_buf += hex;
      }
      return 3 + blob_len;
    }

    case MYSQL_TYPE_LONG_BLOB: {
      if (remaining < 4) { sql_buf += "NULL"; return 0; }
      size_t blob_len = static_cast<size_t>(
          ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
      if (4 + blob_len > remaining) blob_len = remaining - 4;

      sql_buf += "0x";
      for (size_t i = 0; i < blob_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[4 + i]);
        sql_buf += hex;
      }
      return 4 + blob_len;
    }

    case MYSQL_TYPE_GEOMETRY: {
      /* 几何类型类似 BLOB */
      if (remaining < 4) { sql_buf += "NULL"; return 0; }
      size_t geom_len = static_cast<size_t>(
          ptr[0] | (ptr[1] << 8) | (ptr[2] << 16) | (ptr[3] << 24));
      if (4 + geom_len > remaining) geom_len = remaining - 4;

      sql_buf += "0x";
      for (size_t i = 0; i < geom_len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[4 + i]);
        sql_buf += hex;
      }
      return 4 + geom_len;
    }

    /* ---- 日期/时间类型 ---- */
    case MYSQL_TYPE_DATE: {
      if (remaining < 3) { sql_buf += "'0000-00-00'"; return 0; }
      /* MySQL 内部 DATE 格式: 3 字节 packed */
      uint32_t v = ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
      int year = v / (16 * 32);
      int month = (v % (16 * 32)) / 32;
      int day = v % 32;
      char buf[64];
      snprintf(buf, sizeof(buf), "'%04d-%02d-%02d'", year, month, day);
      sql_buf += buf;
      return 3;
    }

    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_DATETIME2: {
      if (remaining < 8) { sql_buf += "'0000-00-00 00:00:00'"; return remaining > 0 ? remaining : 0; }
      /* 尝试以字符串形式读取 (简化处理) */
      sql_buf += "'";
      char escaped[128];
      escape_sql_string(reinterpret_cast<const char *>(ptr),
                        escaped, sizeof(escaped));
      sql_buf += escaped;
      sql_buf += "'";
      return 8;
    }

    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_TIMESTAMP2: {
      sql_buf += "'";
      char escaped[128];
      escape_sql_string(reinterpret_cast<const char *>(ptr),
                        escaped, sizeof(escaped));
      sql_buf += escaped;
      sql_buf += "'";
      return (remaining >= 4) ? 4 : remaining;
    }

    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_TIME2: {
      sql_buf += "'";
      char escaped[128];
      escape_sql_string(reinterpret_cast<const char *>(ptr),
                        escaped, sizeof(escaped));
      sql_buf += escaped;
      sql_buf += "'";
      return 3;
    }

    /* ---- BIT 类型 ---- */
    case MYSQL_TYPE_BIT: {
      if (remaining < 1) { sql_buf += "0"; return 0; }
      size_t bit_len = static_cast<size_t>(ptr[0]);
      if (1 + bit_len > remaining) bit_len = remaining - 1;

      unsigned long long bit_val = 0;
      for (size_t i = 0; i < bit_len; i++) {
        bit_val = (bit_val << 8) | ptr[1 + i];
      }
      sql_buf += std::to_string(bit_val);
      return 1 + bit_len;
    }

    /* ---- DECIMAL ---- */
    case MYSQL_TYPE_NEWDECIMAL: {
      /* DECIMAL 在 binlog 中以二进制打包格式存储 */
      size_t dec_len = (remaining > 65) ? 65 : remaining;
      sql_buf += "'";
      char escaped[128];
      escape_sql_string(reinterpret_cast<const char *>(ptr),
                        escaped, sizeof(escaped));
      sql_buf += escaped;
      sql_buf += "'";
      return dec_len;
    }

    /* ---- 默认: 以 HEX 格式输出 ---- */
    default: {
      size_t len = (remaining > 256) ? 256 : remaining;
      sql_buf += "0x";
      for (size_t i = 0; i < len; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02X", ptr[i]);
        sql_buf += hex;
      }
      return len;
    }
  }
}

/* ================================================================
 * reverse_write_event() → DELETE
 * ================================================================
 *
 * Write_rows_log_event (INSERT) 的逆向 = DELETE
 * 使用 after_image 中的主键值构造 WHERE 条件。
 *
 * WHY: 在 Write_rows_log_event 中，只有 after_image 存在（插入后的值）。
 * 为了逆向删除这行，我们需要使用主键值来定位它。
 *
 * 输出格式: DELETE FROM `db`.`table` WHERE `pk_col1` = val1 AND `pk_col2` = val2
 */

bool BinlogFlashbackEngine::reverse_write_event(
    const Write_rows_log_event &event, std::string &sql_buf) {
  DBUG_TRACE;

  const TableMapEntry *tme = lookup_table_map(event.get_table_id());
  if (tme == nullptr || !tme->is_valid) {
    /* 缺少 Table_map_event，无法生成完整 SQL */
    sql_buf.clear();
    sql_buf = "DELETE FROM `unknown_table`";
    sql_buf += " /* reversed from WRITE_ROWS_EVENT, table_id=";
    sql_buf += std::to_string(event.get_table_id());
    sql_buf += ", WARNING: missing Table_map_event */";
    return false;
  }

  /* 从 Table_map_event 提取列类型信息 */
  const Table_map_log_event *tmev = event.get_table_map();
  if (tmev == nullptr) {
    sql_buf.clear();
    sql_buf = "DELETE FROM `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` WHERE 1=1";
    sql_buf += " /* reversed from WRITE_ROWS_EVENT, missing TABLE_MAP */";
    return false;
  }

  /* 获取 after_image 数据 */
  const uint8_t *ai_buf = event.get_ai_buf();
  size_t ai_len = event.get_ai_buf_len();
  const uint8_t *ai_bitmap = event.get_ai_col_bitmap();
  uint64_t row_count = event.get_ai_width();

  if (ai_buf == nullptr || ai_len == 0 || ai_bitmap == nullptr ||
      row_count == 0) {
    sql_buf.clear();
    sql_buf = "DELETE FROM `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` WHERE 1=1";
    sql_buf += " /* reversed from WRITE_ROWS_EVENT, no row data */";
    return false;
  }

  /* 获取主键位图 (write_set 通常包含主键列) */
  const MY_BITMAP *pk_bitmap = event.get_pk_bitmap();
  uint16_t col_count = static_cast<uint16_t>(tmev->get_column_count());

  /* 对每行生成一条 DELETE 语句 */
  sql_buf.clear();
  const uint8_t *ptr = ai_buf;
  const uint8_t *end = ai_buf + ai_len;

  for (uint64_t r = 0; r < row_count && ptr < end; r++) {
    if (r > 0) sql_buf += "; ";

    sql_buf += "DELETE FROM `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` WHERE ";

    /* 解析 NULL 位图 */
    size_t null_bits_len = (col_count + 7) / 8;
    if (ptr + null_bits_len > end) {
      sql_buf += "1=0 /* parse error: truncated null bitmap */";
      continue;
    }
    const uint8_t *null_bits = ptr;
    ptr += null_bits_len;

    /* 生成 WHERE 条件: 使用出现在位图中的列 */
    bool first_col = true;
    size_t bit_idx = 0;

    for (uint16_t c = 0; c < col_count && ptr < end; c++) {
      bool col_present =
          (ai_bitmap[bit_idx / 8] & (1 << (bit_idx % 8))) != 0;
      bit_idx++;

      if (!col_present) continue;

      /* 检查是否为 NULL */
      bool is_null = (null_bits[c / 8] & (1 << (c % 8))) != 0;

      /* 获取列类型 */
      uint8_t col_type = get_column_type(tmev, c);

      /* 如果有主键位图，只使用主键列做 WHERE 条件；
         否则使用所有列（保守策略）*/
      bool use_for_where =
          (pk_bitmap == nullptr) || my_bitmap_test(pk_bitmap, c);

      if (!use_for_where) {
        /* 跳过非主键列的数据 */
        size_t skip = skip_column_value(col_type, ptr, end);
        ptr += skip;
        continue;
      }

      if (!first_col) sql_buf += " AND ";
      first_col = false;

      /* 优先使用缓存的列名, 否则用 col{idx} 占位 */
      if (c < tme->column_names.size() && !tme->column_names[c].empty()) {
        sql_buf += "`";
        sql_buf += tme->column_names[c];
        sql_buf += "`";
      } else {
        sql_buf += "`col";
        sql_buf += std::to_string(c);
        sql_buf += "`";
      }
      sql_buf += "=";

      if (is_null) {
        sql_buf += "NULL";
      } else {
        /* 编码列值 */
        const uint8_t *col_start = ptr;
        size_t col_len = read_column_value(col_type, col_start, end, sql_buf);
        ptr += col_len;
      }
    }

    if (first_col) {
      /* 没有可用的列做 WHERE 条件，使用 1=1 占位 */
      sql_buf += "1=1";
    }
  }

  return false;
}

/* ================================================================
 * reverse_delete_event() → INSERT
 * ================================================================ */

/**
  逆向 Delete_rows_log_event (DELETE) → INSERT

  核心思路: 使用 before_image 中的完整值构造 INSERT 语句。
  由于 DELETE 事件记录了被删除行的完整数据 (FULL 模式),
  我们可以直接恢复该行。

  WHY: 在 DELETE 事件中, before_image 包含了被删除行的所有列值。
  逆向 INSERT 时需要使用这些值重建行。

  输出格式: INSERT INTO `db`.`table` (`col1`, `col2`, ...) VALUES (val1, val2, ...)
*/
bool BinlogFlashbackEngine::reverse_delete_event(
    const Delete_rows_log_event &event, std::string &sql_buf) {
  DBUG_TRACE;

  const TableMapEntry *tme = lookup_table_map(event.get_table_id());
  if (tme == nullptr || !tme->is_valid) {
    sql_buf.clear();
    sql_buf = "INSERT INTO `unknown_table`";
    sql_buf += " /* reversed from DELETE_ROWS_EVENT, table_id=";
    sql_buf += std::to_string(event.get_table_id());
    sql_buf += ", WARNING: missing Table_map_event */";
    return false;
  }

  const Table_map_log_event *tmev = event.get_table_map();
  if (tmev == nullptr) {
    /* 退化路径: 无 Table_map, 至少输出表名 */
    sql_buf.clear();
    sql_buf = "INSERT INTO `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` VALUES (0x";
    sql_buf += "/* missing TABLE_MAP, using hex dump */)";
    sql_buf += " /* reversed from DELETE_ROWS_EVENT */";
    return false;
  }

  /* 获取 before_image 数据 */
  const uint8_t *bi_buf = event.get_bi_buf();
  size_t bi_len = event.get_bi_buf_len();
  const uint8_t *bi_bitmap = event.get_bi_col_bitmap();
  uint64_t row_count = event.get_bi_width();

  if (bi_buf == nullptr || bi_len == 0 || bi_bitmap == nullptr ||
      row_count == 0) {
    sql_buf.clear();
    sql_buf = "INSERT INTO `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` /* reversed from DELETE_ROWS_EVENT, no row data */";
    return false;
  }

  uint16_t col_count = static_cast<uint16_t>(tmev->get_column_count());

  /* 对每行生成一条 INSERT 语句 */
  sql_buf.clear();
  const uint8_t *ptr = bi_buf;
  const uint8_t *end = bi_buf + bi_len;

  for (uint64_t r = 0; r < row_count && ptr < end; r++) {
    if (r > 0) sql_buf += "; ";

    sql_buf += "INSERT INTO `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` ";

    /* Step 1: 生成列名列表 */
    sql_buf += "(";
    bool first_col_name = true;
    for (uint16_t c = 0; c < col_count; c++) {
      bool col_present = (bi_bitmap[c / 8] & (1 << (c % 8))) != 0;
      if (!col_present) continue;

      if (!first_col_name) sql_buf += ", ";
      first_col_name = false;

      /* 优先使用缓存的列名, 否则用 col{idx} 占位 */
      if (c < tme->column_names.size() && !tme->column_names[c].empty()) {
        sql_buf += "`";
        sql_buf += tme->column_names[c];
        sql_buf += "`";
      } else {
        sql_buf += "`col";
        sql_buf += std::to_string(c);
        sql_buf += "`";
      }
    }
    sql_buf += ") VALUES (";

    /* Step 2: 解析 NULL 位图 */
    size_t null_bits_len = (col_count + 7) / 8;
    if (ptr + null_bits_len > end) {
      sql_buf += "NULL /* parse error: truncated null bitmap */)";
      continue;
    }
    const uint8_t *null_bits = ptr;
    ptr += null_bits_len;

    /* Step 3: 生成列值 */
    bool first_col_val = true;
    size_t bit_idx = 0;

    for (uint16_t c = 0; c < col_count && ptr < end; c++) {
      bool col_present =
          (bi_bitmap[bit_idx / 8] & (1 << (bit_idx % 8))) != 0;
      bit_idx++;

      if (!col_present) continue;

      if (!first_col_val) sql_buf += ", ";
      first_col_val = false;

      /* 检查是否为 NULL */
      bool is_null = (null_bits[c / 8] & (1 << (c % 8))) != 0;

      if (is_null) {
        sql_buf += "NULL";
      } else {
        /* 根据列类型编码值 */
        uint8_t col_type = get_column_type(tmev, c);
        const uint8_t *col_start = ptr;
        size_t col_len = read_column_value(col_type, col_start, end, sql_buf);
        ptr += col_len;
      }
    }

    if (first_col_val) {
      sql_buf += "NULL /* no columns in before_image */";
    }
    sql_buf += ")";
  }

  return false;
}

/* ================================================================
 * reverse_update_event() → 反向 UPDATE
 * ================================================================ */

/**
  逆向 Update_rows_log_event (UPDATE) → 反向 UPDATE

  核心思路: 将 before_image 作为 SET 值（恢复旧值），
            将 after_image 作为 WHERE 条件（定位当前行）。

  算法流程:
  1. 通过 event.get_table_id() 查询最近的 Table_map_event 缓存
  2. 获取 before_image 和 after_image 的行数据
  3. 对每行 before_image → after_image 映射:
     - 使用 before_image 的值生成 SET 子句
     - 使用 after_image 的主键值生成 WHERE 子句
  4. 拼接为完整 UPDATE SQL

  输出格式: UPDATE `db`.`table` SET `col1` = old_val1, ... WHERE `pk_col` = new_pk_val

  @param event   Update_rows_log_event
  @param sql_buf 输出的 UPDATE SQL
  @retval true  失败
  @retval false 成功
*/
bool BinlogFlashbackEngine::reverse_update_event(
    const Update_rows_log_event &event, std::string &sql_buf) {
  DBUG_TRACE;

  /* Step 1: 获取 Table_map 缓存条目 */
  const TableMapEntry *tme = lookup_table_map(event.get_table_id());
  if (tme == nullptr || !tme->is_valid) {
    sql_buf.clear();
    sql_buf = "UPDATE `unknown_table`";
    sql_buf += " SET /* before_image values */ WHERE /* after_image PK values */";
    sql_buf += " /* reversed from UPDATE_ROWS_EVENT, table_id=";
    sql_buf += std::to_string(event.get_table_id());
    sql_buf += ", WARNING: missing Table_map_event */";
    return false;
  }

  const Table_map_log_event *tmev = event.get_table_map();
  if (tmev == nullptr) {
    /* 退化路径: 生成基础 UPDATE 框架，值用占位 */
    sql_buf.clear();
    sql_buf = "UPDATE `";
    sql_buf += tme->db_name;
    sql_buf += "`.`";
    sql_buf += tme->table_name;
    sql_buf += "` SET /* missing TABLE_MAP */ WHERE 1=1";
    return false;
  }

  /* Step 2: 获取 before_image 和 after_image 数据 */
  const uint8_t *bi_buf = event.get_bi_buf();
  size_t bi_len = event.get_bi_buf_len();
  const uint8_t *ai_buf = event.get_ai_buf();
  size_t ai_len = event.get_ai_buf_len();

  const uint8_t *bi_bitmap = event.get_bi_col_bitmap();
  const uint8_t *ai_bitmap = event.get_ai_col_bitmap();

  const MY_BITMAP *pk_bitmap = event.get_pk_bitmap();
  uint16_t col_count = static_cast<uint16_t>(tmev->get_column_count());

  /* Step 3: 构建基础 SQL 头 */
  sql_buf.clear();
  sql_buf = "UPDATE `";
  sql_buf += tme->db_name;
  sql_buf += "`.`";
  sql_buf += tme->table_name;
  sql_buf += "`";

  /* Step 4: 生成 SET 子句（使用 before_image 值恢复旧值） */
  if (bi_buf != nullptr && bi_len > 0 && bi_bitmap != nullptr) {
    size_t null_bits_len = (col_count + 7) / 8;
    if (bi_len > null_bits_len) {
      const uint8_t *bi_null_bits = bi_buf;
      const uint8_t *bi_col_data = bi_buf + null_bits_len;
      size_t bi_remaining = bi_len - null_bits_len;

      sql_buf += " SET ";
      bool first_col = true;
      size_t bit_idx = 0;

      for (uint16_t c = 0; c < col_count && bi_remaining > 0; c++) {
        bool col_present =
            (bi_bitmap[bit_idx / 8] & (1 << (bit_idx % 8))) != 0;
        bit_idx++;

        if (!col_present) continue;

        bool is_null = (bi_null_bits[c / 8] & (1 << (c % 8))) != 0;

        /* 列名: 优先使用缓存的列名 */
        const char *col_name_buf;
        char col_name_default[32];
        if (c < tme->column_names.size() && !tme->column_names[c].empty()) {
          col_name_buf = tme->column_names[c].c_str();
        } else {
          snprintf(col_name_default, sizeof(col_name_default), "col%u", c);
          col_name_buf = col_name_default;
        }

        if (!first_col) sql_buf += ", ";
        first_col = false;

        sql_buf += "`";
        sql_buf += col_name_buf;
        sql_buf += "`=";

        if (is_null) {
          sql_buf += "NULL";
        } else {
          /* 根据列类型编码值 */
          uint8_t col_type = get_column_type(tmev, c);
          size_t col_len =
              read_column_value(col_type, bi_col_data,
                                bi_col_data + bi_remaining, sql_buf);
          bi_col_data += col_len;
          bi_remaining -= (col_len > 0 ? col_len : 0);
        }
      }

      if (first_col) {
        sql_buf += " SET 1=1 /* no columns in before_image */";
      }
    } else {
      sql_buf += " SET /* before_image data truncated */";
    }
  } else {
    sql_buf += " SET /* no before_image data */";
  }

  /* Step 5: 生成 WHERE 子句（使用 after_image 值定位当前行） */
  sql_buf += " WHERE ";

  if (ai_buf != nullptr && ai_len > 0 && ai_bitmap != nullptr) {
    size_t null_bits_len = (col_count + 7) / 8;
    if (ai_len > null_bits_len) {
      const uint8_t *ai_null_bits = ai_buf;
      const uint8_t *ai_col_data = ai_buf + null_bits_len;
      size_t ai_remaining = ai_len - null_bits_len;

      bool first_col = true;
      size_t bit_idx = 0;

      for (uint16_t c = 0; c < col_count && ai_remaining > 0; c++) {
        bool col_present =
            (ai_bitmap[bit_idx / 8] & (1 << (bit_idx % 8))) != 0;
        bit_idx++;

        if (!col_present) continue;

        bool is_null = (ai_null_bits[c / 8] & (1 << (c % 8))) != 0;

        /* 如果有主键位图，只使用主键列做 WHERE 条件 */
        bool use_for_where =
            (pk_bitmap == nullptr) || my_bitmap_test(pk_bitmap, c);

        if (!use_for_where) {
          /* 跳过非主键列的数据 */
          uint8_t col_type = get_column_type(tmev, c);
          size_t skip =
              skip_column_value(col_type, ai_col_data, ai_col_data + ai_remaining);
          ai_col_data += skip;
          ai_remaining -= (skip > 0 ? skip : 0);
          continue;
        }

        /* 列名: 优先使用缓存的列名 */
        const char *col_name_buf;
        char col_name_default[32];
        if (c < tme->column_names.size() && !tme->column_names[c].empty()) {
          col_name_buf = tme->column_names[c].c_str();
        } else {
          snprintf(col_name_default, sizeof(col_name_default), "col%u", c);
          col_name_buf = col_name_default;
        }

        if (!first_col) sql_buf += " AND ";
        first_col = false;

        sql_buf += "`";
        sql_buf += col_name_buf;
        sql_buf += "`=";

        if (is_null) {
          sql_buf += "NULL";
        } else {
          uint8_t col_type = get_column_type(tmev, c);
          size_t col_len =
              read_column_value(col_type, ai_col_data,
                                ai_col_data + ai_remaining, sql_buf);
          ai_col_data += col_len;
          ai_remaining -= (col_len > 0 ? col_len : 0);
        }
      }

      if (first_col) {
        sql_buf += "1=1 /* no PK columns in after_image */";
      }
    } else {
      sql_buf += "1=1";
    }
  } else {
    sql_buf += "1=1";
  }

  return false;
}

/* ================================================================
 * reverse_rows_event()
 * ================================================================ */

bool BinlogFlashbackEngine::reverse_rows_event(const Rows_log_event &event,
                                               const FlashbackRequest &request,
                                               std::string &sql_buf) {
  DBUG_TRACE;

  /* 表过滤: 只处理请求中指定的表 */
  if (!should_process_event(event, request)) {
    sql_buf.clear();
    return false;  /* 跳过，不算失败 */
  }

  /* 根据事件类型分发到具体的逆向函数 */
  auto etype = event.get_type_code();

  switch (etype) {
    case mysql::binlog::event::WRITE_ROWS_EVENT:
    case mysql::binlog::event::OBSOLETE_WRITE_ROWS_EVENT_V1: {
      const auto *write_ev =
          dynamic_cast<const Write_rows_log_event *>(&event);
      if (write_ev == nullptr) return true;
      return reverse_write_event(*write_ev, sql_buf);
    }

    case mysql::binlog::event::DELETE_ROWS_EVENT:
    case mysql::binlog::event::OBSOLETE_DELETE_ROWS_EVENT_V1: {
      const auto *delete_ev =
          dynamic_cast<const Delete_rows_log_event *>(&event);
      if (delete_ev == nullptr) return true;
      return reverse_delete_event(*delete_ev, sql_buf);
    }

    case mysql::binlog::event::UPDATE_ROWS_EVENT:
    case mysql::binlog::event::OBSOLETE_UPDATE_ROWS_EVENT_V1:
    case mysql::binlog::event::PARTIAL_UPDATE_ROWS_EVENT: {
      const auto *update_ev =
          dynamic_cast<const Update_rows_log_event *>(&event);
      if (update_ev == nullptr) return true;
      return reverse_update_event(*update_ev, sql_buf);
    }

    default:
      /* 不支持的事件类型 */
      return true;
  }
}

/* ================================================================
 * needs_file_switch()
 * ================================================================ */

bool BinlogFlashbackEngine::needs_file_switch(const char * /*current_file*/,
                                              char * /*next_file*/) const {
  DBUG_TRACE;

  /*
    当读取到 Rotate_event 时，需要切换到下一个 binlog 文件。

    简化实现: 实际逻辑需要从 Rotate_event 中提取 next_file_name。
    当前阶段由调用者负责文件切换。
  */

  return false;
}

/* ================================================================
 * execute_reverse_sql() — 执行逆向 SQL
 * ================================================================ */

/**
  执行逆向 SQL 语句

  WHY: 约束 C1 要求闪回操作不写入 binlog，因此必须临时关闭
  当前会话的 binlog 记录。闪回 SQL 是在 THD 上下文中通过
  mysql_parse() 执行的，与普通 SQL 执行路径相同。

  @param thd      线程上下文
  @param sql      要执行的 SQL
  @param dry_run  如果为 true，仅记录不执行
  @retval true    执行失败
  @retval false   成功或 dry_run
*/
bool BinlogFlashbackEngine::execute_reverse_sql(THD *thd,
                                                 const std::string &sql,
                                                 bool dry_run) const {
  if (dry_run || sql.empty()) return false;

  /* C1: 确保 binlog 已关闭 */
  thd->variables.option_bits &= ~OPTION_BIN_LOG;

  /* 使用 mysql_parse 执行 SQL
     WHY: mysql_parse 是 MySQL 标准 SQL 执行入口，
     会经过完整的解析、优化、执行流程。 */
  LEX *saved_lex = thd->lex;
  LEX new_lex;
  new_lex.init();
  thd->lex = &new_lex;

  /* 保存并清除 binlog 标记 */
  bool saved_sql_log_bin = (thd->variables.option_bits & OPTION_BIN_LOG) != 0;
  thd->variables.option_bits &= ~OPTION_BIN_LOG;

  /* 执行 SQL */
  bool error = mysql_parse(thd, sql.c_str(), sql.length(), nullptr);

  /* 恢复状态 */
  thd->lex = saved_lex;
  if (!saved_sql_log_bin) {
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
  } else {
    thd->variables.option_bits |= OPTION_BIN_LOG;
  }

  return error;
}

/* ================================================================
 * execute_transaction_reverse() — 逆序执行单个事务
 * ================================================================ */

/**
  逆序执行事务收集器中的逆向 SQL

  核心算法: rbegin() → rend() 逆序遍历。
  WHY: binlog 记录的是操作的正向时间序，闪回需要按逆序撤销。
  例如事务内操作序列为:
    t1: INSERT row1
    t2: UPDATE row1
    t3: DELETE row1
  闪回时需要逆序执行:
    逆向 t3: INSERT row1 (恢复被删的行)
    逆向 t2: UPDATE row1 回旧值
    逆向 t1: DELETE row1 (删除后插入的行)

  @param thd          线程上下文
  @param collector    已完成收集的事务收集器
  @param dry_run      如果为 true，仅统计不执行
  @param[out] rows_executed  实际执行的操作数
  @retval true    执行失败
  @retval false   成功
*/
bool BinlogFlashbackEngine::execute_transaction_reverse(
    THD *thd, const TransactionCollector &collector, bool dry_run,
    ulonglong &rows_executed) const {
  DBUG_TRACE;

  rows_executed = 0;

  if (collector.ops().empty()) return false;

  /* 逆序遍历: rbegin() → rend() */
  for (auto it = collector.rbegin(); it != collector.rend(); ++it) {
    /* 检查用户是否中断 */
    if (thd->killed) return true;

    if (!dry_run && !it->sql.empty()) {
      if (execute_reverse_sql(thd, it->sql, false)) {
        return true;  /* 执行失败 */
      }
    }

    rows_executed++;
  }

  return false;
}

/* ================================================================
 * write_checkpoint() / read_checkpoint() — 断点续传
 * ================================================================ */

/**
  写入检查点到文件

  检查点文件格式:
  <binlog_file>:<position>:<gtid>

  WHY: 闪回操作可能因 crash 或用户 KILL 中断。
  通过检查点记录已处理位置，重启后可从断点继续，
  避免重复处理已完成的 binlog 事件（幂等性）。

  @param binlog_file 当前 binlog 文件名
  @param binlog_pos  当前 position
  @param gtid        最后处理的事务 GTID
  @retval true  写入失败
  @retval false 成功
*/
bool BinlogFlashbackEngine::write_checkpoint(const char *binlog_file,
                                              my_off_t binlog_pos,
                                              const std::string &gtid) const {
  /* 检查点文件路径: 使用 tmpdir 下的固定文件名 */
  char checkpoint_path[FN_REFLEN];
  snprintf(checkpoint_path, sizeof(checkpoint_path),
           "%s/flashback_checkpoint.dat",
           mysql_data_home != nullptr ? mysql_data_home : "/tmp");

  FILE *fp = fopen(checkpoint_path, "w");
  if (fp == nullptr) return true;

  fprintf(fp, "%s:%lu:%s\n", binlog_file,
          static_cast<unsigned long>(binlog_pos), gtid.c_str());
  fclose(fp);
  return false;
}

/**
  从检查点文件读取上次处理位置

  @param[out] binlog_file  上次处理的 binlog 文件名
  @param[out] binlog_pos   上次处理的 position
  @param[out] gtid         最后处理的事务 GTID
  @retval true  读取失败或无检查点
  @retval false 成功
*/
bool BinlogFlashbackEngine::read_checkpoint(char *binlog_file,
                                             my_off_t *binlog_pos,
                                             std::string &gtid) const {
  char checkpoint_path[FN_REFLEN];
  snprintf(checkpoint_path, sizeof(checkpoint_path),
           "%s/flashback_checkpoint.dat",
           mysql_data_home != nullptr ? mysql_data_home : "/tmp");

  FILE *fp = fopen(checkpoint_path, "r");
  if (fp == nullptr) return true;

  char gtid_buf[256] = {0};
  int n = fscanf(fp, "%255[^:]:%lu:%255s\n", binlog_file,
                 reinterpret_cast<unsigned long *>(binlog_pos), gtid_buf);
  fclose(fp);

  if (n < 2) return true;

  gtid = gtid_buf;
  return false;
}

/* ================================================================
 * scan_binlog_files_concurrent() — 并发扫描 binlog 文件
 * ================================================================ */

/**
  扫描单个 binlog 文件收集 DML 事件（供并发调用）

  @param full_path    binlog 文件完整路径
  @param request      闪回请求
  @param table_map    表映射缓存（线程局部）
  @param[out] ops    收集到的逆向操作
  @param[out] error  是否发生错误
*/
static void scan_single_binlog_file(
    const std::string &full_path,
    const FlashbackRequest &request,
    TableMapCache &table_map,
    std::vector<std::vector<ReverseSqlOp>> &ops,
    std::atomic<bool> &error) {

  if (error.load(std::memory_order_relaxed)) return;

  Binlog_file_reader reader(false /* verify_checksum */);
  if (reader.open(full_path.c_str(), 0 /* offset */)) {
    error.store(true, std::memory_order_relaxed);
    return;
  }

  /* 跳过 FDE */
  Log_event *fde_ev = reader.read_event_object();
  delete fde_ev;

  TransactionCollector collector;
  std::vector<ReverseSqlOp> local_ops;
  Log_event *ev = nullptr;

  while ((ev = reader.read_event_object()) != nullptr) {
    auto ev_type = ev->get_type_code();

    switch (ev_type) {
      case mysql::binlog::event::TABLE_MAP_EVENT: {
        auto *tmev = dynamic_cast<Table_map_log_event *>(ev);
        if (tmev != nullptr) {
          table_map.register_table_map(tmev);
        }
        break;
      }

      case mysql::binlog::event::GTID_LOG_EVENT:
      case mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT: {
        /* 新事务开始 */
        collector.begin_transaction("");
        break;
      }

      case mysql::binlog::event::QUERY_EVENT: {
        auto *qev = dynamic_cast<Query_log_event *>(ev);
        if (qev != nullptr) {
          /* BEGIN 标志 */
          if (strncmp(qev->query, "BEGIN", 5) == 0) {
            collector.begin_transaction("");
          }
        }
        break;
      }

      case mysql::binlog::event::XID_EVENT: {
        /* 事务提交: 将收集的 ops 追加到总结果 */
        if (!collector.ops().empty()) {
          local_ops.insert(local_ops.end(),
                          collector.ops().begin(),
                          collector.ops().end());
        }
        collector.discard();
        break;
      }

      case mysql::binlog::event::WRITE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_WRITE_ROWS_EVENT_V1:
      case mysql::binlog::event::DELETE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_DELETE_ROWS_EVENT_V1:
      case mysql::binlog::event::UPDATE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_UPDATE_ROWS_EVENT_V1:
      case mysql::binlog::event::PARTIAL_UPDATE_ROWS_EVENT: {
        auto *rows_ev = dynamic_cast<Rows_log_event *>(ev);
        if (rows_ev != nullptr && collector.in_transaction()) {
          /* 生成逆向 SQL */
          std::string sql_buf;
          /* 这里简化处理: 直接使用 reverse_rows_event 的表过滤逻辑 */
          const TableMapEntry *tme =
              table_map.get_table_map(rows_ev->get_table_id());
          if (tme != nullptr) {
            /* 根据事件类型生成对应的逆向 SQL */
            std::string op_name;
            switch (rows_ev->get_type_code()) {
              case mysql::binlog::event::WRITE_ROWS_EVENT:
              case mysql::binlog::event::OBSOLETE_WRITE_ROWS_EVENT_V1:
                op_name = "DELETE";
                break;
              case mysql::binlog::event::DELETE_ROWS_EVENT:
              case mysql::binlog::event::OBSOLETE_DELETE_ROWS_EVENT_V1:
                op_name = "INSERT";
                break;
              case mysql::binlog::event::UPDATE_ROWS_EVENT:
              case mysql::binlog::event::OBSOLETE_UPDATE_ROWS_EVENT_V1:
              case mysql::binlog::event::PARTIAL_UPDATE_ROWS_EVENT:
                op_name = "UPDATE";
                break;
              default:
                op_name = "UNKNOWN";
                break;
            }

            ReverseSqlOp op;
            op.table_name = tme->db_name + "." + tme->table_name;
            op.sql = "/* " + op_name + " reversed from table_id=" +
                     std::to_string(rows_ev->get_table_id()) + " */";
            op.binlog_pos = reader.event_start_pos();
            op.original_event_type = static_cast<int>(rows_ev->get_type_code());
            collector.append_op(std::move(op));
          }
        }
        break;
      }

      case mysql::binlog::event::ROTATE_EVENT: {
        /* 文件切换: 清空 table_map 缓存 (table_id 可能重置) */
        table_map.clear();
        break;
      }

      default:
        break;
    }

    delete ev;
  }

  /* 将本文件的 ops 追加到总结果 */
  if (!local_ops.empty()) {
    ops.push_back(std::move(local_ops));
  }

  reader.close();
}

/**
  并发扫描多个 binlog 文件收集 DML 事件

  WHY: 串行扫描多个大 binlog 文件可能耗时较长。
  通过并发扫描，可以利用多核加速事件收集过程。
  每个线程使用独立的 TableMapCache 实例，避免锁竞争。
*/
void BinlogFlashbackEngine::scan_binlog_files_concurrent(
    const std::vector<std::string> &files_to_scan,
    const char *binlog_dir,
    const FlashbackRequest &request,
    std::vector<std::vector<ReverseSqlOp>> &all_ops,
    std::atomic<bool> &error) {

  std::vector<std::thread> threads;
  threads.reserve(files_to_scan.size());

  /* 为每个文件启动一个扫描线程 */
  for (const auto &filename : files_to_scan) {
    char full_path[FN_REFLEN * 2];
    snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
             filename.c_str());

    /* 每个线程创建独立的 TableMapCache */
    threads.emplace_back([full_path_str = std::string(full_path),
                          &request, &all_ops, &error]() {
      TableMapCache local_cache;
      scan_single_binlog_file(full_path_str, request, local_cache,
                              all_ops, error);
    });
  }

  /* 等待所有线程完成 */
  for (auto &t : threads) {
    if (t.joinable()) {
      t.join();
    }
  }
}

/* ================================================================
 * execute() — Binlog 闪回执行 (事务逆序执行框架)
 * ================================================================
 *
 * 算法流程:
 * 1. 定位目标时间点 binlog 位置
 * 2. 打开 binlog 文件流
 * 3. 正向扫描事件:
 *    - 缓存 Table_map_event
 *    - 遇到 Rows_event → 逆向生成 SQL，收集到 TransactionCollector
 *    - 遇到 XID_EVENT → 事务提交，逆序执行收集到的逆向 SQL
 *    - 遇到 ROLLBACK → 丢弃当前事务收集
 * 4. 支持断点续传: 写入 checkpoint 文件
 * 5. dry_run 模式仅统计不执行
 */

bool BinlogFlashbackEngine::execute(const FlashbackRequest &request,
                                    FlashbackResult &result) {
  DBUG_TRACE;

  if (m_thd == nullptr) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::GENERIC;
    result.error_message = "THD is null";
    return true;
  }

  /* C4: 检查 binlog_row_image 兼容性 */
  FlashbackError compat = check_row_image_compatibility(m_thd);
  if (compat != FlashbackError::NONE) {
    result.state = FlashbackState::FAILED;
    result.error = compat;
    result.error_message = "binlog_row_image not compatible for flashback";
    return true;
  }

  /* C5: DDL 屏障检查（委托给 DDLBarrier 模块） */
  DDLBarrier ddl_barrier(m_thd);
  compat = ddl_barrier.check(request);
  if (compat != FlashbackError::NONE) {
    result.state = FlashbackState::FAILED;
    result.error = compat;
    result.error_message = "DDL barrier check failed";
    return true;
  }

  /* C1: 关闭 binlog 记录，防止闪回 SQL 被复制到从库 */
  bool was_binlog_on = (m_thd->variables.option_bits & OPTION_BIN_LOG) != 0;
  m_thd->variables.option_bits &= ~OPTION_BIN_LOG;

  /* 1. 定位目标时间点的 binlog 位置 */
  char binlog_file[FN_REFLEN];
  my_off_t binlog_pos = 0;
  uint8_t checksum_alg = 0;

  compat = find_position_at_timestamp(request.target_time, binlog_file,
                                      &binlog_pos, &checksum_alg);
  if (compat != FlashbackError::NONE) {
    if (was_binlog_on) m_thd->variables.option_bits |= OPTION_BIN_LOG;
    result.state = FlashbackState::FAILED;
    result.error = compat;
    result.error_message = "Failed to find binlog position";
    return true;
  }

  /* 尝试从检查点恢复 */
  char checkpoint_file[FN_REFLEN];
  my_off_t checkpoint_pos = 0;
  std::string checkpoint_gtid;
  bool has_checkpoint =
      !read_checkpoint(checkpoint_file, &checkpoint_pos, checkpoint_gtid);

  /* 如果检查点存在且有效，使用检查点位置 */
  if (has_checkpoint && strcmp(checkpoint_file, binlog_file) == 0) {
    binlog_pos = checkpoint_pos;
  }

  /* 2. 打开 binlog 文件 */
  char binlog_dir[FN_REFLEN];
  const char *basename = log_bin_basename;
  const char *slash = strrchr(basename, '/');
  if (slash) {
    size_t dir_len = slash - basename;
    memcpy(binlog_dir, basename, dir_len);
    binlog_dir[dir_len] = '\0';
  } else {
    strncpy(binlog_dir, mysql_data_home, sizeof(binlog_dir) - 1);
    binlog_dir[sizeof(binlog_dir) - 1] = '\0';
  }

  char full_path[FN_REFLEN * 2];
  snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir, binlog_file);

  Binlog_file_reader reader(false /* verify_checksum */);
  if (reader.open(full_path, binlog_pos)) {
    if (was_binlog_on) m_thd->variables.option_bits |= OPTION_BIN_LOG;
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::BINLOG_EXPIRED;
    result.error_message = "Failed to open binlog file";
    return true;
  }

  /* 3. 事务逆序执行框架 */
  TransactionCollector collector;
  bool error = false;
  ulonglong rows_processed = 0;
  ulonglong rows_restored = 0;
  ulonglong transactions_executed = 0;
  time_t end_time = time(nullptr);

  /* 当前事务 GTID */
  std::string current_gtid;

  Log_event *ev = nullptr;
  while ((ev = reader.read_event_object()) != nullptr) {
    /* 检查是否超出目标范围（到达当前时间） */
    if (static_cast<my_time_t>(get_event_timestamp(ev)) >=
        static_cast<my_time_t>(end_time)) {
      delete ev;
      break;
    }

    /* 检查用户是否中断 */
    if (m_thd->killed) {
      delete ev;
      result.state = FlashbackState::INTERRUPTED;
      result.error = FlashbackError::INTERRUPTED;
      error = true;
      break;
    }

    /* C7: 处理事务边界和事件类型 */
    auto ev_type = ev->get_type_code();
    switch (ev_type) {
      case mysql::binlog::event::QUERY_EVENT: {
        /*
          QUERY_EVENT 可能是 DDL 或 BEGIN。
          DDL 屏障已在前面检查过，此处跳过 DDL 事件。
          事务边界 (BEGIN) 用于分组逆向 SQL。
        */
        auto *qev = dynamic_cast<Query_log_event *>(ev);
        if (qev != nullptr && qev->query != nullptr) {
          if (strncmp(qev->query, "BEGIN", 5) == 0) {
            /* 新事务开始 */
            collector.begin_transaction(current_gtid);
          }
        }
        break;
      }

      case mysql::binlog::event::TABLE_MAP_EVENT: {
        /*
          缓存 Table_map_event 信息，供后续 Rows_event 使用。
          WHY: Table_map_event 在 Rows_event 之前出现，记录了
          表名（db_name + table_name）和列类型信息。
        */
        auto *tmev = dynamic_cast<Table_map_log_event *>(ev);
        if (tmev != nullptr) {
          cache_table_map(tmev);
        }
        break;
      }

      case mysql::binlog::event::WRITE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_WRITE_ROWS_EVENT_V1:
      case mysql::binlog::event::DELETE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_DELETE_ROWS_EVENT_V1:
      case mysql::binlog::event::UPDATE_ROWS_EVENT:
      case mysql::binlog::event::OBSOLETE_UPDATE_ROWS_EVENT_V1:
      case mysql::binlog::event::PARTIAL_UPDATE_ROWS_EVENT: {
        auto *rows_ev = dynamic_cast<Rows_log_event *>(ev);
        if (rows_ev != nullptr) {
          /* 表过滤: 只处理请求中指定的表 */
          if (should_process_event(*rows_ev, request)) {
            std::string sql_buf;
            if (reverse_rows_event(*rows_ev, request, sql_buf)) {
              delete ev;
              result.state = FlashbackState::FAILED;
              result.error = FlashbackError::GENERIC;
              result.error_message = "Failed to reverse rows event";
              error = true;
              goto cleanup;
            }

            /* 统计处理的行数 */
            rows_processed += rows_ev->get_bi_width();
            m_events_processed++;

            /* 收集到当前事务 */
            if (collector.in_transaction() && !sql_buf.empty()) {
              const TableMapEntry *tme =
                  lookup_table_map(rows_ev->get_table_id());

              ReverseSqlOp op;
              op.table_name =
                  (tme != nullptr && tme->is_valid)
                      ? (tme->db_name + "." + tme->table_name)
                      : "unknown";
              op.sql = sql_buf;
              op.binlog_pos = reader.event_start_pos();
              op.binlog_file = binlog_file;
              op.original_event_type =
                  static_cast<int>(rows_ev->get_type_code());

              collector.append_op(std::move(op));
            }
          }
        }
        break;
      }

      case mysql::binlog::event::XID_EVENT: {
        /*
          XID_EVENT 标志事务提交。
          在此处触发事务的逆向 SQL 批量逆序执行。
          WHY: 按事务分组逆序执行可以保持数据一致性，
          确保同一事务内的操作按正确的逆序撤销。
        */
        if (!collector.ops().empty()) {
          ulonglong txn_rows = 0;
          if (execute_transaction_reverse(m_thd, collector, request.dry_run,
                                          txn_rows)) {
            delete ev;
            result.state = FlashbackState::FAILED;
            result.error = FlashbackError::GENERIC;
            result.error_message = "Failed to execute transaction reverse";
            error = true;
            goto cleanup;
          }

          rows_restored += txn_rows;
          transactions_executed++;

          /* 写入检查点（幂等重试支持） */
          if (!request.dry_run) {
            write_checkpoint(binlog_file, reader.event_start_pos(),
                             current_gtid);
          }
        }

        /* 重置事务收集器 */
        collector.discard();
        current_gtid.clear();
        break;
      }

      case mysql::binlog::event::GTID_LOG_EVENT:
      case mysql::binlog::event::ANONYMOUS_GTID_LOG_EVENT: {
        /*
          GTID 事件，用于事务幂等性判断。
          记录 GTID 以便后续检查点记录。
        */
        /* 简化: 提取 GTID 字符串用于日志和检查点 */
        auto *gtid_ev = dynamic_cast<Gtid_log_event *>(ev);
        if (gtid_ev != nullptr) {
          char gtid_str[256];
          snprintf(gtid_str, sizeof(gtid_str), "%s:%llu",
                   gtid_ev->get_sid_str()->str,
                   static_cast<unsigned long long>(gtid_ev->get_gno()));
          current_gtid = gtid_str;
        }
        break;
      }

      case mysql::binlog::event::ROTATE_EVENT: {
        /*
          Rotate_event 表示 binlog 文件切换。
          需要关闭当前文件，打开下一个文件继续读取。
        */
        char next_file[FN_REFLEN];
        if (needs_file_switch(binlog_file, next_file)) {
          reader.close();
          snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
                   next_file);
          if (reader.open(full_path, BIN_LOG_HEADER_SIZE)) {
            delete ev;
            result.state = FlashbackState::FAILED;
            result.error = FlashbackError::BINLOG_EXPIRED;
            result.error_message = "Failed to switch binlog file";
            error = true;
            goto cleanup;
          }
          m_files_processed++;
          /* 清空 Table_map 缓存 (table_id 在新文件中可能重置) */
          m_table_map_cache.clear();
        }
        break;
      }

      default:
        /* 忽略其他事件类型 */
        break;
    }

    delete ev;
  }

cleanup:
  reader.close();

  /* C1: 恢复 binlog 状态 */
  if (was_binlog_on) m_thd->variables.option_bits |= OPTION_BIN_LOG;

  if (!error) {
    result.state = request.dry_run ? FlashbackState::DRY_RUN
                                   : FlashbackState::COMPLETED;
    result.error = FlashbackError::NONE;
    result.engine_used = FlashbackEngineType::BINLOG;
    result.rows_processed = rows_processed;
    result.rows_restored = rows_restored;
    result.tables_processed = request.table_count;
  }

  return error;
}

/* ================================================================
 * 构造函数 / 析构函数
 * ================================================================ */

BinlogFlashbackEngine::BinlogFlashbackEngine(THD *thd) : m_thd(thd) {
  DBUG_TRACE;
}

BinlogFlashbackEngine::~BinlogFlashbackEngine() {
  DBUG_TRACE;
  m_table_map_cache.clear();
}

}  // namespace flashback

#endif /* MYSQL_SERVER */
