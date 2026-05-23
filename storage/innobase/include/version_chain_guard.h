/*****************************************************************************

Copyright (c) 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and your
derivative works with the separately licensed software that they have included
with the program.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/version_chain_guard.h
 * Version Chain Guard — 版本链完整性保护器
 *
 * 职责:
 * 1. 检测版本链是否完整
 * 2. 防止在遍历时版本链被 Purge 打断
 * 3. 提供版本链断裂的优雅处理
 */

#pragma once

#include "univ.i"
#include "dict0types.h"
#include "trx0types.h"
#include <set>
#include <utility>

/** 版本链完整性保护器
 *
 * 保护闪回查询时的版本链完整性，防止在遍历过程中版本链被 Purge 打断。
 */
class VersionChainGuard {
public:
    /** 版本链状态 */
    enum class ChainStatus {
        INTACT,     /**< 版本链完整 */
        TRUNCATED,  /**< 头部已被 truncate */
        BROKEN,     /**< 中间断裂 */
        PURGED,     /**< 目标版本已被 purge */
        UNKNOWN     /**< 状态未知 */
    };

    /** 位置标识 (space, page, offset) */
    using Position = std::pair<uint32_t, uint32_t>;

    /** 构造函数 */
    VersionChainGuard();

    /** 析构函数 */
    ~VersionChainGuard();

    /** 创建版本链遍历上下文
     * @param table_id 表 ID
     * @param target_trx_id 目标事务 ID
     * @return 遍历上下文指针 */
    static ChainStatus check_chain_integrity(table_id_t table_id,
                                             trx_id_t target_trx_id);

    /** 检查特定位置是否已被 purge
     * @param space 表空间 ID
     * @param page 页号
     * @param offset 偏移
     * @return true 如果已被 purge */
    static bool is_purged(uint32_t space, uint32_t page, uint32_t offset);

    /** 通知 Purge 线程某个表正在被闪回查询
     * @param table_id 表 ID
     * @param oldest_trx_id 最老需要保留的事务 ID */
    static void notify_flashback_in_progress(table_id_t table_id,
                                             trx_id_t oldest_trx_id);

    /** 通知闪回查询完成
     * @param table_id 表 ID */
    static void notify_flashback_complete(table_id_t table_id);

    /** 检查表是否有正在进行的闪回查询
     * @param table_id 表 ID
     * @return true 有进行中的闪回 */
    static bool is_flashback_active(table_id_t table_id);

    /** 获取表的版本链状态
     * @param table_id 表 ID
     * @return 版本链状态 */
    static ChainStatus get_table_chain_status(table_id_t table_id);

private:
    /** 内部 mutex */
    mutable ib_mutex_t m_mutex;
};
