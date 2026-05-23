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

/** @file include/undo_timestamp_mapper.h
 * Undo Timestamp Mapper — 时间戳 ↔ trx_id 映射
 *
 * 职责:
 * 1. 维护 trx_id ↔ timestamp 的近似映射
 * 2. 支持按时间戳查找对应的事务边界
 * 3. 估算给定时间戳的 undo 是否已过期
 */

#pragma once

#include "univ.i"
#include "my_time.h"
#include "trx0types.h"
#include <atomic>
#include <map>

/** Undo 时间戳映射器
 *
 * 维护事务 ID 与提交时间戳的映射关系，支持按时间查询事务边界。
 */
class UndoTimestampMapper {
public:
    /** 单条映射记录 */
    struct MappingEntry {
        trx_id_t trx_id;          /**< 事务 ID */
        my_time_t commit_timestamp; /**< 提交时间戳 */
        int64_t inserted_at;      /**< 插入时间 (用于 LRU 淘汰) */
    };

    /** 查询结果 */
    struct QueryResult {
        trx_id_t trx_id;           /**< 估算的事务 ID */
        my_time_t timestamp;       /**< 对应的时间戳 */
        bool is_exact;             /**< 是否精确匹配 */
        my_time_t oldest_undo_ts;  /**< 最老可用时间戳 */
        bool is_within_window;     /**< 是否在闪回窗口内 */
    };

    /** 缓存统计 */
    struct CacheStats {
        size_t cache_size;
        size_t hit_count;
        size_t miss_count;
        double hit_rate;
        my_time_t oldest_entry;
        my_time_t newest_entry;
    };

    /** 构造函数
     * @param cache_size 缓存大小 (默认 10000) */
    explicit UndoTimestampMapper(size_t cache_size = 10000);

    /** 析构函数 */
    ~UndoTimestampMapper();

    /** 注册事务提交时间 (事务提交时调用)
     * @param trx_id 事务 ID
     * @param commit_timestamp 提交时间戳 */
    void register_commit(trx_id_t trx_id, my_time_t commit_timestamp);

    /** 查询目标时间戳对应的 trx_id 边界
     * @param target_timestamp 目标时间戳
     * @return QueryResult 查询结果 */
    QueryResult query_trx_id_at_timestamp(my_time_t target_timestamp) const;

    /** 获取最老的可用 Undo 时间戳
     * @return 最老可用时间戳, 0 表示无记录 */
    my_time_t get_oldest_available_timestamp() const;

    /** 检查目标时间是否在窗口内
     * @param target_timestamp 目标时间戳
     * @return true 在窗口内 */
    bool is_timestamp_available(my_time_t target_timestamp) const;

    /** 批量注册 (用于启动时加载)
     * @param entries 映射记录列表 */
    void register_commits_batch(const std::vector<MappingEntry> &entries);

    /** 获取缓存统计
     * @return CacheStats 缓存统计 */
    CacheStats get_cache_stats() const;

    /** 获取当前映射条目数
     * @return 条目数 */
    size_t size() const;

private:
    /** 内部: 二分查找目标时间戳
     * @param target 目标时间戳
     * @return 索引位置 */
    int binary_search(my_time_t target) const;

    /** 内部 mutex */
    mutable ib_mutex_t m_mutex;

    /** 有序映射 (按 trx_id 升序) */
    std::map<trx_id_t, MappingEntry> m_mapping;

    /** 最大缓存大小 */
    size_t m_max_cache_size;

    /** 统计 */
    mutable std::atomic<size_t> m_hits{0};
    mutable std::atomic<size_t> m_misses{0};
};
