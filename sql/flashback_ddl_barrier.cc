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
  @file sql/flashback_ddl_barrier.cc
  Flashback DDL 屏障实现

  实现 DDLBarrier 类的核心方法:
  - check(): 并发扫描 binlog 检查闪回窗口内的 DDL 兼容性
  - acquire_exclusive_lock(): 获取 MDL_EXCLUSIVE 锁
  - release_exclusive_lock(): 释放排他锁

  设计参考: mysql_flashback_implementation.md §2.2.6
            mysql_flashback_implementation_deep.md §5
*/

#include "sql/flashback_ddl_barrier.h"

#include <atomic>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <future>
#include <new>
#include <vector>

#include "lex_string.h"
#include "mdl.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql/psi/mysql_file.h"
#include "mysql_com.h"
#include "sql/binlog.h"            // log_bin_basename
#include "sql/binlog_reader.h"     // Binlog_file_reader
#include "sql/log_event.h"         // Log_event, Query_log_event
#include "sql/sql_class.h"         // THD

namespace flashback {

/* ================================================================
 * 内部辅助函数
 * ================================================================ */

/**
  判断字符是否为 SQL 标识符的合法字符（字母、数字、下划线、反引号）

  @param c 字符
  @retval true  是标识符字符
  @retval false 非标识符字符（空格、标点等，即词边界）
*/
static bool is_identifier_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '`';
}

/**
  在文本中查找关键字，要求精确单词匹配（前后必须为词边界）

  与 strstr 的区别:
  - strstr("ADD COLUMN truncate_col INT", "TRUNCATE") 会误匹配
  - find_keyword("ADD COLUMN truncate_col INT", "TRUNCATE") 返回 nullptr

  @param text   待搜索文本（大写）
  @param keyword 要查找的关键字（大写）
  @return 关键字起始位置，未找到返回 nullptr
*/
static const char *find_keyword(const char *text, const char *keyword) {
  if (text == nullptr || keyword == nullptr) return nullptr;

  size_t kw_len = strlen(keyword);
  const char *pos = text;

  while ((pos = strstr(pos, keyword)) != nullptr) {
    /* 检查前边界: 必须是文本开头或非标识符字符 */
    bool before_ok = (pos == text) || !is_identifier_char(pos[-1]);
    /* 检查后边界: 必须是文本结尾或非标识符字符 */
    bool after_ok = (pos[kw_len] == '\0') || !is_identifier_char(pos[kw_len]);

    if (before_ok && after_ok) return pos;

    /* 不匹配词边界，继续往后找 */
    pos += kw_len;
  }
  return nullptr;
}

/**
  将字符串转为大写（原地修改）

  @param str 要转换的字符串（可为 nullptr）
  @param buf 输出缓冲区
  @param buflen 缓冲区大小
*/
static void to_upper_buffer(const char *str, char *buf, size_t buflen) {
  if (str == nullptr || buflen == 0) {
    if (buflen > 0) buf[0] = '\0';
    return;
  }
  size_t len = strlen(str);
  if (len >= buflen) len = buflen - 1;
  for (size_t i = 0; i < len; i++) {
    buf[i] = static_cast<char>(toupper(static_cast<unsigned char>(str[i])));
  }
  buf[len] = '\0';
}

/**
  从 binlog 文件名提取序号

  binlog 文件名格式: <basename>.NNNNNN
*/
static uint64_t extract_binlog_sequence(const char *filename) {
  const char *dot = strrchr(filename, '.');
  if (dot == nullptr) return 0;
  return strtoull(dot + 1, nullptr, 10);
}

/**
  获取 binlog 索引文件路径
*/
static bool get_binlog_index_path(char *buf, size_t buflen) {
  if (log_bin_basename == nullptr || log_bin_basename[0] == '\0') return true;

  size_t base_len = strlen(log_bin_basename);
  const char *suffix = ".index";
  size_t suffix_len = strlen(suffix);

  if (base_len + suffix_len >= buflen) return true;

  memcpy(buf, log_bin_basename, base_len);
  memcpy(buf + base_len, suffix, suffix_len + 1);
  return false;
}

/**
  读取 binlog 索引文件，返回排序后的文件列表

  @param[out] files  输出文件名列表（按序号升序）
  @retval true  读取失败
  @retval false 成功
*/
static bool read_sorted_binlog_files(std::vector<std::string> &files) {
  char index_path[FN_REFLEN];
  if (get_binlog_index_path(index_path, sizeof(index_path))) return true;

  File fd = mysql_file_open(key_file_binlog_index, index_path, O_RDONLY,
                            MYF(0));
  if (fd < 0) return true;

  std::vector<std::string> raw_files;
  char line[FN_REFLEN];
  while (true) {
    ssize_t n = my_read(fd, reinterpret_cast<unsigned char *>(line),
                        sizeof(line) - 1, MYF(0));
    if (n <= 0) break;

    line[n] = '\0';
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    if (line[0] == '\0') continue;

    const char *base = strrchr(line, '/');
    if (base)
      raw_files.emplace_back(base + 1);
    else
      raw_files.emplace_back(line);
  }

  mysql_file_close(fd, MYF(0));

  /* 按序号排序 */
  std::sort(raw_files.begin(), raw_files.end(),
            [](const std::string &a, const std::string &b) {
              return extract_binlog_sequence(a.c_str()) <
                     extract_binlog_sequence(b.c_str());
            });

  files = std::move(raw_files);
  return false;
}

/* ================================================================
 * 构造函数 / 析构函数
 * ================================================================ */

DDLBarrier::DDLBarrier(THD *thd) : m_thd(thd) {
  DBUG_TRACE;
  assert(thd != nullptr);
}

DDLBarrier::~DDLBarrier() {
  if (m_locked) {
    release_exclusive_lock();
  }
  if (m_tickets != nullptr) {
    delete[] m_tickets;
  }
}

/* ================================================================
 * classify_is_compatible() — 静态快速检查 (线程安全)
 * ================================================================ */

/**
  静态方法: 快速判断单个 DDL SQL 是否与闪回兼容

  WHY: 在 scan_binlog_for_ddl() 并发扫描函数中，多个线程同时调用
  DDL 检查。使用静态方法避免创建 DDLBarrier 实例的开销，同时保证
  线程安全（无状态函数）。

  @param ddl_sql DDL 语句文本
  @retval true  兼容
  @retval false 不兼容
*/
bool DDLBarrier::classify_is_compatible(const char *ddl_sql) {
  if (ddl_sql == nullptr || ddl_sql[0] == '\0') {
    return true;
  }

  DdlType type = classify_ddl(ddl_sql);

  switch (type) {
    case DdlType::DROP_TABLE:
    case DdlType::TRUNCATE:
    case DdlType::DROP_COLUMN:
    case DdlType::CHANGE_COLUMN:
    case DdlType::MODIFY_COLUMN:
    case DdlType::RENAME:
      return false;

    case DdlType::ADD_INDEX:
    case DdlType::DROP_INDEX:
    case DdlType::OTHER:
    case DdlType::UNKNOWN:
      return true;
  }

  return true;
}

/* ================================================================
 * classify_ddl() — 精确分类 DDL 类型
 * ================================================================ */

/**
  精确分类 DDL 语句类型

  设计思路:
  1. 将 SQL 转为大写，消除大小写差异
  2. 按优先级检查关键字（从不兼容到兼容）
  3. 使用词边界匹配，避免 "truncate_col" 被误判为 TRUNCATE

  分类优先级 (从高到低):
  1. DROP TABLE    — 最危险，优先匹配
  2. TRUNCATE      — 同上
  3. RENAME TABLE  — 表名变化
  4. DROP COLUMN   — 数据结构变更
  5. CHANGE COLUMN — 列改名/改类型
  6. MODIFY COLUMN — 列类型变更
  7. DROP INDEX    — 索引操作
  8. ADD INDEX     — 索引操作 (含 ADD UNIQUE/PRIMARY KEY)
  9. RENAME INDEX  — 仅元数据
  10. RENAME TO    — ALTER TABLE ... RENAME TO
  11. OTHER        — 其他 DDL (ADD COLUMN 等)
  12. UNKNOWN      — 非 DDL 或无法识别

  WHY: 使用词边界匹配替代 strstr，避免列名中包含关键字时的误判。
  例如 "ALTER TABLE t1 ADD COLUMN truncate_col INT" 不应被归类为 TRUNCATE。

  @param ddl_sql DDL 语句文本

  @return DDL 类型枚举值
*/
DdlType DDLBarrier::classify_ddl(const char *ddl_sql) {
  if (ddl_sql == nullptr || ddl_sql[0] == '\0') {
    return DdlType::UNKNOWN;
  }

  /* 转为大写用于匹配 */
  char upper_sql[1024];
  to_upper_buffer(ddl_sql, upper_sql, sizeof(upper_sql));

  /* 1. DROP TABLE (含 DROP TABLE IF EXISTS) */
  if (find_keyword(upper_sql, "DROP TABLE") != nullptr) {
    return DdlType::DROP_TABLE;
  }

  /* 2. TRUNCATE (含 TRUNCATE TABLE) */
  if (find_keyword(upper_sql, "TRUNCATE") != nullptr) {
    return DdlType::TRUNCATE;
  }

  /* 3. RENAME TABLE (单独语句) */
  if (find_keyword(upper_sql, "RENAME TABLE") != nullptr) {
    return DdlType::RENAME;
  }

  /* 4. ALTER TABLE 子操作 */
  if (find_keyword(upper_sql, "ALTER TABLE") != nullptr) {
    /* 4a. DROP COLUMN */
    if (find_keyword(upper_sql, "DROP COLUMN") != nullptr) {
      return DdlType::DROP_COLUMN;
    }

    /* 4b. CHANGE COLUMN */
    if (find_keyword(upper_sql, "CHANGE COLUMN") != nullptr) {
      return DdlType::CHANGE_COLUMN;
    }

    /* 4c. MODIFY COLUMN */
    if (find_keyword(upper_sql, "MODIFY COLUMN") != nullptr) {
      return DdlType::MODIFY_COLUMN;
    }

    /* 4d. DROP INDEX (必须在 ADD INDEX 之前检查) */
    if (find_keyword(upper_sql, "DROP INDEX") != nullptr) {
      return DdlType::DROP_INDEX;
    }

    /* 4e. ADD INDEX / ADD UNIQUE / ADD UNIQUE INDEX / ADD PRIMARY KEY */
    if (find_keyword(upper_sql, "ADD INDEX") != nullptr ||
        find_keyword(upper_sql, "ADD UNIQUE INDEX") != nullptr ||
        find_keyword(upper_sql, "ADD UNIQUE") != nullptr ||
        find_keyword(upper_sql, "ADD PRIMARY KEY") != nullptr ||
        find_keyword(upper_sql, "ADD FULLTEXT") != nullptr ||
        find_keyword(upper_sql, "ADD SPATIAL") != nullptr) {
      return DdlType::ADD_INDEX;
    }

    /* 4f. RENAME INDEX */
    if (find_keyword(upper_sql, "RENAME INDEX") != nullptr) {
      return DdlType::RENAME;
    }

    /* 4g. RENAME TO (表重命名) */
    if (find_keyword(upper_sql, "RENAME TO") != nullptr ||
        find_keyword(upper_sql, "RENAME AS") != nullptr) {
      return DdlType::RENAME;
    }

    /* 4h. 其他 ALTER TABLE 操作 (ADD COLUMN, CHANGE 不带 COLUMN 等) */
    return DdlType::OTHER;
  }

  /* 5. CREATE TABLE / CREATE INDEX 等 (非破坏性) */
  if (find_keyword(upper_sql, "CREATE") != nullptr) {
    return DdlType::OTHER;
  }

  /* 6. 无法识别 */
  return DdlType::UNKNOWN;
}

/* ================================================================
 * is_ddl_compatible() — DDL 兼容性判断
 * ================================================================ */

/**
  判断 DDL SQL 语句是否兼容闪回

  不兼容的 DDL 类型:
  - DROP TABLE / TRUNCATE TABLE: 数据结构不存在
  - DROP COLUMN: 历史数据中有该列，无法适配新结构
  - CHANGE COLUMN / MODIFY COLUMN (类型变更): 类型不兼容
  - RENAME TABLE: 表名变化导致闪回目标不存在

  兼容的 DDL 类型:
  - ADD INDEX / DROP INDEX: 仅索引变更
  - ADD COLUMN (带默认值或可空): 旧数据填充 NULL/默认值
  - RENAME INDEX: 仅元数据变更

  WHY: 使用 classify_ddl() 进行精确分类，替代之前的 strstr 关键字匹配。
  这避免了列名中包含关键字时的误判 (如 "truncate_col" 被误认为 TRUNCATE)。

  @param ddl_sql DDL 语句文本

  @retval true  兼容，闪回可以继续
  @retval false 不兼容，闪回必须中止
*/
bool DDLBarrier::is_ddl_compatible(const char *ddl_sql) const {
  DBUG_TRACE;

  if (ddl_sql == nullptr || ddl_sql[0] == '\0') {
    return true;
  }

  DdlType type = classify_ddl(ddl_sql);

  /* 不兼容类型列表 */
  switch (type) {
    case DdlType::DROP_TABLE:
    case DdlType::TRUNCATE:
    case DdlType::DROP_COLUMN:
    case DdlType::CHANGE_COLUMN:
    case DdlType::MODIFY_COLUMN:
    case DdlType::RENAME:
      return false;

    /* 兼容类型 */
    case DdlType::ADD_INDEX:
    case DdlType::DROP_INDEX:
    case DdlType::OTHER:
      return true;

    /* 未知类型默认放行（避免过于保守导致误报） */
    case DdlType::UNKNOWN:
      return true;
  }

  /* 防御性代码，不应到达此处 */
  return true;
}

/* ================================================================
 * scan_binlog_for_ddl() — 扫描单个 binlog 文件的 DDL
 * ================================================================ */

/**
  扫描单个 binlog 文件，检查其中是否存在不兼容 DDL

  此函数被多个线程并发调用，每个线程负责一个 binlog 文件。

  @param full_path  binlog 文件完整路径
  @param target_ts  闪回目标时间戳
  @param incompatible 输出标志: 是否发现不兼容 DDL
  @param error_occurred 输出标志: 是否发生 I/O 错误
*/
static void scan_binlog_for_ddl(const std::string &full_path,
                                my_time_t target_ts,
                                std::atomic<bool> &incompatible,
                                std::atomic<bool> &error_occurred) {
  /* 如果其他线程已经发现不兼容，提前退出 */
  if (incompatible.load(std::memory_order_relaxed)) return;

  Binlog_file_reader reader(false /* verify_checksum */);
  if (reader.open(full_path.c_str(), 0 /* offset */)) {
    error_occurred.store(true, std::memory_order_relaxed);
    return;
  }

  /* 跳过 FDE */
  Log_event *fde_ev = reader.read_event_object();
  delete fde_ev;

  Log_event *ev = nullptr;
  while ((ev = reader.read_event_object()) != nullptr) {
    my_time_t event_ts = 0;
    if (ev->common_header != nullptr) {
      event_ts = static_cast<my_time_t>(ev->common_header->when.tv_sec);
    }

    /* 跳过目标时间之前的事件 */
    if (event_ts < target_ts) {
      delete ev;
      continue;
    }

    /* 进入闪回窗口，检查是否为 DDL 事件 (QUERY_EVENT) */
    if (ev->get_type_code() == mysql::binlog::event::QUERY_EVENT) {
      auto *qev = dynamic_cast<Query_log_event *>(ev);
      if (qev != nullptr && qev->query != nullptr) {
        /* 使用 classify_ddl() 精确判断，避免 strstr 误判 */
        if (!DDLBarrier::classify_is_compatible(qev->query)) {
          incompatible.store(true, std::memory_order_relaxed);
        }
      }
    }

    delete ev;

    /* 如果发现不兼容，提前退出 */
    if (incompatible.load(std::memory_order_relaxed)) break;
  }

  reader.close();
}

/* ================================================================
 * check() — 并发 DDL 屏障检查
 * ================================================================ */

/**
  检查闪回窗口内是否存在不兼容 DDL

  实现策略 (并发优化):
  1. 读取 binlog 索引文件，获取所有文件列表
  2. 定位目标时间对应的 binlog 文件范围
  3. 使用 std::async 并发扫描每个 binlog 文件
  4. 使用 atomic<bool> 实现线程安全的短路退出
  5. 任何文件发现不兼容 DDL 时立即返回 DDL_INCOMPATIBLE

  WHY: 串行扫描多个大 binlog 文件可能耗时较长。
  通过并发扫描，可以利用多核加速检查过程。
  由于 DDL 检查是只读的，不存在并发写冲突问题。
*/
FlashbackError DDLBarrier::check(const FlashbackRequest &request) {
  DBUG_TRACE;

  /* 参数校验 */
  if (request.tables == nullptr || request.table_count == 0) {
    return FlashbackError::GENERIC;
  }

  /* 如果未启用 binlog，跳过检查 */
  if (log_bin_basename == nullptr || log_bin_basename[0] == '\0') {
    return FlashbackError::NONE;
  }

  /* 1. 获取排序后的 binlog 文件列表 */
  std::vector<std::string> binlog_files;
  if (read_sorted_binlog_files(binlog_files)) {
    return FlashbackError::BINLOG_EXPIRED;
  }

  if (binlog_files.empty()) return FlashbackError::NONE;

  /* 2. 构造 binlog 目录路径 */
  char binlog_dir[FN_REFLEN];
  const char *slash = strrchr(log_bin_basename, '/');
  if (slash) {
    size_t dir_len = static_cast<size_t>(slash - log_bin_basename);
    if (dir_len >= sizeof(binlog_dir)) dir_len = sizeof(binlog_dir) - 1;
    memcpy(binlog_dir, log_bin_basename, dir_len);
    binlog_dir[dir_len] = '\0';
  } else {
    strncpy(binlog_dir, mysql_data_home, sizeof(binlog_dir) - 1);
    binlog_dir[sizeof(binlog_dir) - 1] = '\0';
  }

  /* 3. 定位目标时间范围对应的文件 */
  my_time_t target_ts = request.target_time;

  std::vector<std::string> files_to_scan;

  /* WHY: 事务闪回 (TRANSACTION 类型) 可能没有 target_time,
     但 DDL 屏障检查仍然需要扫描 binlog。
     对于事务闪回, 我们扫描最近的 binlog 文件以捕获潜在的不兼容 DDL。 */
  if (target_ts == 0 && request.type == FlashbackType::TRANSACTION) {
    /* 事务闪回: 扫描当前所有 binlog 文件 (不基于时间窗口过滤) */
    files_to_scan = binlog_files;
  } else if (target_ts == 0) {
    return FlashbackError::NONE;
  } else {
    /* 找出目标时间范围内的 binlog 文件 */
    for (const auto &filename : binlog_files) {
      char full_path[FN_REFLEN * 2];
      snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
               filename.c_str());

      Binlog_file_reader reader(false /* verify_checksum */);
      if (reader.open(full_path, 0 /* offset */)) continue;

      /* 读取 FDE */
      Log_event *fde_ev = reader.read_event_object();
      my_time_t first_ts = 0;
      if (fde_ev != nullptr && fde_ev->common_header != nullptr) {
        first_ts = static_cast<my_time_t>(fde_ev->common_header->when.tv_sec);
      }
      delete fde_ev;

      /* 读取最后一个事件的时间戳 */
      Log_event *last_ev = nullptr;
      Log_event *ev = nullptr;
      while ((ev = reader.read_event_object()) != nullptr) {
        delete last_ev;
        last_ev = ev;
      }
      delete last_ev;

      my_time_t last_ts = 0;
      if (reader.last_event_ptr() != nullptr) {
        last_ts = static_cast<my_time_t>(
            reader.last_event_ptr()->common_header->when.tv_sec);
      }

      reader.close();

      /* 如果文件时间范围与闪回窗口有重叠，加入扫描列表 */
      if (first_ts <= target_ts || last_ts >= target_ts) {
        files_to_scan.push_back(filename);
      }
    }  /* end for */
  }  /* end else */

  if (files_to_scan.empty()) {
    return FlashbackError::NONE;
  }

  /* 4. 并发扫描每个 binlog 文件 */
  std::atomic<bool> incompatible{false};
  std::atomic<bool> error_occurred{false};

  std::vector<std::future<void>> futures;
  futures.reserve(files_to_scan.size());

  for (const auto &filename : files_to_scan) {
    char full_path[FN_REFLEN * 2];
    snprintf(full_path, sizeof(full_path), "%s/%s", binlog_dir,
             filename.c_str());

    futures.emplace_back(std::async(std::launch::async,
                                     scan_binlog_for_ddl,
                                     std::string(full_path),
                                     target_ts,
                                     std::ref(incompatible),
                                     std::ref(error_occurred)));
  }

  /* 等待所有扫描完成 */
  for (auto &f : futures) {
    if (f.valid()) {
      try {
        f.get();
      } catch (...) {
        error_occurred.store(true, std::memory_order_relaxed);
      }
    }
  }

  /* 5. 检查扫描结果 */
  if (incompatible.load(std::memory_order_relaxed)) {
    return FlashbackError::DDL_INCOMPATIBLE;
  }

  if (error_occurred.load(std::memory_order_relaxed)) {
    return FlashbackError::BINLOG_EXPIRED;
  }

  return FlashbackError::NONE;
}

/* ================================================================
 * acquire_exclusive_lock() — 获取 MDL_EXCLUSIVE 排他锁
 * ================================================================ */

bool DDLBarrier::acquire_exclusive_lock(const FlashbackRequest &request) {
  DBUG_TRACE;

  /* 参数校验 */
  if (request.tables == nullptr || request.table_count == 0) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback DDL barrier: no tables specified");
    return true;
  }

  /* 防止重复加锁 */
  if (m_locked) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback DDL barrier: lock already acquired");
    return true;
  }

  /* WHY: 使用 MDL_EXPLICIT 持续时间，这样锁不会在语句结束时自动释放，
     而是由我们手动控制释放时机。这对于闪回操作的长时间运行是必要的。 */

  /* 分配 ticket 数组 */
  m_tickets = new (std::nothrow) MDL_ticket *[request.table_count];
  if (m_tickets == nullptr) {
    my_error(ER_OUTOFMEMORY, MYF(0),
             sizeof(MDL_ticket *) * request.table_count);
    return true;
  }

  /* 对每个表获取 MDL_EXCLUSIVE 锁 */
  for (uint32_t i = 0; i < request.table_count; ++i) {
    const LEX_CSTRING &table_ref = request.tables[i];

    /* 解析表名: 可能是 "db.table" 或仅 "table" */
    const char *db_name = "";
    const char *table_name = nullptr;

    if (table_ref.length == 0) {
      continue;
    }

    /* WHY: MDL_key::TABLE 需要独立的 db 和 table name 参数。
       表名引用可能是 "db.table" 格式，需要分离。 */
    char db_buf[NAME_LEN + 1] = {0};
    const char *dot = static_cast<const char *>(
        memchr(table_ref.str, '.', table_ref.length));
    if (dot != nullptr) {
      /* "db.table" 格式 */
      size_t db_len = static_cast<size_t>(dot - table_ref.str);
      if (db_len > NAME_LEN) db_len = NAME_LEN;
      memcpy(db_buf, table_ref.str, db_len);
      db_buf[db_len] = '\0';
      db_name = db_buf;
      table_name = dot + 1;
    } else {
      /* 仅 "table" 格式，db 留空（使用当前默认 db） */
      table_name = table_ref.str;
    }

    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, db_name, table_name,
                     MDL_EXCLUSIVE, MDL_EXPLICIT);

    /* WHY: 使用 thd->variables.lock_wait_timeout 作为超时时间，
       与 MySQL 标准锁等待行为保持一致。 */
    if (m_thd->mdl_context.acquire_lock(
            &mdl_request, m_thd->variables.lock_wait_timeout)) {
      /* 加锁失败: 可能是超时、被杀或内存不足 */
      release_exclusive_lock();  /* 清理已获取的部分锁 */
      return true;
    }

    m_tickets[m_ticket_count++] = mdl_request.ticket;
  }

  m_locked = true;
  return false;
}

/* ================================================================
 * release_exclusive_lock() — 释放排他锁
 * ================================================================ */

void DDLBarrier::release_exclusive_lock() {
  DBUG_TRACE;

  if (!m_locked) {
    return;
  }

  /* 逐个释放 ticket */
  for (uint32_t i = 0; i < m_ticket_count; ++i) {
    if (m_tickets[i] != nullptr) {
      m_thd->mdl_context.release_lock(m_tickets[i]);
      m_tickets[i] = nullptr;
    }
  }

  m_ticket_count = 0;
  m_locked = false;
}

}  // namespace flashback
