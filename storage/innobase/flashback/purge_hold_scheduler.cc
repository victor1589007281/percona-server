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

/** @file flashback/purge_hold_scheduler.cc
 * Purge Hold Scheduler — 时间维度 Purge 延迟调度实现
 */

#include "purge_hold_scheduler.h"
#include "my_time.h"
#include "my_systime.h"
#include <ctime>

/** 构造函数 */
PurgeHoldScheduler::PurgeHoldScheduler() = default;

/** 析构函数 */
PurgeHoldScheduler::~PurgeHoldScheduler() = default;

/** 添加 Hold 请求
 * @param[in] hold_until 保留截止时间
 * @param[in] priority 优先级
 * @param[in] is_flashback 是否为闪回操作
 * @param[in] affected_tables 受影响的表
 * @return 请求 ID, 0 表示失败 */
uint64_t PurgeHoldScheduler::add_hold(
    my_time_t hold_until,
    Priority priority,
    bool is_flashback,
    const std::vector<table_id_t> &affected_tables) {
    std::lock_guard<std::mutex> lock(m_mutex);

    uint64_t id = m_next_id.fetch_add(1, std::memory_order_relaxed);

    HoldRequest req{};
    req.request_id = id;
    req.hold_until = hold_until;
    req.priority = priority;
    req.is_flashback = is_flashback;
    req.affected_tables = affected_tables;
    req.created_at = my_micro_time();

    m_holds[id] = req;

    return id;
}

/** 移除 Hold 请求
 * @param request_id 请求 ID
 * @return true 成功, false 请求不存在 */
bool PurgeHoldScheduler::remove_hold(uint64_t request_id) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_holds.erase(request_id) > 0;
}

/** 查询调度决策 (Purge 线程调用)
 * @param[in] batch_size 当前批次大小
 * @return ScheduleResult 调度结果 */
PurgeHoldScheduler::ScheduleResult
PurgeHoldScheduler::query_schedule(uint64_t batch_size) const {
    ScheduleResult result{};
    result.estimated_delay_us = 0;
    result.should_pause = false;
    result.oldest_hold_until = 0;
    result.active_holds = 0;

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_holds.empty()) {
        return result;
    }

    result.active_holds = m_holds.size();

    /* 找出最老的 Hold 截止时间 */
    my_time_t now = static_cast<my_time_t>(std::time(nullptr));
    my_time_t oldest = 0;
    bool should_pause = false;

    for (const auto &[id, req] : m_holds) {
        if (req.hold_until > now) {
            if (oldest == 0 || req.hold_until < oldest) {
                oldest = req.hold_until;
            }
            /* 高优先级或关键操作要求暂停 Purge */
            if (req.priority >= Priority::HIGH) {
                should_pause = true;
            }
        }
    }

    result.oldest_hold_until = oldest;
    result.should_pause = should_pause;

    /* 计算延迟: 距离最老 Hold 截止时间的差值 (微秒) */
    if (oldest > now) {
        result.estimated_delay_us = static_cast<uint64_t>(oldest - now) * 1000000;
    }

    return result;
}

/** 检查特定表的 Hold 状态
 * @param table_id 表 ID
 * @return true 表被 Hold, false 表未被 Hold */
bool PurgeHoldScheduler::is_table_held(table_id_t table_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (const auto &[id, req] : m_holds) {
        for (auto held_table : req.affected_tables) {
            if (held_table == table_id) {
                return true;
            }
        }
    }
    return false;
}

/** 获取当前 Hold 请求数
 * @return 活跃 Hold 请求数 */
size_t PurgeHoldScheduler::active_hold_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_holds.size();
}
