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
  @file sql/flashback_binlog_engine.h
  Binlog-based Flashback Engine

  本模块实现基于 Binlog 的长窗口闪回能力（天级），核心功能包括:
  - find_position_at_timestamp(): 定位指定时间点的 binlog 文件与位置
  - reverse_rows_event(): 逆向单个 Rows Event（Write→Delete, Delete→Insert,
    Update 前后镜像互换）
  - check_row_image_compatibility(): 检查当前 binlog_row_image 是否支持闪回

  设计参考: mysql_flashback_implementation_v2.md §5.2.2
            mysql_flashback_implementation.md §2.2.5
            mysql_flashback_synthesis_report.md §3.2(4b)

  约束:
  - C1: 闪回操作期间必须设置 sql_log_bin=OFF，防止闪回 SQL 被复制到从库
  - C4: 必须检查 binlog_row_image 兼容性，MINIMAL 模式无法完整逆向
  - C5: 闪回前必须检查 DDL 屏障
  - C7: Binlog 解析过程中需正确处理事务边界（GTID、XID）
*/

#ifndef FLASHBACK_BINLOG_ENGINE_INCLUDED
#define FLASHBACK_BINLOG_ENGINE_INCLUDED

#include <string>
#include <vector>

#include "my_inttypes.h"
#include "my_time_t.h"
#include "sql/flashback_types.h"  // FlashbackError, FlashbackRequest, FlashbackResult

class THD;
class Rows_log_event;
class Update_rows_log_event;
class Write_rows_log_event;
class Delete_rows_log_event;
class Log_event;

namespace flashback {

/**
  Binlog 闪回引擎

  基于 Row-based Binlog 的逆向解析能力，实现长窗口（天级）闪回。
  工作原理:
  1. 定位目标时间点的 binlog 文件与位置
  2. 从该位置正向读取 binlog 事件到当前时间点
  3. 对每个 DML 事件进行逆向（INSERT→DELETE, DELETE→INSERT, UPDATE 互换）
  4. 执行逆向 SQL 完成闪回

  线程安全: 实例不可跨线程共享，每个 THD 使用独立实例。
*/
class BinlogFlashbackEngine {
 public:
  /**
    构造函数

    @param thd 当前线程上下文
  */
  explicit BinlogFlashbackEngine(THD *thd);

  /** 析构函数 */
  ~BinlogFlashbackEngine();

  /**
    执行 Binlog 闪回

    从目标时间点开始，正向读取 binlog 事件并逆向执行。
    闪回期间自动设置 sql_log_bin=OFF（约束 C1）。

    @param request 闪回请求参数
    @param result  闪回执行结果（由调用者提供存储）

    @retval true  执行失败
    @retval false 执行成功
  */
  bool execute(const FlashbackRequest &request, FlashbackResult &result);

  /**
    定位指定时间点的 binlog 文件与位置

    扫描 binlog 索引文件，找到包含目标时间点的 binlog 文件，
    并在该文件中查找最接近目标时间点的 position。

    算法:
    1. 读取 binlog 索引文件，获取所有 binlog 文件列表
    2. 依次打开每个 binlog 文件，读取第一个事件的时间戳
    3. 找到第一个起始时间 >= target_ts 的文件，回退到前一个文件
    4. 在目标文件中正向扫描，找到最接近 target_ts 的 position

    @param[in]  target_ts      目标时间戳（秒级精度）
    @param[out] binlog_file    输出的 binlog 文件名（长度 ≥ FN_REFLEN）
    @param[out] binlog_pos     输出的 binlog 位置偏移量
    @param[out] checksum_alg   输出的 binlog 校验算法

    @retval FlashbackError::NONE              定位成功
    @retval FlashbackError::BINLOG_EXPIRED    binlog 文件已被清理
    @retval FlashbackError::OUT_OF_WINDOW     目标时间超出 binlog 保留窗口
    @retval FlashbackError::GENERIC           其他错误（如 I/O 失败）
  */
  static FlashbackError find_position_at_timestamp(
      my_time_t target_ts, char *binlog_file, my_off_t *binlog_pos,
      uint8_t *checksum_alg = nullptr);

  /**
    逆向单个 Rows Event

    根据 Rows Event 的类型，生成对应的逆向操作:
    - Write_rows_log_event (INSERT)  → 生成反向 DELETE
    - Delete_rows_log_event (DELETE) → 生成反向 INSERT
    - Update_rows_log_event (UPDATE) → before/after 镜像互换，生成反向 UPDATE

    WHY: 在 Row-based replication 中，每个 Rows_event 只包含一种操作类型。
    逆向的核心思路是:
    - INSERT 的逆向 = 用 after_image 的 PK 值构造 DELETE
    - DELETE 的逆向 = 用 before_image 的完整值构造 INSERT
    - UPDATE 的逆向 = 将 before_image 作为 SET 值，after_image 作为 WHERE 条件

    @param[in]  event   要逆向的 Rows Event（不修改原始事件）
    @param[in]  request 闪回请求参数（用于表过滤）
    @param[out] sql_buf 输出的逆向 SQL 缓冲区（由调用者管理）

    @retval true  逆向失败（如事件类型不支持）
    @retval false 逆向成功
  */
  bool reverse_rows_event(const Rows_log_event &event,
                          const FlashbackRequest &request,
                          std::string &sql_buf);

  /**
    检查 binlog_row_image 兼容性

    在 Binlog-based 闪回前，检查当前系统的 binlog_row_image 设置
    是否支持完整逆向。

    WHY: binlog_row_image 有三种模式:
    - FULL:    记录所有列的 before/after 值 → 支持完整逆向 ✅
    - MINIMAL: 仅记录 PK 列和变更列 → 无法逆向未变更的列 ❌
    - NOBLOB:  类似 FULL 但排除 BLOB/TEXT → 部分场景受限 ⚠️

    当 flashback_require_full_row_image=true 时，只有 FULL 模式通过检查。
    否则，FULL 和 NOBLOB 模式均可接受。

    @param thd 当前线程上下文（用于读取系统变量）

    @retval FlashbackError::NONE                     兼容，可以执行闪回
    @retval FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL binlog_row_image 不兼容
    @retval FlashbackError::GENERIC                  其他错误
  */
  static FlashbackError check_row_image_compatibility(THD *thd);

 private:
  /**
    逆向 Write_rows_log_event (INSERT) → DELETE

    使用 after_image 中的主键值构造 DELETE 语句。

    @param event  Write_rows_log_event
    @param sql_buf 输出的 DELETE SQL
    @retval true  失败
    @retval false 成功
  */
  bool reverse_write_event(const Write_rows_log_event &event,
                           std::string &sql_buf);

  /**
    逆向 Delete_rows_log_event (DELETE) → INSERT

    使用 before_image 中的完整值构造 INSERT 语句。

    @param event  Delete_rows_log_event
    @param sql_buf 输出的 INSERT SQL
    @retval true  失败
    @retval false 成功
  */
  bool reverse_delete_event(const Delete_rows_log_event &event,
                            std::string &sql_buf);

  /**
    逆向 Update_rows_log_event (UPDATE) → 反向 UPDATE

    将 before_image 作为 SET 值（恢复旧值），
    将 after_image 作为 WHERE 条件（定位当前行）。

    @param event  Update_rows_log_event
    @param sql_buf 输出的 UPDATE SQL
    @retval true  失败
    @retval false 成功
  */
  bool reverse_update_event(const Update_rows_log_event &event,
                            std::string &sql_buf);

  /**
    将行数据编码为 SQL 值字符串

    根据列类型将原始字节数据转换为 SQL 可接受的字面量格式。

    @param row_data    行数据指针
    @param row_len     行数据长度
    @param null_bitmap NULL 位图
    @param sql_buf     输出的 SQL 值字符串
  */
  void encode_row_as_sql_values(const std::vector<uint8_t> &row_data,
                                const std::vector<uint8_t> &null_bitmap,
                                std::string &sql_buf);

  /**
    检查当前 binlog 文件是否需要切换到下一个

    @param current_file 当前 binlog 文件名
    @param next_file    下一个 binlog 文件名（长度 ≥ FN_REFLEN）
    @retval true  需要切换
    @retval false 不需要切换
  */
  bool needs_file_switch(const char *current_file, char *next_file) const;

  /** 当前线程上下文 */
  THD *m_thd;

  /** 闪回执行期间已处理的 binlog 文件数 */
  uint32_t m_files_processed{0};

  /** 闪回执行期间已处理的事件数 */
  uint64_t m_events_processed{0};
};

}  // namespace flashback

#endif /* FLASHBACK_BINLOG_ENGINE_INCLUDED */
