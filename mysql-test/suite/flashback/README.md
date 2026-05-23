# Flashback Suite — End-to-End Integration Tests

## 概述

本测试套件为 Percona Server 8.4.7-7 的 Flashback 功能提供端到端集成测试。

### 验收标准

根据 §9 Phase 1 验收标准：

```sql
SELECT * FROM t AS OF TIMESTAMP NOW() - INTERVAL 5 MINUTE
```

- **成功路径**: 返回正确历史数据
- **失败路径**: 超出窗口返回 `ER_FLASHBACK_TIMESTAMP_UNAVAILABLE`

## 测试文件

### MTR (MySQL Test Run) 测试

| 文件 | 描述 |
|------|------|
| `t/flashback_asof_e2e.test` | **AS OF TIMESTAMP 端到端集成测试** |
| `t/flashback_query.test` | 闪回查询基础功能测试 (TC-001, TC-004) |
| `t/flashback_table.test` | FLASHBACK TABLE 功能测试 (TC-002) |
| `t/flashback_dry_run.test` | DRY RUN 模式测试 |
| `t/flashback_ddl_barrier.test` | DDL 屏障测试 (TC-005) |
| `t/flashback_binlog.test` | Binlog 引擎测试 |
| `t/flashback_purge_race.test` | Purge 竞态测试 |
| `t/flashback_space_monitor.test` | 空间监控测试 |

### GUnit 单元测试

| 文件 | 描述 |
|------|------|
| `unittest/gunit/flashback_asof_e2e-t.cc` | **AS OF TIMESTAMP 单元测试** |
| `unittest/gunit/flashback_view_manager-t.cc` | ReadView 管理器测试 |
| `unittest/gunit/flashback_ddl_barrier-t.cc` | DDL 分类逻辑测试 |
| `unittest/gunit/flashback_checkpoint-t.cc` | 检查点测试 |
| `unittest/gunit/flashback_purge_guard-t.cc` | Purge 保护器 RAII 测试 |
| `unittest/gunit/flashback_types-t.cc` | 类型定义测试 |

## TC-E2E 测试用例

### TC-E2E-001: Happy Path
**描述**: 5分钟窗口内闪回查询返回正确历史数据

**验证**:
- 创建多版本数据 (初始 → 更新1 → 更新2)
- 使用 `AS OF TIMESTAMP` 回溯到任意中间版本
- 验证返回的历史数据正确

### TC-E2E-002: Error Path
**描述**: 超出窗口返回 `ER_FLASHBACK_TIMESTAMP_UNAVAILABLE`

**验证**:
- 设置极短保留窗口 (5秒)
- 等待窗口过期
- 验证返回预期错误码

### TC-E2E-003: 约束 C4
**描述**: binlog_row_image 验证

**验证**:
- 设置 `binlog_row_image=MINIMAL`
- 验证 Binlog 引擎不可用
- 恢复 `binlog_row_image=FULL`

### TC-E2E-004: 约束 C1
**描述**: 闪回操作不写入 binlog

**验证**:
- 记录 binlog 位置
- 执行 FLASHBACK TABLE
- 验证数据已恢复但 binlog 未记录闪回操作

### TC-E2E-005: 多版本数据
**描述**: 链式 UPDATE 的历史版本查询

**验证**:
- 创建 4 个版本 (V0 → V1 → V2 → V3)
- 分别回溯到每个版本
- 验证每个版本数据正确

### TC-E2E-006: 边界条件
**描述**: `NOW()-INTERVAL` 精确时间点测试

**验证**:
- 使用 `SELECT * FROM t AS OF TIMESTAMP NOW() - INTERVAL 30 SECOND`
- 验证语法正确解析和执行

### TC-E2E-007: 权限验证 (C6)
**描述**: 闪回查询权限与普通 SELECT 一致

**验证**:
- 创建无 SELECT 权限用户
- 验证闪回查询被拒绝
- 授予权限后验证查询成功

### TC-E2E-008: 并发安全
**描述**: 闪回查询不阻塞并发写入 (C9)

**验证**:
- 执行闪回查询
- 在闪回期间执行 UPDATE
- 验证 UPDATE 正常完成

## 约束覆盖

| 约束 | 描述 | 测试覆盖 |
|------|------|---------|
| C1 | 闪回操作不写入 binlog | TC-E2E-004 |
| C4 | binlog_row_image=FULL 验证 | TC-E2E-003 |
| C6 | 权限检查 | TC-E2E-007 |
| C9 | 不阻塞并发写入 | TC-E2E-008 |

## 运行测试

### MTR 测试

```bash
cd /home/victor/base/git/others/percona-server
./mysql-test/mysql-test-run.pl --suite=flashback --test-case=flashback_asof_e2e
```

### GUnit 测试

需要在 CMake 配置中启用单元测试：

```bash
cd /home/victor/base/git/others/percona-server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_UNIT_TESTS=ON
make -j$(nproc) flashback_asof_e2e-t
./unittest/gunit/flashback_asof_e2e-t
```

### 完整测试套件

```bash
./mysql-test/mysql-test-run.pl --suite=flashback
```

## 预期结果

所有测试通过时应输出：

```
# =====================================================================
# End-to-End Integration Test Summary
# =====================================================================
#
# TC-E2E-001: Happy Path — 闪回查询返回正确历史数据          [PASSED]
# TC-E2E-002: Error Path — 超出窗口返回错误                  [PASSED]
# TC-E2E-003: 约束 C4 — binlog_row_image 验证                [PASSED]
# TC-E2E-004: 约束 C1 — 闪回操作不写入 binlog               [PASSED]
# TC-E2E-005: 多版本数据 — 链式 UPDATE 历史版本查询         [PASSED]
# TC-E2E-006: 边界条件 — NOW()-INTERVAL 精确时间点测试      [PASSED]
# TC-E2E-007: 权限验证 (C6) — 闪回查询权限与 SELECT 一致     [PASSED]
# TC-E2E-008: 并发安全 — 闪回查询不阻塞并发写入 (C9)        [PASSED]
#
# 验收标准达成情况:
#   ✓ SELECT * FROM t AS OF TIMESTAMP NOW() - INTERVAL 5 MINUTE
#     返回正确历史数据
#   ✓ 超出窗口返回 ER_FLASHBACK_TIMESTAMP_UNAVAILABLE
#
```

## 设计参考

- **DESIGN.md §3.1**: 闪回查询数据流
- **DESIGN.md §9**: Phase 1 验收标准
- **DESIGN.md §5.2**: 错误码定义
- **flashback_errors.h**: 错误码声明
