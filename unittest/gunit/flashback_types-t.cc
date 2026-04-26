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
  @file unittest/gunit/flashback_types-t.cc
  @brief 单元测试: Flashback 类型系统

  测试覆盖:
  - 枚举值的完整性与边界
  - FlashbackRequest 结构体的默认值初始化
  - FlashbackResult 结构体的状态机转换
  - FlashbackTableInfo 的默认值
  - INVALID_TRX_ID 常量
*/

#include <gtest/gtest.h>

#include "sql/flashback_types.h"

namespace flashback_types_unittest {

// =====================================================================
// 测试组: 枚举值完整性
// =====================================================================

class FlashbackEnumTest : public ::testing::Test {};

/* 测试: FlashbackType 枚举应包含所有 4 种操作类型 */
TEST_F(FlashbackEnumTest, 当枚举FlashbackType时应包含QUERY_TABLE_VERSIONS_TRANSACTION) {
  // 验证所有枚举值可正确构造
  flashback::FlashbackType types[] = {
      flashback::FlashbackType::QUERY,
      flashback::FlashbackType::TABLE,
      flashback::FlashbackType::VERSIONS,
      flashback::FlashbackType::TRANSACTION};

  // 验证每个值都能正确存储和比较
  EXPECT_EQ(types[0], flashback::FlashbackType::QUERY);
  EXPECT_EQ(types[1], flashback::FlashbackType::TABLE);
  EXPECT_EQ(types[2], flashback::FlashbackType::VERSIONS);
  EXPECT_EQ(types[3], flashback::FlashbackType::TRANSACTION);
}

/* 测试: FlashbackEngineType 枚举应包含 AUTO/NONE/UNDO/BINLOG */
TEST_F(FlashbackEnumTest, 当枚举FlashbackEngineType时应包含四种引擎) {
  flashback::FlashbackEngineType engines[] = {
      flashback::FlashbackEngineType::AUTO,
      flashback::FlashbackEngineType::NONE,
      flashback::FlashbackEngineType::UNDO,
      flashback::FlashbackEngineType::BINLOG};

  EXPECT_EQ(engines[0], flashback::FlashbackEngineType::AUTO);
  EXPECT_EQ(engines[1], flashback::FlashbackEngineType::NONE);
  EXPECT_EQ(engines[2], flashback::FlashbackEngineType::UNDO);
  EXPECT_EQ(engines[3], flashback::FlashbackEngineType::BINLOG);
}

/* 测试: FlashbackState 状态机应包含所有预期状态 */
TEST_F(FlashbackEnumTest, 当枚举FlashbackState时应包含所有状态) {
  flashback::FlashbackState states[] = {
      flashback::FlashbackState::INIT,
      flashback::FlashbackState::CHECKING,
      flashback::FlashbackState::RUNNING,
      flashback::FlashbackState::COMPLETED,
      flashback::FlashbackState::FAILED,
      flashback::FlashbackState::DRY_RUN,
      flashback::FlashbackState::INTERRUPTED};

  EXPECT_EQ(states[0], flashback::FlashbackState::INIT);
  EXPECT_EQ(states[1], flashback::FlashbackState::CHECKING);
  EXPECT_EQ(states[2], flashback::FlashbackState::RUNNING);
  EXPECT_EQ(states[3], flashback::FlashbackState::COMPLETED);
  EXPECT_EQ(states[4], flashback::FlashbackState::FAILED);
  EXPECT_EQ(states[5], flashback::FlashbackState::DRY_RUN);
  EXPECT_EQ(states[6], flashback::FlashbackState::INTERRUPTED);
}

/* 测试: FlashbackError 错误码应包含所有预期类型 */
TEST_F(FlashbackEnumTest, 当枚举FlashbackError时应包含所有错误类型) {
  flashback::FlashbackError errors[] = {
      flashback::FlashbackError::NONE,
      flashback::FlashbackError::GENERIC,
      flashback::FlashbackError::OUT_OF_WINDOW,
      flashback::FlashbackError::BINLOG_EXPIRED,
      flashback::FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL,
      flashback::FlashbackError::DDL_INCOMPATIBLE,
      flashback::FlashbackError::INTERRUPTED};

  EXPECT_EQ(errors[0], flashback::FlashbackError::NONE);
  EXPECT_EQ(errors[1], flashback::FlashbackError::GENERIC);
  EXPECT_EQ(errors[2], flashback::FlashbackError::OUT_OF_WINDOW);
  EXPECT_EQ(errors[3], flashback::FlashbackError::BINLOG_EXPIRED);
  EXPECT_EQ(errors[4], flashback::FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL);
  EXPECT_EQ(errors[5], flashback::FlashbackError::DDL_INCOMPATIBLE);
  EXPECT_EQ(errors[6], flashback::FlashbackError::INTERRUPTED);
}

// =====================================================================
// 测试组: INVALID_TRX_ID 常量
// =====================================================================

class FlashbackConstantTest : public ::testing::Test {};

/* 测试: INVALID_TRX_ID 应为 0 */
TEST_F(FlashbackConstantTest, 当检查INVALID_TRX_ID时应等于0) {
  EXPECT_EQ(flashback::INVALID_TRX_ID, 0ULL);
}

/* 测试: INVALID_TRX_ID 应为 constexpr 编译时常量 */
TEST_F(FlashbackConstantTest, 当使用INVALID_TRX_ID作为模板参数时应可编译) {
  // 如果能编译通过，说明确实是 constexpr
  constexpr uint64_t k = flashback::INVALID_TRX_ID;
  EXPECT_EQ(k, 0ULL);
}

// =====================================================================
// 测试组: FlashbackRequest 默认值
// =====================================================================

class FlashbackRequestTest : public ::testing::Test {};

/* 测试: FlashbackRequest 默认构造应使用安全的初始值 */
TEST_F(FlashbackRequestTest, 默认构造时应使用安全的初始值) {
  flashback::FlashbackRequest req;

  EXPECT_EQ(req.type, flashback::FlashbackType::QUERY);
  EXPECT_EQ(req.engine, flashback::FlashbackEngineType::AUTO);
  EXPECT_EQ(req.target_time, 0);
  EXPECT_EQ(req.target_trx_id, flashback::INVALID_TRX_ID);
  EXPECT_EQ(req.start_timestamp, 0);
  EXPECT_EQ(req.end_timestamp, 0);
  EXPECT_EQ(req.tables, nullptr);
  EXPECT_EQ(req.table_count, 0U);
  EXPECT_TRUE(req.db_name.empty());
  EXPECT_EQ(req.dry_run, false);
  EXPECT_EQ(req.max_rows, 10000000ULL);
  EXPECT_EQ(req.lock_wait_timeout, 300UL);
  EXPECT_EQ(req.audit_log, true);
}

/* 测试: FlashbackRequest 的 max_rows 默认值应为 1000 万 */
TEST_F(FlashbackRequestTest, 默认max_rows应为1000万) {
  flashback::FlashbackRequest req;
  EXPECT_EQ(req.max_rows, 10000000ULL);
}

/* 测试: FlashbackRequest 的 lock_wait_timeout 默认值应为 300 秒 */
TEST_F(FlashbackRequestTest, 默认lock_wait_timeout应为300秒) {
  flashback::FlashbackRequest req;
  EXPECT_EQ(req.lock_wait_timeout, 300UL);
}

/* 测试: FlashbackRequest 的 dry_run 默认为 false */
TEST_F(FlashbackRequestTest, 默认dry_run应为false) {
  flashback::FlashbackRequest req;
  EXPECT_FALSE(req.dry_run);
}

/* 测试: FlashbackRequest 的 audit_log 默认为 true */
TEST_F(FlashbackRequestTest, 默认audit_log应为true) {
  flashback::FlashbackRequest req;
  EXPECT_TRUE(req.audit_log);
}

// =====================================================================
// 测试组: FlashbackResult 默认值与状态机
// =====================================================================

class FlashbackResultTest : public ::testing::Test {};

/* 测试: FlashbackResult 默认构造应处于 INIT 状态 */
TEST_F(FlashbackResultTest, 默认构造时应处于INIT状态且无错误) {
  flashback::FlashbackResult result;

  EXPECT_EQ(result.state, flashback::FlashbackState::INIT);
  EXPECT_EQ(result.error, flashback::FlashbackError::NONE);
  EXPECT_EQ(result.engine_used, flashback::FlashbackEngineType::NONE);
  EXPECT_EQ(result.rows_processed, 0ULL);
  EXPECT_EQ(result.rows_restored, 0ULL);
  EXPECT_EQ(result.tables_processed, 0U);
  EXPECT_EQ(result.elapsed_ms, 0ULL);
  EXPECT_TRUE(result.error_message.empty());
  EXPECT_TRUE(result.dry_run_summary.empty());
}

/* 测试: FlashbackResult 在模拟失败后应正确设置错误状态 */
TEST_F(FlashbackResultTest, 当模拟失败时应正确设置错误状态) {
  flashback::FlashbackResult result;

  result.state = flashback::FlashbackState::FAILED;
  result.error = flashback::FlashbackError::OUT_OF_WINDOW;
  result.error_message = "Target time exceeds retention window";

  EXPECT_EQ(result.state, flashback::FlashbackState::FAILED);
  EXPECT_EQ(result.error, flashback::FlashbackError::OUT_OF_WINDOW);
  EXPECT_FALSE(result.error_message.empty());
}

/* 测试: FlashbackResult 在模拟 DRY_RUN 后应正确设置状态和摘要 */
TEST_F(FlashbackResultTest, 当模拟DRY_RUN时应正确设置状态和摘要) {
  flashback::FlashbackResult result;

  result.state = flashback::FlashbackState::DRY_RUN;
  result.engine_used = flashback::FlashbackEngineType::UNDO;
  result.rows_processed = 5000;
  result.dry_run_summary = "Would restore 5000 rows using UNDO engine";

  EXPECT_EQ(result.state, flashback::FlashbackState::DRY_RUN);
  EXPECT_EQ(result.engine_used, flashback::FlashbackEngineType::UNDO);
  EXPECT_EQ(result.rows_processed, 5000ULL);
  EXPECT_FALSE(result.dry_run_summary.empty());
}

/* 测试: FlashbackResult 在模拟 COMPLETED 后应正确设置统计值 */
TEST_F(FlashbackResultTest, 当模拟COMPLETED时应正确设置统计值) {
  flashback::FlashbackResult result;

  result.state = flashback::FlashbackState::COMPLETED;
  result.engine_used = flashback::FlashbackEngineType::BINLOG;
  result.rows_processed = 100000;
  result.rows_restored = 50000;
  result.tables_processed = 3;
  result.elapsed_ms = 15000;

  EXPECT_EQ(result.state, flashback::FlashbackState::COMPLETED);
  EXPECT_EQ(result.engine_used, flashback::FlashbackEngineType::BINLOG);
  EXPECT_EQ(result.rows_processed, 100000ULL);
  EXPECT_EQ(result.rows_restored, 50000ULL);
  EXPECT_EQ(result.tables_processed, 3U);
  EXPECT_EQ(result.elapsed_ms, 15000ULL);
}

// =====================================================================
// 测试组: FlashbackTableInfo 默认值
// =====================================================================

class FlashbackTableInfoTest : public ::testing::Test {};

/* 测试: FlashbackTableInfo 默认构造应使用安全的初始值 */
TEST_F(FlashbackTableInfoTest, 默认构造时应使用安全的初始值) {
  flashback::FlashbackTableInfo info;

  EXPECT_EQ(info.table_id, 0ULL);
  EXPECT_TRUE(info.table_name.empty());
  EXPECT_TRUE(info.schema_name.empty());
  EXPECT_FALSE(info.has_primary_key);
}

// =====================================================================
// 测试组: 枚举类型可互换性（switch 语句覆盖）
// =====================================================================

class FlashbackSwitchCoverageTest : public ::testing::Test {};

/* 测试: 确保所有 FlashbackType 枚举值可在 switch 中正确处理 */
TEST_F(FlashbackSwitchCoverageTest, 当对所有FlashbackType进行switch时应正确处理) {
  auto get_type_name = [](flashback::FlashbackType t) -> const char * {
    switch (t) {
      case flashback::FlashbackType::QUERY:
        return "QUERY";
      case flashback::FlashbackType::TABLE:
        return "TABLE";
      case flashback::FlashbackType::VERSIONS:
        return "VERSIONS";
      case flashback::FlashbackType::TRANSACTION:
        return "TRANSACTION";
    }
    return "UNKNOWN";
  };

  EXPECT_STREQ(get_type_name(flashback::FlashbackType::QUERY), "QUERY");
  EXPECT_STREQ(get_type_name(flashback::FlashbackType::TABLE), "TABLE");
  EXPECT_STREQ(get_type_name(flashback::FlashbackType::VERSIONS), "VERSIONS");
  EXPECT_STREQ(get_type_name(flashback::FlashbackType::TRANSACTION),
               "TRANSACTION");
}

/* 测试: 确保所有 FlashbackEngineType 枚举值可在 switch 中正确处理 */
TEST_F(FlashbackSwitchCoverageTest,
       当对所有FlashbackEngineType进行switch时应正确处理) {
  auto get_engine_name = [](flashback::FlashbackEngineType e) -> const char * {
    switch (e) {
      case flashback::FlashbackEngineType::AUTO:
        return "AUTO";
      case flashback::FlashbackEngineType::NONE:
        return "NONE";
      case flashback::FlashbackEngineType::UNDO:
        return "UNDO";
      case flashback::FlashbackEngineType::BINLOG:
        return "BINLOG";
    }
    return "UNKNOWN";
  };

  EXPECT_STREQ(get_engine_name(flashback::FlashbackEngineType::AUTO), "AUTO");
  EXPECT_STREQ(get_engine_name(flashback::FlashbackEngineType::NONE), "NONE");
  EXPECT_STREQ(get_engine_name(flashback::FlashbackEngineType::UNDO), "UNDO");
  EXPECT_STREQ(get_engine_name(flashback::FlashbackEngineType::BINLOG),
               "BINLOG");
}

/* 测试: 确保所有 FlashbackError 枚举值可在 switch 中正确处理 */
TEST_F(FlashbackSwitchCoverageTest,
       当对所有FlashbackError进行switch时应正确处理) {
  auto get_error_name = [](flashback::FlashbackError e) -> const char * {
    switch (e) {
      case flashback::FlashbackError::NONE:
        return "NONE";
      case flashback::FlashbackError::GENERIC:
        return "GENERIC";
      case flashback::FlashbackError::OUT_OF_WINDOW:
        return "OUT_OF_WINDOW";
      case flashback::FlashbackError::BINLOG_EXPIRED:
        return "BINLOG_EXPIRED";
      case flashback::FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL:
        return "BINLOG_ROW_IMAGE_NOT_FULL";
      case flashback::FlashbackError::DDL_INCOMPATIBLE:
        return "DDL_INCOMPATIBLE";
      case flashback::FlashbackError::INTERRUPTED:
        return "INTERRUPTED";
    }
    return "UNKNOWN";
  };

  EXPECT_STREQ(get_error_name(flashback::FlashbackError::NONE), "NONE");
  EXPECT_STREQ(get_error_name(flashback::FlashbackError::GENERIC), "GENERIC");
  EXPECT_STREQ(get_error_name(flashback::FlashbackError::OUT_OF_WINDOW),
               "OUT_OF_WINDOW");
  EXPECT_STREQ(get_error_name(flashback::FlashbackError::BINLOG_EXPIRED),
               "BINLOG_EXPIRED");
  EXPECT_STREQ(
      get_error_name(flashback::FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL),
      "BINLOG_ROW_IMAGE_NOT_FULL");
  EXPECT_STREQ(get_error_name(flashback::FlashbackError::DDL_INCOMPATIBLE),
               "DDL_INCOMPATIBLE");
  EXPECT_STREQ(get_error_name(flashback::FlashbackError::INTERRUPTED),
               "INTERRUPTED");
}

}  // namespace flashback_types_unittest
