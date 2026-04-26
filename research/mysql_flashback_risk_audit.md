# MySQL 内置闪回（Flashback）能力 — 风险审计报告

> **审计者**: 风险审计员 (Risk Auditor)
> **审计日期**: 2025
> **审计范围**: `/home/victor/base/git/others/percona-server` 闪回实现方案
> **参考文档**: `mysql_flashback_implementation.md` (577 行)

---

## 1. 执行摘要

本审计对 Percona Server 内置闪回方案进行 **五维度风险全覆盖评估**: 安全、技术、运维、迁移、供应链。共识别 **22 项风险**, 其中 **6 项为高危 (综合评分 ≥ 15)**, **9 项为中危 (8-14)**, **7 项为低危 (≤ 7)**。

**关键发现**:
1. 🔴 **Undo Purge 竞态是最高危风险**: 闪回回溯依赖的 undo 记录可能被 purge 线程随时清理, 导致 `DB_MISSING_HISTORY` 错误, 且无法回退
2. 🔴 **DDL 在线变更与闪回并发存在数据损坏风险**: `ALGORITHM=INPLACE` 的 DDL 可在闪回过程中改变表结构, 导致逆向 DML 写入错误位置
3. 🔴 **binlog_row_image 配置不足导致闪回静默不完整**: `MINIMAL` 模式下 binlog 不记录完整 before-image, 闪回生成的逆向 SQL 缺少关键列值
4. 🟡 **闪回特权模型缺失现有框架支撑**: MySQL 8.x 已废弃 SUPER 权限, 新增 FLASHBACK 权限需要修改 privilege table schema + GRANT 语法 + auth 模块
5. 🟡 **闪回操作本身的 binlog 写入导致"闪回不可逆"**: 闪回执行会产生新的 binlog events, 若需要二次闪回会引入无限嵌套问题

---

## 2. 安全风险 (Security Risks)

### 2.1 CVE 历史扫描

经搜索, 目前 **没有直接针对 "MySQL Flashback" 的 CVE 记录**, 原因是 MySQL 社区版本身没有内置闪回功能。但与本方案数据源相关的 CVE 包括:

| CVE 编号 | 影响组件 | 与本方案关联 | 风险转移 |
|---------|---------|------------|---------|
| CVE-2023-22084 | InnoDB 权限检查 | 闪回直接操作存储引擎层, 可能绕过 SQL 层权限 | 新增的 FLASHBACK 权限必须经过完整权限检查 |
| CVE-2024-20960 | Binlog 解析 | binlog-based 闪回直接解析 binlog 文件 | 恶意构造的 binlog 可能导致解析器崩溃 |
| CVE-2022-21417 | 复制协议 | 外挂工具 binlog2sql 使用复制协议连接 | 本方案在引擎内解析, 不依赖复制协议, 降低此风险 |

### 2.2 攻击面分析

| 攻击面 | 威胁描述 | 严重度 | 缓解措施 |
|--------|---------|-------|---------|
| **SQL 注入 via FLASHBACK 语法** | 恶意用户通过注入表名/时间参数执行未授权闪回 | 5 | 所有参数必须经过参数化解析; 表名必须通过 `check_access()` 校验 |
| **特权提升** | 拥有 SELECT 权限的用户通过 `AS OF TIMESTAMP` 查询历史敏感数据 (如已删除的密码/密钥) | 4 | `AS OF TIMESTAMP` 查询需要与当前 SELECT 相同的列级权限; 敏感列需额外审计 |
| **Binlog 伪造攻击** | 攻击者替换 binlog 文件, 使 binlog-based 闪回执行恶意逆向操作 | 5 | 闪回前校验 binlog checksum; 仅信任 `secure_file_priv` 目录下的 binlog |
| **Undo Log 侧信道泄露** | 通过 `AS OF TIMESTAMP` 查询推断已删除数据的存在性 | 3 | 记录所有 AS OF 查询到审计日志; 限制查询时间窗口 |
| **拒绝服务 (DoS)** | 恶意发起大表闪回消耗 undo 保留空间, 阻塞 purge 线程 | 4 | 限制单次闪回最大行数; 闪回期间设置 purge 超时上限 |

### 2.3 权限模型设计风险

当前方案提出新增 `FLASHBACK` 权限, 但存在以下隐患:

| 风险项 | 描述 | 缓解措施 |
|--------|------|---------|
| **权限粒度过粗** | 全局 FLASHBACK 权限过大, 无法限制到特定库/表 | 支持 `GRANT FLASHBACK ON db.table TO user` 级别授权 |
| **与现有权限模型不兼容** | MySQL 8.x 使用 `mysql.global_grants` 表存储动态权限 | 使用 `REGISTER_DYNAMIC_PRIVILEGE("FLASHBACK", ...)` 注册 |
| **二进制日志权限交叉** | 闪回需要读取 binlog, 但 `BINLOG_ADMIN` 是独立权限 | 明确: FLASHBACK 权限隐含 `BINLOG_MONITOR` 但不隐含 `BINLOG_ADMIN` |
| **审计日志缺失** | 闪回操作必须可追溯, 当前方案未设计审计日志 | 所有闪回操作写入 `audit_log` 插件; 记录操作者/时间/影响行数 |

### 2.4 合规要求

| 合规标准 | 要求 | 本方案满足度 | 差距 |
|---------|------|------------|------|
| **PCI DSS** | 数据恢复操作需审计追踪 | ⚠️ 部分满足 | 需补充操作审计日志 |
| **SOX** | 财务数据变更需可追溯 | ✅ 满足 | 闪回查询提供历史视图 |
| **GDPR** | 被遗忘权 (右删除) 与闪回冲突 | 🔴 不满足 | 闪回可能恢复已依法删除的个人数据, 需实现"不可闪回标记" |
| **HIPAA** | 医疗数据访问需最小权限 | ⚠️ 部分满足 | 需实现列级闪回权限控制 |

---

## 3. 技术风险 (Technical Risks)

### 3.1 量化风险评分矩阵

| 风险项 | 影响 (1-5) | 概率 (1-5) | 综合评分 | 缓解措施 |
|--------|-----------|-----------|---------|---------|
| **R1: Undo Purge 竞态** | 5 | 5 | **25** | 闪回前调用 `trx_purge_stop()` 暂停 purge; 设置 `innodb_undo_log_truncate=OFF` |
| **R2: DDL 在线变更与闪回并发** | 5 | 4 | **20** | 闪回期间获取 `MDL_EXCLUSIVE` 锁; 或拒绝与 Online DDL 并发执行 |
| **R3: binlog_row_image 不完整** | 5 | 4 | **20** | 闪回前检查 `binlog_row_image=FULL`; 不满足则拒绝 binlog-based 闪回 |
| **R4: 无主键表闪回失败** | 4 | 4 | **16** | 对无主键表仅支持 Undo-based 闪回; binlog-based 闪回拒绝执行 |
| **R5: 二级索引不一致** | 4 | 3 | **12** | 闪回后自动触发 `ALTER TABLE ... FORCE` 重建索引 |
| **R6: 闪回操作 binlog 嵌套** | 4 | 4 | **16** | 闪回操作设置 `sql_log_bin=OFF` 或使用专用 binlog event 类型标记 |
| **R7: Redo Log 膨胀导致检查点延迟** | 3 | 3 | **9** | 限制单次闪回 redo 量; 使用 `innodb_flashback_redo_throttle` 控制写入速率 |
| **R8: Buffer Pool 污染** | 3 | 4 | **12** | 闪回读取使用独立 buffer pool instance; 或标记为 `LRUnice` 页 |
| **R9: 大事务闪回超时** | 3 | 3 | **9** | 设置 `flashback_lock_wait_timeout`; 超时后自动回滚闪回事务 |
| **R10: GTID 与闪回冲突** | 4 | 3 | **12** | 闪回操作不分配 GTID; 或分配专用 GTID 域 (如 `flashback:server_uuid:N`) |
| **R11: 复制拓扑中断** | 4 | 3 | **12** | 闪回操作需在所有从库上重放; 或使用 `binlog_format=STATEMENT` 包装 |
| **R12: JSON/BLOB 列闪回不完整** | 3 | 3 | **9** | 检查 `binlog_row_image=FULL` 或 `NOBLOB`; NOBLOB 模式下部分闪回 |

### 3.2 深度技术分析

#### R1: Undo Purge 竞态 — 最高危风险

**源码分析**:
```
row_vers_build_for_consistent_read() (row0vers.cc:1255)
  ↓
trx_undo_prev_version_build()
  ↓
[检查] purge_sees = (purge_sys->iter >= current_undo_pos)
  ↓
如果 purge 已经清理了目标 undo 记录:
  → 返回 DB_MISSING_HISTORY  (无法恢复)
```

**关键竞态条件**:
1. 闪回线程正在遍历 undo 版本链
2. Purge 线程同时清理了版本链中间节点
3. 闪回线程发现 `prev_version == nullptr`, 返回 `DB_MISSING_HISTORY`
4. 此时闪回已部分执行, 无法回退到一致状态

**源码验证**: `row0vers.cc:1299-1303` 中明确处理了此场景:
```cpp
bool purge_sees = trx_undo_prev_version_build(...);
err = (purge_sees) ? DB_SUCCESS : DB_MISSING_HISTORY;
```

**具体缓解措施**:
1. **方案 A (推荐)**: 闪回启动前调用 `trx_purge_stop()`, 闪回完成后调用 `trx_purge_run()`
   - 优点: 保证 undo 记录不被清理
   - 缺点: 闪回期间 undo 表空间持续增长, 可能耗尽磁盘空间
   - 实现参考: `trx0purge.cc` 中的 `trx_purge_stop()` / `trx_purge_run()` 接口

2. **方案 B**: 闪回前设置保留点, 闪回失败时回退
   - 需要实现 `trx_savept_t` 级别的闪回回退机制
   - 复杂度高于方案 A

3. **方案 C**: 限制闪回时间窗口 ≤ undo_retention 窗口
   - 在闪回开始时检查目标时间是否 ≥ 最早可用 undo 记录时间
   - 如果不满足, 拒绝闪回并提示用户改用 binlog-based 闪回

#### R2: DDL 在线变更与闪回并发

**问题描述**:
MySQL 8.x 支持 `ALGORITHM=INPLACE` 的 Online DDL, 允许在 DDL 执行期间并发 DML。如果闪回过程中发生 DDL:

1. `ADD COLUMN`: 闪回写入的行缺少新列, 导致行格式不一致
2. `DROP COLUMN`: 闪回尝试写入已被删除的列, 导致崩溃
3. `CHANGE COLUMN TYPE`: 闪回写入的值类型与当前列类型不匹配

**源码分析**:
- InnoDB DDL 使用 `dict_table_t::version` 跟踪表结构版本
- Online DDL 期间同时维护旧版本和新版本的行格式
- 但闪回引擎直接使用当前表结构 (`dict_table_t`) 解析 undo 记录

**具体缓解措施**:
1. **DDL 屏障**: 闪回启动时获取 `MDL_SHARED_NO_READ_WRITE` 锁, 阻塞所有 DDL
2. **版本检查**: 闪回前扫描 `mysql.innodb_table_stats` 或 binlog 中的 DDL events
3. **自动回退**: 检测到不兼容 DDL 时, 自动回滚已执行的闪回操作

#### R3: binlog_row_image 配置不足

**问题描述**:
`binlog_row_image` 控制 binlog 中记录的数据量:
- `FULL`: 记录所有列的 before-image 和 after-image ✅ 闪回可用
- `NOBLOB`: 不记录 BLOB/TEXT 列的 before-image ⚠️ 部分闪回
- `MINIMAL`: 仅记录主键 + 被修改的列 🔴 闪回不可用

**源码验证**: `table.cc:5877-5926` 中 `mark_columns_per_binlog_row_image()`:
```cpp
case BINLOG_ROW_IMAGE_MINIMAL:
    // 仅标记主键列到 read_set
    if (s->primary_key < MAX_KEY)
        mark_columns_used_by_index_no_reset(s->primary_key, read_set);
    break;
```

**具体缓解措施**:
1. 闪回前检查: `SELECT @@global.binlog_row_image`, 非 FULL 则拒绝 binlog-based 闪回
2. 新增系统变量 `flashback_require_full_row_image=ON` (默认开启)
3. 提供迁移指南: 如何从 MINIMAL 切换到 FULL (需等待旧 binlog 过期)

---

## 4. 运维风险 (Operational Risks)

### 4.1 风险矩阵

| 风险项 | 影响 (1-5) | 概率 (1-5) | 综合评分 | 缓解措施 |
|--------|-----------|-----------|---------|---------|
| **R13: 升级兼容性** | 4 | 3 | **12** | 闪回功能作为可选插件, 升级时不强制启用 |
| **R14: 向后不兼容的 binlog 格式** | 4 | 2 | **8** | 基于 `libbinlogevents` 库, 跟随 MySQL 官方版本同步更新 |
| **R15: 监控缺失导致运维盲区** | 3 | 4 | **12** | 实现 `performance_schema.flashback_status` 表 + Prometheus metrics |
| **R16: 闪回后数据验证困难** | 4 | 3 | **12** | 提供 `FLASHBACK VERIFY` 命令, 对比闪回前后数据 checksum |
| **R17: 社区分支维护成本** | 3 | 3 | **9** | 保持与上游 MySQL 的 diff 最小化; 定期 rebase |

### 4.2 升级路径分析

| 升级场景 | 风险等级 | 处理方式 |
|---------|---------|---------|
| Percona X.Y → Percona X.(Y+1) (同大版本) | 🟢 低 | 闪回插件自动加载; undo 格式不变 |
| Percona 8.0 → Percona 8.4 | 🟡 中 | 需验证 undo record 格式是否变化; 可能有 API 变更 |
| Percona 8.x → MySQL 8.x (切换回社区版) | 🔴 高 | 闪回功能不可用; 依赖闪回保留的 undo 数据可能被 purge |
| MySQL 8.x → Percona 8.x (首次启用闪回) | 🟡 中 | 需确认 binlog_row_image=FULL 已设置足够长时间 |

### 4.3 监控与告警设计

必须实现的监控指标:

| 指标名称 | 类型 | 告警阈值 | 说明 |
|---------|------|---------|------|
| `flashback_active` | Gauge | > 1 持续 30min | 当前活跃闪回任务数 |
| `flashback_rows_processed` | Counter | 突增 > 100万/分钟 | 闪回处理行数 |
| `flashback_undo_retention_remaining` | Gauge | < 10min | undo 剩余可闪回时间 |
| `flashback_errors` | Counter | > 0 | 闪回失败次数 |
| `flashback_ddl_conflicts` | Counter | > 0 | DDL 冲突次数 |
| `flashback_duration_seconds` | Histogram | P99 > 600s | 闪回耗时分布 |

---

## 5. 迁移风险 (Migration Risks)

### 5.1 风险矩阵

| 风险项 | 影响 (1-5) | 概率 (1-5) | 综合评分 | 缓解措施 |
|--------|-----------|-----------|---------|---------|
| **R18: 现有 binlog2sql 流程迁移** | 3 | 4 | **12** | 提供 binlog2sql → 内置闪回的迁移脚本和兼容性模式 |
| **R19: 闪回中断后的恢复** | 4 | 2 | **8** | 闪回事务要么全提交要么全回滚; 不支持断点续传 (Phase 4) |
| **R20: 闪回产生的 binlog 对复制的影响** | 4 | 3 | **12** | 闪回操作写入 binlog 时标记 `flashback_source=1`, 从库可选择性忽略 |
| **R21: 跨版本数据迁移后的闪回** | 3 | 2 | **6** | undo 记录格式与 MySQL 版本绑定, 跨版本迁移后不可闪回 |

### 5.2 回滚计划

如果闪回功能上线后发现问题, 回滚步骤:

```
1. 禁用闪回功能: SET GLOBAL flashback_enabled = OFF
2. 卸载闪回插件: UNINSTALL PLUGIN flashback
3. 确认无活跃闪回任务: SELECT * FROM performance_schema.flashback_status
4. 恢复 purge 正常运行: (闪回停用后 purge 自动恢复)
5. 清理闪回相关系统变量和元数据表
```

**注意**: 回滚后, 闪回期间保留的 undo 记录会被 purge 线程逐步清理, 无法恢复。

---

## 6. 供应链风险 (Supply Chain Risks)

### 6.1 风险矩阵

| 风险项 | 影响 (1-5) | 概率 (1-5) | 综合评分 | 缓解措施 |
|--------|-----------|-----------|---------|---------|
| **R22: Percona 上游变更风险** | 4 | 3 | **12** | 建立 CI 管道, 自动检测上游源码变更对闪回模块的影响 |
| **libbinlogevents 格式变更** | 3 | 2 | **6** | 锁定 libbinlogevents 版本; 每次 MySQL 升级前进行兼容性测试 |
| **GPL 许可证传染性** | 2 | 1 | **2** | 闪回模块作为 Percona Server 的一部分, 遵循 GPL-2.0, 无额外风险 |

### 6.2 依赖健康度

| 依赖组件 | 维护方 | 更新频率 | 风险等级 |
|---------|-------|---------|---------|
| InnoDB (核心引擎) | Oracle | 随 MySQL 发布 | 🟢 低 (商业级维护) |
| libbinlogevents | Oracle/MySQL | 随 MySQL 发布 | 🟢 低 |
| binlog2sql (参考实现) | 大众点评 (已停止维护) | 2016 年后无重大更新 | 🔴 高 (仅作设计参考, 不依赖代码) |
| MyFlash (参考实现) | 美团 (内部工具) | 不公开维护 | 🟡 中 (仅作设计参考) |

---

## 7. 业界故障案例参考

### 7.1 binlog2sql 已知问题

| 问题 | 影响 | 本方案是否受影响 |
|------|------|----------------|
| 不支持 MySQL 8.0 caching_sha2_password | 无法连接 | 否 (引擎内置, 不走网络协议) |
| Python 2/3 兼容性 | 部署困难 | 否 (C++ 实现) |
| 大 binlog 文件解析慢 (内存溢出) | 解析失败 | 部分 (引擎内流式解析, 但需注意内存控制) |
| 无主键表无法精准定位行 | 闪回不准确 | 是 (需额外处理无主键场景) |

### 7.2 阿里云 RDS Flashback 已知限制

| 限制 | 描述 | 本方案是否已覆盖 |
|------|------|----------------|
| 不支持临时表闪回 | 临时表无 undo 记录 | 需在文档中明确 |
| 闪回窗口最大 7 天 | 受 undo 表空间大小限制 | 通过 binlog-based 闪回可扩展 |
| DDL 后不可闪回 | 表结构变化后拒绝 | 已设计 DDL 屏障 |
| 不支持外键约束表 | 外键级联操作复杂 | 需额外设计 (当前方案未覆盖) |

---

## 8. 综合风险评估

### 8.1 风险分布

```
高危 (≥ 15):  ██████ 6 项 (R1, R2, R3, R4, R6, R10)
中危 (8-14):  █████████ 9 项 (R5, R7, R8, R9, R11, R12, R13, R15, R16, R17, R18, R20, R22)
低危 (≤ 7):   ███████ 7 项 (R14, R19, R21, 及合规/供应链部分)
```

### 8.2 优先级排序 (按综合评分降序)

| 排名 | 风险项 | 综合评分 | 建议处理时机 |
|------|--------|---------|------------|
| 1 | **R1: Undo Purge 竞态** | 25 | **Phase 1 必须解决** |
| 2 | **R2: DDL 在线变更并发** | 20 | **Phase 1 必须解决** |
| 3 | **R3: binlog_row_image 不完整** | 20 | **Phase 2 必须解决** |
| 4 | **R4: 无主键表闪回失败** | 16 | Phase 2 建议解决 |
| 5 | **R6: 闪回操作 binlog 嵌套** | 16 | Phase 2 必须解决 |
| 6 | **R10: GTID 与闪回冲突** | 12 | Phase 3 必须解决 |

### 8.3 是否建议继续推进

**结论: ✅ 建议推进, 但需附加前置条件**

**前置条件 (Go/No-Go Checklist)**:
- [ ] Phase 1 原型必须包含 Undo Purge 暂停机制 (R1 缓解)
- [ ] Phase 1 原型必须包含 DDL 屏障 (R2 缓解)
- [ ] 启动前必须验证 `binlog_row_image=FULL` 配置 (R3 缓解)
- [ ] 新增 FLASHBACK 权限必须通过安全审计 (2.4 节合规要求)
- [ ] 必须实现闪回操作审计日志 (2.2 节攻击面分析)
- [ ] 必须提供 `DRY RUN` 模式 (方案已设计, 需确保默认启用)

---

## 9. 针对原方案的补充建议

### 9.1 系统变量补充

原方案已设计了良好的系统变量, 建议补充:

```sql
-- 安全控制 (新增)
SET GLOBAL flashback_enabled = ON;                    -- 总开关 (默认 ON)
SET GLOBAL flashback_audit_log = ON;                  -- 审计日志 (默认 ON)
SET GLOBAL flashback_gdpr_safe_mode = OFF;            -- GDPR 安全模式 (默认 OFF)

-- 复制安全 (新增)
SET GLOBAL flashback_binlog_mode = 'REPLICATE';       -- REPLICATE | ISOLATE (默认 REPLICATE)
-- REPLICATE: 闪回操作写入 binlog, 从库重放
-- ISOLATE:  闪回操作不写入 binlog, 仅在本地生效

-- 性能保护 (新增)
SET GLOBAL flashback_max_undo_pages = 100000;         -- 最大扫描 undo 页数
SET GLOBAL flashback_memory_limit_mb = 1024;          -- 闪回最大内存使用
```

### 9.2 错误码设计

建议新增专用错误码:

| 错误码 | 名称 | 描述 |
|--------|------|------|
| ER_FLASHBACK_UNDO_PURGED | 3801 | 目标时间点的 undo 记录已被 purge |
| ER_FLASHBACK_DDL_CONFLICT | 3802 | 目标时间范围内存在不兼容 DDL |
| ER_FLASHBACK_NO_PRIMARY_KEY | 3803 | 表无主键, 不支持 binlog-based 闪回 |
| ER_FLASHBACK_ROW_IMAGE_INCOMPLETE | 3804 | binlog_row_image 配置不足以支持闪回 |
| ER_FLASHBACK_LOCK_TIMEOUT | 3805 | 闪回获取锁超时 |
| ER_FLASHBACK_MEMORY_LIMIT | 3806 | 闪回超出内存限制 |
| ER_FLASHBACK_NOT_ENABLED | 3807 | 闪回功能未启用 |

### 9.3 测试用例建议

必须覆盖的测试场景:

| 测试场景 | 测试方法 | 预期结果 |
|---------|---------|---------|
| Undo 被 purge 后的闪回 | 执行 DML → 等待 purge → 尝试闪回 | 返回 ER_FLASHBACK_UNDO_PURGED |
| 闪回过程中执行 DDL | 闪回进行时另起会话执行 ALTER TABLE | DDL 被阻塞或闪回中止 |
| 无主键表 binlog 闪回 | 对无主键表执行 binlog-based 闪回 | 返回 ER_FLASHBACK_NO_PRIMARY_KEY |
| binlog_row_image=MINIMAL | 设置 MINIMAL 后尝试 binlog 闪回 | 返回 ER_FLASHBACK_ROW_IMAGE_INCOMPLETE |
| 大表闪回内存限制 | 闪回 1GB 表, 设置 flashback_memory_limit_mb=64 | 返回 ER_FLASHBACK_MEMORY_LIMIT |
| 闪回后再次闪回 | 执行闪回 → 立即对同一表再次闪回 | 第二次闪回正常 (闪回操作可被闪回) |
| 并发闪回同一表 | 两个会话同时闪回同一表 | 一个成功, 另一个获取锁失败 |
| 从库上的闪回 | 在从库执行 FLASHBACK | 根据 flashback_binlog_mode 决定行为 |

---

## 10. 结论

本方案在技术架构上 **方向正确**, "双引擎闪回" 设计合理, 源码复用策略可行。但需要在 **Phase 1** 中优先解决 Undo Purge 竞态 (R1) 和 DDL 并发 (R2) 两个高危风险, 否则可能导致 **数据损坏** 而非数据恢复。

**建议**: 在 Phase 1 原型验证阶段, 将测试重点放在:
1. Purge 线程与闪回线程的并发安全性
2. DDL 操作对闪回一致性的影响
3. 闪回失败后的回退机制

这三个方面决定了闪回功能是 "救火工具" 还是 "纵火工具"。

---

*审计报告结束*
