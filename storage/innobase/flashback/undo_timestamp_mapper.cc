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

/** @file flashback/undo_timestamp_mapper.cc
 * Undo Timestamp Mapper — 时间戳 ↔ trx_id 映射实现
 */

#include "undo_timestamp_mapper.h"
#include "my_sys.h"
#include "my_systime.h"
#include "ut0ut.h"

/** 构造函数
 * @param cache_size 缓存大小 (默认 10000) */
UndoTimestampMapper::UndoTimestampMapper(size_t cache_size)
    : m_max_cache_size(cache_size > 0 ? cache_size : 10000) {}

/** 析构函数 */
UndoTimestampMapper::~UndoTimestampMapper() = default;

/** 注册事务提交时间 (事务提交时调用)
 * @param trx_id 事务 ID
 * @param commit_timestamp 提交时间戳 */
void UndoTimestampMapper::register_commit(trx_id_t trx_id,
                                          my_time_t commit_timestamp) {
    /* TODO: 加锁保护 */

    MappingEntry entry{};
    entry.trx_id = trx_id;
    entry.commit_timestamp = commit_timestamp;
    entry.inserted_at = my_micro_time();

    /* 如果缓存已满, 淘汰最老的条目 */
    if (m_mapping.size() >= m_max_cache_size) {
        m_mapping.erase(m_mapping.begin());
    }

    m_mapping[trx_id] = entry;
}

/** 查询目标时间戳对应的 trx_id 边界
 * @param target_timestamp 目标时间戳
 * @return QueryResult 查询结果 */
UndoTimestampMapper::QueryResult
UndoTimestampMapper::query_trx_id_at_timestamp(my_time_t target_timestamp) const {
    QueryResult result{};
    result.trx_id = 0;
    result.timestamp = target_timestamp;
    result.is_exact = false;
    result.oldest_undo_ts = get_oldest_available_timestamp();
    result.is_within_window = is_timestamp_available(target_timestamp);

    if (m_mapping.empty()) {
        return result;
    }

    /* 二分查找 >= target_timestamp 的最小记录 */
    auto it = m_mapping.lower_bound(0);
    for (; it != m_mapping.end(); ++it) {
        if (it->second.commit_timestamp >= target_timestamp) {
            result.trx_id = it->first;
            result.is_exact = (it->second.commit_timestamp == target_timestamp);
            break;
        }
    }

    return result;
}

/** 获取最老的可用 Undo 时间戳
 * @return 最老可用时间戳, 0 表示无记录 */
my_time_t UndoTimestampMapper::get_oldest_available_timestamp() const {
    if (m_mapping.empty()) {
        return 0;
    }
    return m_mapping.begin()->second.commit_timestamp;
}

/** 检查目标时间是否在窗口内
 * @param target_timestamp 目标时间戳
 * @return true 在窗口内 */
bool UndoTimestampMapper::is_timestamp_available(my_time_t target_timestamp) const {
    my_time_t oldest = get_oldest_available_timestamp();
    return oldest > 0 && target_timestamp >= oldest;
}

/** 批量注册 (用于启动时加载)
 * @param entries 映射记录列表 */
void UndoTimestampMapper::register_commits_batch(
    const std::vector<MappingEntry> &entries) {
    for (const auto &entry : entries) {
        if (m_mapping.size() >= m_max_cache_size) {
            m_mapping.erase(m_mapping.begin());
        }
        m_mapping[entry.trx_id] = entry;
    }
}

/** 获取缓存统计
 * @return CacheStats 缓存统计 */
UndoTimestampMapper::CacheStats UndoTimestampMapper::get_cache_stats() const {
    CacheStats stats{};
    stats.cache_size = m_mapping.size();
    stats.hit_count = m_hits.load(std::memory_order_relaxed);
    stats.miss_count = m_misses.load(std::memory_order_relaxed);

    size_t total = stats.hit_count + stats.miss_count;
    stats.hit_rate = total > 0 ? static_cast<double>(stats.hit_count) / total : 0.0;

    if (!m_mapping.empty()) {
        stats.oldest_entry = m_mapping.begin()->second.commit_timestamp;
        stats.newest_entry = m_mapping.rbegin()->second.commit_timestamp;
    }

    return stats;
}

/** 获取当前映射条目数
 * @return 条目数 */
size_t UndoTimestampMapper::size() const {
    return m_mapping.size();
}

/** 内部: 二分查找目标时间戳
 * @param target 目标时间戳
 * @return 索引位置 */
int UndoTimestampMapper::binary_search(my_time_t target) const {
    /* TODO: 实现二分查找优化 */
    return -1;
}
