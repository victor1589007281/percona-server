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
  @file unittest/gunit/flashback_ddl_barrier-t.cc
  @brief 单元测试: DDLBarrier 的 classify_ddl() 和 DDL 兼容性判断逻辑

  测试覆盖:
  - classify_ddl(): DDL 精确分类，返回 DdlType 枚举
  - is_ddl_compatible(): 各类 DDL 语句的兼容性判断
  - 边界条件: null 指针、空字符串
  - 不兼容 DDL: DROP TABLE, TRUNCATE, DROP COLUMN, CHANGE COLUMN, MODIFY COLUMN
  - 兼容 DDL: ADD INDEX, DROP INDEX, RENAME INDEX, ADD COLUMN

  注意: DDLBarrier 的 check() 和 acquire_exclusive_lock() 需要真实的 THD
  上下文，因此本测试仅覆盖 classify_ddl() 和 is_ddl_compatible() 这两个纯判断逻辑。
*/

#include <gtest/gtest.h>

#include <cstring>

#include "sql/flashback_ddl_barrier.h"
#include "sql/flashback_types.h"

namespace flashback_ddl_barrier_unittest {

using flashback::DdlType;

// =====================================================================
// 测试组: classify_ddl() — DDL 精确分类
// =====================================================================

struct ClassifyTestCase {
  const char *ddl_sql;
  DdlType expected_type;
  const char *reason;
};

class ClassifyDdlTest : public ::testing::TestWithParam<ClassifyTestCase> {};

/* 测试: classify_ddl() 应正确分类各类 DDL 语句 */
TEST_P(ClassifyDdlTest, classify_ddl应返回预期类型) {
  const auto &tc = GetParam();
  EXPECT_EQ(flashback::DDLBarrier::classify_ddl(tc.ddl_sql), tc.expected_type)
      << tc.reason;
}

INSTANTIATE_TEST_SUITE_P(
    DdlClassification, ClassifyDdlTest,
    ::testing::Values(
        /* DROP_TABLE */
        ClassifyTestCase{"DROP TABLE t1", DdlType::DROP_TABLE,
                         "DROP TABLE 应分类为 DROP_TABLE"},
        ClassifyTestCase{"DROP TABLE IF EXISTS t1", DdlType::DROP_TABLE,
                         "DROP TABLE IF EXISTS 应分类为 DROP_TABLE"},
        ClassifyTestCase{"drop table t1", DdlType::DROP_TABLE,
                         "小写 drop table 也应分类为 DROP_TABLE"},

        /* TRUNCATE */
        ClassifyTestCase{"TRUNCATE TABLE t1", DdlType::TRUNCATE,
                         "TRUNCATE TABLE 应分类为 TRUNCATE"},
        ClassifyTestCase{"TRUNCATE t1", DdlType::TRUNCATE,
                         "TRUNCATE 应分类为 TRUNCATE"},

        /* DROP_COLUMN */
        ClassifyTestCase{"ALTER TABLE t1 DROP COLUMN col1", DdlType::DROP_COLUMN,
                         "DROP COLUMN 应分类为 DROP_COLUMN"},

        /* CHANGE_COLUMN */
        ClassifyTestCase{
            "ALTER TABLE t1 CHANGE COLUMN old_name new_name INT NOT NULL",
            DdlType::CHANGE_COLUMN, "CHANGE COLUMN 应分类为 CHANGE_COLUMN"},

        /* MODIFY_COLUMN */
        ClassifyTestCase{"ALTER TABLE t1 MODIFY COLUMN col1 VARCHAR(255)",
                         DdlType::MODIFY_COLUMN,
                         "MODIFY COLUMN 应分类为 MODIFY_COLUMN"},

        /* ADD_INDEX */
        ClassifyTestCase{"ALTER TABLE t1 ADD INDEX idx_name (col1)",
                         DdlType::ADD_INDEX,
                         "ADD INDEX 应分类为 ADD_INDEX"},
        ClassifyTestCase{"ALTER TABLE t1 ADD UNIQUE INDEX idx_unique (col1)",
                         DdlType::ADD_INDEX,
                         "ADD UNIQUE INDEX 应分类为 ADD_INDEX"},
        ClassifyTestCase{"ALTER TABLE t1 ADD PRIMARY KEY (id)",
                         DdlType::ADD_INDEX,
                         "ADD PRIMARY KEY 应分类为 ADD_INDEX"},

        /* DROP_INDEX */
        ClassifyTestCase{"ALTER TABLE t1 DROP INDEX idx_name",
                         DdlType::DROP_INDEX,
                         "DROP INDEX 应分类为 DROP_INDEX"},

        /* RENAME */
        ClassifyTestCase{"RENAME TABLE t1 TO t2", DdlType::RENAME,
                         "RENAME TABLE 应分类为 RENAME"},
        ClassifyTestCase{"ALTER TABLE t1 RENAME TO t2", DdlType::RENAME,
                         "ALTER TABLE RENAME TO 应分类为 RENAME"},
        ClassifyTestCase{"ALTER TABLE t1 RENAME INDEX old_idx TO new_idx",
                         DdlType::RENAME,
                         "RENAME INDEX 应分类为 RENAME"},

        /* OTHER */
        ClassifyTestCase{"ALTER TABLE t1 ADD COLUMN new_col INT DEFAULT 0",
                         DdlType::OTHER,
                         "ADD COLUMN 应分类为 OTHER"},
        ClassifyTestCase{"CREATE TABLE t1 (id INT PRIMARY KEY)", DdlType::OTHER,
                         "CREATE TABLE 应分类为 OTHER"},
        ClassifyTestCase{"ALTER TABLE t1 COMMENT='updated comment'",
                         DdlType::OTHER,
                         "修改表注释应分类为 OTHER"},

        /* UNKNOWN */
        ClassifyTestCase{nullptr, DdlType::UNKNOWN, "nullptr 应分类为 UNKNOWN"},
        ClassifyTestCase{"", DdlType::UNKNOWN, "空字符串应分类为 UNKNOWN"},
        ClassifyTestCase{"SELECT * FROM t1", DdlType::UNKNOWN,
                         "非 DDL 语句应分类为 UNKNOWN"}),
    [](const testing::TestParamInfo<ClassifyTestCase> &info) {
      std::string name = info.param.reason;
      for (auto &c : name) {
        if (c == ' ' || c == '(' || c == ')' || c == '/') c = '_';
      }
      return name;
    });

// =====================================================================
// 测试 Fixture: 提供 is_ddl_compatible 的访问接口
// =====================================================================

class TestableDDLBarrier : public flashback::DDLBarrier {
 public:
  // 构造函数接受一个 nullptr THD，仅用于暴露 is_ddl_compatible 方法
  TestableDDLBarrier() : flashback::DDLBarrier(nullptr) {}

  // 暴露私有方法
  bool test_is_ddl_compatible(const char *ddl_sql) const {
    return is_ddl_compatible(ddl_sql);
  }
};

// =====================================================================
// 测试组: 边界条件
// =====================================================================

class DDLBarrierBoundaryTest : public ::testing::Test {
 protected:
  TestableDDLBarrier barrier;
};

/* 测试: 当 DDL SQL 为 nullptr 时应返回 true (兼容) */
TEST_F(DDLBarrierBoundaryTest, 当DDL_SQL为nullptr时应视为兼容) {
  EXPECT_TRUE(barrier.test_is_ddl_compatible(nullptr));
}

/* 测试: 当 DDL SQL 为空字符串时应返回 true (兼容) */
TEST_F(DDLBarrierBoundaryTest, 当DDL_SQL为空字符串时应视为兼容) {
  EXPECT_TRUE(barrier.test_is_ddl_compatible(""));
}

// =====================================================================
// 测试组: Table-driven 测试 — 不兼容 DDL
// =====================================================================

struct IncompatibleDDLTestCase {
  const char *ddl_sql;
  const char *reason;
};

class DDLBarrierIncompatibleTest
    : public ::testing::TestWithParam<IncompatibleDDLTestCase> {};

/* 测试: 各类不兼容 DDL 应返回 false */
TEST_P(DDLBarrierIncompatibleTest, 当DDL不兼容时应返回false) {
  TestableDDLBarrier barrier;
  const auto &tc = GetParam();
  EXPECT_FALSE(barrier.test_is_ddl_compatible(tc.ddl_sql)) << tc.reason;
}

INSTANTIATE_TEST_SUITE_P(
    IncompatibleDDLs, DDLBarrierIncompatibleTest,
    ::testing::Values(
        IncompatibleDDLTestCase{"DROP TABLE t1", "DROP TABLE 应不兼容"},
        IncompatibleDDLTestCase{"DROP TABLE IF EXISTS t1",
                                "DROP TABLE IF EXISTS 应不兼容"},
        IncompatibleDDLTestCase{"TRUNCATE TABLE t1",
                                "TRUNCATE TABLE 应不兼容"},
        IncompatibleDDLTestCase{"TRUNCATE t1", "TRUNCATE 应不兼容"},
        IncompatibleDDLTestCase{"ALTER TABLE t1 DROP COLUMN col1",
                                "DROP COLUMN 应不兼容"},
        IncompatibleDDLTestCase{
            "ALTER TABLE t1 DROP COLUMN col1, DROP COLUMN col2",
            "多个 DROP COLUMN 应不兼容"},
        IncompatibleDDLTestCase{"ALTER TABLE t1 CHANGE COLUMN old_name "
                                "new_name INT NOT NULL",
                                "CHANGE COLUMN 应不兼容"},
        IncompatibleDDLTestCase{
            "ALTER TABLE t1 MODIFY COLUMN col1 VARCHAR(255)",
            "MODIFY COLUMN 应不兼容"},
        /* RENAME 现在也视为不兼容（表名变化导致闪回目标不存在） */
        IncompatibleDDLTestCase{"RENAME TABLE t1 TO t2",
                                "RENAME TABLE 应不兼容"},
        IncompatibleDDLTestCase{"ALTER TABLE t1 RENAME TO t2",
                                "ALTER TABLE RENAME TO 应不兼容"}),
    [](const testing::TestParamInfo<IncompatibleDDLTestCase> &info) {
      // 生成有意义的测试名
      std::string name = info.param.reason;
      // 移除空格和特殊字符
      for (auto &c : name) {
        if (c == ' ' || c == '(' || c == ')') c = '_';
      }
      return name;
    });

// =====================================================================
// 测试组: Table-driven 测试 — 兼容 DDL
// =====================================================================

struct CompatibleDDLTestCase {
  const char *ddl_sql;
  const char *reason;
};

class DDLBarrierCompatibleTest
    : public ::testing::TestWithParam<CompatibleDDLTestCase> {};

/* 测试: 各类兼容 DDL 应返回 true */
TEST_P(DDLBarrierCompatibleTest, 当DDL兼容时应返回true) {
  TestableDDLBarrier barrier;
  const auto &tc = GetParam();
  EXPECT_TRUE(barrier.test_is_ddl_compatible(tc.ddl_sql)) << tc.reason;
}

INSTANTIATE_TEST_SUITE_P(
    CompatibleDDLs, DDLBarrierCompatibleTest,
    ::testing::Values(
        CompatibleDDLTestCase{"ALTER TABLE t1 ADD INDEX idx_name (col1)",
                              "ADD INDEX 应兼容"},
        CompatibleDDLTestCase{
            "ALTER TABLE t1 ADD UNIQUE INDEX idx_unique (col1)",
            "ADD UNIQUE INDEX 应兼容"},
        CompatibleDDLTestCase{"ALTER TABLE t1 DROP INDEX idx_name",
                              "DROP INDEX 应兼容"},
        CompatibleDDLTestCase{"ALTER TABLE t1 RENAME INDEX old_idx TO new_idx",
                              "RENAME INDEX 应兼容"},
        CompatibleDDLTestCase{"ALTER TABLE t1 ADD COLUMN new_col INT DEFAULT 0",
                              "ADD COLUMN 应兼容"},
        CompatibleDDLTestCase{
            "ALTER TABLE t1 ADD COLUMN new_col VARCHAR(64) NULL",
            "ADD COLUMN NULL 应兼容"},
        CompatibleDDLTestCase{"ALTER TABLE t1 COMMENT='updated comment'",
                              "修改表注释应兼容"},
        /* 词边界修复: truncate_col 不应被误判为 TRUNCATE */
        CompatibleDDLTestCase{
            "ALTER TABLE t1 ADD COLUMN truncate_col INT",
            "列名含 truncate 不应误判"}),
    [](const testing::TestParamInfo<CompatibleDDLTestCase> &info) {
      std::string name = info.param.reason;
      for (auto &c : name) {
        if (c == ' ' || c == '(' || c == ')') c = '_';
      }
      return name;
    });

// =====================================================================
// 测试组: 边界/模糊匹配测试
// =====================================================================

class DDLBarrierEdgeCaseTest : public ::testing::Test {
 protected:
  TestableDDLBarrier barrier;
};

/* 测试: 包含 DROP TABLE 子串的复杂 SQL 应返回 false */
TEST_F(DDLBarrierEdgeCaseTest,
       当SQL包含DROP_TABLE子串时应视为不兼容) {
  EXPECT_FALSE(barrier.test_is_ddl_compatible(
      "/* comment */ DROP TABLE t1 /* end comment */"));
}

/* 测试: DROP INDEX 不应被误判为 DROP COLUMN */
TEST_F(DDLBarrierEdgeCaseTest,
       当DDL为DROP_INDEX时不应被误判为DROP_COLUMN) {
  EXPECT_TRUE(
      barrier.test_is_ddl_compatible("ALTER TABLE t1 DROP INDEX idx_name"));
}

/* 测试: 小写 DROP TABLE 也应被正确识别（classify_ddl 内部转为大写） */
TEST_F(DDLBarrierEdgeCaseTest,
       当DDL为小写drop_table时应正确识别为不兼容) {
  EXPECT_FALSE(barrier.test_is_ddl_compatible("drop table t1"));
}

/* 测试: 混合大小写也应被正确识别 */
TEST_F(DDLBarrierEdgeCaseTest,
       当DDL为混合大小写Drop_Table时应正确识别) {
  EXPECT_FALSE(barrier.test_is_ddl_compatible("Drop Table t1"));
}

/* 测试: check() 方法在无表参数时应返回 GENERIC 错误 */
TEST_F(DDLBarrierEdgeCaseTest, 当check无表参数时应返回GENERIC错误) {
  flashback::FlashbackRequest request;
  request.tables = nullptr;
  request.table_count = 0;

  // check() 是 stub 实现，但参数校验部分已实现
  EXPECT_EQ(barrier.check(request), flashback::FlashbackError::GENERIC);
}

}  // namespace flashback_ddl_barrier_unittest
