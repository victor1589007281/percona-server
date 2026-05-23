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
  @file sql/flashback_checkpoint.h
  Flashback 检查点 (Checkpoint) 结构体 — 用于断点续写与幂等恢复

  设计参考:
  - DESIGN.md §6.1 FlashbackCheckpoint 结构体
  - mysql_flashback_implementation_gap_analysis.md §难点4: 断点续写与检查点

  核心用途:
  1. 闪回过程中定期持久化进度, 支持中断后从最近检查点恢复
  2. 通过记录当前 binlog 位置、表索引、事务 ID, 实现精确断点续传
  3. 结合逆向 SQL 的幂等语义 (INSERT IGNORE / 条件 UPDATE),
     保证重复执行不会产生副作用

  使用方式:
  @code
  FlashbackCheckpoint cp("/var/lib/mysql", "session_123");
  cp.binlog_file = "mysql-bin.000003";
  cp.binlog_pos  = 1234567;
  cp.table_idx   = 2;
  cp.rows_processed = 50000;
  cp.txn_id = 9876543;

  // 定期写入磁盘
  cp.save_to_file();

  // 下次启动时恢复
  FlashbackCheckpoint restored("/var/lib/mysql", "session_123");
  if (restored.load_from_file() == FlashbackError::NONE) {
    // 从 restored 记录的位置继续
  }
  @endcode
*/

#ifndef FLASHBACK_CHECKPOINT_INCLUDED
#define FLASHBACK_CHECKPOINT_INCLUDED

#include <string>

#include "my_inttypes.h"
#include "sql/flashback_types.h"  // FlashbackError

namespace flashback {

/**
  闪回检查点结构体

  记录闪回操作的中间状态, 用于断点续写 (resume after interruption)
  和幂等重放 (idempotent replay)。

  字段说明:
  - binlog_file:  当前处理到的 binlog 文件名
  - binlog_pos:   当前处理到的 binlog 位置偏移 (字节)
  - table_idx:    当前处理到的表在请求表列表中的索引 (从 0 开始)
  - rows_processed: 累计已处理的行数
  - txn_id:       最近一次提交的事务 ID (用于 GTID 定位)

  时序保证:
  检查点必须在事务提交**之后**写入, 以确保 "已提交的事务" 与
  "已记录的进度" 之间的一致性。如果进程在检查点写入前崩溃,
  下次会从旧检查点重新处理最近的事务 — 但逆向 SQL 的幂等性
  (INSERT IGNORE / 条件 DELETE) 会保证不会产生重复数据。
*/
struct FlashbackCheckpoint {
  /* ================================================================
   * 核心字段 (持久化)
   * ================================================================ */

  /** 当前处理到的 binlog 文件名 (如 "mysql-bin.000003") */
  std::string binlog_file;

  /** 当前处理到的 binlog 位置偏移 (字节偏移, 从文件头算起) */
  my_off_t binlog_pos{0};

  /** 当前处理到的表在请求表列表中的索引 (从 0 开始) */
  uint32_t table_idx{0};

  /** 累计已处理的行数 */
  ulonglong rows_processed{0};

  /** 最近一次提交的事务 ID (0 表示尚未处理任何事务) */
  uint64_t txn_id{0};

  /* ================================================================
   * 辅助字段 (非核心, 用于调试)
   * ================================================================ */

  /** 检查点写入时间 (Unix 秒) */
  my_time_t checkpoint_time{0};

  /** 闪回会话标识 (用于生成检查点文件名) */
  std::string session_id;

  /* ================================================================
   * 构造函数
   * ================================================================ */

  FlashbackCheckpoint() = default;

  /**
    构造函数

    @param data_dir    MySQL 数据目录 (用于默认检查点路径)
    @param session_id  闪回会话唯一标识
  */
  FlashbackCheckpoint(const std::string &data_dir,
                      const std::string &session_id);

  /** 重置检查点状态到初始值 */
  void reset();

  /* ================================================================
   * 持久化接口
   * ================================================================ */

  /**
    将检查点写入 JSON 文件

    使用 RapidJSON 序列化检查点字段, 写入后执行 fsync 确保持久化。
    文件路径: <data_dir>/flashback_checkpoint_<session_id>.json

    WHY: 使用 JSON 而非二进制格式的原因:
    1. 人类可读, 便于运维排查问题
    2. 字段增删兼容性好 (解析器忽略未知字段)
    3. 与 MySQL 已有的 meta 文件格式一致

    @retval FlashbackError::NONE   写入成功
    @retval FlashbackError::GENERIC 写入失败 (I/O 错误、磁盘满等)
  */
  FlashbackError save_to_file() const;

  /**
    从 JSON 文件加载检查点

    读取检查点文件, 反序列化到当前结构体。
    如果文件不存在或格式错误, 返回 FlashbackError::GENERIC。

    WHY: 不覆盖已有字段 (保留构造函数设置的 data_dir/session_id)。
    仅更新从文件中成功解析的字段。

    @retval FlashbackError::NONE   加载成功
    @retval FlashbackError::GENERIC 加载失败 (文件不存在、JSON 格式错误等)
  */
  FlashbackError load_from_file();

  /**
    获取检查点文件的完整路径

    @return 检查点文件的绝对路径
  */
  std::string get_checkpoint_path() const;

  /**
    检查是否存在检查点文件

    @retval true  检查点文件存在
    @retval false 不存在
  */
  bool checkpoint_file_exists() const;

  /**
    删除检查点文件 (闪回成功完成后清理)

    @retval FlashbackError::NONE   删除成功或文件不存在
    @retval FlashbackError::GENERIC 删除失败
  */
  FlashbackError remove_file() const;

  /* ================================================================
   * 内部成员
   * ================================================================ */

 private:
  /** MySQL 数据目录 (用于定位检查点文件) */
  std::string m_data_dir;
};

}  // namespace flashback

#endif /* FLASHBACK_CHECKPOINT_INCLUDED */
