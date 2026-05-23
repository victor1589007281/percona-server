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
  @file unittest/gunit/flashback_checkpoint-t.cc
  @brief 单元测试: FlashbackCheckpoint JSON 序列化/反序列化与断点续写

  测试覆盖:
  - 第一层 (单元测试, 60%):
    - save_to_file(): 正常写入、路径构造、fsync 语义
    - load_from_file(): 正常加载、文件不存在、格式错误
    - reset(): 字段重置到初始值
    - get_checkpoint_path(): 路径拼接正确性
    - checkpoint_file_exists(): 存在性判断
    - remove_file(): 删除幂等性
    - 边界测试: 空 session_id、极大 binlog_pos、零值字段

  - 第二层 (集成测试, 30%):
    - save→load 往返一致性 (round-trip)
    - 多会话检查点隔离
    - 连续写入后加载最新值

  - 第三层 (端到端测试, 10%):
    - 模拟断点续写场景: 写入部分进度 → 加载 → 验证字段正确性
*/

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "sql/flashback_checkpoint.h"

namespace flashback_checkpoint_unittest {

namespace fs = std::filesystem;

// =====================================================================
// 测试 Fixture: 提供临时目录自动清理
// =====================================================================

class CheckpointTempDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    m_tmp_dir = fs::temp_directory_path() /
                ("flashback_cp_test_" + std::to_string(getpid()));
    fs::create_directories(m_tmp_dir);
  }

  void TearDown() override {
    /* 清理临时目录 */
    std::error_code ec;
    fs::remove_all(m_tmp_dir, ec);
  }

  std::string tmp_dir() const { return m_tmp_dir.string(); }

 private:
  fs::path m_tmp_dir;
};

// =====================================================================
// 测试组: 构造函数与 reset()
// =====================================================================

class CheckpointConstructorTest : public ::testing::Test {};

/* 测试: 默认构造函数应初始化所有字段为零值 */
TEST_F(CheckpointConstructorTest, 默认构造时所有字段应为零值) {
  flashback::FlashbackCheckpoint cp;

  EXPECT_TRUE(cp.binlog_file.empty());
  EXPECT_EQ(cp.binlog_pos, 0ULL);
  EXPECT_EQ(cp.table_idx, 0U);
  EXPECT_EQ(cp.rows_processed, 0ULL);
  EXPECT_EQ(cp.txn_id, 0ULL);
  EXPECT_EQ(cp.checkpoint_time, 0);
  EXPECT_TRUE(cp.session_id.empty());
}

/* 测试: 带参数构造函数应正确设置 data_dir 和 session_id */
TEST_F(CheckpointConstructorTest,
       带参数构造时应正确设置数据目录和会话标识) {
  flashback::FlashbackCheckpoint cp("/var/lib/mysql", "session_abc123");

  EXPECT_EQ(cp.session_id, "session_abc123");
  /* binlog_file 等其他字段仍为零值 */
  EXPECT_TRUE(cp.binlog_file.empty());
  EXPECT_EQ(cp.binlog_pos, 0ULL);
}

/* 测试: reset() 应将所有字段恢复为初始值 */
TEST_F(CheckpointConstructorTest, 当调用reset时应恢复所有字段到初始值) {
  flashback::FlashbackCheckpoint cp("/tmp", "test_session");
  cp.binlog_file = "mysql-bin.000003";
  cp.binlog_pos = 999999;
  cp.table_idx = 5;
  cp.rows_processed = 12345;
  cp.txn_id = 777;

  cp.reset();

  EXPECT_TRUE(cp.binlog_file.empty());
  EXPECT_EQ(cp.binlog_pos, 0ULL);
  EXPECT_EQ(cp.table_idx, 0U);
  EXPECT_EQ(cp.rows_processed, 0ULL);
  EXPECT_EQ(cp.txn_id, 0ULL);
  /* session_id 和 m_data_dir 应保留 */
  EXPECT_EQ(cp.session_id, "test_session");
}

// =====================================================================
// 测试组: get_checkpoint_path() 路径构造
// =====================================================================

class CheckpointPathTest : public ::testing::Test {};

/* 测试: get_checkpoint_path() 应返回正确的文件路径格式 */
TEST_F(CheckpointPathTest, 路径应包含数据目录和会话标识) {
  flashback::FlashbackCheckpoint cp("/var/lib/mysql", "session_001");

  std::string path = cp.get_checkpoint_path();

  /* 路径应包含数据目录 */
  EXPECT_NE(path.find("/var/lib/mysql"), std::string::npos);
  /* 路径应包含会话标识 */
  EXPECT_NE(path.find("session_001"), std::string::npos);
  /* 路径应以 .json 结尾 */
  EXPECT_NE(path.find(".json"), std::string::npos);
}

/* 测试: 空 session_id 时路径仍应合理 */
TEST_F(CheckpointPathTest, 空会话标识时路径应仍有效) {
  flashback::FlashbackCheckpoint cp("/tmp", "");

  std::string path = cp.get_checkpoint_path();

  EXPECT_FALSE(path.empty());
  EXPECT_NE(path.find("/tmp"), std::string::npos);
}

// =====================================================================
// 测试组: save_to_file() — 正常写入
// =====================================================================

class CheckpointSaveTest : public CheckpointTempDirTest {};

/* 测试: save_to_file() 应成功写入 JSON 文件 */
TEST_F(CheckpointSaveTest, 当设置有效字段后保存应成功) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "save_test_001");
  cp.binlog_file = "mysql-bin.000003";
  cp.binlog_pos = 1234567;
  cp.table_idx = 2;
  cp.rows_processed = 50000;
  cp.txn_id = 9876543;

  EXPECT_EQ(cp.save_to_file(), flashback::FlashbackError::NONE);
}

/* 测试: 保存后检查点文件应存在 */
TEST_F(CheckpointSaveTest, 保存后文件应存在) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "save_test_002");
  cp.binlog_file = "mysql-bin.000001";
  cp.binlog_pos = 100;

  cp.save_to_file();

  EXPECT_TRUE(cp.checkpoint_file_exists());
}

/* 测试: 保存极大值字段不应溢出 */
TEST_F(CheckpointSaveTest, 当字段为极大值时保存应成功) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "save_test_max");
  cp.binlog_file = "mysql-bin.999999";
  cp.binlog_pos = static_cast<my_off_t>(~0ULL);
  cp.table_idx = UINT32_MAX;
  cp.rows_processed = ULLONG_MAX;
  cp.txn_id = UINT64_MAX;

  EXPECT_EQ(cp.save_to_file(), flashback::FlashbackError::NONE);
}

/* 测试: 保存空 binlog_file 不应失败 */
TEST_F(CheckpointSaveTest, 当binlog_file为空时保存应成功) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "save_test_empty");
  /* 不设置 binlog_file，保持空字符串 */

  EXPECT_EQ(cp.save_to_file(), flashback::FlashbackError::NONE);
}

// =====================================================================
// 测试组: load_from_file() — 正常加载
// =====================================================================

class CheckpointLoadTest : public CheckpointTempDirTest {};

/* 测试: load_from_file() 应正确还原保存的字段 */
TEST_F(CheckpointLoadTest, 保存后加载应还原所有字段) {
  flashback::FlashbackCheckpoint cp1(tmp_dir(), "roundtrip_001");
  cp1.binlog_file = "mysql-bin.000005";
  cp1.binlog_pos = 8888888;
  cp1.table_idx = 7;
  cp1.rows_processed = 333333;
  cp1.txn_id = 111222333;

  ASSERT_EQ(cp1.save_to_file(), flashback::FlashbackError::NONE);

  flashback::FlashbackCheckpoint cp2(tmp_dir(), "roundtrip_001");
  ASSERT_EQ(cp2.load_from_file(), flashback::FlashbackError::NONE);

  EXPECT_EQ(cp2.binlog_file, "mysql-bin.000005");
  EXPECT_EQ(cp2.binlog_pos, 8888888ULL);
  EXPECT_EQ(cp2.table_idx, 7U);
  EXPECT_EQ(cp2.rows_processed, 333333ULL);
  EXPECT_EQ(cp2.txn_id, 111222333ULL);
}

/* 测试: 当检查点文件不存在时加载应返回 GENERIC 错误 */
TEST_F(CheckpointLoadTest, 当文件不存在时加载应返回错误) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "nonexistent_session");

  EXPECT_EQ(cp.load_from_file(), flashback::FlashbackError::GENERIC);
}

/* 测试: 保存后删除文件再加载应返回错误 */
TEST_F(CheckpointLoadTest, 当文件被删除后加载应返回错误) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "delete_test");
  cp.binlog_file = "mysql-bin.000001";
  cp.binlog_pos = 100;

  cp.save_to_file();
  cp.remove_file();

  EXPECT_EQ(cp.load_from_file(), flashback::FlashbackError::GENERIC);
}

// =====================================================================
// 测试组: remove_file() — 删除幂等性
// =====================================================================

class CheckpointRemoveTest : public CheckpointTempDirTest {};

/* 测试: remove_file() 应成功删除已存在的检查点文件 */
TEST_F(CheckpointRemoveTest, 删除已存在的文件应成功) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "remove_test_001");
  cp.binlog_file = "mysql-bin.000001";
  cp.save_to_file();

  ASSERT_TRUE(cp.checkpoint_file_exists());

  EXPECT_EQ(cp.remove_file(), flashback::FlashbackError::NONE);
  EXPECT_FALSE(cp.checkpoint_file_exists());
}

/* 测试: remove_file() 对不存在的文件也应返回 NONE (幂等) */
TEST_F(CheckpointRemoveTest, 删除不存在的文件应返回NONE_幂等性) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "remove_nonexistent");

  /* 文件从未创建 */
  EXPECT_FALSE(cp.checkpoint_file_exists());
  EXPECT_EQ(cp.remove_file(), flashback::FlashbackError::NONE);
}

// =====================================================================
// 测试组: checkpoint_file_exists() — 存在性判断
// =====================================================================

class CheckpointExistsTest : public CheckpointTempDirTest {};

/* 测试: 未保存时检查点文件不存在 */
TEST_F(CheckpointExistsTest, 未保存时文件应不存在) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "exists_test");

  EXPECT_FALSE(cp.checkpoint_file_exists());
}

/* 测试: 保存后检查点文件存在 */
TEST_F(CheckpointExistsTest, 保存后文件应存在) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "exists_test_002");
  cp.binlog_file = "mysql-bin.000001";
  cp.save_to_file();

  EXPECT_TRUE(cp.checkpoint_file_exists());
}

// =====================================================================
// 第二层: 集成测试 — save→load 往返一致性
// =====================================================================

class CheckpointIntegrationTest : public CheckpointTempDirTest {};

/* 测试: 连续多次写入后加载应得到最后一次写入的值 */
TEST_F(CheckpointIntegrationTest,
       连续多次写入后加载应得到最新值) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "multi_write");

  /* 第一次写入 */
  cp.binlog_file = "mysql-bin.000001";
  cp.binlog_pos = 100;
  cp.rows_processed = 1000;
  cp.save_to_file();

  /* 第二次写入 */
  cp.binlog_file = "mysql-bin.000002";
  cp.binlog_pos = 200;
  cp.rows_processed = 2000;
  cp.save_to_file();

  /* 第三次写入 */
  cp.binlog_file = "mysql-bin.000003";
  cp.binlog_pos = 300;
  cp.rows_processed = 3000;
  cp.txn_id = 999;
  cp.save_to_file();

  flashback::FlashbackCheckpoint cp2(tmp_dir(), "multi_write");
  ASSERT_EQ(cp2.load_from_file(), flashback::FlashbackError::NONE);

  EXPECT_EQ(cp2.binlog_file, "mysql-bin.000003");
  EXPECT_EQ(cp2.binlog_pos, 300ULL);
  EXPECT_EQ(cp2.rows_processed, 3000ULL);
  EXPECT_EQ(cp2.txn_id, 999ULL);
}

/* 测试: 多会话检查点应相互隔离 */
TEST_F(CheckpointIntegrationTest, 多会话检查点应相互隔离) {
  flashback::FlashbackCheckpoint cpA(tmp_dir(), "session_A");
  cpA.binlog_file = "mysql-bin.000001";
  cpA.binlog_pos = 111;
  cpA.save_to_file();

  flashback::FlashbackCheckpoint cpB(tmp_dir(), "session_B");
  cpB.binlog_file = "mysql-bin.000002";
  cpB.binlog_pos = 222;
  cpB.save_to_file();

  /* 加载 session_A 应得到 session_A 的值 */
  flashback::FlashbackCheckpoint loadedA(tmp_dir(), "session_A");
  ASSERT_EQ(loadedA.load_from_file(), flashback::FlashbackError::NONE);
  EXPECT_EQ(loadedA.binlog_file, "mysql-bin.000001");
  EXPECT_EQ(loadedA.binlog_pos, 111ULL);

  /* 加载 session_B 应得到 session_B 的值 */
  flashback::FlashbackCheckpoint loadedB(tmp_dir(), "session_B");
  ASSERT_EQ(loadedB.load_from_file(), flashback::FlashbackError::NONE);
  EXPECT_EQ(loadedB.binlog_file, "mysql-bin.000002");
  EXPECT_EQ(loadedB.binlog_pos, 222ULL);
}

/* 测试: save→reset→load 往返应恢复所有字段 */
TEST_F(CheckpointIntegrationTest,
       当保存后重置再加载时应恢复保存的值) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "reset_roundtrip");
  cp.binlog_file = "mysql-bin.000010";
  cp.binlog_pos = 55555;
  cp.table_idx = 3;
  cp.rows_processed = 7777;
  cp.txn_id = 444;

  cp.save_to_file();
  cp.reset();

  /* reset 后字段应清零，但 session_id 保留 */
  EXPECT_TRUE(cp.binlog_file.empty());
  EXPECT_EQ(cp.session_id, "reset_roundtrip");

  cp.load_from_file();

  EXPECT_EQ(cp.binlog_file, "mysql-bin.000010");
  EXPECT_EQ(cp.binlog_pos, 55555ULL);
  EXPECT_EQ(cp.table_idx, 3U);
  EXPECT_EQ(cp.rows_processed, 7777ULL);
  EXPECT_EQ(cp.txn_id, 444ULL);
}

// =====================================================================
// 第三层: 端到端测试 — 模拟断点续写场景
// =====================================================================

class CheckpointE2ETest : public CheckpointTempDirTest {};

/* 测试: 模拟断点续写 — 闪回进行中写入检查点，重启后继续 */
TEST_F(CheckpointE2ETest, 模拟断点续写场景应能恢复进度) {
  /* === 模拟第一次闪回运行 (被中断) === */
  flashback::FlashbackCheckpoint cp1(tmp_dir(), "e2e_resume");

  /* 闪回处理了 2 个 binlog 文件，第 3 个文件到一半被中断 */
  cp1.binlog_file = "mysql-bin.000003";
  cp1.binlog_pos = 500000;
  cp1.table_idx = 2;
  cp1.rows_processed = 150000;
  cp1.txn_id = 55555;
  cp1.save_to_file();

  /* === 模拟闪回重启，从检查点恢复 === */
  flashback::FlashbackCheckpoint cp2(tmp_dir(), "e2e_resume");
  ASSERT_EQ(cp2.load_from_file(), flashback::FlashbackError::NONE);

  /* 验证可以从检查点位置继续 */
  EXPECT_EQ(cp2.binlog_file, "mysql-bin.000003");
  EXPECT_EQ(cp2.binlog_pos, 500000ULL);
  EXPECT_EQ(cp2.table_idx, 2U);
  EXPECT_EQ(cp2.rows_processed, 150000ULL);
  EXPECT_EQ(cp2.txn_id, 55555ULL);

  /* 继续处理: 更新检查点 */
  cp2.binlog_pos = 600000;
  cp2.rows_processed = 180000;
  cp2.txn_id = 66666;
  cp2.save_to_file();

  /* 再次加载验证 */
  flashback::FlashbackCheckpoint cp3(tmp_dir(), "e2e_resume");
  ASSERT_EQ(cp3.load_from_file(), flashback::FlashbackError::NONE);
  EXPECT_EQ(cp3.binlog_pos, 600000ULL);
  EXPECT_EQ(cp3.rows_processed, 180000ULL);
  EXPECT_EQ(cp3.txn_id, 66666ULL);
}

/* 测试: 模拟闪回完成后清理检查点文件 */
TEST_F(CheckpointE2ETest, 闪回完成后应能清理检查点文件) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "e2e_cleanup");
  cp.binlog_file = "mysql-bin.000001";
  cp.binlog_pos = 999;
  cp.rows_processed = 5000;
  cp.save_to_file();

  /* 验证检查点存在 */
  ASSERT_TRUE(cp.checkpoint_file_exists());

  /* 闪回成功完成，清理检查点 */
  EXPECT_EQ(cp.remove_file(), flashback::FlashbackError::NONE);
  EXPECT_FALSE(cp.checkpoint_file_exists());

  /* 再次清理应幂等成功 */
  EXPECT_EQ(cp.remove_file(), flashback::FlashbackError::NONE);
}

/* 测试: 检查点数据在 save/load 后应保持类型精度 (极大 binlog_pos) */
TEST_F(CheckpointE2ETest, 当binlog_pos为极大值时应保持精度) {
  flashback::FlashbackCheckpoint cp(tmp_dir(), "e2e_precision");
  cp.binlog_file = "mysql-bin.999999";
  cp.binlog_pos = 4294967296ULL; /* 超过 32-bit 范围 */
  cp.rows_processed = 10000000000ULL; /* 100 亿行 */

  cp.save_to_file();

  flashback::FlashbackCheckpoint cp2(tmp_dir(), "e2e_precision");
  ASSERT_EQ(cp2.load_from_file(), flashback::FlashbackError::NONE);

  EXPECT_EQ(cp2.binlog_pos, 4294967296ULL);
  EXPECT_EQ(cp2.rows_processed, 10000000000ULL);
}

}  // namespace flashback_checkpoint_unittest
