# Flashback 功能代码审查报告

> **审查员**: Evaluator (只读模式)
> **审查日期**: 2025
> **设计参考**: mysql_flashback_implementation_v2.md (§13.1-13.4, §6)
> **审查范围**: /home/victor/base/git/others/percona-server 中所有 flashback 相关文件

---

## 一、总评

| 维度 | 得分 (0-10) | 判定 |
|------|-------------|------|
| **correctness** | 4 | ⛔ BLOCKER — 存在符号重复定义 |
| **completeness** | 5 | ⚠️ 多处 TODO 未完成，语法层缺失 |
| **security** | 5 | ⚠️ 权限模型不完整，DDL 屏障检查未实现 |
| **code_quality** | 7 | ✅ 代码结构清晰，注释充分 |
| **design_alignment** | 5 | ⚠️ 多处接口与设计文档不一致 |
| **pass** | **false** | ⛔ BLOCKER 存在 |

---

## 二、BLOCKER 问题

### BLOCKER-1: `row_build_flashback_version` 符号重复定义

**位置**: 
- `storage/innobase/row/row0vers.cc:1532` — 完整实现
- `storage/innobase/flashback/flashback_undo_engine.cc:32` — 骨架 stub

**问题**: 两个文件定义了同名的全局函数 `row_build_flashback_version`，
会导致 **multiple definition linker error**。

**严重性**: 编译失败，整个项目无法链接。

**建议修复方案**:
- 方案 A: 删除 `flashback_undo_engine.cc` 中的 stub，该行文件只声明不包含实现
- 方案 B: 将 `flashback_undo_engine.cc` 中的实现放入 namespace 并重命名

---

## 三、Correctness 详细分析

### C-1: `row_build_flashback_version` 重复定义 (BLOCKER, 已列于上方)

### C-2: `FlashbackPurgeGuard` 空实现

**位置**: `storage/innobase/flashback/flashback_purge_guard.cc` (仅 11 行)

**问题**: 文件只有空注释，`FlashbackPurgeGuard()` 和 `~FlashbackPurgeGuard()` 没有定义体。
设计文档 (§7.3) 明确要求 `trx_purge_stop()` / `trx_purge_run()` 调用。

**影响**: 编译链接失败 (未定义符号)，或运行时 flashabck 期间 purge 继续运行导致 `DB_MISSING_HISTORY`。

### C-3: `FlashbackScheduler::execute_undo_flashback` 和 `execute_binlog_flashback` 为空骨架

**位置**: `sql/flashback_scheduler.cc:515-581`

**问题**: 两个引擎执行方法仅填充了 `result.tables_processed`，实际闪回逻辑全是 TODO 注释。

**影响**: 闪回操作不会实际修改任何数据，但会返回成功。

### C-4: 竞态条件 — 闪回查询未暂停 Purge

**位置**: `storage/innobase/handler/ha_innodb.cc:11687-11797` (`rnd_next_flashback`)

**问题**: `rnd_next_flashback` 在遍历表记录时没有调用 `FlashbackPurgeGuard` 保护。
如果 Purge 线程在遍历期间清理了 undo 记录，后续记录会返回 `DB_MISSING_HISTORY`，
导致闪回查询中途失败 (部分结果已返回给客户端)。

**影响**: 不一致的闪回结果，违反原子性。

### C-5: `validate_binlog_row_image()` 永远返回 true

**位置**: `sql/flashback_scheduler.cc:466-482`

**问题**: 函数内有 TODO 注释，实际总是返回 `true`，不检查实际的 `binlog_row_image` 值。
这违反了设计文档中 C4 约束 (Binlog 闪回必须检查 row image)。

---

## 四、Completeness 详细分析

### COMP-1: SQL 语法解析层缺失 (高优先级)

**设计文档要求**: `sql/sql_yacc.yy` 新增 FLASHBACK_SYM, VERSIONS_SYM, TRX_ID_SYM 关键字
及 flashback_statement 语法规则 (§5.1.2-5.1.3)。

**实际状态**: `sql/sql_yacc.yy` 中 **没有任何** flashback 相关语法。
没有关键字注册，没有语法规则。

**影响**: `FLASHBACK TABLE ...` 和 `FLASHBACK TRANSACTION ...` 等 SQL 语句无法被解析器识别。
用户无法使用 flashback 功能。**这是功能无法拉起的关键阻塞项。**

### COMP-2: `AS OF TIMESTAMP` 和 `VERSIONS BETWEEN` 语法缺失

**设计文档要求**: 扩展现有 SELECT 语法，支持 `table_name AS OF TIMESTAMP` 和
`table_name VERSIONS BETWEEN` (§5.1.3)。

**实际状态**: 未在 sql_yacc.yy 中实现。

**影响**: 闪回查询 (Flashback Query) 和闪回版本查询不可用。

### COMP-3: 错误消息未注册

**位置**: `sql/share/errmsg-utf8.txt`

**问题**: 设计文档要求新增 ER_FLASHBACK_* 错误消息。
虽然 `build/mysqld_error.h` 生成了错误码 (ER_FLASHBACK_UNDO_PURGED 3801 ~ ER_FLASHBACK_TABLE_NOT_FOUND 3810)，
但 errmsg-utf8.txt 中 **没有** 对应的错误消息文本。

**影响**: 运行时错误无法显示人类可读的错误消息。

### COMP-4: LEX 闪回字段缺失

**设计文档要求** (§5.1.5): LEX 结构新增 `flashback_query`, `flashback_versions`,
`flashback_timestamp` 等字段。

**实际状态**: `sql/sql_lex.h` 中 **没有** 这些字段。

**注意**: 实现采用了更优的方案 — 通过 Sql_cmd 类直接处理，
不依赖 LEX 扩展字段。这是 **合理偏差 (设计优化)**，
但 `sql_yacc.yy` 中仍需要对应语法规则来触发这些 Sql_cmd。

### COMP-5: FlashbackPurgeGuard 空实现 (已在 C-2 中记录)

### COMP-6: `trx_sys_find_trx_id_by_timestamp` 实现不完整

**位置**: `storage/innobase/trx/trx0sys.cc:866`

**问题**: 只实现了目标时间在未来和边界检查的框架逻辑，
实际的 trx_id 估算逻辑 (线性插值或历史记录查找) 未实现。

---

## 五、Security 详细分析

### SEC-1: 权限检查不足

**位置**: `sql/sql_flashback.cc:95-127` (`check_flashback_privilege`)

**问题**: 
- 闪回表仅检查 `DROP_ACL` 权限。设计文档风险审计 (risk_audit.md §2.3) 
  要求新增独立的 `FLASHBACK` 动态权限，支持 `GRANT FLASHBACK ON db.table TO user`。
- `FLASHBACK TRANSACTION` 使用 `SUPER_ACL`，但 MySQL 8.x 已废弃 SUPER 权限，
  应使用 `REGISTER_DYNAMIC_PRIVILEGE`。

**风险**: 权限粒度过粗，与 MySQL 8.x 最小权限原则不兼容。

### SEC-2: DDL 屏障检查未实现

**位置**: `sql/flashback_ddl_barrier.cc:113-141` (`DDLBarrier::check`)

**问题**: 该函数只做了参数校验，然后直接返回 `FlashbackError::NONE`。
实际的 binlog 扫描和 DDL 检查逻辑全是 TODO 注释。

**风险**: 闪回期间如果存在不兼容 DDL (DROP TABLE, TRUNCATE, DROP COLUMN 等)，
闪回操作不会被阻止，可能导致数据损坏。

### SEC-3: `sql_log_bin=OFF` 的线程安全问题

**位置**: `sql/flashback_binlog_engine.cc:531-532`

**问题**: 直接修改 `m_thd->variables.option_bits` 关闭 binlog。
但 `variables` 是会话级变量，如果事务跨多个语句，状态恢复可能不完整。

**风险**: 闪回操作可能意外地被复制到从库，或在异常退出时未恢复 binlog 状态。

### SEC-4: 无审计日志

**问题**: 设计文档风险审计 (risk_audit.md §2.3) 要求所有闪回操作写入审计日志。
当前实现没有审计日志集成。

---

## 六、Design Alignment 偏差检测

| 设计项 | 设计文档要求 | 实际实现 | 偏差类型 |
|--------|-------------|---------|---------|
| SQL 命令枚举 | `SQLCOM_FLASHBACK_TABLE/QUERY/VERSIONS/TRANSACTION` | ✅ 已实现 (`my_sqlcommand.h:216-219`) | 无偏差 |
| Yacc 语法 | 新增 FLASHBACK_SYM 等关键字 + flashback_statement 规则 | ❌ 未实现 | **错误偏差** |
| PT_flashback_table | `class PT_flashback_table : public Parse_tree_root` | ✅ 已实现 (`parse_tree_nodes.h:5776`) | 无偏差 |
| Sql_cmd_flashback_table | `class Sql_cmd_flashback_table : public Sql_cmd` | ✅ 已实现 (`sql_flashback.h:65`) | 无偏差 |
| FlashbackScheduler | `select_engine()`, `run_safety_checks()`, `execute()` | ✅ 接口一致，实现部分 TODO | 合理偏差 (阶段开发) |
| FlashbackRequest | struct with type/engine/target_time/tables 等字段 | ✅ `flashback_types.h` 完整定义 | 无偏差 |
| FlashbackResult | struct with state/error/engine_used/rows 等字段 | ✅ `flashback_types.h` 完整定义 | 无偏差 |
| BinlogFlashbackEngine | `execute()`, `reverse_rows_event()`, `find_position_at_timestamp()` | ✅ 接口完整，reverse 方法部分实现 | 合理偏差 (阶段开发) |
| UndoFlashbackEngine | `execute_query()`, `execute_table()`, `execute_dry_run()` | ✅ 头文件完整定义 | 合理偏差 (阶段开发) |
| FlashbackPurgeGuard | RAII: 构造时 `trx_purge_stop()`，析构时 `trx_purge_run()` | ❌ 空实现 | **错误偏差** |
| LEX flashback 字段 | `flashback_query`, `flashback_timestamp` 等 | ❌ 未添加 | **合理偏差** (替代方案更优) |
| row_build_flashback_version | 使用 `row_vers_build_for_consistent_read()` 复用 | ✅ `row0vers.cc:1532` 完整实现 | 无偏差 |
| trx_sys_find_trx_id_by_timestamp | 扫描 trx_sys 映射时间戳到 trx_id | ⚠️ 框架实现，估算逻辑未完善 | 合理偏差 |
| 系统参数 (7个) | `innodb_flashback_retention_seconds` 等 | ✅ `flashback_sysvars.cc` 完整定义 | 无偏差 |
| sys_var 注册 | 通过 `MYSQL_SYSVAR_*` 注册到 MySQL | ❌ 仅有全局变量，未注册 sys_var | **合理偏差** (阶段开发) |
| Performance Schema | `performance_schema.flashback_status` 表 | ❌ 未实现 | 合理偏差 (Phase 4) |
| 错误码注册 | ER_FLASHBACK_* 错误码 + errmsg 文本 | ⚠️ 错误码已生成，errmsg 未添加 | **错误偏差** |

---

## 七、数据流审查

### 7.1 闪回表数据流 (FLASHBACK TABLE)

```
SQL Parser (yacc) ← ❌ 缺失 → 语法无法解析
       ↓ (假设语法已实现)
PT_flashback_table::make_cmd() → Sql_cmd_flashback_table
       ↓
Sql_cmd_flashback_table::execute()
  ├── check_flashback_privilege()    ← ✅ 检查 DROP_ACL
  ├── 表达式求值 (timestamp/trx_id)   ← ✅
  ├── 构建 FlashbackRequest          ← ✅
  └── FlashbackScheduler::execute()
        ├── select_engine()          ← ✅ 引擎选择逻辑完整
        ├── run_safety_checks()      ← ⚠️ DDL check 未实现
        ├── acquire_exclusive_lock() ← ✅ MDL 锁获取完整
        └── execute_undo_flashback() ← ❌ TODO 骨架
              或
            execute_binlog_flashback() ← ❌ TODO 骨架 (BinlogFlashbackEngine 未集成)
```

**判定**: 数据流在 `execute_undo_flashback` / `execute_binlog_flashback` 处断裂。

### 7.2 闪回查询数据流 (SELECT ... AS OF TIMESTAMP)

```
SQL Parser ← ❌ 缺失 → AS OF TIMESTAMP 语法无法解析
       ↓
(跳过: 缺少解析入口)
       ↓ (假设已解析)
ha_innobase::rnd_next()
  └── if (m_flashback_mode) → rnd_next_flashback()
        ├── 打开 pcur 扫描聚集索引 ← ✅
        ├── row_build_flashback_version() ← ✅ 完整实现 (复用 row_vers_build_for_consistent_read)
        └── row_sel_store_mysql_rec() ← ✅
```

**判定**: InnoDB 层数据流完整，但缺少从 SQL 层到 handler 层的触发路径。

### 7.3 Binlog 闪回数据流

```
BinlogFlashbackEngine::execute()
  ├── check_row_image_compatibility() ← ✅ 完整
  ├── ddl_barrier.check()             ← ⚠️ 空实现
  ├── 关闭 binlog (option_bits)       ← ✅
  ├── find_position_at_timestamp()    ← ✅ 完整实现
  ├── Binlog_file_reader 扫描事件     ← ✅ 完整
  ├── reverse_rows_event()            ← ⚠️ 骨架 SQL 模板
  ├── 执行逆向 SQL                    ← ❌ TODO
  └── 恢复 binlog 状态                ← ✅
```

**判定**: Binlog 定位和事件读取逻辑完整，但逆向 SQL 执行和实际数据恢复未完成。

---

## 八、循环依赖检查

```
sql/flashback_types.h         → 无依赖 (基础类型)
sql/flashback_sysvars.h       → 无外部依赖
sql/flashback_ddl_barrier.h   → flashback_types.h
sql/flashback_scheduler.h     → flashback_ddl_barrier.h, flashback_types.h
sql/flashback_binlog_engine.h → flashback_types.h, flashback_ddl_barrier.h
sql/sql_flashback.h           → 无 flashback 循环依赖
sql/parse_tree_nodes.h        → sql_flashback.h (单向)
sql/sql_parse.cc              → 无循环

InnoDB 层:
flashback_purge_guard.h       → 无外部依赖
flashback_undo_engine.h       → InnoDB 内部头 (无循环)
row0vers.h                    → InnoDB 内部头
ha_innodb.h                   → flashback_undo_engine.h (单向)
trx0sys.h                     → 无循环
```

**判定**: ✅ 无循环依赖。依赖图是单向的、分层的。

---

## 九、错误处理覆盖 (E1-E4)

设计文档中的错误处理要求:

| 错误场景 | 设计要求 | 实现状态 | 覆盖度 |
|---------|---------|---------|-------|
| **E1: Undo Purged** | 返回 ER_FLASHBACK_UNDO_PURGED | ✅ `row0vers.cc` 返回 DB_MISSING_HISTORY → `HA_ERR_RECORD_DELETED` | 部分 (错误码映射) |
| **E2: DDL Incompatible** | 返回 ER_FLASHBACK_DDL_CONFLICT | ❌ DDLBarrier::check() 未实现 | 未覆盖 |
| **E3: No Primary Key** | 返回 ER_FLASHBACK_NO_PRIMARY_KEY | ⚠️ 错误码已定义，无检查逻辑 | 未覆盖 |
| **E4: Out of Window** | 返回 ER_FLASHBACK_RANGE_INVALID | ✅ `select_engine()` 返回 NONE, `run_safety_checks()` 返回 OUT_OF_WINDOW | 已覆盖 |

**其他错误处理**:
- 权限不足 → ✅ `check_access` + `ER_TABLEACCESS_DENIED_ERROR`
- 表达式求值失败 → ✅ 返回 `ER_INTERNAL_ERROR`
- 内存分配失败 → ✅ `ER_OUTOFMEMORY`
- 加锁失败 → ✅ 清理部分锁 + 返回错误
- Binlog 文件不存在 → ✅ `FlashbackError::BINLOG_EXPIRED`
- 用户中断 → ✅ `m_thd->killed` 检查 + `FlashbackError::INTERRUPTED`

---

## 十、与约束清单 (C1-C10) 的对照

| 约束 | 描述 | 遵守情况 |
|------|------|---------|
| **C1** | 闪回操作不写入 binlog | ✅ `flashback_binlog_engine.cc` 中 `option_bits &= ~OPTION_BIN_LOG` |
| **C2** | 闪回前必须暂停 Purge | ❌ `FlashbackPurgeGuard` 空实现 |
| **C3** | 闪回查询使用 MVCC 一致性读 | ✅ `row_build_flashback_version()` 复用 `row_vers_build_for_consistent_read()` |
| **C4** | Binlog 闪回需检查 binlog_row_image=FULL | ✅ `check_row_image_compatibility()` 完整实现 |
| **C5** | 闪回前必须检查 DDL 屏障 | ⚠️ 接口存在但 `check()` 空实现 |
| **C6** | 完善的错误处理 | ⚠️ 错误路径存在，但 DDL/NoPK 未覆盖 |
| **C7** | Binlog 事务边界处理 (GTID/XID) | ⚠️ `execute()` 中 switch 处理了事件类型，但未处理 GTID/XID 边界 |
| **C8** | 无主键表仅支持 Undo 闪回 | ❌ 无检查逻辑 |
| **C9** | 闪回使用 MDL_EXCLUSIVE 锁 | ✅ `DDLBarrier::acquire_exclusive_lock()` 完整实现 |
| **C10** | 编译通过，无未定义符号 | ❌ `FlashbackPurgeGuard` 未定义体，`row_build_flashback_version` 重复 |

---

## 十一、总结与建议

### 当前状态判定: **不可拉起**

主要原因:
1. **语法层完全缺失** — 没有 yacc 语法规则，用户无法执行任何 flashback SQL
2. **链接错误** — `row_build_flashback_version` 重复定义 + `FlashbackPurgeGuard` 空实现
3. **核心执行逻辑为空** — `execute_undo_flashback` 和 `execute_binlog_flashback` 仅填充计数

### 优先级排序的修复建议

| 优先级 | 任务 | 预计工作量 |
|--------|------|-----------|
| P0 (BLOCKER) | 删除 `flashback_undo_engine.cc` 中 `row_build_flashback_version` stub | 10 分钟 |
| P0 (BLOCKER) | 实现 `FlashbackPurgeGuard` 的 `trx_purge_stop/run` 调用 | 1 小时 |
| P0 (BLOCKER) | 在 `sql_yacc.yy` 中新增 FLASHBACK_SYM 关键字和 flashback_statement 语法规则 | 4-6 小时 |
| P1 | 实现 `execute_undo_flashback()` 的完整表扫描和版本恢复逻辑 | 1-2 天 |
| P1 | 实现 `DDLBarrier::check()` 的 binlog 扫描逻辑 | 1-2 天 |
| P2 | 在 errmsg-utf8.txt 中注册 ER_FLASHBACK_* 错误消息 | 30 分钟 |
| P2 | 实现 `reverse_write/delete/update_event` 的完整逆向 SQL 生成 | 1 天 |
| P2 | 集成 `BinlogFlashbackEngine` 到 `FlashbackScheduler::execute_binlog_flashback` | 2 小时 |
| P3 | 注册 sys_var 系统变量 (而非全局变量) | 2 小时 |
| P3 | 新增 FLASHBACK 动态权限 | 半天 |
| P3 | 实现无主键表检查 (C8) | 1 小时 |
