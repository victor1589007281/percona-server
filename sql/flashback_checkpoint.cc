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
  @file sql/flashback_checkpoint.cc
  Flashback 检查点实现 — JSON 持久化与断点续写

  实现 FlashbackCheckpoint 的 save_to_file() 和 load_from_file() 方法,
  使用 RapidJSON 进行序列化/反序列化。

  设计参考:
  - DESIGN.md §6.1 FlashbackCheckpoint 结构体
  - mysql_flashback_implementation_gap_analysis.md §难点4: 断点续写与检查点
  - C7 约束: 断点续写需保证幂等重放

  检查点写入时机:
  每处理完一个完整事务 (遇到 XID_EVENT 或 COMMIT_QUERY_EVENT) 后,
  调用 save_to_file() 将当前进度持久化到磁盘。

  恢复策略:
  1. 启动时检查检查点文件是否存在
  2. 如果存在, 加载并验证 (binlog_file 必须在当前 binlog 索引中)
  3. 从记录的 binlog_pos 开始继续处理
  4. 因逆向 SQL 具有幂等性, 重复处理不会产生副作用
*/

#include "sql/flashback_checkpoint.h"

#include <time.h>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"  // my_access, mysql_file_open, mysql_file_close, my_write,
                     // my_sync, my_read, my_delete
#include "mysql/components/services/log_builtins.h"  // LogErr
#include "mysql/psi/mysql_file.h"
#include "mysqld_error.h"  // ER_INTERNAL_ERROR
#include "sql/mysqld.h"    // key_file_misc

/* RapidJSON headers (header-only library) */
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#ifdef MYSQL_SERVER

namespace flashback {

/* ================================================================
 * 常量定义
 * ================================================================ */

/** 检查点文件名前缀 */
static constexpr const char *CHECKPOINT_PREFIX = "flashback_checkpoint_";

/** 检查点文件名后缀 */
static constexpr const char *CHECKPOINT_SUFFIX = ".json";

/* ================================================================
 * 内部辅助函数
 * ================================================================ */

/**
  检查点文件使用的 PSI 文件 key

  复用 key_file_misc 类别, 因为检查点文件属于元数据文件。
*/
static PSI_file_key get_file_key() {
  return key_file_misc;
}

/* ================================================================
 * 构造函数
 * ================================================================ */

FlashbackCheckpoint::FlashbackCheckpoint(const std::string &data_dir,
                                         const std::string &sess_id)
    : session_id(sess_id), m_data_dir(data_dir) {
  DBUG_TRACE;
}

/* ================================================================
 * reset() — 重置状态
 * ================================================================ */

void FlashbackCheckpoint::reset() {
  DBUG_TRACE;
  binlog_file.clear();
  binlog_pos = 0;
  table_idx = 0;
  rows_processed = 0;
  txn_id = 0;
  checkpoint_time = 0;
}

/* ================================================================
 * get_checkpoint_path() — 获取文件路径
 * ================================================================ */

std::string FlashbackCheckpoint::get_checkpoint_path() const {
  std::string path = m_data_dir;

  /* WHY: 确保路径末尾有分隔符 */
  if (!path.empty() && path.back() != '/') {
    path += '/';
  }

  path += CHECKPOINT_PREFIX;
  if (!session_id.empty()) {
    path += session_id;
  } else {
    path += "default";
  }
  path += CHECKPOINT_SUFFIX;

  return path;
}

/* ================================================================
 * checkpoint_file_exists() — 检查文件是否存在
 * ================================================================ */

bool FlashbackCheckpoint::checkpoint_file_exists() const {
  std::string path = get_checkpoint_path();
  /* F_OK 检查文件是否存在 */
  return my_access(path.c_str(), F_OK) == 0;
}

/* ================================================================
 * remove_file() — 删除检查点文件
 * ================================================================ */

FlashbackError FlashbackCheckpoint::remove_file() const {
  DBUG_TRACE;

  std::string path = get_checkpoint_path();

  /* 文件不存在视为成功 (幂等删除) */
  if (!checkpoint_file_exists()) {
    return FlashbackError::NONE;
  }

  if (my_delete(path.c_str(), MYF(MY_WME)) != 0) {
    LogErr(ERROR_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: failed to remove file: %s", path.c_str());
    return FlashbackError::GENERIC;
  }

  return FlashbackError::NONE;
}

/* ================================================================
 * save_to_file() — 序列化并写入 JSON 文件
 * ================================================================
 *
 * JSON 格式示例:
 * {
 *   "binlog_file": "mysql-bin.000003",
 *   "binlog_pos": 1234567,
 *   "table_idx": 2,
 *   "rows_processed": 50000,
 *   "txn_id": 9876543,
 *   "checkpoint_time": 1722153600,
 *   "session_id": "session_123"
 * }
 */

FlashbackError FlashbackCheckpoint::save_to_file() const {
  DBUG_TRACE;

  std::string path = get_checkpoint_path();

  /* 1. 构建 JSON 文档 */
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);

  writer.StartObject();

  /* 核心字段 */
  writer.Key("binlog_file");
  writer.String(binlog_file.c_str());

  writer.Key("binlog_pos");
  writer.Uint64(static_cast<uint64_t>(binlog_pos));

  writer.Key("table_idx");
  writer.Uint(table_idx);

  writer.Key("rows_processed");
  writer.Uint64(rows_processed);

  writer.Key("txn_id");
  writer.Uint64(txn_id);

  /* 辅助字段 */
  writer.Key("checkpoint_time");
  writer.Uint64(static_cast<uint64_t>(checkpoint_time));

  writer.Key("session_id");
  writer.String(session_id.c_str());

  writer.EndObject();

  /* 2. 打开文件 (覆盖写入) */
  File fd = mysql_file_open(get_file_key(), path.c_str(),
                            O_WRONLY | O_CREAT | O_TRUNC, MYF(MY_WME));
  if (fd < 0) {
    LogErr(ERROR_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: failed to open file for writing: %s",
           path.c_str());
    return FlashbackError::GENERIC;
  }

  /* 3. 写入 JSON 数据 */
  const char *json_str = buffer.GetString();
  size_t json_len = buffer.GetSize();

  ssize_t bytes_written = my_write(fd,
                                   reinterpret_cast<const unsigned char *>(json_str),
                                   json_len, MYF(MY_WME | MY_NABP));
  if (static_cast<size_t>(bytes_written) != json_len) {
    LogErr(ERROR_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: write failed for: %s", path.c_str());
    mysql_file_close(fd, MYF(0));
    return FlashbackError::GENERIC;
  }

  /* 4. fsync 确保数据落盘 */
  if (my_sync(fd, MYF(MY_WME)) != 0) {
    LogErr(ERROR_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: fsync failed for: %s", path.c_str());
    mysql_file_close(fd, MYF(0));
    return FlashbackError::GENERIC;
  }

  mysql_file_close(fd, MYF(0));

  return FlashbackError::NONE;
}

/* ================================================================
 * load_from_file() — 读取并反序列化 JSON 文件
 * ================================================================
 *
 * 解析策略:
 * 1. 先尝试读取整个文件到内存缓冲区
 * 2. 使用 RapidJSON DOM 解析
 * 3. 逐个提取字段, 未知字段被忽略 (向前兼容)
 * 4. 必填字段 (binlog_file, binlog_pos) 缺失时返回错误
 */

FlashbackError FlashbackCheckpoint::load_from_file() {
  DBUG_TRACE;

  std::string path = get_checkpoint_path();

  /* 1. 打开文件 */
  File fd = mysql_file_open(get_file_key(), path.c_str(), O_RDONLY, MYF(0));
  if (fd < 0) {
    /* 文件不存在是正常情况 (首次运行) */
    return FlashbackError::GENERIC;
  }

  /* 2. 获取文件大小 */
  my_off_t file_size = my_seek(fd, 0, MY_SEEK_END, MYF(0));
  if (file_size <= 0 || file_size > 1024 * 1024) {
    /* 文件大小不合理 (空文件或 > 1MB), 视为损坏 */
    LogErr(WARNING_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: invalid file size %lld for: %s",
           static_cast<longlong>(file_size), path.c_str());
    mysql_file_close(fd, MYF(0));
    return FlashbackError::GENERIC;
  }
  my_seek(fd, 0, MY_SEEK_SET, MYF(0));

  /* 3. 读取文件内容 */
  std::string json_str(static_cast<size_t>(file_size), '\0');
  ssize_t bytes_read =
      my_read(fd, reinterpret_cast<unsigned char *>(&json_str[0]),
              static_cast<size_t>(file_size), MYF(0));
  mysql_file_close(fd, MYF(0));

  if (bytes_read != static_cast<ssize_t>(file_size)) {
    LogErr(WARNING_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: read incomplete file: %s (read %zd, "
           "expected %lld)",
           path.c_str(), bytes_read, static_cast<longlong>(file_size));
    return FlashbackError::GENERIC;
  }

  /* 4. RapidJSON 解析 */
  rapidjson::Document doc;
  doc.Parse(json_str.c_str());

  if (doc.HasParseError()) {
    LogErr(WARNING_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: JSON parse error at offset %zu in: %s",
           doc.GetErrorOffset(), path.c_str());
    return FlashbackError::GENERIC;
  }

  if (!doc.IsObject()) {
    LogErr(WARNING_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: JSON root is not an object in: %s",
           path.c_str());
    return FlashbackError::GENERIC;
  }

  /* 5. 提取核心字段 (逐个检查, 未知字段忽略) */
  bool has_binlog_file = false;
  bool has_binlog_pos = false;

  /* binlog_file (string, 可选 — 缺失时保持当前值) */
  if (doc.HasMember("binlog_file") && doc["binlog_file"].IsString()) {
    binlog_file = doc["binlog_file"].GetString();
    has_binlog_file = true;
  }

  /* binlog_pos (uint64, 可选 — 缺失时保持当前值) */
  if (doc.HasMember("binlog_pos") && doc["binlog_pos"].IsUint64()) {
    binlog_pos = static_cast<my_off_t>(doc["binlog_pos"].GetUint64());
    has_binlog_pos = true;
  }

  /* table_idx (uint, 可选) */
  if (doc.HasMember("table_idx") && doc["table_idx"].IsUint()) {
    table_idx = doc["table_idx"].GetUint();
  }

  /* rows_processed (uint64, 可选) */
  if (doc.HasMember("rows_processed") &&
      doc["rows_processed"].IsUint64()) {
    rows_processed = doc["rows_processed"].GetUint64();
  }

  /* txn_id (uint64, 可选) */
  if (doc.HasMember("txn_id") && doc["txn_id"].IsUint64()) {
    txn_id = doc["txn_id"].GetUint64();
  }

  /* checkpoint_time (uint64, 可选) */
  if (doc.HasMember("checkpoint_time") &&
      doc["checkpoint_time"].IsUint64()) {
    checkpoint_time =
        static_cast<my_time_t>(doc["checkpoint_time"].GetUint64());
  }

  /* 6. 校验: 核心字段必须完整 */
  if (!has_binlog_file || !has_binlog_pos) {
    LogErr(WARNING_LEVEL, ER_INTERNAL_ERROR,
           "Flashback checkpoint: missing required fields "
           "(binlog_file=%d, binlog_pos=%d) in: %s",
           has_binlog_file, has_binlog_pos, path.c_str());
    return FlashbackError::GENERIC;
  }

  return FlashbackError::NONE;
}

}  // namespace flashback

#endif /* MYSQL_SERVER */
