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
  @file sql/flashback_ddl_barrier.h
  Flashback DDL 屏障模块

  本模块负责在闪回操作期间防止不兼容的 DDL 操作：
  - check():        检查闪回窗口内是否存在不兼容 DDL
  - acquire_exclusive_lock():  获取 MDL_EXCLUSIVE 锁，阻塞并发 DDL
  - release_exclusive_lock():  释放排他锁

  设计参考: mysql_flashback_implementation.md §2.2.6
            mysql_flashback_implementation_deep.md §5
*/

#ifndef FLASHBACK_DDL_BARRIER_INCLUDED
#define FLASHBACK_DDL_BARRIER_INCLUDED

#include "my_inttypes.h"
#include "sql/flashback_types.h"  // FlashbackError, FlashbackRequest

class THD;
class MDL_ticket;

namespace flashback {

/**
  DDL 屏障类

  在闪回操作前和执行期间保护目标表不被不兼容的 DDL 修改。

  使用方式:
  1. 调用 check() 检查闪回窗口内是否已有不兼容 DDL
  2. 调用 acquire_exclusive_lock() 获取排他锁
  3. 执行闪回操作
  4. 调用 release_exclusive_lock() 释放锁（或在语句/事务结束时自动释放）

  线程安全: 实例不可跨线程共享，每个 THD 使用独立实例。
*/
class DDLBarrier {
 public:
  /**
    构造函数

    @param thd 当前线程上下文
  */
  explicit DDLBarrier(THD *thd);

  /** 析构函数: 如果锁未释放，自动清理 */
  ~DDLBarrier();

  /**
    检查闪回窗口内是否存在不兼容 DDL

    扫描从 target_time 到当前时间的所有 binlog，查找对目标表的不兼容 DDL
    操作（DROP TABLE, TRUNCATE TABLE, DROP COLUMN, CHANGE COLUMN 等）。

    @param request 闪回请求参数

    @retval FlashbackError::NONE  无冲突，可以安全闪回
    @retval FlashbackError::DDL_INCOMPATIBLE  发现不兼容 DDL
    @retval FlashbackError::BINLOG_EXPIRED    binlog 文件已被清理
    @retval FlashbackError::GENERIC           其他错误
  */
  FlashbackError check(const FlashbackRequest &request);

  /**
    获取 MDL_EXCLUSIVE 排他锁

    对请求中的所有表获取 MDL_EXCLUSIVE 锁，使用 MDL_EXPLICIT 持续时间，
    以便在闪回完成后手动释放。

    获取此锁后，其他会话的 DDL 操作将被阻塞，防止闪回期间表结构变化。

    @param request 闪回请求参数

    @retval true  加锁失败（超时、被杀或内存不足）
    @retval false 加锁成功
  */
  bool acquire_exclusive_lock(const FlashbackRequest &request);

  /**
    释放之前获取的排他锁

    逐个释放 acquire_exclusive_lock() 获取的所有 MDL ticket。
    如果锁已被释放或从未获取，此函数是安全的（no-op）。
  */
  void release_exclusive_lock();

  /**
    检查是否已持有排他锁

    @retval true  已持有锁
    @retval false 未持有锁
  */
  bool has_lock() const { return m_locked; }

 private:
  /** 检查单个 DDL event 是否与闪回兼容 */
  bool is_ddl_compatible(const char *ddl_sql) const;

  /** 当前线程上下文 */
  THD *m_thd;

  /** 是否已持有排他锁 */
  bool m_locked{false};

  /** 获取的 MDL ticket 数组（由调用者管理生命周期） */
  MDL_ticket **m_tickets{nullptr};

  /** ticket 数组大小 */
  uint32_t m_ticket_count{0};
};

}  // namespace flashback

#endif /* FLASHBACK_DDL_BARRIER_INCLUDED */
