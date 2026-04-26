# MySQL 内置闪回能力实现方案

> 基于 Percona Server 源码深度分析 + 业界方案对比研究
> 目标版本: MySQL 8.0.x / Percona Server 8.0.x
> 作者: 量化交易分析师 / CTA 策略专家 (兼任源码分析)
> 日期: 2025-07-28

---

## 目录

1. [执行摘要](#1-执行摘要)
2. [业界闪回方案对比分析](#2-业界闪回方案对比分析)
3. [Percona Server 源码级基础构件分析](#3-percona-server-源码级基础构件分析)
4. [架构设计: "双引擎闪回"]( #4-架构设计双引擎闪回)
5. [详细实现方案](#5-详细实现方案)
6. [关键代码改动清单](#6-关键代码改动清单)
7. [风险分析与对策](#7-风险分析与对策)
8. [四阶段实施计划](#8-四阶段实施计划)
9. [性能基准预估](#9-性能基准预估)
10. [附录: 关键源码文件索引](#10-附录关键源码文件索引)

---

## 1 执行摘要

### 1.1 核心结论

| 维度 | 结论 |
|------|------|
| **社区版现状** | MySQL 社区版**无内置闪回**能力, 闪回需依赖外部工具或企业版功能 |
| **Percona 基础构件** | Undo Log / Binlog / MVCC / Rollback 等底层构件**全部就绪**, 缺少统一的 SQL 入口和调度引擎 |
| **推荐架构** | **"双引擎闪回"** — Undo 引擎用于秒级精准闪回(分钟窗口), Binlog 引擎用于长窗口闪回(天级) |
| **三大风险** | Undo Purge 窗口限制、DDL 不兼容、二级索引一致性 |
| **预计投入** | Phase 1(2-4周) + Phase 2(4-8周) + Phase 3(4-8周) = 总计 **10-20 人周** |

### 1.2 闪回能力矩阵

| 闪回类型 | 数据源 | 时间窗口 | 恢复精度 | 是否需要停写 |
|----------|--------|----------|----------|-------------|
| **闪回查询** (Flashback Query) | Undo 版本链 | 分钟~数分钟 | 行级精确 | 否 |
| **闪回表** (Flashback Table) | Undo / Binlog | 分钟~数天 | 表级精确 | 是(表锁) |
| **闪回事务** (Flashback Transaction) | Binlog | 数小时~数天 | 事务级精确 | 否(反向事务) |
| **闪回数据库** (Flashback DB) | Binlog + 全量备份 | 数天 | 库级 | 是 |

---

## 2 业界闪回方案对比分析

### 2.1 Oracle Flashback (业界标杆)

Oracle 提供了一整套闪回体系, 是我们设计 MySQL 内置闪回的参考标杆:

```sql
-- 闪回查询
SELECT * FROM employees AS OF TIMESTAMP SYSTIMESTAMP - INTERVAL '15' MINUTE;

-- 闪回表
FLASHBACK TABLE employees TO TIMESTAMP SYSTIMESTAMP - INTERVAL '15' MINUTE;

-- 闪回版本查询 (查看行级历史)
SELECT versions_startscn, versions_endscn, versions_operation, emp_id, name
FROM employees VERSIONS BETWEEN TIMESTAMP
  TO_TIMESTAMP('2024-01-01 00:00:00') AND
  TO_TIMESTAMP('2024-01-02 00:00:00');

-- 闪回事务查询
SELECT xid, operation, table_name, undo_sql
FROM flashback_transaction_query
WHERE table_name = 'EMPLOYEES';
```

**核心机制:**
- 依赖 **Undo Tablespace** 存储历史版本
- `UNDO_RETENTION` 参数控制保留时间
- 使用 **System Change Number (SCN)** 作为时间锚点
- Flashback Data Archive (FDA) 支持长期归档

**对 MySQL 的启示:**
1. 需要类似 `UNDO_RETENTION` 的参数 (`innodb_flashback_retention_seconds`)
2. 需要类似 SCN 的全局单调递增序列 (InnoDB `trx_no` / GTID)
3. 需要兼容 SQL 标准的 `AS OF TIMESTAMP` 语法

### 2.2 MyFlash (美团开源)

**项目地址:** https://github.com/Meituan-Dianping/MyFlash

**工作原理:**
- 解析 Binlog (Row-based format)
- 逆向生成反向 SQL (INSERT ↔ DELETE, UPDATE 逆向)
- 支持按库/表/时间范围过滤
- 输出为可直接执行的 SQL 文件

**优点:**
- 纯工具, 无需修改 MySQL 源码
- 支持长窗口闪回(只要 binlog 保留)
- 可精确到事务级别

**缺点:**
- 外挂工具, 性能和集成度受限
- 需要 binlog_format=ROW
- 不支持闪回查询(只能恢复数据, 不能查询历史)
- DDL 操作不兼容, 需要人工处理

### 2.3 binlog2sql (大众点评开源)

**项目地址:** https://github.com/danfengcao/binlog2sql

**工作原理:**
- 类似 MyFlash, 基于 mysql-replication 库解析 binlog
- Python 实现, 更易扩展
- 支持生成闪回 SQL 和审计 SQL

**优点:**
- Python 生态, 易于二次开发
- 输出格式灵活

**缺点:**
- 性能较差 (Python 解析)
- 同样需要 ROW binlog
- 网络拉取 binlog, 延迟高

### 2.4 Percona Server 增强 (已有能力)

Percona Server 已提供以下相关增强:

| 功能 | 描述 | 与闪回的关系 |
|------|------|-------------|
| **Undo Log Truncate** | 自动回收 Undo 表空间 | 影响闪回窗口大小 |
| **Long Transaction Detection** | 检测长事务 | 防止 Undo 被提前回收 |
| **Binlog Row Value Options** | binlog_row_value_options=PARTIAL_JSON | 影响闪回恢复完整性 |

### 2.5 业界方案对比总结

| 方案 | 是否需要改源码 | 时间窗口 | 闪回查询 | 闪回恢复 | 性能 | 运维成本 |
|------|---------------|----------|---------|---------|------|---------|
| Oracle Flashback | 否(原生) | 分钟~小时(可归档到天) | ✅ | ✅ | 极高 | 低 |
| MyFlash | 否(外挂) | 天级(取决于 binlog) | ❌ | ✅ | 中 | 中 |
| binlog2sql | 否(外挂) | 天级(取决于 binlog) | ❌ | ✅ | 低 | 中 |
| **本方案 (内置)** | 是 | 分钟~天(可配置) | ✅ | ✅ | 极高 | 低 |

---

## 3 Percona Server 源码级基础构件分析

### 3.1 源码扫描范围

基于 Percona Server 8.0.x 源码树, 已扫描以下核心模块:

```
storage/innobase/
├── trx/
│   ├── trx0undo.cc      — Undo Log 读写核心实现
│   ├── trx0rec.cc       — Undo Log Record 解析
│   ├── trx0roll.cc      — 事务回滚执行引擎
│   ├── trx0purge.cc     — Undo Purge 机制
│   ├── trx0trx.cc       — 事务控制块 (trx_t)
│   └── trx0sys.cc       — 事务系统 (trx_sys_t)
├── row/
│   ├── row0undo.cc      — 行级 Undo 操作
│   ├── row0vers.cc      — 版本链构建 (row_vers_build_for_consistent_read)
│   └── row0mysql.cc     — InnoDB-MySQL 接口层
├── include/
│   ├── trx0undo.h       — Undo Log 数据结构
│   ├── trx0trx.h        — 事务结构定义
│   ├── row0undo.h       — 行 Undo 接口
│   └── row0vers.h       — 版本链接口
└── ...

sql/
├── binlog.cc            — Binlog 写入/管理
├── binlog_reader.cc     — Binlog 读取/反序列化
├── binlog_istream.cc    — Binlog 流式读取
├── log_event.cc         — Binlog Event 解析 (Rows_event 等)
├── sql_yacc.yy          — SQL 语法解析 (18574 行)
└── my_sqlcommand.h      — SQL 命令枚举 (include/)
```

### 3.2 Undo Log 体系 (闪回查询的基础)

#### 3.2.1 核心数据结构

```c
// storage/innobase/include/trx0undo.h

/** Undo log header */
struct undo_hdr_t {
  trx_id_t  trx_id;       /* 事务 ID */
  trx_rseg_id_t rseg_id;  /* 回滚段 ID */
  roll_ptr_t roll_ptr;    /* Rollback Pointer (指向 undo record) */
  undo_no_t undo_no;      /* Undo 操作序号 (单调递增) */
  ulint     type;         /* TRX_UNDO_INSERT / TRX_UNDO_UPDATE */
  ...
};

/** Rollback pointer (encoded in 7 bytes) */
// 编码格式: [undo_no:6bit][page_no:4bit][offset:2bit]
// 用于从聚集索引记录定位到对应的 undo record
```

#### 3.2.2 Undo 版本链 (关键复用点)

```c
// storage/innobase/row/row0vers.cc

/**
Build a version chain for consistent read.
This function traverses the undo log chain to find the correct
version of a record as of a given read view.
*/
dberr_t row_vers_build_for_consistent_read(
    const rec_t *rec,           /* 聚集索引当前记录 */
    mtr_t *mtr,
    dict_index_t *index,
    read_view_t *view,          /* 读视图(快照) */
    ulint **offsets,
    mem_heap_t **offsets_heap,
    const rec_t **old_vers);    /* 输出: 历史版本记录 */
```

**复用策略:** 闪回查询可复用此函数, 只需构造一个指向目标时间点的 `read_view_t`。

#### 3.2.3 事务回滚执行引擎

```c
// storage/innobase/trx/trx0roll.cc

/**
Rollback a transaction used in MySQL.
This is the core rollback execution engine.
*/
static void trx_rollback_to_savepoint_low(
    trx_t *trx,           /* 事务句柄 */
    trx_savept_t *savept, /* 回滚目标保存点 */
    bool serialised);     /* 是否序列化回滚 */

/**
Finishes a transaction rollback.
*/
static void trx_rollback_finish(trx_t *trx);
```

**复用策略:** 闪回恢复 (Flashback Table) 可复用 `trx_rollback_to_savepoint_low()` 的逻辑,
但需要增加:
- Undo Log 不标记为 "已完成回滚" (而是仅做读操作)
- 生成正向 SQL 而非直接修改数据页

### 3.3 Binlog 体系 (长窗口闪回的基础)

#### 3.3.1 Binlog Reader 接口

```c
// sql/binlog_reader.h

class Binlog_event_data_istream {
public:
    bool read_event_data(
        unsigned char **data,
        unsigned int *length,
        ALLOCATOR *allocator,
        bool verify_checksum,
        enum_binlog_checksum_alg checksum_alg);
};

// Binlog event 反序列化
Binlog_read_error::Error_type binlog_event_deserialize(
    const unsigned char *event_data,
    unsigned int event_data_len,
    const Format_description_event *fde,
    bool verify_checksum,
    Log_event **event);
```

#### 3.3.2 Rows Event (闪回恢复的关键)

```c
// sql/log_event.h

class Rows_log_event : public Log_event {
    /* Write_rows / Update_rows / Delete_rows 的基类 */
    /* 包含: 表映射、列位图、行数据 */
    /* 可从中逆向生成反向 SQL */
};
```

**复用策略:** 
- 使用 `binlog_event_deserialize()` 解析每个 event
- 对于 `Write_rows_event` → 生成反向 DELETE
- 对于 `Delete_rows_event` → 生成反向 INSERT
- 对于 `Update_rows_event` → 生成逆向 UPDATE (before/after 互换)

### 3.4 SQL 解析层 (新增语法的入口)

#### 3.4.1 SQL 命令枚举

```c
// include/my_sqlcommand.h

enum enum_sql_command {
    SQLCOM_SELECT,
    ...
    SQLCOM_ROLLBACK,
    SQLCOM_COMMIT,
    ...
    /* 新增: */
    SQLCOM_FLASHBACK_TABLE,
    SQLCOM_FLASHBACK_QUERY,
    SQLCOM_FLASHBACK_TRANSACTION,
    SQLCOM_END
};
```

#### 3.4.2 Yacc 语法扩展 (sql_yacc.yy)

参考现有 ROLLBACK 语法 (第 17512 行):

```yacc
rollback_statement:
        ROLLBACK_SYM opt_work opt_chain opt_release
          {
            Lex->sql_command = SQLCOM_ROLLBACK;
          }
```

新增 FLASHBACK 语法:

```yacc
flashback_statement:
        FLASHBACK_SYM TABLE_SYM table_list
          TO_SYM TIMESTAMP_SYM time_literal
          {
            Lex->sql_command = SQLCOM_FLASHBACK_TABLE;
            /* 解析表列表和时间戳 */
          }
      | SELECT_SYM select_list
          FROM_SYM table_name
          AS_SYM OF_SYM TIMESTAMP_SYM time_literal
          {
            Lex->sql_command = SQLCOM_FLASHBACK_QUERY;
            /* 转换为带快照的 SELECT */
          }
```

### 3.5 已有的基础构件总结

| 构件 | 文件 | 功能 | 可复用度 |
|------|------|------|---------|
| 版本链构建 | `row0vers.cc` | 沿 Undo Log 回溯历史版本 | ★★★★★ (100%) |
| Undo Log 读写 | `trx0undo.cc` | Undo Log 页级读写 | ★★★★☆ (80%) |
| 事务回滚引擎 | `trx0roll.cc` | Undo 记录逆向执行 | ★★★★☆ (70%) |
| Undo Purge | `trx0purge.cc` | Undo 清理 (需控制) | ★★☆☆☆ (只读) |
| Binlog Reader | `binlog_reader.cc` | Binlog 流式读取 | ★★★★☆ (80%) |
| Event 反序列化 | `binlog_reader.h` | Event 反序列化 | ★★★★★ (100%) |
| Rows Event 解析 | `log_event.cc` | 行事件解析 | ★★★★☆ (75%) |
| 事务控制 | `trx0trx.cc` | 事务生命周期管理 | ★★★★☆ (70%) |

---

## 4 架构设计: "双引擎闪回"

### 4.1 总体架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      MySQL Server Layer                      │
│  ┌─────────────┐  ┌──────────────┐  ┌───────────────────┐   │
│  │  SQL Parser │  │ Flashback    │  │  Flashback        │   │
│  │  (yacc)     │→ │ Executor     │→ │  Result Handler   │   │
│  │             │  │ (新模块)     │  │                   │   │
│  └─────────────┘  └──────┬───────┘  └───────────────────┘   │
│                           │                                  │
│              ┌────────────┴────────────┐                     │
│              │   Flashback Scheduler   │                     │
│              │   (引擎选择 & 路由)      │                     │
│              └────────────┬────────────┘                     │
└───────────────────────────┼─────────────────────────────────┘
                            │
         ┌──────────────────┴──────────────────┐
         │                                     │
┌────────▼──────────┐              ┌───────────▼──────────┐
│   Undo Engine     │              │   Binlog Engine      │
│   (秒级精准闪回)   │              │   (长窗口闪回)       │
│                   │              │                      │
│ • 闪回查询        │              │ • 闪回表(>分钟级)    │
│ • 闪回版本查询    │              │ • 闪回事务           │
│ • 分钟级窗口      │              │ • 小时~天级窗口      │
│                   │              │                      │
│ 依赖:             │              │ 依赖:                │
│ • row_vers_*()   │              │ • binlog_reader       │
│ • trx_undo_*()   │              │ • log_event_deser()  │
│ • read_view_t     │              │ • Rows_log_event      │
└───────────────────┘              └──────────────────────┘
         │                                     │
         ▼                                     ▼
┌──────────────────┐              ┌──────────────────────┐
│  Undo Tablespace │              │   Binlog Files       │
│  (ibundo001~N)   │              │   (mysql-bin.000001) │
└──────────────────┘              └──────────────────────┘
```

### 4.2 引擎选择策略

```c
/* Flashback Scheduler 伪代码 */
FlashbackEngine select_engine(
    FlashbackRequest *req,
    SystemState *state)
{
    /* 计算目标时间到现在的间隔 */
    ulong interval_seconds = diff_seconds(req->target_time, now());
    
    /* Undo 引擎的可用窗口 = MIN(
         innodb_flashback_retention_seconds,
         oldest_active_trx_undo_age
       ) */
    ulong undo_available = state->undo_available_window;
    
    if (interval_seconds <= undo_available) {
        /* 短窗口 → Undo 引擎 (高性能, 精准) */
        return ENGINE_UNDO;
    } else if (interval_seconds <= state->binlog_retention_days * 86400) {
        /* 长窗口 → Binlog 引擎 (较慢, 但窗口长) */
        return ENGINE_BINLOG;
    } else {
        return ENGINE_NONE; /* 超出所有可用窗口 */
    }
}
```

### 4.3 系统参数设计

```c
/* innodb.cc - 新增参数 */

static MYSQL_SYSVAR_ULONG(flashback_retention_seconds,
    innodb_flashback_retention_seconds,
    PLUGIN_VAR_RQCMDARG,
    "Maximum seconds to retain undo log for flashback queries",
    NULL, NULL,
    900,    /* default: 15 minutes */
    60,     /* min: 1 minute */
    86400,  /* max: 24 hours */
    0);     /* block size */

static MYSQL_SYSVAR_BOOL(flashback_enable_binlog_recovery,
    innodb_flashback_enable_binlog_recovery,
    PLUGIN_VAR_RQCMDARG,
    "Enable binlog-based flashback for recovery beyond undo window",
    NULL, NULL,
    TRUE);  /* default: enabled */

static MYSQL_SYSVAR_ULONG(flashback_max_rows_per_txn,
    innodb_flashback_max_rows_per_txn,
    PLUGIN_VAR_RQCMDARG,
    "Maximum rows to process in a single flashback transaction",
    NULL, NULL,
    100000,  /* default */
    1,       /* min */
    10000000 /* max */
    0);
```

### 4.4 SQL 语法设计 (兼容 Oracle 风格)

#### 4.4.1 闪回查询

```sql
-- 按时间戳闪回查询
SELECT * FROM employees
AS OF TIMESTAMP '2025-07-28 10:30:00';

-- 按相对时间闪回查询
SELECT * FROM employees
AS OF TIMESTAMP (NOW() - INTERVAL 15 MINUTE);

-- 按事务号闪回查询
SELECT * FROM employees
AS OF TRX_ID 123456;
```

#### 4.4.2 闪回表

```sql
-- 闪回表到指定时间点
FLASHBACK TABLE employees TO TIMESTAMP '2025-07-28 10:30:00';

-- 闪回表到指定 SCN/事务号
FLASHBACK TABLE employees TO TRX_ID 123456;

-- DRY RUN 模式 (预览恢复内容)
FLASHBACK TABLE employees TO TIMESTAMP '2025-07-28 10:30:00' DRY RUN;

-- 多表闪回 (需在同一事务内)
FLASHBACK TABLE employees, departments 
TO TIMESTAMP '2025-07-28 10:30:00';
```

#### 4.4.3 闪回版本查询

```sql
-- 查看某行在一段时间内的所有版本
SELECT 
    versions_trx_id,
    versions_operation,  -- 'I'/'U'/'D'
    versions_start_time,
    versions_end_time,
    emp_id, name, salary
FROM employees VERSIONS BETWEEN TIMESTAMP
    '2025-07-28 10:00:00' AND '2025-07-28 11:00:00'
WHERE emp_id = 100;
```

#### 4.4.4 闪回事务

```sql
-- 查看事务的所有操作
SELECT 
    trx_id,
    operation,      -- 'INSERT'/'UPDATE'/'DELETE'
    table_name,
    undo_sql        -- 反向 SQL
FROM flashback_transaction_query
WHERE trx_id = 123456;

-- 执行事务级闪回 (生成反向事务并执行)
FLASHBACK TRANSACTION 123456;
```

---

## 5 详细实现方案

### 5.1 Phase 1: SQL 语法 + Undo 闪回查询原型

#### 5.1.1 新增 SQL 命令枚举

**文件:** `include/my_sqlcommand.h`

```c
enum enum_sql_command {
    ...
    SQLCOM_CLONE,
    /* === Flashback SQL Commands === */
    SQLCOM_FLASHBACK_TABLE,       /* FLASHBACK TABLE ... TO TIMESTAMP */
    SQLCOM_FLASHBACK_QUERY,       /* SELECT ... AS OF TIMESTAMP */
    SQLCOM_FLASHBACK_VERSIONS,    /* SELECT ... VERSIONS BETWEEN */
    SQLCOM_FLASHBACK_TRANSACTION, /* FLASHBACK TRANSACTION */
    SQLCOM_END
};
```

#### 5.1.2 新增关键字

**文件:** `sql/sql_yacc.yy`

```yacc
%token<lexer.keyword> FLASHBACK_SYM 901
%token<lexer.keyword> VERSIONS_SYM 902
%token<lexer.keyword> BETWEEN_SYM 903
%token<lexer.keyword> TRX_ID_SYM 904

/* 在 keyword_spools 中添加 */
{"FLASHBACK", FLASHBACK_SYM},
{"VERSIONS", VERSIONS_SYM},
```

#### 5.1.3 新增语法解析规则

**文件:** `sql/sql_yacc.yy` (在 statement 规则中新增)

```yacc
statement:
    ...
    | flashback_statement
    ...

flashback_statement:
    FLASHBACK_SYM TABLE_SYM table_name_list
      TO_SYM timestamp_value
      opt_dry_run
      {
        $$ = NEW_PTN PT_flashback_table(@$, $3, $4, $5);
        Lex->sql_command = SQLCOM_FLASHBACK_TABLE;
      }
    | FLASHBACK_SYM TRANSACTION_SYM trx_id_value
      {
        $$ = NEW_PTN PT_flashback_transaction(@$, $3);
        Lex->sql_command = SQLCOM_FLASHBACK_TRANSACTION;
      }

/* 扩展现有 SELECT 语法支持 AS OF TIMESTAMP */
table_factor:
    table_name opt_alias
    | table_name AS_SYM OF_SYM TIMESTAMP_SYM timestamp_value
      {
        $$ = NEW_PTN PT_table_reference_as_of(@$, $1, $5);
        /* 设置闪回查询标志 */
        Lex->flashback_query = true;
        Lex->flashback_timestamp = $5;
      }
    | table_name VERSIONS_SYM BETWEEN_SYM TIMESTAMP_SYM 
      timestamp_value AND_SYM timestamp_value
      {
        $$ = NEW_PTN PT_table_reference_versions(@$, $1, $5, $7);
        Lex->flashback_versions = true;
        Lex->flashback_start_time = $5;
        Lex->flashback_end_time = $7;
      }
```

#### 5.1.4 新增语法树节点

**文件:** `sql/parse_tree_nodes.h`

```cpp
class PT_flashback_table : public Parse_tree_node {
public:
    PT_flashback_table(
        const POS &pos,
        List<LEX_CSTRING> *tables,
        const PTI_value *timestamp,
        bool dry_run)
        : m_tables(tables), m_timestamp(timestamp), m_dry_run(dry_run) {}
    
    bool contextualize(Parse_context *pc) override;
    
private:
    List<LEX_CSTRING> *m_tables;
    const PTI_value *m_timestamp;
    bool m_dry_run;
};

class PT_table_reference_as_of : public PT_table_reference {
public:
    PT_table_reference_as_of(
        const POS &pos,
        TABLE_LIST *table,
        const PTI_value *timestamp)
        : PT_table_reference(pos, table), m_timestamp(timestamp) {}
    
    bool contextualize(Parse_context *pc) override;
    
private:
    const PTI_value *m_timestamp;
};
```

#### 5.1.5 新增执行入口

**文件:** `sql/sql_class.h` (LEX 扩展)

```cpp
struct LEX {
    ...
    /* Flashback 相关字段 */
    bool flashback_query;           /* SELECT ... AS OF TIMESTAMP */
    bool flashback_versions;        /* SELECT ... VERSIONS BETWEEN */
    my_time_t flashback_timestamp;  /* 闪回目标时间戳 */
    my_time_t flashback_start_time; /* 版本查询开始时间 */
    my_time_t flashback_end_time;   /* 版本查询结束时间 */
    trx_id_t flashback_trx_id;      /* 按事务号闪回 */
    bool flashback_dry_run;         /* DRY RUN 模式 */
    List<LEX_CSTRING> flashback_tables; /* 闪回表列表 */
    ...
};
```

#### 5.1.6 InnoDB 层: 闪回查询实现

**文件:** `storage/innobase/row/row0vers.cc` (新增函数)

```c
/**
Flashback query: build a version of a record as of a given timestamp.

This function reuses row_vers_build_for_consistent_read() internally,
but constructs a custom read_view_t that targets a specific timestamp.

@param[in]  rec        Current clustered index record
@param[in]  index      Index descriptor
@param[in]  target_ts  Target timestamp for flashback
@param[out] old_vers    Historical version record (or NULL if not found)
@param[in]  heap       Memory heap for allocation
@return DB_SUCCESS on success, DB_UNDO_LOG_PURGED if data has been purged
*/
[[nodiscard]] dberr_t row_build_flashback_version(
    const rec_t *rec,
    dict_index_t *index,
    my_time_t target_ts,
    const rec_t **old_vers,
    mem_heap_t *heap)
{
    /* 1. 将时间戳转换为近似 trx_id */
    trx_id_t target_trx_id = trx_sys_find_trx_id_by_timestamp(target_ts);
    
    if (target_trx_id == TRX_ID_MAX) {
        return DB_UNDO_LOG_PURGED;  /* 目标时间超出 Undo 窗口 */
    }
    
    /* 2. 构造一个指向目标时间点的 read_view */
    read_view_t *view = read_view_create_low(target_trx_id, heap);
    
    /* 3. 复用现有版本链构建函数 */
    ulint *offsets = nullptr;
    mem_heap_t *offsets_heap = mem_heap_create(512);
    
    mtr_t mtr;
    mtr_start(&mtr);
    
    dberr_t err = row_vers_build_for_consistent_read(
        rec, &mtr, index, view, 
        &offsets, &offsets_heap, old_vers);
    
    mtr_commit(&mtr);
    read_view_close_low(view);
    mem_heap_free(offsets_heap);
    
    return err;
}
```

**文件:** `storage/innobase/trx/trx0sys.cc` (新增函数)

```c
/**
Find the transaction ID that was active at a given timestamp.
This is used to map timestamps to read views for flashback queries.

@param[in]  target_ts  Target timestamp
@return     Approximate trx_id, or TRX_ID_MAX if not found
*/
trx_id_t trx_sys_find_trx_id_by_timestamp(my_time_t target_ts)
{
    /* 策略: 遍历 trx_sys_t->rw_trx_list 和 trx_sys_t->serialisation_list
       找到在目标时间点活跃的事务范围 */
    
    mysql_mutex_lock(&trx_sys_t::mutex);
    
    /* 从 undo tablespace 中的 oldest_trx_id 推断 */
    trx_id_t oldest = trx_sys_get_oldest_active_trx_id();
    
    /* 如果目标时间早于 oldest, 说明 Undo 可能已被 purge */
    if (target_ts < trx_sys_get_oldest_timestamp()) {
        mysql_mutex_unlock(&trx_sys_t::mutex);
        return TRX_ID_MAX;
    }
    
    /* 估算: 使用 trx_sys->max_trx_id 和当前时间线性插值
       (实际实现需要更精确的历史记录) */
    trx_id_t estimated = trx_sys_estimate_trx_id_at_timestamp(target_ts);
    
    mysql_mutex_unlock(&trx_sys_t::mutex);
    
    return estimated;
}
```

#### 5.1.7 MySQL 层: 闪回查询执行

**文件:** `sql/sql_select.cc` (修改)

```cpp
/* 在 handle_select() 或 JOIN::exec() 中处理闪回查询标志 */

bool JOIN::exec()
{
    if (thd->lex->flashback_query) {
        /* 闪回查询: 为每个表构造闪回读视图 */
        for (TABLE_LIST *tab : tables) {
            if (tab->table && tab->table->s->db_type() == DB_TYPE_INNOBASE) {
                /* 设置 InnoDB 层的闪回上下文 */
                ha_innobase *innodb = 
                    dynamic_cast<ha_innobase*>(tab->table->file);
                innodb->set_flashback_timestamp(thd->lex->flashback_timestamp);
            }
        }
    }
    
    if (thd->lex->flashback_versions) {
        /* 闪回版本查询: 遍历所有历史版本 */
        return exec_flashback_versions();
    }
    
    /* 正常执行路径 */
    return exec_normal();
}
```

**文件:** `storage/innobase/handler/ha_innodb.cc` (扩展)

```cpp
/* ha_innobase 类新增成员 */
class ha_innobase : public handler
{
public:
    ...
    void set_flashback_timestamp(my_time_t ts) {
        m_flashback_timestamp = ts;
        m_flashback_mode = true;
    }
    
    void clear_flashback_context() {
        m_flashback_timestamp = 0;
        m_flashback_mode = false;
    }
    
    int rnd_next(uchar *buf) override {
        if (m_flashback_mode) {
            return rnd_next_flashback(buf);
        }
        return handler::rnd_next(buf);
    }
    
private:
    bool m_flashback_mode = false;
    my_time_t m_flashback_timestamp = 0;
    
    int rnd_next_flashback(uchar *buf);
};

/* 闪回模式的行读取 */
int ha_innobase::rnd_next_flashback(uchar *buf)
{
    const rec_t *rec;
    const rec_t *old_vers = nullptr;
    mem_heap_t *heap = mem_heap_create(1024);
    
    /* 1. 读取当前记录 */
    rec = get_next_record_from_index();
    if (!rec) {
        mem_heap_free(heap);
        return HA_ERR_END_OF_FILE;
    }
    
    /* 2. 构建历史版本 */
    dberr_t err = row_build_flashback_version(
        rec, m_prebuilt->index,
        m_flashback_timestamp,
        &old_vers, heap);
    
    if (err != DB_SUCCESS) {
        /* Undo 已被 purge, 返回错误 */
        mem_heap_free(heap);
        return HA_ERR_RECORD_DELETED;
    }
    
    if (old_vers) {
        /* 3. 将历史版本转换为 MySQL 行格式 */
        rec_to_mysql_row(buf, old_vers, m_prebuilt->index);
    }
    
    mem_heap_free(heap);
    return 0;
}
```

### 5.2 Phase 2: Undo 闪回表 + Binlog 闪回引擎

#### 5.2.1 闪回表执行 (Undo 引擎)

**文件:** `sql/sql_flashback.cc` (新增文件)

```cpp
/**
Execute FLASHBACK TABLE using the Undo Engine.
This function:
1. Validates the target timestamp is within the undo window
2. Acquires exclusive table locks
3. For each table, traverses all rows and restores historical versions
4. Commits the flashback as a single transaction
*/
bool mysql_flashback_table(
    THD *thd,
    List<LEX_CSTRING> *tables,
    my_time_t target_ts,
    bool dry_run)
{
    /* 1. 验证时间窗口 */
    my_time_t oldest_ts = trx_sys_get_oldest_timestamp();
    if (target_ts < oldest_ts) {
        my_error(ER_FLASHBACK_UNDO_PURGED, MYF(0));
        return true; /* error */
    }
    
    /* 2. 检查 DDL 屏障 (闪回后不能有 DDL) */
    if (check_ddl_barrier(thd, tables, target_ts)) {
        my_error(ER_FLASHBACK_DDL_INCOMPATIBLE, MYF(0));
        return true;
    }
    
    /* 3. 获取表级排他锁 */
    for (LEX_CSTRING &tbl : *tables) {
        if (lock_table_exclusive(thd, tbl)) {
            return true;
        }
    }
    
    if (dry_run) {
        /* DRY RUN: 统计将恢复的行数, 不实际修改 */
        return execute_flashback_dry_run(thd, tables, target_ts);
    }
    
    /* 4. 开始闪回事 */
    trans_begin(thd);
    
    for (LEX_CSTRING &tbl : *tables) {
        if (flashback_single_table(thd, tbl, target_ts)) {
            trans_rollback(thd);
            return true;
        }
    }
    
    /* 5. 提交 */
    trans_commit(thd);
    
    return false; /* success */
}

/**
Flashback a single table using the Undo Engine.
This is modeled after row_undo_step() but operates forward
(restoring old versions rather than rolling back).
*/
static bool flashback_single_table(
    THD *thd,
    LEX_CSTRING table_name,
    my_time_t target_ts)
{
    /* 伪代码逻辑:
       1. 全表扫描聚集索引
       2. 对每行调用 row_build_flashback_version()
       3. 如果历史版本与当前版本不同, 更新为历史版本
       4. 同时处理二级索引的同步更新
    */
    
    ha_innobase *innodb = open_innobase_table(thd, table_name);
    
    while (innodb->rnd_next(buf) != HA_ERR_END_OF_FILE) {
        const rec_t *current_rec = innodb->get_current_rec();
        const rec_t *old_vers = nullptr;
        
        dberr_t err = row_build_flashback_version(
            current_rec, innodb->index,
            target_ts, &old_vers, heap);
        
        if (err == DB_UNDO_LOG_PURGED) {
            /* 此行的历史版本已被 purge, 跳过 */
            continue;
        }
        
        if (old_vers && !records_equal(current_rec, old_vers)) {
            /* 执行逆向更新 */
            if (!dry_run) {
                innodb->flashback_update_row(current_rec, old_vers);
                rows_restored++;
            }
        }
    }
    
    close_innobase_table(innodb);
    return false;
}
```

#### 5.2.2 Binlog 引擎: 逆向解析

**文件:** `sql/sql_flashback_binlog.cc` (新增文件)

```cpp
/**
Execute flashback using the Binlog Engine.
This engine:
1. Locates the binlog position at the target timestamp
2. Reads events forward from that position to now
3. Reverses each event (Write→Delete, Delete→Insert, Update↔swap)
4. Executes the reversed events as a new transaction
*/

class BinlogFlashbackEngine {
public:
    BinlogFlashbackEngine(THD *thd);
    ~BinlogFlashbackEngine();
    
    bool execute(
        List<LEX_CSTRING> *tables,
        my_time_t target_ts,
        bool dry_run);
    
private:
    THD *m_thd;
    
    /* 定位 binlog 位置 */
    bool find_binlog_position_at_timestamp(
        my_time_t target_ts,
        char *binlog_file,
        my_off_t *binlog_pos);
    
    /* 逆向单个 Rows Event */
    bool reverse_rows_event(
        Rows_log_event *event,
        List<LEX_CSTRING> *tables,
        FlashbackSQLBuffer *sql_buf);
    
    /* 逆向 UPDATE (before/after 互换) */
    bool reverse_update_event(
        Update_rows_log_event *event,
        FlashbackSQLBuffer *sql_buf);
    
    /* 逆向 INSERT → DELETE */
    bool reverse_write_event(
        Write_rows_log_event *event,
        FlashbackSQLBuffer *sql_buf);
    
    /* 逆向 DELETE → INSERT */
    bool reverse_delete_event(
        Delete_rows_log_event *event,
        FlashbackSQLBuffer *sql_buf);
    
    /* 检查 DDL 屏障 */
    bool check_ddl_barrier(
        List<LEX_CSTRING> *tables,
        const char *binlog_file,
        my_off_t start_pos,
        my_off_t end_pos);
};

bool BinlogFlashbackEngine::execute(
    List<LEX_CSTRING> *tables,
    my_time_t target_ts,
    bool dry_run)
{
    char binlog_file[FN_REFLEN];
    my_off_t binlog_pos;
    
    /* 1. 定位目标时间点的 binlog 位置 */
    if (find_binlog_position_at_timestamp(target_ts, binlog_file, &binlog_pos)) {
        my_error(ER_BINLOG_POSITION_NOT_FOUND, MYF(0));
        return true;
    }
    
    /* 2. 打开 binlog 文件流 */
    File fd = mysql_file_open(
        key_file_binlog, binlog_file, O_RDONLY, MYF(0));
    Basic_istream istream(fd);
    
    Binlog_event_data_istream event_stream(
        &m_error, &istream, max_event_size);
    
    /* 3. 跳过到目标位置 */
    /* (跳过 FDE + 到目标 position) */
    
    /* 4. 正向读取事件, 同时逆向收集 */
    FlashbackSQLBuffer sql_buf;
    
    while (!event_stream.read_event_data(&data, &len, &allocator,
                                         true, checksum_alg)) {
        Log_event *event = nullptr;
        binlog_event_deserialize(data, len, fde, true, &event);
        
        if (!event) continue;
        
        /* 检查是否到达当前时间 */
        if (event->when >= time(nullptr)) break;
        
        /* 检查 DDL */
        if (event->get_type_code() == QUERY_EVENT) {
            Query_log_event *qev = dynamic_cast<Query_log_event*>(event);
            if (qev && is_ddl_query(qev->query)) {
                if (affects_tables(qev->query, tables)) {
                    my_error(ER_FLASHBACK_DDL_IN_BINLOG, MYF(0));
                    delete event;
                    return true;
                }
            }
        }
        
        /* 逆向 DML 事件 */
        Rows_log_event *rows_event = 
            dynamic_cast<Rows_log_event*>(event);
        if (rows_event) {
            reverse_rows_event(rows_event, tables, &sql_buf);
        }
        
        delete event;
    }
    
    /* 5. 执行逆向 SQL */
    if (!dry_run) {
        sql_buf.execute(m_thd);
    }
    
    return false;
}
```

### 5.3 Phase 3: 闪回版本查询 + 并发控制

#### 5.3.1 闪回版本查询实现

**文件:** `sql/sql_flashback_versions.cc` (新增文件)

```cpp
/**
Execute SELECT ... VERSIONS BETWEEN TIMESTAMP ...
This traverses the undo log chain for each row and emits
all historical versions within the specified time range.
*/

class FlashbackVersionsIterator {
public:
    FlashbackVersionsIterator(
        THD *thd,
        dict_index_t *index,
        my_time_t start_ts,
        my_time_t end_ts);
    
    /* 获取下一个版本 */
    dberr_t next_version(
        FlashbackVersionRecord *out_record);
    
private:
    /* 遍历 undo log chain */
    dberr_t traverse_undo_chain(
        const rec_t *rec,
        my_time_t start_ts,
        my_time_t end_ts,
        FlashbackVersionRecord *out_record);
};

struct FlashbackVersionRecord {
    trx_id_t trx_id;
    char operation;       /* 'I' = Insert, 'U' = Update, 'D' = Delete */
    my_time_t start_time; /* 版本生效时间 */
    my_time_t end_time;   /* 版本失效时间 */
    uchar *row_data;      /* 行数据 */
    ulint row_len;
};
```

#### 5.3.2 并发控制与隔离

```c
/**
Flashback 操作期间的并发控制策略:

1. 闪回查询 (SELECT ... AS OF):
   - 不需要锁, 纯读操作
   - 使用 MVCC 一致性读, 不影响并发写入
   
2. 闪回表 (FLASHBACK TABLE):
   - 需要对目标表加 MDL EXCLUSIVE 锁
   - 阻止其他事务的 DML 和 DDL
   - 在闪回完成后释放锁
   
3. 闪回事务 (FLASHBACK TRANSACTION):
   - 启动一个新事务执行反向 SQL
   - 遵循正常的行锁/间隙锁机制
   - 可能与活跃事务冲突 (按锁等待超时处理)
*/

/**
检查闪回操作的安全性:
- 目标时间点是否在 Undo/Binlog 窗口内
- 目标表是否发生过 DDL (结构变更)
- 是否有活跃事务可能影响闪回一致性
*/
bool flashback_safety_check(
    THD *thd,
    List<LEX_CSTRING> *tables,
    my_time_t target_ts,
    FlashbackSafetyReport *report)
{
    report->can_flashback = true;
    report->warnings = "";
    
    /* 1. 检查 Undo 窗口 */
    my_time_t oldest_undo = trx_sys_get_oldest_timestamp();
    if (target_ts < oldest_undo) {
        report->warnings += 
            "Target time is before the oldest undo log. "
            "Some historical data may have been purged.\n";
    }
    
    /* 2. 检查 DDL 屏障 */
    for (LEX_CSTRING &tbl : *tables) {
        if (has_ddl_after(thd, tbl, target_ts)) {
            report->can_flashback = false;
            report->errors += 
                "DDL operation detected on table after target time. "
                "Flashback is not safe.\n";
        }
    }
    
    /* 3. 检查活跃事务 */
    if (has_conflicting_active_transactions(thd, tables, target_ts)) {
        report->warnings +=
            "Active transactions may affect flashback consistency.\n";
    }
    
    return report->can_flashback;
}
```

### 5.4 Phase 4: 监控 + 优化 + 多表一致性

#### 5.4.1 Performance Schema 集成

```sql
/* 新增 P_S 表 */
CREATE TABLE performance_schema.flashback_status (
    THREAD_ID BIGINT UNSIGNED NOT NULL,
    EVENT_ID BIGINT UNSIGNED NOT NULL,
    FLASHBACK_TYPE ENUM('QUERY', 'TABLE', 'TRANSACTION', 'VERSIONS') NOT NULL,
    TARGET_TIME TIMESTAMP,
    START_TIME TIMESTAMP,
    END_TIME TIMESTAMP,
    TABLES_SCANNED INT UNSIGNED,
    ROWS_PROCESSED BIGINT UNSIGNED,
    ROWS_RESTORED BIGINT UNSIGNED,
    ENGINE ENUM('UNDO', 'BINLOG') NOT NULL,
    STATUS ENUM('SUCCESS', 'ERROR', 'CANCELLED') NOT NULL,
    ERROR_MESSAGE TEXT
);

/* 查询闪回历史 */
SELECT * FROM performance_schema.flashback_status 
ORDER BY START_TIME DESC LIMIT 100;
```

#### 5.4.2 新增状态变量

```c
/* SHOW GLOBAL STATUS 新增 */
static SHOW_VAR flashback_status_vars[] = {
    {"Flashback_queries_executed",   (char*)&flashback_queries_count, SHOW_LONG_STATUS},
    {"Flashback_tables_executed",    (char*)&flashback_tables_count,  SHOW_LONG_STATUS},
    {"Flashback_rows_restored",      (char*)&flashback_rows_restored, SHOW_LONG_STATUS},
    {"Flashback_undo_purge_hits",    (char*)&flashback_undo_purge_hits, SHOW_LONG_STATUS},
    {"Flashback_ddl_conflicts",      (char*)&flashback_ddl_conflicts,   SHOW_LONG_STATUS},
    {"Flashback_binlog_events_parsed", (char*)&flashback_binlog_events, SHOW_LONG_STATUS},
    {nullptr, nullptr, SHOW_LONG_STATUS}
};
```

---

## 6 关键代码改动清单

### 6.1 SQL 层改动

| 文件 | 改动类型 | 描述 | 预计行数 |
|------|---------|------|---------|
| `include/my_sqlcommand.h` | 新增枚举 | `SQLCOM_FLASHBACK_TABLE/QUERY/VERSIONS/TRANSACTION` | +10 |
| `sql/sql_yacc.yy` | 新增语法 | FLASHBACK 语句 + AS OF / VERSIONS BETWEEN 扩展 | +200 |
| `sql/sql_lex.h` | 扩展结构 | LEX 新增 flashback_* 字段 | +20 |
| `sql/parse_tree_nodes.h` | 新增类 | PT_flashback_table, PT_table_reference_as_of 等 | +150 |
| `sql/sql_class.cc` | 新增函数 | LEX::cleanup_flashback() | +20 |
| `sql/sql_select.cc` | 修改逻辑 | JOIN::exec() 中处理闪回查询标志 | +80 |
| `sql/sql_flashback.cc` | **新增文件** | 闪回表执行引擎 | +600 |
| `sql/sql_flashback_binlog.cc` | **新增文件** | Binlog 引擎实现 | +800 |
| `sql/sql_flashback_versions.cc` | **新增文件** | 版本查询执行器 | +400 |
| `sql/sql_error.cc` | 新增错误 | ER_FLASHBACK_* 错误码 | +20 |
| `sql/share/errmsg-utf8.txt` | 新增消息 | 闪回相关错误消息 | +30 |
| `sql/mysqld.cc` | 注册 | 状态变量 + 命令名称映射 | +15 |

### 6.2 InnoDB 层改动

| 文件 | 改动类型 | 描述 | 预计行数 |
|------|---------|------|---------|
| `storage/innobase/row/row0vers.cc` | 新增函数 | `row_build_flashback_version()` | +120 |
| `storage/innobase/include/row0vers.h` | 新增声明 | 导出闪回查询接口 | +15 |
| `storage/innobase/trx/trx0sys.cc` | 新增函数 | `trx_sys_find_trx_id_by_timestamp()` | +100 |
| `storage/innobase/trx/trx0sys.h` | 新增声明 | 导出时间戳→trx_id 映射 | +10 |
| `storage/innobase/handler/ha_innodb.cc` | 扩展类 | ha_innobase 新增闪回上下文 | +200 |
| `storage/innobase/handler/ha_innodb.h` | 扩展类 | 新增 flashback 成员变量 | +20 |
| `storage/innobase/trx/trx0purge.cc` | 修改逻辑 | 增加 purge 保护机制 | +80 |
| `storage/innobase/include/trx0sys.h` | 新增函数 | `trx_sys_get_oldest_timestamp()` | +10 |

### 6.3 新增系统参数

| 参数名 | 类型 | 默认值 | 描述 |
|--------|------|--------|------|
| `innodb_flashback_retention_seconds` | ULONG | 900 | Undo 保留时间(秒), 控制闪回查询窗口 |
| `innodb_flashback_enable_binlog_recovery` | BOOL | ON | 是否启用 Binlog 长窗口闪回 |
| `innodb_flashback_max_rows_per_txn` | ULONG | 100000 | 单次闪回事务最大处理行数 |
| `innodb_flashback_dry_run_default` | BOOL | OFF | 默认是否启用 DRY RUN |

### 6.4 总改动量预估

| 层级 | 新增文件 | 修改文件 | 新增代码行数 | 修改代码行数 |
|------|---------|---------|-------------|-------------|
| SQL 层 | 3 | 8 | ~2,300 | ~165 |
| InnoDB 层 | 0 | 6 | ~555 | ~90 |
| 测试 | ~20 | 0 | ~5,000 | 0 |
| **合计** | **~23** | **~14** | **~7,855** | **~255** |

---

## 7 风险分析与对策

### 7.1 风险矩阵

| 风险项 | 严重度 | 概率 | 影响描述 | 对策 |
|--------|--------|------|---------|------|
| **Undo Purge 导致数据不可恢复** | 高 | 中 | 目标时间点的 Undo 已被 Purge 线程清理 | ① 新增 `innodb_flashback_retention_seconds` 控制 Purge 行为 ② 闪回前做 Safety Check ③ 超出窗口自动降级到 Binlog 引擎 |
| **DDL 导致表结构不兼容** | 高 | 中 | 闪回后表结构已变 (列增删改), 恢复的数据无法适配 | ① 闪回前检查 DDL 屏障 ② 支持 DRY RUN 预览 ③ 记录表结构快照 (类似 Oracle FDA) |
| **二级索引不一致** | 高 | 低 | 主键恢复后, 二级索引未及时更新 | ① 闪回操作在 InnoDB 内部完成, 自动同步二级索引 ② 完成后执行 `ANALYZE TABLE` |
| **闪回期间并发写入冲突** | 中 | 中 | 闪回表时其他事务正在写入 | ① FLASHBACK TABLE 加 MDL EXCLUSIVE 锁 ② 闪回查询不需要锁 |
| **Binlog 解析性能瓶颈** | 中 | 高 | 长窗口闪回需要解析大量 Binlog Event | ① 使用多线程并行解析 ② 支持按表过滤 ③ 可暂停/恢复 |
| **大事务闪回导致 Undo 膨胀** | 中 | 低 | 闪回操作本身产生大量 Undo | ① 限制单次闪回行数 (`innodb_flashback_max_rows_per_txn`) ② 分批提交 |
| **主从复制不一致** | 高 | 低 | 闪回操作写入 Binlog, 复制到从库可能导致不一致 | ① 闪回语句标记为 `SQL_LOG_BIN=0` ② 或支持在从库也执行闪回 |

### 7.2 DDL 兼容性详细分析

```
DDL 类型          | 是否阻止闪回 | 原因
------------------|-------------|---------------------------
ADD COLUMN        | 否          | 新列在历史版本中为 NULL, 可兼容
DROP COLUMN       | 是          | 历史版本包含已删除列, 无法映射
RENAME COLUMN     | 是          | 列名变化导致数据映射失败
CHANGE COLUMN     | 是          | 数据类型变化可能导致数据丢失
MODIFY COLUMN     | 部分        | 类型兼容时允许, 不兼容时阻止
ADD INDEX         | 否          | 新索引无历史数据, 可重建
DROP INDEX        | 否          | 历史索引数据已无效
RENAME TABLE      | 是          | 表名变化, 需要特殊处理
TRUNCATE TABLE    | 是          | 所有历史数据已丢失
ALTER TABLE ENGINE| 是          | 存储引擎变化
```

### 7.3 Undo Purge 保护机制

```c
/**
Flashback-aware Undo Purge:
When flashback is enabled, the Purge thread must not remove
undo records that are still within the flashback retention window.
*/

/* 修改 purge 判断逻辑 */
bool trx_purge_limit_reached(
    trx_purge_t *purge,
    trx_undo_rec_t *undo_rec)
{
    /* 原有逻辑: 检查 undo_no 和 purge_limit */
    
    /* 新增: 闪回保留窗口检查 */
    if (innodb_flashback_retention_seconds > 0) {
        my_time_t undo_ts = trx_undo_get_timestamp(undo_rec);
        my_time_t cutoff_ts = my_time(nullptr) - innodb_flashback_retention_seconds;
        
        if (undo_ts >= cutoff_ts) {
            /* 此 undo record 仍在闪回窗口内, 不 Purge */
            return true;  /* limit reached */
        }
    }
    
    /* 原有逻辑继续... */
}
```

---

## 8 四阶段实施计划

### 8.1 Phase 1: SQL 语法 + Undo 闪回查询原型 (2-4 周)

**目标:** 实现 `SELECT ... AS OF TIMESTAMP` 闪回查询

**Week 1-2: SQL 语法解析**
- [ ] 新增 `FLASHBACK_SYM` 等关键字到 `sql_yacc.yy`
- [ ] 新增 `SQLCOM_FLASHBACK_*` 枚举到 `my_sqlcommand.h`
- [ ] 实现 `PT_flashback_table` / `PT_table_reference_as_of` 语法树节点
- [ ] 编译通过, 可解析但不执行

**Week 3-4: InnoDB 闪回查询**
- [ ] 实现 `row_build_flashback_version()` 函数
- [ ] 实现 `trx_sys_find_trx_id_by_timestamp()` 函数
- [ ] 扩展 `ha_innobase::rnd_next()` 支持闪回模式
- [ ] 基本单元测试: 单表闪回查询

**验收标准:**
```sql
-- 应能正确执行
SELECT * FROM employees AS OF TIMESTAMP '2025-07-28 10:30:00';

-- 应返回错误 (超出窗口)
SELECT * FROM employees AS OF TIMESTAMP '2025-01-01 00:00:00';
```

### 8.2 Phase 2: Undo 闪回表 + Binlog 闪回引擎 (4-8 周)

**目标:** 实现 `FLASHBACK TABLE` 和 Binlog 长窗口闪回

**Week 5-7: Undo 闪回表**
- [ ] 实现 `mysql_flashback_table()` 函数
- [ ] 实现 `flashback_single_table()` 全表扫描恢复
- [ ] 实现 DRY RUN 模式
- [ ] 实现 DDL 屏障检查
- [ ] 二级索引同步更新

**Week 8-10: Binlog 引擎**
- [ ] 实现 `BinlogFlashbackEngine` 类
- [ ] 实现 `find_binlog_position_at_timestamp()` 定位
- [ ] 实现 Rows Event 逆向解析
- [ ] 实现 DDL 屏障检查 (Binlog 层面)
- [ ] 集成测试: 闪回表 (Binlog 引擎路径)

**Week 11-12: 引擎调度 + 优化**
- [ ] 实现 Flashback Scheduler (引擎自动选择)
- [ ] 新增系统参数 `innodb_flashback_retention_seconds` 等
- [ ] 性能调优: 批量处理, 分批提交

**验收标准:**
```sql
-- Undo 引擎路径
FLASHBACK TABLE employees TO TIMESTAMP '2025-07-28 10:30:00';

-- Binlog 引擎路径
FLASHBACK TABLE employees TO TIMESTAMP '2025-07-27 00:00:00';

-- DRY RUN
FLASHBACK TABLE employees TO TIMESTAMP '2025-07-28 10:30:00' DRY RUN;
```

### 8.3 Phase 3: 闪回版本查询 + 并发控制 + 监控 (4-8 周)

**目标:** 实现版本查询、事务级闪回、监控集成

**Week 13-15: 闪回版本查询**
- [ ] 实现 `FlashbackVersionsIterator` 类
- [ ] 实现 `SELECT ... VERSIONS BETWEEN` 语法执行
- [ ] 返回 `versions_trx_id`, `versions_operation` 等伪列
- [ ] 单元测试: 版本链遍历

**Week 16-17: 闪回事务**
- [ ] 实现 `FLASHBACK TRANSACTION` 命令
- [ ] 解析事务的所有操作, 生成反向 SQL
- [ ] 执行反向事务
- [ ] 集成测试

**Week 18-20: 监控与测试**
- [ ] P_S 表 `performance_schema.flashback_status` 实现
- [ ] 新增 SHOW STATUS 变量
- [ ] 全面测试: 单元 + 集成 + MTR
- [ ] 性能基准测试

**验收标准:**
```sql
-- 版本查询
SELECT versions_trx_id, versions_operation, emp_id, name
FROM employees VERSIONS BETWEEN TIMESTAMP
    '2025-07-28 10:00:00' AND '2025-07-28 11:00:00';

-- 事务闪回
SELECT * FROM flashback_transaction_query WHERE trx_id = 123456;
FLASHBACK TRANSACTION 123456;

-- 监控
SELECT * FROM performance_schema.flashback_status ORDER BY START_TIME DESC;
```

### 8.4 Phase 4: 持续优化 + 多表一致性 (持续)

- [ ] Undo 表空间扩容策略优化
- [ ] 多表一致性闪回 (跨表事务一致性)
- [ ] 闪回数据归档 (类似 Oracle FDA)
- [ ] 从库闪回支持
- [ ] 文档完善

---

## 9 性能基准预估

### 9.1 闪回查询性能 (Undo 引擎)

| 场景 | 表大小 | 查询耗时 | 说明 |
|------|--------|---------|------|
| 点查 (主键) | 100 万行 | ~1ms | 仅需版本链回溯 |
| 范围扫描 | 100 万行, 返回 1000 行 | ~50ms | 每行版本链回溯 |
| 全表扫描 | 100 万行 | ~500ms | 全表版本链回溯 |
| 全表扫描 | 1000 万行 | ~5s | 需批量处理优化 |

**对比正常查询:** 闪回查询比正常查询慢约 **2-5 倍** (版本链回溯开销)

### 9.2 闪回表性能

| 引擎 | 表大小 | 恢复耗时 | 说明 |
|------|--------|---------|------|
| Undo | 100 万行, 10% 变更 | ~10s | 仅处理变更行 |
| Undo | 1000 万行, 10% 变更 | ~100s | 需分批提交 |
| Binlog | 100 万行, 10% 变更 | ~30s | Binlog 解析开销 |
| Binlog | 100 万行, 全量变更 | ~300s | 解析所有 event |

### 9.3 资源消耗

| 操作 | CPU | 内存 | 磁盘 IO | 网络 |
|------|-----|------|---------|------|
| 闪回查询 | 低 | 低 (每行 ~1KB) | 中 (读 Undo) | 无 |
| 闪回表 (Undo) | 中 | 中 (批量处理) | 高 (读写数据页) | 无 |
| 闪回表 (Binlog) | 高 | 中 (Event 缓存) | 高 (读 Binlog) | 无 |

---

## 10 附录: 关键源码文件索引

### 10.1 InnoDB Undo Log 核心文件

| 文件路径 | 行数 | 核心功能 |
|----------|------|---------|
| `storage/innobase/trx/trx0undo.cc` | ~2000 | Undo Log 创建/读取/释放 |
| `storage/innobase/trx/trx0rec.cc` | ~1500 | Undo Record 解析 |
| `storage/innobase/trx/trx0roll.cc` | ~800 | 事务回滚执行 |
| `storage/innobase/trx/trx0purge.cc` | ~1200 | Undo Purge 机制 |
| `storage/innobase/trx/trx0trx.cc` | ~2500 | 事务控制块管理 |
| `storage/innobase/row/row0undo.cc` | ~1200 | 行级 Undo 操作 |
| `storage/innobase/row/row0vers.cc` | ~1000 | 版本链构建 (关键!) |
| `storage/innobase/include/trx0undo.h` | ~600 | Undo Log 数据结构 |
| `storage/innobase/include/row0undo.h` | ~100 | 行 Undo 接口 |
| `storage/innobase/include/row0vers.h` | ~150 | 版本链接口 |

### 10.2 SQL 层核心文件

| 文件路径 | 行数 | 核心功能 |
|----------|------|---------|
| `sql/sql_yacc.yy` | 18574 | SQL 语法解析 (需新增 ~200 行) |
| `sql/binlog_reader.cc` | ~500 | Binlog 读取 |
| `sql/binlog_reader.h` | ~200 | Binlog Reader 接口 |
| `sql/log_event.cc` | ~8000 | Binlog Event 解析 |
| `sql/log_event.h` | ~4000 | Event 类定义 |
| `include/my_sqlcommand.h` | ~250 | SQL 命令枚举 |
| `sql/parse_tree_nodes.h` | ~8000 | 语法树节点定义 |

### 10.3 Handler 层文件

| 文件路径 | 行数 | 核心功能 |
|----------|------|---------|
| `storage/innobase/handler/ha_innodb.cc` | ~15000 | InnoDB Handler 实现 |
| `storage/innobase/handler/ha_innodb.h` | ~2000 | Handler 接口 |

---

## 11 总结

本方案基于 Percona Server 8.0.x 源码深度分析, 提出了一套完整的 MySQL 内置闪回实现方案。

### 方案亮点:

1. **双引擎架构**: Undo 引擎提供秒级精准闪回, Binlog 引擎提供天级长窗口恢复
2. **最大复用**: 复用 InnoDB 已有的版本链构建 (`row_vers_*`) 和回滚引擎 (`trx_rollback_*`)
3. **兼容 Oracle**: 采用 Oracle Flashback 风格的 SQL 语法 (`AS OF TIMESTAMP`, `VERSIONS BETWEEN`)
4. **安全第一**: 内置 DDL 屏障检查、DRY RUN 预览、并发锁保护
5. **可观测**: 集成 Performance Schema, 新增闪回状态监控

### 建议的启动顺序:

1. 先实现闪回查询 (Phase 1) — 风险最低, 价值最高
2. 再实现闪回表 (Phase 2) — 核心恢复能力
3. 最后实现版本查询和事务闪回 (Phase 3) — 增强能力

### 风险提醒:

- **DDL 不兼容** 是最严重的风险, 需要在实现中做严格检查
- **Undo Purge 窗口** 需要合理配置, 建议默认 15 分钟
- **主从复制** 需要决定闪回语句是否写入 Binlog

---

> *本文档由量化交易分析师 / CTA 策略专家 基于 Percona Server 8.0.x 源码分析编写.*
> *技术分析维度: 均线系统 (MA/EMA) → Undo/Binlog 双均线交叉; 动量指标 (MACD) → 闪回窗口动量分析; K线形态 → Undo Record 版本链模式识别.*
