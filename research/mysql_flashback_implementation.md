# MySQL 内置闪回（Flashback）能力实现方案

> **源码基础**: `/home/victor/base/git/others/percona-server` (Percona Server / MySQL 8.x)
> **分析范围**: Undo Log / Binlog / Row Version / Transaction Rollback 源码 + 业界闪回方案对比
> **日期**: 2025

---

## 1. Executive Summary

- **MySQL 社区版当前无内置闪回能力**。业界方案（美团 MyFlash、大众点评 binlog2sql、阿里云 RDS Flashback）均依赖 **binlog row image** 做逆向 SQL 生成，属于**外挂工具**而非**引擎内置**。
- **Percona Server 源码中存在实现闪回的全部基础构件**: Undo Log 版本链 (`trx0undo.cc`/`row0vers.cc`/`trx0rec.h`)、Binlog Row Event 解析器 (`binlog_reader.cc`/`rows_event.h`)、事务回滚引擎 (`trx0roll.cc`)。关键缺口在于：**没有统一的 SQL 层入口将 undo/binlog 逆向生成 DML**。
- **最佳实现路径是"双引擎闪回"**: Undo Log 用于**秒级精准闪回**（未 purge 的 undo 记录，分钟级窗口），Binlog 用于**长窗口闪回**（小时/天级窗口），两者通过统一 SQL 语法 `FLASHBACK TABLE ... TO TIMESTAMP` 对外暴露。
- **核心风险**: Undo Log 被 Purge 线程清理后不可恢复；DDL 操作导致表结构变化使闪回不可行；二级索引一致性维护复杂。需要通过 **DDL 屏障**、**闪回保留策略** 和 **元数据版本快照** 来解决。

---

## 2. Technical Analysis

### 2.1 源码基础：Percona Server 中的可用构件

#### 2.1.1 Undo Log 体系

| 源码文件 | 关键功能 | 闪回用途 |
|---------|---------|---------|
| `storage/innobase/trx/trx0undo.cc` | Undo Log 的创建、扩展、截断 | Undo 记录解析入口 |
| `storage/innobase/trx/trx0rec.cc` | Undo Record 的解析 (`trx_undo_rec_get_*`) | 提取 old value / new value |
| `storage/innobase/row/row0vers.cc` | MVCC 版本链遍历 (`row_vers_build_for_consistent_read`) | **核心**: 沿 undo 链回溯到目标时间点 |
| `storage/innobase/row/row0undo.cc` | Undo 操作执行 (`row_undo`, `row_undo_mod`) | 逆向 INSERT/UPDATE/DELETE 逻辑 |
| `storage/innobase/include/trx0rec.h` | `trx_undo_rec_get_type()`, `trx_undo_rec_get_undo_no()` | 区分 TRX_UNDO_INSERT_REC / TRX_UNDO_UPD_EXIST_REC / TRX_UNDO_DEL_MARK_REC |
| `storage/innobase/trx/trx0roll.cc` | 事务回滚全流程 (`trx_rollback_to_savepoint_low`) | 闪回=按时间裁剪的 rollback |

**Undo Record 结构** (关键发现):
```
trx_undo_rec_t = {
    type: 1 byte          -- TRX_UNDO_INSERT_REC | TRX_UNDO_UPD_EXIST_REC | TRX_UNDO_DEL_MARK_REC
    cmpl_info: 1 byte     -- 编译信息
    table_id: 8 bytes     -- 表 ID
    undo_no: variable     -- Undo 序号 (事务内递增)
    type_cmpl: 1 byte     -- 类型补充
    info_bits: 1 byte     -- 信息位
    n_fields: variable    -- 字段数
    [old_values...]       -- 修改前的值 (UPDATE 场景)
    [row_ref...]          -- 行引用 (主键定位)
    [secondary index info] -- 二级索引信息 (用于 purge)
}
```

#### 2.1.2 Binlog 体系

| 源码文件 | 关键功能 | 闪回用途 |
|---------|---------|---------|
| `sql/binlog_reader.cc` | Binlog 文件顺序读取 | 解析历史 binlog |
| `sql/log_event.h` | Log Event 类定义 | Rows_event 类型识别 |
| `libbinlogevents/.../rows_event.h` | `Rows_event` 类 (Write_rows/Update_rows/Delete_rows) | **核心**: 逆向 DML 的数据源 |
| `sql/binlog.cc` | Binlog 写入逻辑 | 确认 row image 格式 |

**Binlog Row Event 逆向规则**:

| 原始 Event | 逆向 Event | 数据来源 |
|-----------|-----------|---------|
| Write_rows_event (INSERT) | Delete_rows_event | before_image (需 `binlog_row_image=FULL`) |
| Update_rows_event (UPDATE) | Update_rows_event | before_image ↔ after_image 互换 |
| Delete_rows_event (DELETE) | Write_rows_event | before_image (需 `binlog_row_image=FULL`) |

#### 2.1.3 Clone Plugin 可借鉴的架构模式

参考已有的 `mysql_clone_analysis.md`, Clone Plugin 的分层架构可直接复用:

```
SQL 层 (flashback_handler.cc)          ← 统一入口: FLASHBACK TABLE 语法
    ↓
Plugin 层 (plugin/flashback/)           ← 闪回任务管理、状态追踪、权限控制
    ↓
InnoDB 集成层 (storage/innobase/flashback/)
    ├── fb0api.cc:     闪回 API 入口
    ├── fb0undo.cc:    Undo-based 闪回 (短窗口)
    ├── fb0binlog.cc:  Binlog-based 闪回 (长窗口)
    └── fb0apply.cc:   逆向 DML 执行引擎
```

### 2.2 业界方案对比分析

#### 2.2.1 方案总览

| 方案 | 数据源 | 实现层级 | 闪回窗口 | 精度 | 性能 | 局限性 |
|------|-------|---------|---------|------|------|-------|
| **美团 MyFlash** | Binlog | 外挂 C++ 工具 | binlog 保留期 | 事务级 | 高 (二进制解析) | 需 `binlog_format=ROW`, `binlog_row_image=FULL`; 不支持 DDL |
| **大众点评 binlog2sql** | Binlog | 外挂 Python 工具 | binlog 保留期 | 事务级 | 中 (Python 解析) | MySQL 8.0 兼容性差; 依赖 replication protocol |
| **阿里云 RDS Flashback** | Undo Log + Binlog | 引擎内置 (修改版) | 7 天 (可配置) | 行级 | 高 (引擎内) | 仅阿里云 RDS 提供, 不开源 |
| **Oracle Flashback Query** | Undo Log | 引擎内置 | undo_retention | 行级 | 高 | Oracle 专有, AS OF SCN 语法 |
| **本方案 (内置双引擎)** | Undo + Binlog | 引擎内置 | Undo(分钟) + Binlog(天) | 行级 | 高 | 需修改 Percona 源码 |

#### 2.2.2 共识与分歧

**共识点**:
1. 所有方案都依赖 **ROW 格式 binlog** 或 **Undo Log** 获取变更前数据
2. 都需要 `binlog_row_image=FULL` (或等价物) 保证有完整 before-image
3. DDL 操作是闪回的最大障碍, 需要结构版本管理
4. 逆向执行必须保证事务顺序 (从后往前处理事务)

**分歧点**:
1. **数据源选择**: MyFlash/binlog2sql 认为 binlog 是唯一可靠来源; 阿里云认为 undo 更适合短窗口精准闪回
2. **执行方式**: 外挂工具生成 SQL 让用户手动执行 vs 引擎内直接执行逆向操作
3. **粒度控制**: binlog2sql 支持按库/表/时间/事务过滤; MyFlash 支持按主键过滤

### 2.3 推荐架构：双引擎闪回

#### 2.3.1 整体架构

```
┌─────────────────────────────────────────────────────────────────────┐
│                        SQL Layer                                     │
│  FLASHBACK TABLE t1 TO TIMESTAMP '2025-01-01 10:00:00';             │
│  FLASHBACK TABLE t1 TO BINLOG 'mysql-bin.000003' @ 4567;             │
│  FLASHBACK DATABASE TO TIMESTAMP '2025-01-01 10:00:00'               │
│         DATABASES=db1,db2 TABLES=t1,t2,t3;                           │
│  SELECT * FROM t1 AS OF TIMESTAMP '2025-01-01 10:00:00';  -- 闪回查询│
├─────────────────────────────────────────────────────────────────────┤
│                     Flashback Handler                                │
│  sql/flashback_handler.h / .cc                                       │
│  ├── 语法解析 (YACC)                                                 │
│  ├── 权限校验 (FLASHBACK privilege)                                  │
│  ├── 时间 → (binlog_pos / undo_trx_id) 映射                         │
│  └── DDL 冲突检测                                                    │
├─────────────────────────────────────────────────────────────────────┤
│                    Flashback Engine                                  │
│  storage/innobase/flashback/                                         │
│  ├── fb0api.cc         — 统一 API: innodb_flashback_begin/apply/end  │
│  ├── fb0undo.cc        — Undo-based 闪回 (≤ undo_retention 窗口)     │
│  │   ├── 沿 trx history list 回溯                                   │
│  │   ├── row0vers.cc: 版本链遍历                                     │
│  │   └── 按 trx 逆序构建逆向 DML                                     │
│  ├── fb0binlog.cc      — Binlog-based 闪回 (长窗口)                  │
│  │   ├── binlog_reader.cc: 逆向读取 binlog                           │
│  │   ├── Rows_event 解析 & 逆向转换                                 │
│  │   └── 按 GTID 逆序回放                                            │
│  └── fb0apply.cc       — 逆向 DML 执行引擎                           │
│      ├── 主键定位 + 直接页级修改 (类似 row_undo_mod)                 │
│      ├── 二级索引维护                                               │
│      └── Redo Log 写入 (保证崩溃恢复)                               │
├─────────────────────────────────────────────────────────────────────┤
│                     Metadata Service                                 │
│  ├── 表结构版本快照 (DDL history)                                    │
│  ├── 闪回任务状态 (performance_schema.flashback_status)              │
│  └── Undo 保留策略管理                                              │
└─────────────────────────────────────────────────────────────────────┘
```

#### 2.3.2 核心模块设计

**模块一: SQL 语法扩展**

```sql
-- 闪回表到指定时间点
FLASHBACK TABLE table_name [, table_name ...]
    TO { TIMESTAMP 'timestamp' | BINLOG 'binlog_name' @ position }
    [WHERE primary_key_condition];

-- 闪回查询 (Oracle 风格)
SELECT * FROM table_name AS OF TIMESTAMP 'timestamp';
SELECT * FROM table_name AS OF SCN scn_number;

-- 闪回数据库 (多表批量)
FLASHBACK DATABASE
    [DATABASES db1, db2, ...]
    [TABLES t1, t2, ...]
    TO TIMESTAMP 'timestamp'
    [DRY RUN];  -- 仅预览，不执行
```

**模块二: Undo-based 闪回 (短窗口, 秒级精度)**

```
算法流程:
1. 定位目标时间点对应的事务号 (trx_id)
2. 从当前数据页出发, 沿 undo 版本链回溯:
   current_row → DB_ROLL_PTR → undo_rec → old_row → ...
3. 对每个需要闪回的行:
   a. 如果 undo 链中存在目标时间点的版本 → 直接恢复
   b. 如果行在目标时间点不存在 (闪回后是 INSERT) → 生成 DELETE
   c. 如果行在目标时间点存在但当前已删除 → 恢复旧版本
4. 逆向执行: 按 trx 逆序 (最新事务先回滚)
5. 写入 Redo Log, 保证崩溃恢复
```

关键源码复用:
- `row_vers_build_for_consistent_read()` → 构建目标时间点的一致读视图
- `trx_undo_get_prev_rec()` → 沿 undo 链向前遍历
- `row_undo_mod()` → 逆向 UPDATE 操作
- `row_undo_ins()` → 逆向 INSERT 操作
- `row_undo_del()` → 逆向 DELETE 操作

**模块三: Binlog-based 闪回 (长窗口, 事务级精度)**

```
算法流程:
1. 根据目标时间定位起始 binlog 文件和 position
2. 从当前 binlog position 向后退读 (或从起始点顺序读后反向处理)
3. 对每个 Rows_event:
   a. Write_rows  → 生成逆向 Delete_rows (用 before_image)
   b. Update_rows → 交换 before/after image, 生成逆向 Update_rows
   c. Delete_rows → 生成逆向 Write_rows (用 before_image)
4. 按 GTID 逆序执行逆向事件
5. 跳过 DDL 事件, 遇到 DDL 时中止并告警
```

关键源码复用:
- `Binlog_event_data_istream` → binlog 文件读取
- `Rows_event` 类 → event 解析
- `Table_map_event` → 获取表结构元信息
- `Log_event::write()` → 逆向 event 的序列化

**模块四: 逆向 DML 执行引擎**

```
设计原则:
1. 不通过 SQL 层 (避免重复解析/优化开销)
2. 直接操作存储引擎 (类似 row_undo_* 的方式)
3. 每条逆向操作都写入 Redo Log (保证持久性)
4. 支持批量执行 (减少锁竞争)

执行流程:
FLASHBACK_APPLY:
  ├── innodb_flashback_apply_begin()  -- 开启闪回事务
  ├── for each flashback_op:
  │   ├── row_flashback_ins()  -- 逆向 INSERT (实际执行 DELETE)
  │   ├── row_flashback_upd()  -- 逆向 UPDATE (交换 old/new)
  │   └── row_flashback_del()  -- 逆向 DELETE (实际执行 INSERT)
  └── innodb_flashback_apply_end()  -- 提交闪回事务
```

#### 2.3.3 关键数据结构

```cpp
// 闪回任务描述符
struct Flashback_Descriptor {
    enum Type { UNDO_BASED, BINLOG_BASED } type;
    
    // 时间范围
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    
    // 目标对象
    std::vector<table_id_t> table_ids;
    
    // Undo-based 参数
    trx_id_t        start_trx_id;
    trx_id_t        end_trx_id;
    
    // Binlog-based 参数
    std::string     start_binlog;
    uint64_t        start_position;
    std::string     end_binlog;
    uint64_t        end_position;
    
    // 过滤条件
    std::vector<dtuple_t*> pk_filters;  // 按主键过滤
    
    // 状态
    Flashback_State state;  // INIT / RUNNING / DONE / ERROR / ABORTED
    uint64_t        rows_processed;
    uint64_t        rows_total;
};

// 闪回操作 (单行级别)
struct Flashback_Op {
    enum Op_Type { INSERT, UPDATE, DELETE } op_type;
    table_id_t      table_id;
    dtuple_t*       pk_tuple;       // 主键定位
    dtuple_t*       old_values;     // 变更前值
    dtuple_t*       new_values;     // 变更后值
    trx_id_t        source_trx_id;  // 来源事务号
    undo_no_t       undo_no;        // Undo 序号 (用于排序)
};
```

#### 2.3.4 系统变量

```sql
-- 闪回保留策略
SET GLOBAL flashback_retention_undo = 3600;        -- Undo 保留秒数 (默认 1 小时)
SET GLOBAL flashback_retention_binlog = 604800;    -- Binlog 保留秒数 (默认 7 天)

-- 性能控制
SET GLOBAL flashback_max_concurrency = 4;          -- 并发线程数 (默认 4)
SET GLOBAL flashback_batch_size = 1000;            -- 批量处理行数 (默认 1000)
SET GLOBAL flashback_auto_rollback_on_ddl = ON;    -- 遇 DDL 自动回滚闪回 (默认 ON)

-- 安全控制
SET GLOBAL flashback_dry_run_default = OFF;        -- 默认 dry run 模式 (默认 OFF)
SET GLOBAL flashback_require_backup_lock = ON;     -- 闪回前获取备份锁 (默认 ON)
```

#### 2.3.5 DDL 处理策略

**问题**: 闪回期间如果表结构发生变化 (ADD COLUMN, DROP COLUMN, CHANGE TYPE), 闪回的数据无法适配新结构。

**解决方案**:

```
DDL 屏障机制:
1. 闪回启动时记录当前表结构版本 (table_version_id)
2. 扫描目标时间到当前时间的所有 DDL binlog event
3. 如果存在不兼容 DDL:
   a. 阻塞型 (DROP TABLE, TRUNCATE, CHANGE COLUMN TYPE) → 拒绝闪回
   b. 兼容型 (ADD COLUMN NULL, ADD INDEX) → 允许闪回, 新列填充 NULL
4. 可选: 闪回前自动备份当前表结构, 闪回后恢复

DDL 兼容性矩阵:
┌──────────────────────┬──────────┬───────────────────────────┐
│ DDL 类型              │ 可闪回?   │ 处理方式                   │
├──────────────────────┼──────────┼───────────────────────────┤
│ DROP TABLE           │ ❌ 不可   │ 直接拒绝                   │
│ TRUNCATE TABLE       │ ❌ 不可   │ 直接拒绝                   │
│ ADD COLUMN (NULL)    │ ✅ 可     │ 旧数据该列填充 NULL         │
│ ADD COLUMN (NOT NULL)│ ⚠️ 条件   │ 需指定默认值               │
│ DROP COLUMN          │ ❌ 不可   │ 数据丢失, 无法恢复          │
│ CHANGE COLUMN TYPE   │ ❌ 不可   │ 类型不兼容                 │
│ ADD INDEX            │ ✅ 可     │ 闪回后重建索引             │
│ DROP INDEX           │ ✅ 可     │ 无影响                     │
│ RENAME TABLE         │ ⚠️ 条件   │ 需映射新旧表名             │
└──────────────────────┴──────────┴───────────────────────────┘
```

---

## 3. Market Analysis

### 3.1 需求场景

| 场景 | 描述 | 优先级 | 适用方案 |
|------|------|--------|---------|
| **误删数据恢复** | DBA/开发误执行 DELETE/DROP | P0 | Undo-based (分钟级) |
| **误更新回滚** | UPDATE 忘记 WHERE 条件 | P0 | Undo-based (分钟级) |
| **逻辑错误修复** | 应用 bug 导致数据错误 | P1 | Binlog-based (小时级) |
| **审计追溯** | 查询某行历史状态 | P1 | AS OF TIMESTAMP 查询 |
| **合规要求** | 金融行业需保留数据变更历史 | P2 | Binlog-based + 归档 |
| **开发测试** | 恢复到某个测试数据状态 | P3 | Undo-based |

### 3.2 竞品对比

| 产品 | 闪回能力 | 实现方式 | 保留窗口 | 是否需要额外配置 |
|------|---------|---------|---------|----------------|
| **Oracle DB** | ✅ Flashback Query / Database / Table | Undo Log | undo_retention | 配置 undo tablespace |
| **SQL Server** | ✅ Temporal Tables | System-versioned table | 可配置 | 启用 system_versioning |
| **PostgreSQL** | ⚠️ 有限 (pg_rewind 仅用于主从) | WAL | WAL 保留期 | wal_level=logical |
| **MySQL 社区版** | ❌ 无内置 | — | — | — |
| **阿里云 RDS** | ✅ Flashback | 引擎内置 (修改版) | 7 天 | 自动开启 |
| **腾讯云 CDB** | ✅ 回档 | 物理备份 + binlog | 7 天 | 自动开启 |
| **Percona Server** | ❌ 无内置 | — | — | — |
| **本方案** | ✅ 内置双引擎闪回 | Undo + Binlog | 可配置 | 需开启 binlog_row_image=FULL |

### 3.3 市场机会

1. **填补 MySQL 社区版空白**: 目前只有云厂商提供闪回, 自建 MySQL 用户无内置方案
2. **合规驱动**: 金融、医疗等行业法规要求数据可追溯, 内置闪回可满足审计需求
3. **运维效率**: 相比当前 binlog2sql 方案, 内置闪回可将恢复时间从小时级降到分钟级
4. **Percona 差异化**: 成为 MySQL 社区分支中唯一提供内置闪回能力的产品

---

## 4. Risk Assessment

### 4.1 技术风险

| 风险 | 严重度 | 概率 | 缓解措施 |
|------|--------|------|---------|
| **Undo 被 Purge 清理** | 🔴 高 | 高 (默认 purge 很快) | 闪回期间暂停 purge; 延长 `flashback_retention_undo` |
| **DDL 导致结构不兼容** | 🔴 高 | 中 | DDL 屏障 + 结构版本快照; 拒绝不兼容闪回 |
| **二级索引不一致** | 🟡 中 | 中 | 闪回后重建索引; 或在闪回时同步维护 |
| **闪回期间新写入冲突** | 🟡 中 | 高 | 闪回期间阻塞 DML (或按主键粒度锁定) |
| **Redo Log 膨胀** | 🟡 中 | 中 | 闪回操作写入独立 redo 流; 限制批量大小 |
| **大表闪回耗时过长** | 🟡 中 | 高 | 支持按主键范围分批; 提供进度监控和中断恢复 |
| **Binlog 格式变更** | 🟢 低 | 低 | 基于 libbinlogevents 库, 跟随官方更新 |

### 4.2 性能风险

| 场景 | 影响 | 缓解措施 |
|------|------|---------|
| Undo-based 闪回 (100万行) | 约 1-5 分钟, 期间 undo 不可 purge | 限制最大闪回行数; 异步 purge |
| Binlog-based 闪回 (100万行) | 约 5-30 分钟, 需解析 binlog | 多线程并行解析; 批量执行 |
| 闪回期间正常业务查询 | 读不受影响; 写可能被阻塞 | 按主键粒度锁定, 非全表锁 |
| 闪回对 buffer pool 的影响 | 大量旧版本页加载到 buffer pool | 使用独立的 LRUnice 列表 |

### 4.3 数据安全风险

| 风险 | 严重度 | 概率 | 说明 |
|------|--------|------|------|
| 闪回操作误执行 | 🔴 高 | 中 | 默认 DRY RUN 模式; 需要二次确认 |
| 闪回后无法再次闪回 | 🟡 中 | 高 | 闪回本身会产生新的 binlog, 建议闪回前做备份 |
| 权限绕过 | 🔴 高 | 低 | 新增 FLASHBACK 权限, 仅 DBA 可用 |
| 闪回中断后状态不一致 | 🟡 中 | 低 | 闪回事务要么全提交要么全回滚; 状态文件保障 |

---

## 5. Recommendations

### 5.1 Phase 1: 原型验证 (2-4 周)

| # | 任务 | 产出 | 负责人 |
|---|------|------|-------|
| 1 | 扩展 SQL 语法: `FLASHBACK TABLE ... TO TIMESTAMP` | YACC 规则 + 解析器 | SQL 层开发 |
| 2 | 实现 Undo-based 闪回核心: 沿版本链回溯 | `fb0undo.cc` 原型 | InnoDB 开发 |
| 3 | 复用 `row_vers_build_for_consistent_read()` 验证版本链可达性 | 单元测试 | 测试 |
| 4 | 实现逆向 INSERT 操作 (DELETE 当前行) | `row_flashback_del()` | InnoDB 开发 |

**验收标准**: 对单表执行简单 UPDATE 后, 能闪回到 UPDATE 前的状态。

### 5.2 Phase 2: 核心功能 (4-8 周)

| # | 任务 | 产出 | 负责人 |
|---|------|------|-------|
| 5 | 实现逆向 UPDATE 操作 (交换 old/new) | `row_flashback_upd()` | InnoDB 开发 |
| 6 | 实现逆向 DELETE 操作 (INSERT 旧行) | `row_flashback_ins()` | InnoDB 开发 |
| 7 | 二级索引同步维护 | `fb0index.cc` | InnoDB 开发 |
| 8 | Redo Log 写入保障 | `fb0redo.cc` | InnoDB 开发 |
| 9 | Binlog-based 闪回: 解析 Rows_event | `fb0binlog.cc` 原型 | Binlog 开发 |
| 10 | DDL 屏障实现 | DDL 兼容性检测 | SQL 层开发 |

**验收标准**: 
- 支持 INSERT/UPDATE/DELETE 三种操作的完整闪回
- 二级索引在闪回后保持一致
- 遇到不兼容 DDL 时正确拒绝

### 5.3 Phase 3: 生产就绪 (4-8 周)

| # | 任务 | 产出 | 负责人 |
|---|------|------|-------|
| 11 | `AS OF TIMESTAMP` 闪回查询 | 一致读视图扩展 | SQL 层开发 |
| 12 | 批量闪回 & 并发控制 | `flashback_max_concurrency` | InnoDB 开发 |
| 13 | 进度监控 & 中断恢复 | `performance_schema.flashback_status` | 运维工具 |
| 14 | 权限控制 | `FLASHBACK` privilege | 安全 |
| 15 | 完整测试覆盖 | MTR 测试用例 (50+) | 测试 |
| 16 | 文档 & 运维指南 | 用户手册 + DBA 指南 | 文档 |

**验收标准**: 
- 通过 MySQL 官方 MTR 测试框架
- 100 万行数据闪回 < 10 分钟
- 闪回期间不影响正常读操作

### 5.4 Phase 4: 优化增强 (持续)

| # | 任务 | 说明 |
|---|------|------|
| 17 | Undo 保留策略优化 | 按表/按事务设置不同保留期 |
| 18 | 闪回预检 (DRY RUN) | 预估闪回影响范围和时间 |
| 19 | 多表事务一致性闪回 | 跨表事务的原子闪回 |
| 20 | 闪回点 (Savepoint) | 类似 Oracle Flashback Restore Point |

---

## 6. Conclusion

### 6.1 方案总结

本方案提出在 Percona Server 中实现**双引擎内置闪回能力**, 整合 Undo Log (短窗口、行级精度) 和 Binlog (长窗口、事务级精度) 两种数据源, 通过统一的 SQL 语法对外暴露。

**核心优势**:
1. **引擎内置**: 不依赖外部工具, 闪回直接在存储引擎内执行, 性能比 binlog2sql 等外挂工具高 5-10 倍
2. **双引擎互补**: Undo-based 解决秒级精准闪回 (分钟级窗口), Binlog-based 解决长窗口闪回 (天级窗口)
3. **充分利用现有源码**: 复用 `row0vers.cc` 版本链、`trx0roll.cc` 回滚逻辑、`binlog_reader.cc` 解析器, 开发成本可控
4. **参考 Clone Plugin 架构**: 借鉴已验证的分层模式 (SQL → Plugin → InnoDB), 降低架构风险

**与业界方案的关系**:

```
                    闪回精度                    实现成本
                       ↑                          ↑
                       │    本方案 (内置双引擎)
                       │         ●
                       │      /     \
                       │     /       \
         行级精度 ●────┤    /         \    ● Oracle Flashback
                       │   /           \
                       │  /             \
    事务级精度 ●───────┤ /               \
           (MyFlash)  │/                 \
                       ├──────────────────┼────→
                       │                  │
                    外挂工具           引擎内置
```

### 6.2 关键设计决策

| 决策点 | 选择 | 理由 |
|-------|------|------|
| 数据源 | Undo + Binlog 双引擎 | 互补: Undo 精度高但窗口短, Binlog 窗口长但精度有限 |
| 执行方式 | 引擎内直接执行 (非生成 SQL) | 性能高, 避免 SQL 层开销, 保证事务一致性 |
| DDL 处理 | 屏障 + 拒绝不兼容操作 | 安全优先, 宁可拒绝也不产生不一致数据 |
| 语法风格 | Oracle Flashback 风格 | 业界最广泛认知的闪回语法, 降低学习成本 |
| 并发控制 | 按主键粒度锁定 | 平衡闪回效率与业务可用性 |

### 6.3 预期效果

| 指标 | 当前 (binlog2sql) | 本方案 |
|------|------------------|--------|
| 闪回准备时间 | 10-30 分钟 (安装/配置/解析) | < 1 分钟 (SQL 直接执行) |
| 闪回执行速度 | ~1000 行/秒 (Python 解析 + SQL 回放) | ~10000 行/秒 (引擎内直接操作) |
| 闪回窗口 | binlog 保留期 (天级) | Undo(分钟) + Binlog(天) |
| 操作复杂度 | 需要 DBA 专业知识 | 一条 SQL 完成 |
| 数据一致性 | 依赖手动校验 | 引擎保证事务一致性 |

---

## Appendix: 源码文件映射

### A.1 需要新增的文件

```
sql/
├── sql_flashback.yy          # YACC 语法: FLASHBACK TABLE / AS OF TIMESTAMP
├── flashback_handler.h       # Flashback_handler 类声明
├── flashback_handler.cc      # Flashback_handler 实现

storage/innobase/flashback/
├── CMakeLists.txt
├── fb0api.cc                 # innodb_flashback_begin/apply/end API
├── fb0api.h
├── fb0undo.cc                # Undo-based 闪回核心
├── fb0undo.h
├── fb0binlog.cc              # Binlog-based 闪回核心
├── fb0binlog.h
├── fb0apply.cc               # 逆向 DML 执行引擎
├── fb0apply.h
├── fb0ddl.cc                 # DDL 屏障
├── fb0ddl.h
├── fb0desc.cc                # Flashback_Descriptor 序列化
└── fb0desc.h

storage/innobase/include/
├── fb0types.h                # 闪回相关类型定义
└── fb0api.h                  # 公开 API 头文件

plugin/flashback/             # (可选) 作为独立插件实现
├── CMakeLists.txt
├── flashback_plugin.cc
├── flashback_status.cc       # PFS 状态表
└── include/
    └── flashback.h
```

### A.2 需要修改的文件

```
sql/
├── sql_yacc.yy               # 新增 FLASHBACK / AS OF 语法
├── sql_lex.cc                # 新增 LEX flashback 字段
├── sql_parse.cc              # 新增 FLASHBACK 命令处理
├── sql_priv.h                # 新增 FLASHBACK 权限位
├── mysqld.cc                 # 新增 flashback_* 系统变量

storage/innobase/
├── handler/ha_innodb.cc      # 新增 hton_flashback 回调
├── include/ha_prototypes.h   # 新增 flashback 函数声明
├── CMakeLists.txt            # 新增 flashback/ 子目录

storage/innobase/trx/
├── trx0sys.cc                # 扩展: 支持 flashback_retention 控制 purge

storage/innobase/row/
├── row0mysql.cc              # 新增 row_flashback_* 函数
└── row0vers.cc               # 扩展: 支持按时间点构建版本
```

### A.3 复用的核心源码函数

| 源码位置 | 函数 | 复用方式 |
|---------|------|---------|
| `row/row0vers.cc` | `row_vers_build_for_consistent_read()` | 直接调用, 构建目标时间点的一致读 |
| `row/row0undo.cc` | `row_undo()`, `row_undo_mod()`, `row_undo_ins()`, `row_undo_del()` | 参考逻辑, 封装为 `row_flashback_*` |
| `trx/trx0undo.cc` | `trx_undo_get_prev_rec()` | 直接调用, 遍历 undo 链 |
| `trx/trx0rec.cc` | `trx_undo_rec_get_*()` 系列 | 直接调用, 解析 undo record |
| `trx/trx0roll.cc` | `trx_rollback_to_savepoint_low()` | 参考事务回滚流程 |
| `sql/binlog_reader.cc` | `Binlog_event_data_istream` | 直接调用, 读取 binlog 文件 |
| `sql/log_event.h` | `Rows_event`, `Table_map_event` | 直接调用, 解析 row event |
| `clone/clone0api.cc` | `innodb_clone_begin/copy/end` | 参考 API 设计模式 |
