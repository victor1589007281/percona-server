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

  实现 DDLBarrier 类的三个核心方法:
  - check(): 检查闪回窗口内的 DDL 兼容性
  - acquire_exclusive_lock(): 获取 MDL_EXCLUSIVE 锁
  - release_exclusive_lock(): 释放排他锁

  设计参考: mysql_flashback_implementation.md §2.2.6
            mysql_flashback_implementation_deep.md §5
*/

#include "sql/flashback_ddl_barrier.h"

#include <new>

#include "lex_string.h"
#include "mdl.h"
#include "my_alloc.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysql_com.h"
#include "sql/sql_class.h"  // THD

namespace flashback {

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
 * check() - 检查闪回窗口内是否存在不兼容 DDL
 * ================================================================ */

/**
  判断 DDL SQL 语句是否兼容闪回

  不兼容的 DDL 类型:
  - DROP TABLE / TRUNCATE TABLE: 数据结构不存在
  - DROP COLUMN: 历史数据中有该列，无法适配新结构
  - CHANGE COLUMN / MODIFY COLUMN (类型变更): 类型不兼容

  兼容的 DDL 类型:
  - ADD INDEX / DROP INDEX: 仅索引变更
  - ADD COLUMN (带默认值或可空): 旧数据填充 NULL/默认值
  - RENAME INDEX: 仅元数据变更

  @param ddl_sql DDL 语句文本

  @retval true  兼容，闪回可以继续
  @retval false 不兼容，闪回必须中止
*/
bool DDLBarrier::is_ddl_compatible(const char *ddl_sql) const {
  DBUG_TRACE;

  // 快速判断: 如果 SQL 为空，视为兼容
  if (ddl_sql == nullptr || ddl_sql[0] == '\0') {
    return true;
  }

  // WHY: 使用简单关键字匹配，避免完整 SQL 解析的复杂度。
  // 在生产环境中应使用更精确的 SQL 解析器。
  const char *upper_sql = ddl_sql;  // TODO: 实际应转为大写

  // 不兼容操作: DROP TABLE, TRUNCATE
  if (strstr(upper_sql, "DROP TABLE") != nullptr ||
      strstr(upper_sql, "TRUNCATE") != nullptr) {
    return false;
  }

  // 不兼容操作: DROP COLUMN (排除 DROP INDEX)
  if (strstr(upper_sql, "DROP COLUMN") != nullptr) {
    return false;
  }

  // 不兼容操作: CHANGE COLUMN / MODIFY COLUMN
  if (strstr(upper_sql, "CHANGE COLUMN") != nullptr ||
      strstr(upper_sql, "MODIFY COLUMN") != nullptr) {
    return false;
  }

  // 其他 DDL 视为兼容（ADD INDEX, RENAME INDEX, ADD COLUMN 等）
  return true;
}

FlashbackError DDLBarrier::check(const FlashbackRequest &request) {
  DBUG_TRACE;

  // WHY: Binlog 扫描实现涉及 Binlog_reader 基础设施，
  // 当前阶段返回 NONE 表示无冲突。
  // 完整实现需要:
  // 1. 定位 target_time 对应的 binlog 文件和位置
  // 2. 顺序读取到当前 binlog 位置
  // 3. 对每个 DDL event 调用 is_ddl_compatible()
  // 4. 发现不兼容时立即返回 DDL_INCOMPATIBLE

  // 参数校验
  if (request.tables == nullptr || request.table_count == 0) {
    return FlashbackError::GENERIC;
  }

  // TODO: 实现完整的 binlog 扫描逻辑
  // 参考 mysql_flashback_implementation_deep.md §5.2
  //
  // 伪代码:
  // for each table in request.tables:
  //   find_binlog_range(target_time, now)
  //   for each binlog_file in range:
  //     for each event in binlog_file:
  //       if is_ddl_event(event) and !is_ddl_compatible(event):
  //         return FlashbackError::DDL_INCOMPATIBLE

  return FlashbackError::NONE;
}

/* ================================================================
 * acquire_exclusive_lock() - 获取 MDL_EXCLUSIVE 排他锁
 * ================================================================ */

bool DDLBarrier::acquire_exclusive_lock(const FlashbackRequest &request) {
  DBUG_TRACE;

  // 参数校验
  if (request.tables == nullptr || request.table_count == 0) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback DDL barrier: no tables specified");
    return true;
  }

  // 防止重复加锁
  if (m_locked) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Flashback DDL barrier: lock already acquired");
    return true;
  }

  // WHY: 使用 MDL_EXPLICIT 持续时间，这样锁不会在语句结束时自动释放，
  // 而是由我们手动控制释放时机。这对于闪回操作的长时间运行是必要的。

  // 分配 ticket 数组
  m_tickets = new (std::nothrow) MDL_ticket *[request.table_count];
  if (m_tickets == nullptr) {
    my_error(ER_OUTOFMEMORY, MYF(0),
             sizeof(MDL_ticket *) * request.table_count);
    return true;
  }

  // 对每个表获取 MDL_EXCLUSIVE 锁
  for (uint32_t i = 0; i < request.table_count; ++i) {
    const LEX_CSTRING &table_ref = request.tables[i];

    // 解析表名: 可能是 "db.table" 或仅 "table"
    const char *db_name = "";
    const char *table_name = nullptr;

    if (table_ref.length == 0) {
      continue;
    }

    // WHY: MDL_key::TABLE 需要独立的 db 和 table name 参数。
    // 表名引用可能是 "db.table" 格式，需要分离。
    char db_buf[NAME_LEN + 1] = {0};
    const char *dot = static_cast<const char *>(
        memchr(table_ref.str, '.', table_ref.length));
    if (dot != nullptr) {
      // "db.table" 格式
      size_t db_len = static_cast<size_t>(dot - table_ref.str);
      if (db_len > NAME_LEN) db_len = NAME_LEN;
      memcpy(db_buf, table_ref.str, db_len);
      db_buf[db_len] = '\0';
      db_name = db_buf;
      table_name = dot + 1;
    } else {
      // 仅 "table" 格式，db 留空（使用当前默认 db）
      table_name = table_ref.str;
    }

    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, db_name, table_name,
                     MDL_EXCLUSIVE, MDL_EXPLICIT);

    // WHY: 使用 thd->variables.lock_wait_timeout 作为超时时间，
    // 与 MySQL 标准锁等待行为保持一致。
    if (m_thd->mdl_context.acquire_lock(
            &mdl_request, m_thd->variables.lock_wait_timeout)) {
      // 加锁失败: 可能是超时、被杀或内存不足
      // 错误已由 acquire_lock 内部设置
      release_exclusive_lock();  // 清理已获取的部分锁
      return true;
    }

    m_tickets[m_ticket_count++] = mdl_request.ticket;
  }

  m_locked = true;
  return false;
}

/* ================================================================
 * release_exclusive_lock() - 释放排他锁
 * ================================================================ */

void DDLBarrier::release_exclusive_lock() {
  DBUG_TRACE;

  if (!m_locked) {
    return;
  }

  // 逐个释放 ticket
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
