/* Copyright (c) 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file sql/sql_flashback.h
  Flashback SQL 命令类声明

  本文件定义 FLASHBACK TABLE 和 FLASHBACK TRANSACTION 的命令类，
  它们是 SQL 层与闪回调度器之间的桥梁。

  执行流程:
  1. SQL 解析器构建 PT_flashback_table / PT_flashback_transaction 语法树节点
  2. make_cmd() 创建对应的 Sql_cmd 对象
  3. dispatch_command() 调用 Sql_cmd::execute()
  4. execute() 内部执行: 权限检查 → DDL 屏障 → 调度器执行 → 审计日志

  设计参考: mysql_flashback_implementation_v2.md §5.2.1
            mysql_flashback_synthesis_report.md §2.4
*/

#ifndef SQL_FLASHBACK_INCLUDED
#define SQL_FLASHBACK_INCLUDED

#include "my_sqlcommand.h"
#include "sql/mem_root_array.h"
#include "sql/parse_tree_nodes.h"  // Mem_root_array_YY
#include "sql/sql_cmd.h"  // Sql_cmd

class THD;
class Item;
class Table_ident;

/**
  FLASHBACK TABLE 命令类

  对应 SQL 语法:
    FLASHBACK TABLE tbl1, tbl2 TO TIMESTAMP '2025-01-01 00:00:00' [DRY RUN];
    FLASHBACK TABLE tbl TO TRX_ID 123456;

  执行流程:
  1. 权限检查: 需要对目标表有 DROP 权限（闪回是破坏性操作）
  2. DDL 屏障: 检查闪回窗口内是否有不兼容 DDL
  3. 调度器执行: 委托给 FlashbackScheduler::execute()
  4. 审计日志: 记录闪回操作的结果和统计信息
*/
class Sql_cmd_flashback_table final : public Sql_cmd {
 public:
  /**
    构造函数

    @param tables       闪回表列表 (由 yacc 解析)
    @param timestamp    目标时间戳表达式 (TIMESTAMP 模式)。TRX_ID 模式时为 nullptr
    @param trx_id       目标事务 ID 表达式 (TRX_ID 模式)。TIMESTAMP 模式时为 nullptr
    @param dry_run      是否为 DRY RUN 模式
  */
  Sql_cmd_flashback_table(Mem_root_array_YY<Table_ident *> *tables,
                           Item *timestamp, Item *trx_id, bool dry_run);

  /// @copydoc Sql_cmd::sql_command_code()
  enum_sql_command sql_command_code() const override;

  /// @copydoc Sql_cmd::execute()
  bool execute(THD *thd) override;

 private:
  /** 闪回表列表 */
  Mem_root_array_YY<Table_ident *> *m_tables;

  /** 目标时间戳表达式 (TIMESTAMP 模式非空) */
  Item *m_timestamp;

  /** 目标事务 ID 表达式 (TRX_ID 模式非空) */
  Item *m_trx_id;

  /** DRY RUN 标志 */
  bool m_dry_run;
};

/**
  FLASHBACK TRANSACTION 命令类

  对应 SQL 语法:
    FLASHBACK TRANSACTION 123456;

  闪回事务通过解析 binlog 中指定事务的所有操作，
  生成反向 SQL 并执行，从而撤销该事务的影响。
*/
class Sql_cmd_flashback_transaction final : public Sql_cmd {
 public:
  /**
    构造函数

    @param trx_id  目标事务 ID 表达式
  */
  explicit Sql_cmd_flashback_transaction(Item *trx_id);

  /// @copydoc Sql_cmd::sql_command_code()
  enum_sql_command sql_command_code() const override;

  /// @copydoc Sql_cmd::execute()
  bool execute(THD *thd) override;

 private:
  /** 目标事务 ID 表达式 */
  Item *m_trx_id;
};

#endif /* SQL_FLASHBACK_INCLUDED */
