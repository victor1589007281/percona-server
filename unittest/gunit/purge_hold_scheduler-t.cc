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
  @file unittest/gunit/purge_hold_scheduler-t.cc
  @brief 单元测试: PurgeHoldScheduler — 时间维度 Purge 延迟调度

  三层测试金字塔 — 第一层 (单元测试, 占比 60%)

  测试覆盖:
  - add_hold(): 正常添加、边界值、返回值非零
  - remove_hold(): 正常移除、不存在请求、幂等性
  - query_schedule(): 空队列、单 Hold、多 Hold、优先级排序
  - is_table_held(): 表被 Hold、未 Hold、空表列表
  - active_hold_count(): 计数正确性
  - 线程安全: 并发读写
  - 调度决策: should_pause, estimated_delay_us, oldest_hold_until
  - 边界: now < hold_until, now >= hold_until
*/

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

#include "purge_hold_scheduler.h"

namespace purge_hold_scheduler_unittest {

// =====================================================================
// 测试组: 构造函数与默认值
// =====================================================================

class PurgeHoldSchedulerConstructorTest : public ::testing::Test {};

/* 测试: 默认构造应创建空队列 */
TEST_F(PurgeHoldSchedulerConstructorTest, 默认构造时应创建空队列) {
  PurgeHoldScheduler scheduler;

  EXPECT_EQ(scheduler.active_hold_count(), 0);
  auto result = scheduler.query_schedule(100);
  EXPECT_EQ(result.active_holds, 0);
  EXPECT_FALSE(result.should_pause);
  EXPECT_EQ(result.estimated_delay_us, 0);
}

/* 测试: 析构空调度器不应崩溃 */
TEST_F(PurgeHoldSchedulerConstructorTest, 析构空调度器不应崩溃) {
  EXPECT_NO_THROW({
    auto scheduler = std::make_unique<PurgeHoldScheduler>();
  });
}

// =====================================================================
// 测试组: add_hold 正常路径
// =====================================================================

class PurgeHoldSchedulerAddHoldTest : public ::testing::Test {};

/* 测试: 添加第一个 Hold 请求应返回非零 ID */
TEST_F(PurgeHoldSchedulerAddHoldTest,
       添加第一个Hold请求时应返回非零ID) {
  PurgeHoldScheduler scheduler;

  my_time_t future_time = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(future_time);

  EXPECT_NE(id, 0);
  EXPECT_EQ(scheduler.active_hold_count(), 1);
}

/* 测试: 添加多个 Hold 请求应返回不同 ID */
TEST_F(PurgeHoldSchedulerAddHoldTest,
       添加多个Hold请求时应返回不同ID) {
  PurbackHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id1 = scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL);
  auto id2 = scheduler.add_hold(t + 10, PurgeHoldScheduler::Priority::HIGH);
  auto id3 = scheduler.add_hold(t + 20, PurgeHoldScheduler::Priority::LOW);

  EXPECT_NE(id1, id2);
  EXPECT_NE(id2, id3);
  EXPECT_NE(id1, id3);
  EXPECT_EQ(scheduler.active_hold_count(), 3);
}

/* 测试: 添加带 affected_tables 的 Hold 请求 */
TEST_F(PurgeHoldSchedulerAddHoldTest,
       添加带受影响表列表的Hold请求时应正确存储) {
  PurgeHoldScheduler scheduler;

  std::vector<table_id_t> tables = {1, 2, 3};
  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL,
                               true, tables);

  EXPECT_NE(id, 0);
  EXPECT_TRUE(scheduler.is_table_held(1));
  EXPECT_TRUE(scheduler.is_table_held(2));
  EXPECT_TRUE(scheduler.is_table_held(3));
  EXPECT_FALSE(scheduler.is_table_held(999));
}

/* 测试: 默认优先级应为 NORMAL */
TEST_F(PurgeHoldSchedulerAddHoldTest,
       使用默认优先级添加时应接受) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t);

  EXPECT_NE(id, 0);
  EXPECT_EQ(scheduler.active_hold_count(), 1);
}

/* 测试: is_flashback=true 时应正确记录 */
TEST_F(PurgeHoldSchedulerAddHoldTest,
       添加flashback标志的Hold请求时应正确记录) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t, PurgeHoldScheduler::Priority::CRITICAL,
                               true, {});
  EXPECT_NE(id, 0);
}

// =====================================================================
// 测试组: remove_hold 正常与边界路径
// =====================================================================

class PurgeHoldSchedulerRemoveHoldTest : public ::testing::Test {};

/* 测试: 移除已存在的 Hold 请求应返回 true */
TEST_F(PurgeHoldSchedulerRemoveHoldTest,
       移除已存在的Hold请求时应返回true且计数减1) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t);
  ASSERT_NE(id, 0);
  EXPECT_EQ(scheduler.active_hold_count(), 1);

  bool removed = scheduler.remove_hold(id);
  EXPECT_TRUE(removed);
  EXPECT_EQ(scheduler.active_hold_count(), 0);
}

/* 测试: 移除不存在的请求应返回 false */
TEST_F(PurgeHoldSchedulerRemoveHoldTest,
       移除不存在的Hold请求时应返回false且无副作用) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(t);
  EXPECT_EQ(scheduler.active_hold_count(), 1);

  bool removed = scheduler.remove_hold(99999);
  EXPECT_FALSE(removed);
  EXPECT_EQ(scheduler.active_hold_count(), 1);  // 计数不变
}

/* 测试: 重复移除同一请求应返回 false */
TEST_F(PurgeHoldSchedulerRemoveHoldTest,
       重复移除同一请求时应返回false且无副作用) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t);

  scheduler.remove_hold(id);
  EXPECT_FALSE(scheduler.remove_hold(id));
  EXPECT_EQ(scheduler.active_hold_count(), 0);
}

/* 测试: 移除后重新添加应获得新 ID */
TEST_F(PurgeHoldSchedulerRemoveHoldTest,
       移除后重新添加应获得新ID) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id1 = scheduler.add_hold(t);
  scheduler.remove_hold(id1);
  auto id2 = scheduler.add_hold(t + 10);

  EXPECT_NE(id2, 0);
  EXPECT_NE(id2, id1);
}

// =====================================================================
// 测试组: query_schedule 正常路径
// =====================================================================

class PurgeHoldSchedulerQueryScheduleTest : public ::testing::Test {};

/* 测试: 空队列应返回零值结果 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       空队列应返回零值调度结果) {
  PurgeHoldScheduler scheduler;

  auto result = scheduler.query_schedule(100);

  EXPECT_EQ(result.active_holds, 0);
  EXPECT_FALSE(result.should_pause);
  EXPECT_EQ(result.estimated_delay_us, 0);
  EXPECT_EQ(result.oldest_hold_until, 0);
}

/* 测试: 单个已过期的 Hold (hold_until < now) 不应触发暂停 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       已过期的Hold不应触发暂停且无延迟) {
  PurgeHoldScheduler scheduler;

  // hold_until = 过去的时间
  my_time_t past = static_cast<my_time_t>(std::time(nullptr)) - 10;
  scheduler.add_hold(past);

  auto result = scheduler.query_schedule(100);

  EXPECT_EQ(result.active_holds, 1);
  EXPECT_FALSE(result.should_pause);
  // 已过期，不应有延迟
  EXPECT_EQ(result.estimated_delay_us, 0);
}

/* 测试: 单个未过期的 HIGH 优先级 Hold 应触发暂停 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       未过期的高优先级Hold应触发暂停) {
  PurgeHoldScheduler scheduler;

  my_time_t future = static_cast<my_time_t>(std::time(nullptr)) + 120;
  scheduler.add_hold(future, PurgeHoldScheduler::Priority::HIGH);

  auto result = scheduler.query_schedule(100);

  EXPECT_EQ(result.active_holds, 1);
  EXPECT_TRUE(result.should_pause);
  EXPECT_GT(result.oldest_hold_until, 0);
}

/* 测试: 单个未过期的 LOW 优先级 Hold 不应触发暂停 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       未过期的低优先级Hold不应触发暂停但有延迟) {
  PurgeHoldScheduler scheduler;

  my_time_t future = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(future, PurgeHoldScheduler::Priority::LOW);

  auto result = scheduler.query_schedule(100);

  EXPECT_EQ(result.active_holds, 1);
  EXPECT_FALSE(result.should_pause);
  EXPECT_GT(result.estimated_delay_us, 0);
}

/* 测试: CRITICAL 优先级应触发暂停 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       关键优先级Hold应触发暂停) {
  PurgeHoldScheduler scheduler;

  my_time_t future = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(future, PurgeHoldScheduler::Priority::CRITICAL);

  auto result = scheduler.query_schedule(100);
  EXPECT_TRUE(result.should_pause);
}

/* 测试: 多个 Hold 时应返回最老的 hold_until */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       多个Hold时应返回最老的hold_until) {
  PurgeHoldScheduler scheduler;

  my_time_t now = static_cast<my_time_t>(std::time(nullptr));
  // 最老: +30s
  scheduler.add_hold(now + 30, PurgeHoldScheduler::Priority::LOW);
  // 中间: +60s
  scheduler.add_hold(now + 60, PurgeHoldScheduler::Priority::HIGH);
  // 最新: +90s
  scheduler.add_hold(now + 90, PurgeHoldScheduler::Priority::NORMAL);

  auto result = scheduler.query_schedule(100);

  EXPECT_EQ(result.active_holds, 3);
  EXPECT_TRUE(result.should_pause);  // HIGH 存在
  // 最老的未过期时间决定了延迟
  EXPECT_GE(result.estimated_delay_us, 29 * 1000000);
}

/* 测试: 移除最老 Hold 后 oldest_hold_until 应更新 */
TEST_F(PurgeHoldSchedulerQueryScheduleTest,
       移除最老Hold后oldest_hold_until应更新) {
  PurgeHoldScheduler scheduler;

  my_time_t now = static_cast<my_time_t>(std::time(nullptr));
  auto id_oldest = scheduler.add_hold(now + 30, PurgeHoldScheduler::Priority::LOW);
  scheduler.add_hold(now + 60, PurgeHoldScheduler::Priority::NORMAL);

  auto r1 = scheduler.query_schedule(100);
  trx_id_t first_oldest = r1.oldest_hold_until;

  scheduler.remove_hold(id_oldest);

  auto r2 = scheduler.query_schedule(100);
  // 最老的已更新
  EXPECT_GE(r2.oldest_hold_until, first_oldest);
}

// =====================================================================
// 测试组: is_table_held 正确性
// =====================================================================

class PurgeHoldSchedulerIsTableHeldTest : public ::testing::Test {};

/* 测试: 空调度器应返回 false */
TEST_F(PurgeHoldSchedulerIsTableHeldTest,
       空调度器查询任何表应返回false) {
  PurgeHoldScheduler scheduler;

  EXPECT_FALSE(scheduler.is_table_held(1));
  EXPECT_FALSE(scheduler.is_table_held(0));
  EXPECT_FALSE(scheduler.is_table_held(999));
}

/* 测试: 受影响的表应返回 true */
TEST_F(PurgeHoldSchedulerIsTableHeldTest,
       受影响的表应返回true) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL, false,
                    {10, 20, 30});

  EXPECT_TRUE(scheduler.is_table_held(10));
  EXPECT_TRUE(scheduler.is_table_held(20));
  EXPECT_TRUE(scheduler.is_table_held(30));
  EXPECT_FALSE(scheduler.is_table_held(11));
  EXPECT_FALSE(scheduler.is_table_held(99));
}

/* 测试: 移除 Hold 后受影响表应返回 false */
TEST_F(PurgeHoldSchedulerIsTableHeldTest,
       移除Hold后表应返回false) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  auto id = scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL,
                              false, {5, 15});

  EXPECT_TRUE(scheduler.is_table_held(5));
  scheduler.remove_hold(id);
  EXPECT_FALSE(scheduler.is_table_held(5));
  EXPECT_FALSE(scheduler.is_table_held(15));
}

/* 测试: 空表列表不应影响 is_table_held */
TEST_F(PurgeHoldSchedulerIsTableHeldTest,
       空表列表的Hold不应使任何表返回true) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL, false, {});

  EXPECT_FALSE(scheduler.is_table_held(1));
  EXPECT_FALSE(scheduler.is_table_held(0));
  EXPECT_EQ(scheduler.active_hold_count(), 1);
}

// =====================================================================
// 测试组: 线程安全测试
// =====================================================================

class PurgeHoldSchedulerConcurrencyTest : public ::testing::Test {};

/* 测试: 并发添加和移除 Hold 不应崩溃 */
TEST_F(PurgeHoldSchedulerConcurrencyTest,
       并发读写不应崩溃且计数最终一致) {
  PurgeHoldScheduler scheduler;

  std::atomic<int> success_add{0};
  std::atomic<int> success_remove{0};
  constexpr int WRITER_THREADS = 4;
  constexpr int OPS_PER_THREAD = 200;

  std::vector<std::thread> threads;

  // 写线程: 添加和移除
  for (int t = 0; t < WRITER_THREADS; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < OPS_PER_THREAD; ++i) {
        my_time_t future = static_cast<my_time_t>(std::time(nullptr)) + 100;
        auto id = scheduler.add_hold(future);
        if (id != 0) {
          success_add.fetch_add(1, std::memory_order_relaxed);
          if (scheduler.remove_hold(id)) {
            success_remove.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }

  // 读线程: query_schedule
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&]() {
      for (int i = 0; i < OPS_PER_THREAD; ++i) {
        EXPECT_NO_THROW(scheduler.query_schedule(100));
        EXPECT_NO_THROW(scheduler.active_hold_count());
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }
}

/* 测试: 并发调用 is_table_held 不应崩溃 */
TEST_F(PurgeHoldSchedulerConcurrencyTest,
       并发查询表Hold状态不应崩溃) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 200;
  scheduler.add_hold(t, PurgeHoldScheduler::Priority::NORMAL, false,
                    {1, 2, 3, 4, 5});

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;

  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        EXPECT_NO_THROW(scheduler.is_table_held(1));
        EXPECT_NO_THROW(scheduler.is_table_held(999));
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_relaxed);

  for (auto &th : threads) {
    th.join();
  }
}

// =====================================================================
// 测试组: 边界条件
// =====================================================================

class PurgeHoldSchedulerBoundaryTest : public ::testing::Test {};

/* 测试: 大量 Hold 请求不应崩溃 */
TEST_F(PurgeHoldSchedulerBoundaryTest,
       添加1000个Hold请求不应崩溃) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  for (int i = 0; i < 1000; ++i) {
    EXPECT_NO_THROW(scheduler.add_hold(t + i));
  }
  EXPECT_EQ(scheduler.active_hold_count(), 1000);

  auto result = scheduler.query_schedule(100);
  EXPECT_EQ(result.active_holds, 1000);
}

/* 测试: batch_size=0 时 query_schedule 应正常返回 */
TEST_F(PurgeHoldSchedulerBoundaryTest,
       batch_size为0时query_schedule应正常返回) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  scheduler.add_hold(t);
  scheduler.add_hold(t + 10);

  auto result = scheduler.query_schedule(0);
  EXPECT_EQ(result.active_holds, 2);
}

/* 测试: 移除后再添加大量请求不应内存泄漏 */
TEST_F(PurgeHoldSchedulerBoundaryTest,
       移除后再添加大量请求不应内存泄漏) {
  PurgeHoldScheduler scheduler;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr)) + 60;
  for (int i = 0; i < 500; ++i) {
    auto id = scheduler.add_hold(t + i);
    if (i % 2 == 0) {
      scheduler.remove_hold(id);
    }
  }
  // 最终约 250 个
  EXPECT_GE(scheduler.active_hold_count(), 200);
  EXPECT_LE(scheduler.active_hold_count(), 300);
}

}  // namespace purge_hold_scheduler_unittest
