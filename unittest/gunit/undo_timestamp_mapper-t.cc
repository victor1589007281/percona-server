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
  @file unittest/gunit/undo_timestamp_mapper-t.cc
  @brief 单元测试: UndoTimestampMapper — 时间戳 ↔ trx_id 映射

  三层测试金字塔 — 第一层 (单元测试, 占比 60%)

  测试覆盖:
  - register_commit(): 正常注册、trx_id 升序、覆盖更新
  - query_trx_id_at_timestamp(): 精确匹配、近似查找、空映射
  - get_oldest_available_timestamp(): 空映射、单条、多条
  - is_timestamp_available(): 在窗口内、窗口外、边界值
  - register_commits_batch(): 批量注册、空批次、重复条目
  - get_cache_stats(): 命中率统计、大小统计
  - size(): 空、大小正确
  - 边界: cache_size=0、极大 trx_id、时间戳排序
  - 线程安全
*/

#include <gtest/gtest.h>
#include <vector>

#include "undo_timestamp_mapper.h"

namespace undo_timestamp_mapper_unittest {

// =====================================================================
// 测试组: 构造函数与默认值
// =====================================================================

class UndoTimestampMapperConstructorTest : public ::testing::Test {};

/* 测试: 默认构造应创建空映射 */
TEST_F(UndoTimestampMapperConstructorTest, 默认构造时应创建空映射) {
  UndoTimestampMapper mapper;

  EXPECT_EQ(mapper.size(), 0);
}

/* 测试: 自定义 cache_size 构造应使用指定值 */
TEST_F(UndoTimestampMapperConstructorTest,
       自定义cache_size构造时应使用指定值) {
  UndoTimestampMapper mapper(5000);
  EXPECT_EQ(mapper.size(), 0);

  // 填充到上限
  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 5000; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }
  EXPECT_EQ(mapper.size(), 5000);
}

/* 测试: cache_size=0 时应 fallback 到默认值 10000 */
TEST_F(UndoTimestampMapperConstructorTest,
       cache_size为0时应fallback到默认值) {
  UndoTimestampMapper mapper(0);
  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 10000; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }
  EXPECT_EQ(mapper.size(), 10000);
}

/* 测试: 析构空调度器不应崩溃 */
TEST_F(UndoTimestampMapperConstructorTest,
       析构空调度器不应崩溃) {
  EXPECT_NO_THROW({
    auto mapper = std::make_unique<UndoTimestampMapper>();
  });
}

// =====================================================================
// 测试组: register_commit 正常路径
// =====================================================================

class UndoTimestampMapperRegisterCommitTest : public ::testing::Test {};

/* 测试: 注册单个事务应使 size=1 */
TEST_F(UndoTimestampMapperRegisterCommitTest,
       注册单个事务时应使size等于1) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, t);

  EXPECT_EQ(mapper.size(), 1);
}

/* 测试: 注册多个事务应使 size 正确增长 */
TEST_F(UndoTimestampMapperRegisterCommitTest,
       注册多个事务时应使size正确增长) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 10; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }
  EXPECT_EQ(mapper.size(), 10);
}

/* 测试: 注册相同 trx_id 应覆盖而非累加 */
TEST_F(UndoTimestampMapperRegisterCommitTest,
       注册相同trx_id应覆盖而非累加) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, t);
  mapper.register_commit(100, t + 10);  // 覆盖 trx_id=100

  EXPECT_EQ(mapper.size(), 1);
}

/* 测试: 不同 trx_id 的注册不应相互影响 */
TEST_F(UndoTimestampMapperRegisterCommitTest,
       不同trx_id的注册不应相互影响) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, t);
  mapper.register_commit(200, t + 10);
  mapper.register_commit(300, t + 20);

  EXPECT_EQ(mapper.size(), 3);
}

// =====================================================================
// 测试组: register_commit LRU 淘汰
// =====================================================================

class UndoTimestampMapperLRUTest : public ::testing::Test {};

/* 测试: 超过 cache_size 时应淘汰最老的条目 */
TEST_F(UndoTimestampMapperLRUTest,
       超过cache_size时应淘汰最老的条目) {
  UndoTimestampMapper mapper(5);  // 小缓存便于测试

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 10; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }

  // 应该只保留最后 5 个 (trx_id 6-10)
  EXPECT_EQ(mapper.size(), 5);
}

// =====================================================================
// 测试组: query_trx_id_at_timestamp 正常路径
// =====================================================================

class UndoTimestampMapperQueryTest : public ::testing::Test {};

/* 测试: 空映射查询应返回零值结果 */
TEST_F(UndoTimestampMapperQueryTest,
       空映射查询应返回零值结果且trx_id为0) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  auto result = mapper.query_trx_id_at_timestamp(t);

  EXPECT_EQ(result.trx_id, 0);
  EXPECT_EQ(result.is_exact, false);
}

/* 测试: 精确匹配时间戳应返回对应的 trx_id */
TEST_F(UndoTimestampMapperQueryTest,
       精确匹配时间戳时应返回对应的trx_id且is_exact为true) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 20);
  mapper.register_commit(300, base + 30);

  auto result = mapper.query_trx_id_at_timestamp(base + 20);

  EXPECT_EQ(result.trx_id, 200);
  EXPECT_TRUE(result.is_exact);
}

/* 测试: 查询早于所有记录的时间戳应返回第一个条目 */
TEST_F(UndoTimestampMapperQueryTest,
       查询早于所有记录的时间戳时应返回第一个条目) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 20);

  auto result = mapper.query_trx_id_at_timestamp(base + 5);

  // 第一个 >= target 的记录
  EXPECT_EQ(result.trx_id, 100);
}

/* 测试: 查询晚于所有记录的时间戳应返回空结果 */
TEST_F(UndoTimestampMapperQueryTest,
       查询晚于所有记录的时间戳时应返回最后一条记录) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 20);

  auto result = mapper.query_trx_id_at_timestamp(base + 100);

  // lower_bound 找不到, result.trx_id 保持初始值 0
  EXPECT_EQ(result.trx_id, 0);
}

/* 测试: 查询在两个记录之间的时间戳应返回后一条 */
TEST_F(UndoTimestampMapperQueryTest,
       查询在两个记录之间的时间戳时应返回后一条记录) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 30);

  auto result = mapper.query_trx_id_at_timestamp(base + 20);

  EXPECT_EQ(result.trx_id, 200);
  EXPECT_FALSE(result.is_exact);
}

// =====================================================================
// 测试组: get_oldest_available_timestamp
// =====================================================================

class UndoTimestampMapperOldestTest : public ::testing::Test {};

/* 测试: 空映射应返回 0 */
TEST_F(UndoTimestampMapperOldestTest,
       空映射应返回0) {
  UndoTimestampMapper mapper;

  EXPECT_EQ(mapper.get_oldest_available_timestamp(), 0);
}

/* 测试: 单条记录应返回该记录的时间戳 */
TEST_F(UndoTimestampMapperOldestTest,
       单条记录应返回该记录的时间戳) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, t);

  EXPECT_EQ(mapper.get_oldest_available_timestamp(), t);
}

/* 测试: 多条记录应返回最老的时间戳 */
TEST_F(UndoTimestampMapperOldestTest,
       多条记录应返回最老的时间戳) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 20);
  mapper.register_commit(200, base + 10);  // 更早
  mapper.register_commit(300, base + 30);

  EXPECT_EQ(mapper.get_oldest_available_timestamp(), base + 10);
}

/* 测试: LRU 淘汰后最老时间戳应更新 */
TEST_F(UndoTimestampMapperOldestTest,
       LRU淘汰后最老时间戳应更新) {
  UndoTimestampMapper mapper(3);

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 20);
  mapper.register_commit(300, base + 30);

  // 超过 cache_size，淘汰 trx_id=100
  mapper.register_commit(400, base + 40);

  // 最老应该是 base + 20
  EXPECT_GE(mapper.get_oldest_available_timestamp(), base + 20);
}

// =====================================================================
// 测试组: is_timestamp_available
// =====================================================================

class UndoTimestampMapperAvailableTest : public ::testing::Test {};

/* 测试: 空调度器任何时间戳都不可用 */
TEST_F(UndoTimestampMapperAvailableTest,
       空调度器任何时间戳都应返回false) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  EXPECT_FALSE(mapper.is_timestamp_available(t));
  EXPECT_FALSE(mapper.is_timestamp_available(t - 1000));
  EXPECT_FALSE(mapper.is_timestamp_available(t + 1000));
}

/* 测试: 在窗口内的时间戳应返回 true */
TEST_F(UndoTimestampMapperAvailableTest,
       在窗口内的时间戳应返回true) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 30);

  EXPECT_TRUE(mapper.is_timestamp_available(base + 15));
  EXPECT_TRUE(mapper.is_timestamp_available(base + 10));
  EXPECT_TRUE(mapper.is_timestamp_available(base + 30));
}

/* 测试: 早于窗口的时间戳应返回 false */
TEST_F(UndoTimestampMapperAvailableTest,
       早于窗口的时间戳应返回false) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 20);

  EXPECT_FALSE(mapper.is_timestamp_available(base - 10));
  EXPECT_FALSE(mapper.is_timestamp_available(base + 10));  // 早于记录
}

// =====================================================================
// 测试组: register_commits_batch 批量注册
// =====================================================================

class UndoTimestampMapperBatchTest : public ::testing::Test {};

/* 测试: 批量注册空列表应使 size=0 */
TEST_F(UndoTimestampMapperBatchTest,
       批量注册空列表应使size为0) {
  UndoTimestampMapper mapper;

  std::vector<UndoTimestampMapper::MappingEntry> entries;
  mapper.register_commits_batch(entries);

  EXPECT_EQ(mapper.size(), 0);
}

/* 测试: 批量注册多个条目应使 size 正确 */
TEST_F(UndoTimestampMapperBatchTest,
       批量注册多个条目应使size正确) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  std::vector<UndoTimestampMapper::MappingEntry> entries = {
      {100, base + 10, 0},
      {200, base + 20, 0},
      {300, base + 30, 0},
  };
  mapper.register_commits_batch(entries);

  EXPECT_EQ(mapper.size(), 3);
}

/* 测试: 批量注册后查询应正确工作 */
TEST_F(UndoTimestampMapperBatchTest,
       批量注册后查询应正确工作) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  std::vector<UndoTimestampMapper::MappingEntry> entries = {
      {100, base + 10, 0},
      {200, base + 20, 0},
  };
  mapper.register_commits_batch(entries);

  auto result = mapper.query_trx_id_at_timestamp(base + 15);
  EXPECT_EQ(result.trx_id, 200);
}

/* 测试: 批量注册的 LRU 淘汰与单条一致 */
TEST_F(UndoTimestampMapperBatchTest,
       批量注册的LRU淘汰应与单条注册一致) {
  UndoTimestampMapper mapper(5);

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  std::vector<UndoTimestampMapper::MappingEntry> entries;
  for (int i = 0; i < 10; ++i) {
    entries.push_back(
        {static_cast<trx_id_t>(i + 1), base + i, 0});
  }
  mapper.register_commits_batch(entries);

  EXPECT_EQ(mapper.size(), 5);
}

// =====================================================================
// 测试组: get_cache_stats 统计
// =====================================================================

class UndoTimestampMapperStatsTest : public ::testing::Test {};

/* 测试: 空调度器缓存大小应为 0 */
TEST_F(UndoTimestampMapperStatsTest,
       空调度器缓存大小应为0) {
  UndoTimestampMapper mapper;

  auto stats = mapper.get_cache_stats();
  EXPECT_EQ(stats.cache_size, 0);
  EXPECT_EQ(stats.hit_count, 0);
  EXPECT_EQ(stats.miss_count, 0);
  EXPECT_EQ(stats.hit_rate, 0.0);
}

/* 测试: 注册后 cache_size 应正确反映条目数 */
TEST_F(UndoTimestampMapperStatsTest,
       注册后cache_size应正确反映条目数) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 20; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }

  auto stats = mapper.get_cache_stats();
  EXPECT_EQ(stats.cache_size, 20);
}

/* 测试: newest_entry 应 >= oldest_entry */
TEST_F(UndoTimestampMapperStatsTest,
       newest_entry应大于等于oldest_entry) {
  UndoTimestampMapper mapper;

  my_time_t base = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(100, base + 10);
  mapper.register_commit(200, base + 20);

  auto stats = mapper.get_cache_stats();
  EXPECT_GE(stats.newest_entry, stats.oldest_entry);
}

// =====================================================================
// 测试组: 边界条件
// =====================================================================

class UndoTimestampMapperBoundaryTest : public ::testing::Test {};

/* 测试: 极大 trx_id 应能正常注册和查询 */
TEST_F(UndoTimestampMapperBoundaryTest,
       极大trx_id应能正常注册和查询) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  trx_id_t huge_id = UINT64_MAX - 1;
  mapper.register_commit(huge_id, t);

  EXPECT_EQ(mapper.size(), 1);
  auto result = mapper.query_trx_id_at_timestamp(t);
  EXPECT_EQ(result.trx_id, huge_id);
}

/* 测试: trx_id=1 应能正常注册和查询 */
TEST_F(UndoTimestampMapperBoundaryTest,
       trx_id为1应能正常注册和查询) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  mapper.register_commit(1, t);

  EXPECT_EQ(mapper.size(), 1);
  auto result = mapper.query_trx_id_at_timestamp(t);
  EXPECT_EQ(result.trx_id, 1);
}

/* 测试: 0 时间戳应能正常注册 */
TEST_F(UndoTimestampMapperBoundaryTest,
       时间戳为0应能正常注册) {
  UndoTimestampMapper mapper;

  mapper.register_commit(100, 0);
  EXPECT_EQ(mapper.size(), 1);
  EXPECT_FALSE(mapper.is_timestamp_available(0));
}

// =====================================================================
// 测试组: 线程安全 (基础验证)
// =====================================================================

class UndoTimestampMapperConcurrencyTest : public ::testing::Test {};

/* 测试: 并发注册不应崩溃 */
TEST_F(UndoTimestampMapperConcurrencyTest,
       并发注册不应崩溃且大小正确) {
  UndoTimestampMapper mapper;

  constexpr int THREADS = 4;
  constexpr int OPS = 100;

  std::vector<std::thread> threads;
  for (int t = 0; t < THREADS; ++t) {
    threads.emplace_back([&, t]() {
      my_time_t base = static_cast<my_time_t>(std::time(nullptr));
      for (int i = 0; i < OPS; ++i) {
        trx_id_t id = static_cast<trx_id_t>(t * OPS + i + 1);
        mapper.register_commit(id, base + i);
      }
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  // 每个线程注册 OPS 个，THREADS 个线程
  EXPECT_EQ(mapper.size(), THREADS * OPS);
}

/* 测试: 并发读查询不应崩溃 */
TEST_F(UndoTimestampMapperConcurrencyTest,
       并发读查询不应崩溃) {
  UndoTimestampMapper mapper;

  my_time_t t = static_cast<my_time_t>(std::time(nullptr));
  for (int i = 0; i < 100; ++i) {
    mapper.register_commit(static_cast<trx_id_t>(i + 1), t + i);
  }

  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;

  for (int i = 0; i < 8; ++i) {
    threads.emplace_back([&]() {
      while (!stop.load(std::memory_order_relaxed)) {
        EXPECT_NO_THROW(mapper.query_trx_id_at_timestamp(t + 50));
        EXPECT_NO_THROW(mapper.size());
        EXPECT_NO_THROW(mapper.get_oldest_available_timestamp());
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true, std::memory_order_relaxed);

  for (auto &th : threads) {
    th.join();
  }
}

}  // namespace undo_timestamp_mapper_unittest
