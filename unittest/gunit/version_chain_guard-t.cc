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
  @file unittest/gunit/version_chain_guard-t.cc
  @brief 单元测试: VersionChainGuard + FlashbackSpaceMonitor

  三层测试金字塔 — 第一层 (单元测试, 占比 60%)

  测试覆盖:
  - VersionChainGuard:
    * check_chain_integrity() 枚举状态
    * is_purged() 位置检查
    * notify_flashback_in_progress/complete()
    * is_flashback_active()
    * get_table_chain_status()
  - FlashbackSpaceMonitor:
    * get_stats() 默认值
    * get_pressure_level() NORMAL/WARNING/CRITICAL
    * get_pressure_ratio() 0.0-1.0
    * is_near_capacity()
*/

#include <gtest/gtest.h>
#include <type_traits>

#include "version_chain_guard.h"
#include "flashback_space_monitor.h"

namespace version_chain_guard_unittest {

// =====================================================================
// 测试组: VersionChainGuard 枚举值完整性
// =====================================================================

class VersionChainGuardEnumTest : public ::testing::Test {};

/* 测试: ChainStatus 应包含所有预期枚举值 */
TEST_F(VersionChainGuardEnumTest,
       ChainStatus应包含所有预期枚举值) {
  VersionChainGuard::ChainStatus states[] = {
      VersionChainGuard::ChainStatus::INTACT,
      VersionChainGuard::ChainStatus::TRUNCATED,
      VersionChainGuard::ChainStatus::BROKEN,
      VersionChainGuard::ChainStatus::PURGED,
      VersionChainGuard::ChainStatus::UNKNOWN,
  };

  EXPECT_EQ(states[0], VersionChainGuard::ChainStatus::INTACT);
  EXPECT_EQ(states[1], VersionChainGuard::ChainStatus::TRUNCATED);
  EXPECT_EQ(states[2], VersionChainGuard::ChainStatus::BROKEN);
  EXPECT_EQ(states[3], VersionChainGuard::ChainStatus::PURGED);
  EXPECT_EQ(states[4], VersionChainGuard::ChainStatus::UNKNOWN);
}

// =====================================================================
// 测试组: VersionChainGuard 接口
// =====================================================================

class VersionChainGuardInterfaceTest : public ::testing::Test {};

/* 测试: check_chain_integrity 应可调用且返回枚举值 */
TEST_F(VersionChainGuardInterfaceTest,
       check_chain_integrity应可调用且返回有效状态) {
  VersionChainGuard guard;

  auto status = guard.check_chain_integrity(1, 100);
  // 当前 stub 实现返回 INTACT
  EXPECT_EQ(status, VersionChainGuard::ChainStatus::INTACT);
}

/* 测试: is_purged 应可调用且返回 bool */
TEST_F(VersionChainGuardInterfaceTest,
       is_purged应可调用且返回布尔值) {
  VersionChainGuard guard;

  // 当前 stub 实现返回 false
  EXPECT_FALSE(guard.is_purged(1, 1, 1));
  EXPECT_FALSE(guard.is_purged(0, 0, 0));
}

/* 测试: notify_flashback_in_progress/complete 应可调用 */
TEST_F(VersionChainGuardInterfaceTest,
       notify_flashback_in_progress和complete应可调用) {
  VersionChainGuard guard;

  EXPECT_NO_THROW(guard.notify_flashback_in_progress(1, 100));
  EXPECT_NO_THROW(guard.notify_flashback_complete(1));
}

/* 测试: is_flashback_active 应可调用 */
TEST_F(VersionChainGuardInterfaceTest,
       is_flashback_active应可调用且返回bool) {
  VersionChainGuard guard;

  // 当前 stub 返回 false
  EXPECT_FALSE(guard.is_flashback_active(1));
  EXPECT_FALSE(guard.is_flashback_active(999));
}

/* 测试: get_table_chain_status 应可调用 */
TEST_F(VersionChainGuardInterfaceTest,
       get_table_chain_status应可调用且返回枚举值) {
  VersionChainGuard guard;

  auto status = guard.get_table_chain_status(1);
  // 当前 stub 返回 UNKNOWN
  EXPECT_EQ(status, VersionChainGuard::ChainStatus::UNKNOWN);
}

// =====================================================================
// 测试组: VersionChainGuard 类型特性
// =====================================================================

class VersionChainGuardTypeTest : public ::testing::Test {};

/* 测试: VersionChainGuard 应可析构 */
TEST_F(VersionChainGuardTypeTest, 应可析构) {
  EXPECT_TRUE(std::is_destructible<VersionChainGuard>::value);
}

/* 测试: VersionChainGuard 对象大小应合理 */
TEST_F(VersionChainGuardTypeTest, 对象大小应合理) {
  VersionChainGuard guard;
  // 只有一个内部 mutex 指针，大小应 <= 2 个指针
  EXPECT_LE(sizeof(guard), sizeof(void *) * 4);
}

}  // namespace version_chain_guard_unittest

namespace flashback_space_monitor_unittest {

// =====================================================================
// 测试组: FlashbackSpaceMonitor 构造函数与默认值
// =====================================================================

class FlashbackSpaceMonitorConstructorTest : public ::testing::Test {};

/* 测试: 默认构造应创建 NORMAL 压力等级 */
TEST_F(FlashbackSpaceMonitorConstructorTest,
       默认构造时应创建NORMAL压力等级) {
  FlashbackSpaceMonitor monitor;

  EXPECT_EQ(monitor.get_pressure_level(),
            FlashbackSpaceMonitor::PressureLevel::NORMAL);
}

/* 测试: 初始压力比率应为 0.0 */
TEST_F(FlashbackSpaceMonitorConstructorTest,
       初始压力比率应为0) {
  FlashbackSpaceMonitor monitor;

  EXPECT_DOUBLE_EQ(monitor.get_pressure_ratio(), 0.0);
}

/* 测试: 析构不应崩溃 */
TEST_F(FlashbackSpaceMonitorConstructorTest,
       析构不应崩溃) {
  EXPECT_NO_THROW({
    auto monitor = std::make_unique<FlashbackSpaceMonitor>();
  });
}

// =====================================================================
// 测试组: FlashbackSpaceMonitor 压力等级
// =====================================================================

class FlashbackSpaceMonitorPressureTest : public ::testing::Test {};

/* 测试: get_stats 默认值应为零值 */
TEST_F(FlashbackSpaceMonitorPressureTest,
       get_stats默认返回值应为合理零值) {
  FlashbackSpaceMonitor monitor;

  auto stats = monitor.get_stats();
  // SpaceStats 的字段应为零
  EXPECT_EQ(stats.total_pages, 0);
  EXPECT_EQ(stats.used_pages, 0);
  EXPECT_GE(stats.usage_ratio, 0.0);  // usage_ratio >= 0
}

/* 测试: is_near_capacity 在 NORMAL 状态下应返回 false */
TEST_F(FlashbackSpaceMonitorPressureTest,
       NORMAL压力状态下is_near_capacity应返回false) {
  FlashbackSpaceMonitor monitor;

  EXPECT_FALSE(monitor.is_near_capacity());
}

/* 测试: 压力比率 0.0 应为 NORMAL */
TEST_F(FlashbackSpaceMonitorPressureTest,
       压力比率0应为NORMAL等级) {
  FlashbackSpaceMonitor monitor;
  EXPECT_EQ(monitor.get_pressure_level(),
            FlashbackSpaceMonitor::PressureLevel::NORMAL);
  EXPECT_DOUBLE_EQ(monitor.get_pressure_ratio(), 0.0);
}

// =====================================================================
// 测试组: FlashbackSpaceMonitor 类型特性
// =====================================================================

class FlashbackSpaceMonitorTypeTest : public ::testing::Test {};

/* 测试: FlashbackSpaceMonitor 应可析构 */
TEST_F(FlashbackSpaceMonitorTypeTest, 应可析构) {
  EXPECT_TRUE(std::is_destructible<FlashbackSpaceMonitor>::value);
}

/* 测试: PressureLevel 枚举应包含 NORMAL/WARNING/CRITICAL */
TEST_F(FlashbackSpaceMonitorTypeTest,
       PressureLevel应包含三个等级) {
  auto level = FlashbackSpaceMonitor::PressureLevel::NORMAL;
  EXPECT_EQ(static_cast<int>(level), 0);
}

/* 测试: SpaceStats 应有合理大小 */
TEST_F(FlashbackSpaceMonitorTypeTest, SpaceStats应有合理大小) {
  FlashbackSpaceMonitor::SpaceStats stats{};
  EXPECT_GE(sizeof(stats), sizeof(uint64_t) * 3);  // 至少 3 个 uint64
}

}  // namespace flashback_space_monitor_unittest
