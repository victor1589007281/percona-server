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

/** @file include/purge_hold_scheduler.h
 * Purge Hold Scheduler — 时间维度 Purge 延迟调度
 *
 * 职责:
 * 1. 维护时间维度的 Purge Hold 队列
 * 2. 根据 hold 列表动态调整 Purge 延迟
 * 3. 防止 Undo Tablespace 膨胀
 * 4. 与 FlashbackViewManager 协同工作
 */

#pragma once

#include "univ.i"
#include "my_time.h"
#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <vector>

/** 前向声明 */
using table_id_t = uint64_t;

/** Purge 延迟调度器
 *
 * 根据 Hold 请求队列动态决定 Purge 线程是否应延迟或暂停。
 */
class PurgeHoldScheduler {
public:
    /** Hold 请求优先级 */
    enum class Priority : int {
        LOW = 0,      /**< 常规延迟 */
        NORMAL = 1,   /**< 默认优先级 */
        HIGH = 2,     /**< 高优先级 (金融场景) */
        CRITICAL = 3  /**< 关键操作 (正在进行的闪回) */
    };

    /** Hold 请求结构 */
    struct HoldRequest {
        uint64_t request_id;                    /**< 唯一标识 */
        my_time_t hold_until;                   /**< 保留截止时间 */
        Priority priority;                      /**< 优先级 */
        int64_t created_at;                     /**< 创建时间 (epoch ms) */
        bool is_flashback;                      /**< 是否为闪回操作 */
        std::vector<table_id_t> affected_tables; /**< 受影响的表 */
    };

    /** 调度结果 */
    struct ScheduleResult {
        uint64_t estimated_delay_us;  /**< 估算的 Purge 延迟 (微秒) */
        bool should_pause;            /**< 是否应暂停 Purge */
        my_time_t oldest_hold_until;  /**< 最老的 Hold 截止时间 */
        size_t active_holds;          /**< 当前 Hold 请求数 */
    };

    /** 构造函数 */
    PurgeHoldScheduler();

    /** 析构函数 */
    ~PurgeHoldScheduler();

    /** 添加 Hold 请求
     * @param[in] hold_until 保留截止时间
     * @param[in] priority 优先级
     * @param[in] is_flashback 是否为闪回操作
     * @param[in] affected_tables 受影响的表 (可选)
     * @return 请求 ID, 0 表示失败 */
    [[nodiscard]] uint64_t add_hold(
        my_time_t hold_until,
        Priority priority = Priority::NORMAL,
        bool is_flashback = false,
        const std::vector<table_id_t> &affected_tables = {});

    /** 移除 Hold 请求
     * @param request_id 请求 ID
     * @return true 成功, false 请求不存在 */
    bool remove_hold(uint64_t request_id);

    /** 查询调度决策 (Purge 线程调用)
     * @param[in] batch_size 当前批次大小
     * @return ScheduleResult 调度结果 */
    ScheduleResult query_schedule(uint64_t batch_size) const;

    /** 检查特定表的 Hold 状态
     * @param table_id 表 ID
     * @return true 表被 Hold, false 表未被 Hold */
    bool is_table_held(table_id_t table_id) const;

    /** 获取当前 Hold 请求数
     * @return 活跃 Hold 请求数 */
    size_t active_hold_count() const;

private:
    /** 内部 mutex */
    mutable std::mutex m_mutex;

    /** Hold 请求队列 (按 hold_until 排序) */
    std::map<uint64_t, HoldRequest> m_holds;

    /** 下一个请求 ID */
    std::atomic<uint64_t> m_next_id{1};
};
