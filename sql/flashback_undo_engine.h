/* Copyright (c) 2025, Percona and/or its affiliates.

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
  @file sql/flashback_undo_engine.h
  Undo Log-based Flashback Engine

  本模块实现基于 InnoDB Undo Log 的短窗口闪回（分钟级），核心功能包括:
  - execute_table(): 对单个表执行闪回，扫描聚簇索引恢复历史版本
  - restore_row(): 对单行执行恢复操作（INSERT/UPDATE/DELETE 三种类型）
  - set_flashback_context(): 设置闪回上下文，启用 InnoDB 历史版本读取

  设计参考: mysql_flashback_implementation.md §3
            mysql_flashback_implementation_v2.md §5.1
            DESIGN-v2.md §5 (Undo 引擎全表扫描 + 分批提交)

  约束:
  - C2: 闪回期间需持有 MDL 排他锁，防止并发 DDL
  - C3: Undo 闪回需检查 innodb_flashback_retention_seconds 窗口
  - C6: 闪回操作需分批提交（每 1000 行），避免长事务
*/

#ifndef FLASHBACK_UNDO_ENGINE_INCLUDED
#define FLASHBACK_UNDO_ENGINE_INCLUDED

#include "my_inttypes.h"
#include "my_time_t.h"
#include "sql/flashback_types.h"  // FlashbackRequest, FlashbackResult

class THD;
struct TABLE;

namespace flashback {

/**
  Undo 闪回引擎

  基于 InnoDB MVCC 版本链实现短窗口（分钟级）闪回。
  工作原理:
  1. 对每个目标表，打开表并进入 flashback 模式
  2. 设置 THD 的 flashback_timestamp 上下文
  3. 全表扫描聚簇索引，读取每行的历史版本
  4. 对每行，构建目标时间点的历史版本
  5. 如果历史版本与当前版本不同，执行恢复
  6. 分批提交（每 1000 行），避免长事务

  线程安全: 实例不可跨线程共享，每个 THD 使用独立实例。
*/
class UndoFlashbackEngine {
 public:
  /**
    构造函数

    @param thd 当前线程上下文
  */
  explicit UndoFlashbackEngine(THD *thd);

  /** 析构函数: 释放内部资源 */
  ~UndoFlashbackEngine();

  /**
    执行 Undo 闪回

    对请求中的所有表执行闪回操作。

    @param request 闪回请求参数
    @param result  闪回执行结果（由调用者提供存储）

    @retval true  执行失败
    @retval false 执行成功
  */
  bool execute(const FlashbackRequest &request, FlashbackResult &result);

  /**
    对单个表执行闪回

    核心算法:
    1. 打开目标表
    2. 设置 flashback 时间戳上下文
    3. 全表扫描聚簇索引 (rnd_init → rnd_next)
    4. 对每行调用 restore_row() 恢复历史版本
    5. 每 batch_size 行提交一次事务
    6. 关闭表，清理 flashback 上下文

    @param db_name     数据库名
    @param table_name  表名
    @param target_time 目标时间戳
    @param dry_run     如果为 true，仅统计不执行
    @param[out] result 闪回结果（累计计数）

    @retval true  执行失败
    @retval false 执行成功
  */
  bool execute_table(const char *db_name, const char *table_name,
                     my_time_t target_time, bool dry_run,
                     FlashbackResult &result);

  /**
    恢复单行到目标时间点的历史版本

    算法:
    1. 通过 DB_TRX_ID 和 DB_ROLL_PTR 找到版本链
    2. 逆向遍历版本链，找到目标时间点的版本
    3. 比较当前版本与历史版本
    4. 如果不同，根据操作类型执行恢复:
       - 当前行存在但历史版本不存在 → DELETE (删除后插入的行)
       - 当前行不存在但历史版本存在 → INSERT (恢复被删除的行)
       - 两者都存在但内容不同 → UPDATE (恢复旧值)

    @param table       目标表对象
    @param target_time 目标时间戳
    @param dry_run     如果为 true，仅统计不执行
    @param[out] restored  本次调用恢复的行数

    @retval true  恢复失败
    @retval false 成功
  */
  bool restore_row(TABLE *table, my_time_t target_time, bool dry_run,
                   ulonglong &restored);

  /**
    检查 Undo 闪回是否可用于指定目标时间

    调用 InnoDB 层 trx_sys_get_oldest_timestamp() 获取
    undo 数据仍可用的最早时间戳。

    @param target_time 目标时间戳
    @retval true  可用
    @retval false 不可用（目标时间超出 undo 窗口）
  */
  bool is_available(my_time_t target_time) const;

  /**
    获取引擎类型标识
    @return FlashbackEngineType::UNDO
  */
  static FlashbackEngineType engine_type() {
    return FlashbackEngineType::UNDO;
  }

 private:
  /**
    每批提交的行数。

    WHY: 大批量闪回会产生大量 undo 日志，分批提交可以控制
    undo 膨胀，降低回滚段压力。1000 行是一个经验值，
    在提交开销和事务大小之间取得平衡。
  */
  static constexpr uint32_t BATCH_SIZE = 1000;

  /** 当前线程上下文 */
  THD *m_thd;

  /** 已处理的行数（跨表累计） */
  ulonglong m_rows_processed{0};

  /** 已恢复的行数（跨表累计） */
  ulonglong m_rows_restored{0};
};

}  // namespace flashback

#endif /* FLASHBACK_UNDO_ENGINE_INCLUDED */
