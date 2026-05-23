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

/** @file flashback/flashback_space_monitor.cc
 * Flashback Space Monitor — Undo Tablespace 空间监控实现
 */

#include "flashback_space_monitor.h"
#include "srv0srv.h"
#include "ut0ut.h"

/** 空间压力警告阈值 */
static constexpr double PRESSURE_WARNING_THRESHOLD = 0.5;

/** 空间压力危险阈值 */
static constexpr double PRESSURE_CRITICAL_THRESHOLD = 0.8;

/** 构造函数 */
FlashbackSpaceMonitor::FlashbackSpaceMonitor() {
    /* 初始化时重置压力比率为 0 */
    m_pressure_ratio.store(0.0, std::memory_order_relaxed);
}

/** 析构函数 */
FlashbackSpaceMonitor::~FlashbackSpaceMonitor() = default;

/** 获取当前空间使用统计
 * @return 空间统计信息 */
FlashbackSpaceMonitor::SpaceStats FlashbackSpaceMonitor::get_stats() const {
    SpaceStats stats{};

    /* TODO: 实际实现中应查询 Undo Tablespace 的实际使用量
     * 可通过 fil_space_get_size() 和 fil_space_get_free_pages() 获取 */

    return stats;
}

/** 获取当前空间压力等级
 * @return 压力等级 */
FlashbackSpaceMonitor::PressureLevel
FlashbackSpaceMonitor::get_pressure_level() const {
    double ratio = m_pressure_ratio.load(std::memory_order_relaxed);

    if (ratio >= PRESSURE_CRITICAL_THRESHOLD) {
        return PressureLevel::CRITICAL;
    } else if (ratio >= PRESSURE_WARNING_THRESHOLD) {
        return PressureLevel::WARNING;
    }
    return PressureLevel::NORMAL;
}

/** 获取空间压力比率 (0.0-1.0)
 * @return 压力比率 */
double FlashbackSpaceMonitor::get_pressure_ratio() const {
    return m_pressure_ratio.load(std::memory_order_relaxed);
}

/** 检查是否接近空间上限
 * @return true 如果空间使用率超过阈值 */
bool FlashbackSpaceMonitor::is_near_capacity() const {
    return get_pressure_level() == PressureLevel::CRITICAL;
}
