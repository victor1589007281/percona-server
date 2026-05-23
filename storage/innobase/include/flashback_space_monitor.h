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

/** @file include/flashback_space_monitor.h
 * Flashback Space Monitor — Undo Tablespace 空间监控
 *
 * 职责:
 * 1. 监控 Undo Tablespace 的空间使用率
 * 2. 预测空间不足风险
 * 3. 提供空间压力等级评估
 */

#pragma once

#include "univ.i"
#include "fil0fil.h"
#include <atomic>

/** Undo 表空间空间监控器
 *
 * 监控 Undo Tablespace 的空间使用率，预测空间不足风险，
 * 并为 Purge Hold 调度器提供空间压力等级。
 */
class FlashbackSpaceMonitor {
public:
    /** 空间压力等级 */
    enum class PressureLevel {
        NORMAL = 0,    /* 0.0-0.5: 正常 */
        WARNING = 1,   /* 0.5-0.8: 警告 */
        CRITICAL = 2   /* 0.8-1.0: 危险 */
    };

    /** 表空间统计信息 */
    struct SpaceStats {
        uint64_t total_pages;      /* 总页数 */
        uint64_t used_pages;       /* 已使用页数 */
        double usage_ratio;        /* 使用率 (0.0-1.0) */
        uint64_t free_pages;       /* 空闲页数 */
    };

    /** 构造函数 */
    FlashbackSpaceMonitor();

    /** 析构函数 */
    ~FlashbackSpaceMonitor();

    /** 获取当前空间使用统计
     * @return 空间统计信息 */
    SpaceStats get_stats() const;

    /** 获取当前空间压力等级
     * @return 压力等级 */
    PressureLevel get_pressure_level() const;

    /** 获取空间压力比率 (0.0-1.0)
     * @return 压力比率 */
    double get_pressure_ratio() const;

    /** 检查是否接近空间上限
     * @return true 如果空间使用率超过阈值 */
    bool is_near_capacity() const;

private:
    /** 内部 mutex */
    mutable ib_mutex_t m_mutex;

    /** 空间压力比率 (0.0-1.0) */
    std::atomic<double> m_pressure_ratio{0.0};
};
