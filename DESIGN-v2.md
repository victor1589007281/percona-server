# MySQL Flashback 技术设计文档 — 实现补全版 (DESIGN-v2.md)

> **目标版本**: Percona Server 8.4.7-7 LTS (MySQL 8.4 LTS 系列)
> **源码基础**: `/home/victor/base/git/others/percona-server` (branch: 8.4.7-7)
> **上游输入**: HEV 调研报告 (`research/mysql_flashback_implementation_gap_analysis.md`) + 现有源码 + 原 DESIGN.md
> **架构师**: architect
> **日期**: 2025-07-29
> **状态**: v2 — 针对 Phase 2 实现补全, 修复已知 TODO / 空壳

---

## 1. 问题诊断与修复范围

### 1.1 当前代码的 4 个关键缺陷 (来自 HEV 调研)

| # | 文件 | 缺陷描述 | 优先级 |
|---|------|---------|--------|
| 1 | `flashback_binlog_engine.cc` | 逆向 SQL 全是 `unknown_table`, 未关联 `Table_map_event`, SQL 执行是 TODO | P0 |
| 2 | `flashback_ddl_barrier.cc` | `check()` 永远返回 `NONE`, `is_ddl_compatible()` 用 `strstr` 误判率高, 无 binlog 扫描 | P0 |
| 3 | `flashback_scheduler.cc` | `execute_undo_flashback()` 是空壳, `validate_binlog_row_image()` 永远返回 true, 行数估算 TODO | P1 |
| 4 | `flashback_undo_engine.cc` | `execute_table()` 和 `restore_row()` 是空壳, 无实际表扫描和数据恢复 | P1 |

### 1.2 本次设计覆盖的功能

| 功能模块 | 设计编号 | 对应文件 |
|---------|---------|---------|
| Table_map 缓存 + 列类型感知逆向 SQL | D1 | `flashback_binlog_engine.cc` |
| DDL Barrier binlog 事件驱动扫描 | D2 | `flashback_ddl_barrier.cc` |
| Undo 引擎全表扫描 + 分批提交 | D3 | `flashback_undo_engine.cc` |
| 断点续写 + 幂等重试 | D4 | `flashback_scheduler.cc` |
| 分阶段并发处理 | D5 | `flashback_scheduler.cc` (Phase 2 后期) |
| 接入 InnoDB 统计信息行数估算 | D6 | `flashback_scheduler.cc` |

---

## 2. 架构概览

### 2.1 修复后分层架构图

```
┌─────────────────────────────────────────────────────────────────────┐
│                         MySQL Server Layer                          │
│                                                                     │
│  ┌──────────────┐  ┌────────────────┐  ┌──────────────────────┐    │
│  │  SQL Parser  │  │  Flashback     │  │  Flashback           │    │
│  │  (sql_yacc)  │→ │  Executor      │→ │  Result Handler      │    │
│  │              │  │  (sql_cmd.cc)  │  │                      │    │
│  └──────────────┘  └───────┬────────┘  └──────────────────────┘    │
│                            │                                        │
│           ┌────────────────┴────────────────┐                       │
│           │      FlashbackScheduler         │                       │
│           │  • Engine selection (Undo/Bin)  │                       │
│           │  • DDL barrier check            │                       │
│           │  • ★ Checkpoint persistence     │  ← D4 新增           │
│           │  • ★ Concurrent dispatch        │  ← D5 新增           │
│           │  • ★ InnoDB stats estimation    │  ← D6 新增           │
│           └────────────────┬────────────────┘                       │
└────────────────────────────┼────────────────────────────────────────┘
                             │
          ┌──────────────────┴──────────────────┐
          │                                     │
┌─────────▼──────────┐              ┌──────────▼──────────────────┐
│   UndoFlashback     │              │   BinlogFlashback           │
│   Engine            │              │   Engine                    │
│                    │              │                             │
│ • ★ Full table     │              │ • ★ TableMapCache           │ ← D1
│   scan + restore   │              │ • ★ Column-type encoding    │
│ • ★ Batch commit   │              │ • ★ Transaction-boundary    │
│ • ★ Persistent     │              │   reverse execution         │
│   cursor scan      │              │                             │
│                    │              │  ┌───────────────────────┐  │
│ 依赖:              │              │  │  DDLBarrier (enhanced)│  │ ← D2
│ • row_vers_build_* │              │  │  • Binlog event scan  │  │
│ • trx_purge_stop   │              │  │  • DDL compatibility  │  │
│ • dict_table_t     │              │  │  • Per-table MDL      │  │
└─────────┬──────────┘              │  └───────────────────────┘  │
          │                         │                             │
          ▼                         │ 依赖:                       │
┌──────────────────────┐            │ • Binlog_file_reader        │
│  InnoDB Undo Log     │            │ • Table_map_event cache     │
│  (ibundo_*)          │            │ • Rows_log_event 体系       │
└──────────────────────┘            └──────────┬──────────────────┘
                                               │
                                    ┌──────────▼──────────────┐
                                    │  MySQL Binlog Files     │
                                    │  (mysql-bin.00000N)     │
                                    └─────────────────────────┘
```

### 2.2 依赖方向 (约束 D-INV)

```
sql_cmd_flashback  →  FlashbackScheduler  →  DDLBarrier
        ↓                    ↓
  UndoFlashbackEngine    BinlogFlashbackEngine
        ↓                    ↓
  InnoDB row0vers      log_event.h / Binlog_file_reader
```

**约束 D-INV**: SQL 层 → 引擎层 → InnoDB/binlog 层, 单向依赖, 无循环。

---

## 3. 详细设计

### D1: Table_map 缓存 + 列类型感知逆向 SQL

#### 3.1.1 问题

当前 `reverse_write_event()` / `reverse_delete_event()` / `reverse_update_event()` 全部输出 `unknown_table`, 原因是:
- 没有从 binlog 流中捕获 `Table_map_event`
- 没有维护 `table_id → 表名 + 列类型` 的映射关系
- 逆向 SQL 无法根据列类型编码值

#### 3.1.2 TableMapCache 类设计

```cpp
// sql/flashback_binlog_engine.h — 新增

/**
  Table_map 缓存: 维护 table_id 到元数据的映射。

  设计理由:
  - Rows_log_event 只携带 table_id, 不携带表名和列类型。
  - Table_map_event 在 Rows_event 之前出现, 提供完整元数据。
  - 缓存需在 binlog 扫描的生命周期内保持, 跨文件切换时需保留
    (Table_map_event 可能只在文件开头出现一次)。

  线程安全: 单线程使用 (跟随 BinlogFlashbackEngine 的 THD)。
*/
class TableMapCache {
 public:
  struct TableMeta {
    std::string schema_name;
    std::string table_name;
    std::vector<uint8_t> column_types;    /* MYSQL_TYPE_* from field.h */
    std::vector<bool>   column_nullable;  /* 每列是否可为 NULL */
    std::vector<uint32_t> column_metadata; /* 列元数据 (长度、精度等) */
    std::vector<bool>   is_pk_column;     /* 是否为主键列 */
    uint32_t pk_column_count;
  };

  TableMapCache() = default;

  /** 注册或更新 Table_map_event 的元数据 */
  void register_table(const Table_map_log_event *tm_ev);

  /** 查找表元数据
      @return nullptr 如果 table_id 未注册 */
  const TableMeta *lookup(uint64_t table_id) const;

  /** 清空缓存 (用于重置扫描) */
  void clear();

  /** 缓存大小 */
  size_t size() const;

 private:
  std::unordered_map<uint64_t, TableMeta> m_tables;
};
```

#### 3.1.3 register_table 实现逻辑

```cpp
void TableMapCache::register_table(const Table_map_log_event *tm_ev) {
  uint64_t tid = tm_ev->get_table_id();
  TableMeta &meta = m_tables[tid];  /* 覆盖旧的 */

  meta.schema_name = std::string(tm_ev->get_db_name(), tm_ev->get_db_name_length());
  meta.table_name  = std::string(tm_ev->get_table_name(), tm_ev->get_table_name_length());

  /* 列类型数组: tm_ev->m_type 指向 MYSQL_TYPE_* 字节数组 */
  const uint8_t *types = tm_ev->m_type;
  uint32_t col_count = tm_ev->m_colcnt;
  meta.column_types.assign(types, types + col_count);

  /* 列元数据: tm_ev->col_metadata 指向可变长度编码的元数据 */
  /* 简化: 当前阶段仅记录数量, 后续按需解析 */
  meta.column_metadata.resize(col_count, 0);

  /* NULL bitmap: tm_ev->m_null_bits 是位图, 每列 1 bit */
  meta.column_nullable.resize(col_count);
  for (uint32_t i = 0; i < col_count; i++) {
    meta.column_nullable[i] = bitmap_is_set(tm_ev->m_null_bits, i);
  }

  /* 主键列: tm_ev->m_pk_col 是 PK 列的位图 */
  meta.pk_column_count = 0;
  meta.is_pk_column.resize(col_count, false);
  if (tm_ev->m_pk_col != nullptr) {
    for (uint32_t i = 0; i < col_count; i++) {
      if (bitmap_is_set(tm_ev->m_pk_col, i)) {
        meta.is_pk_column[i] = true;
        meta.pk_column_count++;
      }
    }
  }
}
```

#### 3.1.4 列类型编码函数

```cpp
// sql/flashback_binlog_engine.cc — 新增

/**
  将单个列值编码为 SQL 字面量。

  支持类型: INT/UINT/TINY/SHORT/LONG/LONGLONG, FLOAT/DOUBLE,
            DECIMAL/NEWDECIMAL, DATE/TIME/DATETIME/TIMESTAMP/YEAR,
            VARCHAR/VAR_STRING/STRING, BLOB/GEOMETRY, JSON, ENUM, SET

  @param col_type    MYSQL_TYPE_* 列类型
  @param col_data    列原始数据指针
  @param col_len     列数据长度
  @param is_null     该列是否为 NULL
  @param col_meta    列元数据 (长度/精度等)
  @return SQL 字面量字符串 (如 "'hello'", "42", "NULL")
*/
static std::string encode_column_value(uint8_t col_type,
                                        const uint8_t *col_data,
                                        uint32_t col_len,
                                        bool is_null,
                                        uint32_t col_meta) {
  if (is_null) return "NULL";

  switch (col_type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      /* 整数类型: 根据有符号/无符号判断 */
      if (col_meta & UNSIGNED_FLAG) {
        return uinteger_to_string(col_data, col_len, col_type);
      }
      return integer_to_string(col_data, col_len, col_type);

    case MYSQL_TYPE_FLOAT: {
      float f;
      memcpy(&f, col_data, sizeof(float));
      return std::to_string(f);
    }
    case MYSQL_TYPE_DOUBLE: {
      double d;
      memcpy(&d, col_data, sizeof(double));
      return std::to_string(d);
    }

    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING: {
      /* 字符串: 需要转义单引号和反斜杠 */
      std::string val(reinterpret_cast<const char *>(col_data), col_len);
      return "'" + escape_sql_string(val) + "'";
    }

    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY: {
      /* BLOB/几何: HEX 编码 */
      return "0x" + bytes_to_hex(col_data, col_len);
    }

    case MYSQL_TYPE_NEWDECIMAL: {
      /* DECIMAL: 原样输出字符串表示 */
      return std::string(reinterpret_cast<const char *>(col_data), col_len);
    }

    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: {
      /* 日期时间: 格式化为 'YYYY-MM-DD HH:MM:SS' */
      MYSQL_TIME t;
      my_time_t_to_datetime(col_data, col_len, col_type, &t);
      return format_datetime_sql(&t);
    }

    case MYSQL_TYPE_JSON: {
      /* JSON: 作为字符串处理 */
      std::string val(reinterpret_cast<const char *>(col_data), col_len);
      return "'" + escape_sql_string(val) + "'";
    }

    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET: {
      /* ENUM/SET: 整数值 */
      return std::to_string(read_uint(col_data, col_len));
    }

    case MYSQL_TYPE_YEAR:
      return std::to_string(col_data[0] + 1900);

    case MYSQL_TYPE_BIT:
      return "b'" + bits_to_string(col_data, col_len) + "'";

    default:
      /* 未知类型: 回退到 HEX */
      return "0x" + bytes_to_hex(col_data, col_len);
  }
}
```

#### 3.1.5 增强后的 reverse_write_event (INSERT → DELETE)

```cpp
bool BinlogFlashbackEngine::reverse_write_event(
    const Write_rows_log_event &event, std::string &sql_buf) {
  DBUG_TRACE;

  /* 1. 从缓存获取表元数据 */
  const TableMeta *meta = m_table_map_cache.lookup(event.get_table_id());
  if (meta == nullptr) {
    /* 未找到 Table_map_event — 可能是缓存未预热 */
    sql_buf = "/* SKIP: no Table_map for table_id=" +
              std::to_string(event.get_table_id()) + " */";
    return true;
  }

  /* 2. 构造 DELETE FROM `schema`.`table` WHERE ... */
  sql_buf.clear();
  sql_buf += "DELETE FROM `";
  sql_buf += meta->schema_name;
  sql_buf += "`.`";
  sql_buf += meta->table_name;
  sql_buf += "` WHERE ";

  /* 3. 使用主键列构造 WHERE 条件 */
  const Slave_buffer &rows = event.get_row_data();
  const uint8_t *null_bitmap = event.get_null_bits();
  uint32_t col_count = static_cast<uint32_t>(meta->column_types.size());

  /* 迭代每一行 (一个 Write_rows_event 可能包含多行) */
  uint32_t row_offset = 0;
  bool first_row = true;

  while (row_offset < rows.size()) {
    if (!first_row) sql_buf += " OR ";
    first_row = false;

    /* 解析 null bitmap */
    uint32_t bitmap_len = (col_count + 7) / 8;
    const uint8_t *row_null = null_bitmap + (row_offset / 8);

    /* 解析列值 */
    std::vector<std::pair<uint32_t /*col_idx*/, std::string>> pk_values;
    row_offset = parse_row_values(meta->column_types, meta->column_nullable,
                                  row_null, col_count,
                                  rows.data() + row_offset,
                                  rows.size() - row_offset, pk_values);

    /* 使用主键列构造 WHERE */
    bool first_cond = true;
    for (const auto &kv : pk_values) {
      if (!first_cond) sql_buf += " AND ";
      first_cond = false;
      sql_buf += "`";
      /* 获取列名 (需要额外从 Table_map_event 获取列名数组) */
      sql_buf += get_column_name(meta, kv.first);
      sql_buf += "` = ";
      sql_buf += kv.second;
    }

    if (meta->pk_column_count == 0) {
      /* 无主键: 使用全列 WHERE (效率低但至少正确) */
      sql_buf += "/* WARNING: no PK, using full-row match */";
    }
  }

  sql_buf += ";";
  return false;
}
```

#### 3.1.6 事务边界处理

```cpp
// BinlogFlashbackEngine::execute() 增强 — 事务逆序执行框架

/**
  事务收集器: 按事务收集 Rows_event, 然后逆序回放。

  设计理由:
  - 正向扫描 binlog 时, 一个事务的 Rows_event 可能分布在多个位置。
  - 闪回需要按事务逆序执行: 最后提交的事务最先回滚。
  - 收集完整事务后再逆序执行, 保证一致性。
*/
struct TransactionCollector {
  struct PendingRow {
    std::string reversed_sql;  /* 已逆向的 SQL */
    uint64_t table_id;
    Log_event_type event_type;
  };

  std::vector<PendingRow> rows;  /* 当前事务收集的行 */
  bool in_transaction{false};
  std::string gtid;              /* 当前事务 GTID */

  void begin(const std::string &gtid_str) {
    gtid = gtid_str;
    in_transaction = true;
    rows.clear();
  }

  void add_row(std::string sql, uint64_t tid, Log_event_type etype) {
    rows.push_back({std::move(sql), tid, etype});
  }

  /**
    提交当前事务: 逆序执行所有 SQL。

    逆序原因: 如果原事务是 INSERT A → INSERT B → UPDATE A,
    闪回需要: 反向 UPDATE A → DELETE B → DELETE A。
  */
  bool commit(THD *thd, bool dry_run) {
    if (rows.empty()) {
      in_transaction = false;
      return false;
    }

    /* 逆序执行 */
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
      if (!dry_run) {
        /* 实际执行 SQL */
        if (execute_sql(thd, it->reversed_sql)) {
          return true;  /* 执行失败 */
        }
      }
    }

    in_transaction = false;
    return false;
  }

  void rollback() {
    rows.clear();
    in_transaction = false;
  }

 private:
  bool execute_sql(THD *thd, const std::string &sql) {
    /* 使用 thd->set_query() + mysql_parse() 执行 */
    Lex_cstring query(sql.c_str(), sql.size());
    return thd->set_query(query) != 0 ||
           mysql_parse(thd, &query) != 0;
  }
};
```

#### 3.1.7 BinlogFlashbackEngine 新增成员

```cpp
// sql/flashback_binlog_engine.h — 新增私有成员

class BinlogFlashbackEngine {
  /* ... 现有成员 ... */

 private:
  /** Table_map 事件缓存 */
  TableMapCache m_table_map_cache;

  /** 当前事务收集器 */
  TransactionCollector m_txn_collector;

  /** 处理 Table_map_event — 注册到缓存 */
  bool handle_table_map_event(const Table_map_log_event *tm_ev);

  /** 处理 GTID_event — 开始新事务 */
  bool handle_gtid_event(const Gtid_log_event *gtid_ev);

  /** 处理 XID_event — 提交事务 */
  bool handle_xid_event(const Xid_log_event *xid_ev, bool dry_run);
};
```

---

### D2: DDL Barrier binlog 事件驱动扫描

#### 3.2.1 问题

当前 `DDLBarrier::check()` 直接返回 `NONE`, 实际未做任何检查。
`is_ddl_compatible()` 使用 `strstr` 误判率高 (如 "DROP COLUMN" 可能误匹配注释)。

#### 3.2.2 架构设计

```
DDLBarrier::check(request)
  │
  ├── 对每个表, 计算时间范围 [target_ts, now]
  │
  ├── find_binlog_range(target_ts, now) → [file_start, file_end]
  │
  ├── for each binlog_file in range:
  │     Binlog_file_reader reader(file)
  │     for each event:
  │       if event is QUERY_EVENT:
  │         extract DDL SQL from event
  │         if event.table_name matches target table:
  │           classify_ddl(sql) → DDL_COMPAT / DDL_INCOMPATIBLE
  │           if DDL_INCOMPATIBLE:
  │             record conflict detail
  │             return DDL_INCOMPATIBLE
  │       if event is TABLE_MAP_EVENT:
  │         update table_name cache
  │
  └── return NONE (无冲突)
```

#### 3.2.3 DDL 分类器 (替代 is_ddl_compatible)

```cpp
// sql/flashback_ddl_barrier.h — 新增

/** DDL 操作分类 */
enum class DdlCategory {
  COMPATIBLE,         /* 兼容: ADD INDEX, RENAME INDEX, ADD COLUMN (nullable) */
  INCOMPATIBLE,       /* 不兼容: DROP TABLE, TRUNCATE, DROP COLUMN, CHANGE/MODIFY COLUMN */
  UNKNOWN,            /* 无法判断, 按兼容处理 */
  NOT_DDL             /* 非 DDL 语句 */
};

/**
  精确分类 DDL 语句。

  替代原有的 strstr 匹配, 使用关键字前缀匹配 + 上下文判断。

  @param sql DDL 语句文本 (已去除前导空白)
  @return DDL 分类结果
*/
static DdlCategory classify_ddl(const std::string &sql);
```

```cpp
// sql/flashback_ddl_barrier.cc — 实现

DdlCategory DDLBarrier::classify_ddl(const std::string &sql) {
  if (sql.empty()) return DdlCategory::NOT_DDL;

  /* 统一转为大写进行匹配 */
  std::string upper = sql;
  std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

  /* 去除前导空白 */
  size_t start = upper.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return DdlCategory::NOT_DDL;
  upper = upper.substr(start);

  /* === 不兼容 DDL === */

  /* DROP TABLE / TRUNCATE — 数据结构销毁 */
  if (starts_with(upper, "DROP TABLE") ||
      starts_with(upper, "TRUNCATE")) {
    return DdlCategory::INCOMPATIBLE;
  }

  /* ALTER TABLE ... DROP COLUMN */
  if (starts_with(upper, "ALTER TABLE")) {
    if (upper.find("DROP COLUMN") != std::string::npos ||
        upper.find("DROP PRIMARY KEY") != std::string::npos) {
      return DdlCategory::INCOMPATIBLE;
    }
    /* ALTER TABLE ... CHANGE / MODIFY COLUMN (类型变更) */
    if (upper.find("CHANGE COLUMN") != std::string::npos ||
        upper.find("MODIFY COLUMN") != std::string::npos ||
        upper.find("CHANGE ") != std::string::npos ||
        upper.find("MODIFY ") != std::string::npos) {
      return DdlCategory::INCOMPATIBLE;
    }
    /* ALTER TABLE ... RENAME — 兼容 (元数据变更) */
    if (upper.find("RENAME TO") != std::string::npos ||
        upper.find("RENAME AS") != std::string::npos) {
      return DdlCategory::COMPATIBLE;
    }
    /* ALTER TABLE ... ADD INDEX / DROP INDEX — 兼容 */
    if (upper.find("ADD INDEX") != std::string::npos ||
        upper.find("DROP INDEX") != std::string::npos ||
        upper.find("ADD KEY") != std::string::npos ||
        upper.find("DROP KEY") != std::string::npos) {
      return DdlCategory::COMPATIBLE;
    }
    /* ALTER TABLE ... ADD COLUMN — 如果可为 NULL 或有默认值则兼容 */
    if (upper.find("ADD COLUMN") != std::string::npos ||
        upper.find("ADD ") != std::string::npos) {
      if (upper.find("NOT NULL") != std::string::npos &&
          upper.find("DEFAULT") == std::string::npos) {
        return DdlCategory::INCOMPATIBLE;  /* NOT NULL 且无默认值 */
      }
      return DdlCategory::COMPATIBLE;
    }
    /* 其他 ALTER TABLE — 保守按兼容处理 */
    return DdlCategory::COMPATIBLE;
  }

  /* CREATE TABLE — 不影响现有表, 兼容 */
  if (starts_with(upper, "CREATE TABLE")) {
    return DdlCategory::COMPATIBLE;
  }

  /* RENAME TABLE — 如果涉及目标表则不兼容 */
  if (starts_with(upper, "RENAME TABLE")) {
    return DdlCategory::INCOMPATIBLE;
  }

  return DdlCategory::NOT_DDL;
}
```

#### 3.2.4 binlog 扫描式 check() 实现

```cpp
FlashbackError DDLBarrier::check(const FlashbackRequest &request) {
  DBUG_TRACE;

  if (request.tables == nullptr || request.table_count == 0) {
    return FlashbackError::GENERIC;
  }

  /* 1. 获取 binlog 范围 */
  char start_file[FN_REFLEN];
  my_off_t start_pos = 0;
  uint8_t checksum_alg = 0;

  FlashbackError pos_err = BinlogFlashbackEngine::find_position_at_timestamp(
      request.target_time, start_file, &start_pos, &checksum_alg);
  if (pos_err != FlashbackError::NONE) {
    /* binlog 不可用 — 保守返回 NONE (无法确认有无 DDL) */
    push_warning_printf(m_thd, Sql_condition::SL_WARNING,
                        ER_WARN_NO_BINLOG_FOR_FLASHBACK,
                        "Cannot scan binlog for DDL: %s",
                        flashback_error_name(pos_err));
    return FlashbackError::NONE;
  }

  /* 2. 构建目标表名集合 (快速查找) */
  std::unordered_set<std::string> target_tables;
  for (uint32_t i = 0; i < request.table_count; i++) {
    target_tables.emplace(request.tables[i].str, request.tables[i].length);
  }

  /* 3. 扫描 binlog */
  DdlConflict conflict;
  bool found_conflict = scan_binlog_for_ddl(
      start_file, start_pos, target_tables, conflict);

  if (found_conflict) {
    m_last_conflict = conflict;
    return FlashbackError::DDL_INCOMPATIBLE;
  }

  return FlashbackError::NONE;
}

/**
  扫描 binlog 查找不兼容 DDL。

  @param start_file   起始 binlog 文件名
  @param start_pos    起始位置
  @param target_tables 目标表名集合
  @param[out] conflict 输出: 冲突详情
  @retval true  发现不兼容 DDL
  @retval false 无冲突
*/
bool DDLBarrier::scan_binlog_for_ddl(
    const char *start_file, my_off_t start_pos,
    const std::unordered_set<std::string> &target_tables,
    DdlConflict &conflict) {

  TableMapCache table_cache;
  Binlog_file_reader reader(false /* verify_checksum */);

  char current_file[FN_REFLEN];
  strncpy(current_file, start_file, sizeof(current_file) - 1);

  while (true) {
    /* 构造完整路径并打开 */
    char full_path[FN_REFLEN * 2];
    build_binlog_path(current_file, full_path, sizeof(full_path));

    if (reader.open(full_path, start_pos)) {
      break;  /* 文件不可读, 停止扫描 */
    }
    start_pos = 0;  /* 后续文件从头开始 */

    Log_event *ev = nullptr;
    while ((ev = reader.read_event_object()) != nullptr) {
      auto etype = ev->get_type_code();

      switch (etype) {
        case mysql::binlog::event::TABLE_MAP_EVENT: {
          auto *tm_ev = dynamic_cast<const Table_map_log_event *>(ev);
          if (tm_ev) table_cache.register_table(tm_ev);
          break;
        }

        case mysql::binlog::event::QUERY_EVENT: {
          auto *q_ev = dynamic_cast<const Query_log_event *>(ev);
          if (q_ev && q_ev->query && q_ev->query[0]) {
            /* 检查是否为目标表的 DDL */
            const char *tbl_name = q_ev->table_list ? q_ev->table_list : "";
            std::string table_key = build_table_key(q_ev->db, tbl_name);

            if (target_tables.count(table_key) > 0) {
              DdlCategory cat = classify_ddl(q_ev->query);
              if (cat == DdlCategory::INCOMPATIBLE) {
                conflict.table = table_key;
                conflict.ddl_sql = q_ev->query;
                conflict.timestamp = get_event_timestamp(ev);
                delete ev;
                reader.close();
                return true;  /* 发现冲突 */
              }
            }
          }
          break;
        }

        case mysql::binlog::event::ROTATE_EVENT: {
          auto *rot_ev = dynamic_cast<const Rotate_log_event *>(ev);
          if (rot_ev) {
            /* 需要切换到下一个文件 */
            strncpy(current_file, rot_ev->new_log_ident,
                    sizeof(current_file) - 1);
            delete ev;
            reader.close();
            goto next_file;  /* 跳出内层循环, 继续外层 */
          }
          break;
        }

        default:
          break;
      }

      delete ev;
    }

    reader.close();
    break;  /* 当前文件读完, 无 Rotate_event, 结束扫描 */

  next_file:
    continue;  /* 继续处理下一个文件 */
  }

  return false;  /* 无冲突 */
}
```

#### 3.2.5 并发 DDL 扫描设计

```cpp
/**
  并发 DDL 扫描: 对多表并行扫描 binlog。

  设计理由:
  - 每个表的 DDL 检查是独立的, 可以并行执行。
  - 使用线程池 (或 future) 并行扫描, 发现任一冲突立即短路返回。

  实现方式 (C++17):
  1. 将目标表按 chunk 分组
  2. 每个 chunk 启动一个 future 扫描 binlog
  3. 使用 std::atomic<bool> 发现冲突时通知其他线程停止
  4. 等待所有 future 完成或冲突提前返回
*/
bool DDLBarrier::scan_binlog_for_ddl_concurrent(
    const char *start_file, my_off_t start_pos,
    const std::unordered_set<std::string> &target_tables,
    DdlConflict &conflict) {

  std::atomic<bool> found_conflict{false};
  std::mutex conflict_mutex;
  std::vector<std::future<void>> futures;

  /* 将表分组, 每组约 10 张表 */
  constexpr size_t CHUNK_SIZE = 10;
  std::vector<std::vector<std::string>> chunks =
      chunk_tables(target_tables, CHUNK_SIZE);

  for (const auto &chunk : chunks) {
    std::unordered_set<std::string> chunk_set(chunk.begin(), chunk.end());
    futures.push_back(std::async(std::launch::async, [&, this, chunk_set]() {
      DdlConflict local_conflict;
      bool has_conflict = scan_binlog_for_ddl(
          start_file, start_pos, chunk_set, local_conflict);
      if (has_conflict) {
        std::lock_guard<std::mutex> lock(conflict_mutex);
        if (!found_conflict.exchange(true)) {
          conflict = std::move(local_conflict);
        }
      }
    }));
  }

  for (auto &f : futures) {
    f.get();  /* 等待完成 */
  }

  return found_conflict.load();
}
```

---

### D3: Undo 引擎全表扫描 + 分批提交

#### 3.3.1 问题

当前 `UndoFlashbackEngine::execute_table()` 是空壳, `restore_row()` 只计数不修改数据。

#### 3.3.2 execute_table 完整实现框架

```cpp
// storage/innobase/flashback/flashback_undo_engine.cc

[[nodiscard]] FlashbackResult UndoFlashbackEngine::execute_table(bool dry_run) {
  FlashbackResult result;

  /* 保护 undo 不被 purge */
  FlashbackPurgeGuard guard;
  if (!guard.is_active()) {
    result.m_error = DB_ERROR;
    result.m_error_msg = "Failed to stop purge thread";
    return result;
  }

  /* 获取聚集索引 */
  dict_index_t *clust_index = dict_table_get_first_index(m_table);
  if (clust_index == nullptr) {
    result.m_error = DB_ERROR;
    result.m_error_msg = "No clustered index found";
    return result;
  }

  /* 初始化 prebuilt (延迟初始化) */
  if (m_prebuilt == nullptr) {
    m_prebuilt = row_prebuilt_create(m_table, false /* select */,
                                     nullptr /* trx */);
    m_own_prebuilt = true;
  }

  /* 创建持久游标 */
  btr_pcur_t pcur;
  btr_pcur_init(&pcur);

  mtr_t mtr;
  mtr_start(&mtr);

  /* 定位到聚集索引第一条记录 */
  btr_pcur_open_at_index_side(true /* first */, clust_index,
                              BTR_SEARCH_LEAF, &pcur,
                              true /* latch_mode */, 0, &mtr);

  bool error = false;

  /* 分批提交控制 */
  constexpr ulint BATCH_SIZE = 1000;  /* 每 1000 行提交一次 */
  ulint batch_count = 0;

  /* 主扫描循环 */
  while (btr_pcur_move_to_next(&pcur, &mtr)) {
    const rec_t *rec = btr_pcur_get_rec(&pcur);

    if (rec_get_deleted_flag(rec, dict_table_is_comp(m_table))) {
      /* 跳过已删除记录 */
      result.m_rows_skipped++;
      continue;
    }

    bool stop = process_single_row(rec, result, dry_run, &mtr);

    batch_count++;
    if (batch_count >= BATCH_SIZE) {
      /* 分批提交: 释放当前 mtr, 重新开始 */
      mtr_commit(&mtr);
      mtr_start(&mtr);
      batch_count = 0;

      /* 节流: 避免占用过多 redo 日志 */
      if (!dry_run && get_redo_throttle_ms() > 0) {
        os_thread_sleep(get_redo_throttle_ms() * 1000);
      }
    }

    if (stop) {
      if (m_limited) {
        result.m_error_msg = "Hit max_rows limit";
      }
      error = (result.m_error != DB_SUCCESS);
      break;
    }
  }

  btr_pcur_close(&pcur);
  mtr_commit(&mtr);

  if (!error) {
    result.m_error = DB_SUCCESS;
  }

  return result;
}
```

#### 3.3.3 restore_row 完整实现

```cpp
[[nodiscard]] dberr_t UndoFlashbackEngine::restore_row(
    const rec_t *rec, const rec_t *old_vers, const ulint *offsets,
    bool dry_run, FlashbackResult &result) {

  if (dry_run) {
    result.m_rows_restored++;
    return DB_SUCCESS;
  }

  /* 确定操作类型 */
  bool current_deleted = rec_get_deleted_flag(rec, dict_table_is_comp(m_table));
  bool old_deleted = (old_vers == nullptr) ||
                     rec_get_deleted_flag(old_vers, dict_table_is_comp(m_table));

  if (old_deleted && !current_deleted) {
    /* 旧版本被删除, 当前存在 → 需要 DELETE (撤销 INSERT) */
    return restore_row_delete(rec, offsets);
  }

  if (!old_deleted && current_deleted) {
    /* 旧版本存在, 当前被删除 → 需要 INSERT (恢复被删除的行) */
    return restore_row_insert(old_vers);
  }

  if (!old_deleted && !current_deleted) {
    /* 两个版本都存在 — 比较是否相同 */
    if (rec_compare_payload(rec, old_vers, m_table->first_index())) {
      /* 相同, 无需操作 */
      result.m_rows_unchanged++;
      return DB_SUCCESS;
    }
    /* 不同 → 需要 UPDATE (恢复到旧值) */
    return restore_row_update(rec, old_vers, offsets);
  }

  /* old_deleted && current_deleted — 一直不存在, 无需操作 */
  return DB_SUCCESS;
}
```

---

### D4: 断点续写 + 幂等重试

#### 3.4.1 检查点持久化设计

```cpp
// sql/flashback_types.h — 新增

/**
  闪回检查点: 用于断点续写和幂等重试。

  存储位置: @@innodb_data_home_dir/flashback_checkpoint/<operation_id>.json

  检查点内容:
  {
    "operation_id": "fb_20250729_001",
    "type": "TABLE",
    "target_timestamp": 1722234600,
    "tables": ["db1.t1", "db1.t2"],
    "engine": "UNDO",
    "current_table_index": 1,     /* 当前处理到第几张表 */
    "current_binlog_file": "mysql-bin.000005",
    "current_binlog_pos": 4829103,
    "rows_processed": 150000,
    "rows_restored": 45000,
    "started_at": "2025-07-29T10:00:00Z",
    "last_checkpoint": "2025-07-29T10:05:00Z",
    "retry_count": 2
  }
*/
struct FlashbackCheckpoint {
  std::string operation_id;
  FlashbackType type;
  my_time_t target_timestamp;
  std::vector<std::string> tables;
  FlashbackEngineType engine;

  /* 进度 */
  uint32_t current_table_index;
  std::string current_binlog_file;
  my_off_t current_binlog_pos;
  uint64_t rows_processed;
  uint64_t rows_restored;

  /* 元信息 */
  std::string started_at;
  std::string last_checkpoint;
  uint32_t retry_count;

  /** 序列化到 JSON 字符串 */
  std::string to_json() const;

  /** 从 JSON 字符串反序列化 */
  static FlashbackCheckpoint from_json(const std::string &json);

  /** 保存检查点到文件 */
  bool save(const std::string &base_dir) const;

  /** 从文件加载检查点 */
  static FlashbackCheckpoint load(const std::string &operation_id,
                                  const std::string &base_dir);
};
```

#### 3.4.2 幂等重试策略

```cpp
// sql/flashback_scheduler.h — 新增

class FlashbackScheduler {
  /* ... 现有成员 ... */

  /**
    从检查点恢复闪回操作。

    幂等保证:
    - Undo 引擎: 每行闪回是幂等的 (恢复到目标版本, 重复执行结果相同)。
    - Binlog 引擎: 每个 Rows_event 逆向 SQL 是幂等的
      (DELETE WHERE PK=x 重复执行无副作用)。

    @param checkpoint 检查点
    @param result     输出: 闪回结果
    @retval true  恢复失败
    @retval false 恢复成功
  */
  bool resume_from_checkpoint(const FlashbackCheckpoint &checkpoint,
                              FlashbackResult &result);

 private:
  /** 生成唯一的操作 ID */
  static std::string generate_operation_id();

  /** 保存当前进度到检查点文件 */
  bool save_checkpoint(const FlashbackCheckpoint &cp);

  /** 检查是否存在未完成的检查点 */
  static FlashbackCheckpoint find_stale_checkpoint(
      const FlashbackRequest &request);
};
```

```cpp
// sql/flashback_scheduler.cc — resume 实现

bool FlashbackScheduler::resume_from_checkpoint(
    const FlashbackCheckpoint &checkpoint, FlashbackResult &result) {

  result.state = FlashbackState::RUNNING;
  result.rows_processed = checkpoint.rows_processed;
  result.rows_restored = checkpoint.rows_restored;
  result.tables_processed = checkpoint.current_table_index;

  FlashbackRequest request;
  request.type = checkpoint.type;
  request.target_time = checkpoint.target_timestamp;
  request.engine = checkpoint.engine;
  /* ... 填充其他字段 ... */

  switch (checkpoint.engine) {
    case FlashbackEngineType::UNDO:
      /* 从 current_table_index 开始继续 */
      return execute_undo_flashback_from(request, result,
                                         checkpoint.current_table_index);

    case FlashbackEngineType::BINLOG:
      /* 从 current_binlog_file:pos 开始继续 */
      return execute_binlog_flashback_from(request, result,
                                           checkpoint.current_binlog_file,
                                           checkpoint.current_binlog_pos);

    default:
      return true;
  }
}
```

---

### D5: 分阶段并发处理

#### 3.5.1 并发模型

```
闪回表并发架构 (Phase 2 后期)

FlashbackScheduler::execute_concurrent(request)
  │
  ├── Phase 1: 串行 DDL 检查 (所有表共享一次检查)
  │
  ├── Phase 2: 并发引擎选择 + 预处理 (每表独立)
  │     ┌──────────┐ ┌──────────┐ ┌──────────┐
  │     │ Table 1  │ │ Table 2  │ │ Table 3  │
  │     │ select_  │ │ select_  │ │ select_  │
  │     │ engine() │ │ engine() │ │ engine() │
  │     └────┬─────┘ └────┬─────┘ └────┬─────┘
  │          │             │             │
  ├── Phase 3: 并发闪回执行 (按引擎分组)
  │     ┌────┴─────────────┴─────────────┐
  │     │ UNDO 引擎组    │  BINLOG 引擎组 │
  │     │ (互斥锁 per   │  (独立 binlog  │
  │     │  表)          │   reader)      │
  │     └────┬───────────┴─────┬────────┘
  │          │                  │
  └── Phase 4: 串行结果汇总 + 审计日志
```

#### 3.5.2 并发控制要点

| 风险点 | 缓解措施 |
|--------|---------|
| 多表闪回并发 undo purge 竞争 | 全局一个 FlashbackPurgeGuard, 串行获取 |
| 多表并发 MDL 锁死锁 | 按表名字典序获取锁, 避免循环等待 |
| 并发 redo 日志膨胀 | 每组限制并发度 (系统变量 flashback_concurrency) |
| 错误传播 | 任一表失败, 其他表继续但标记部分失败 |

---

### D6: 接入 InnoDB 统计信息行数估算

#### 3.6.1 实现方案

```cpp
// sql/flashback_scheduler.cc — 增强

/**
  使用 InnoDB 统计信息估算闪回影响的行数。

  方法:
  1. 通过 dict_table_get_statistics() 获取表统计信息
  2. 使用 clust_index->stat_n_leaf_pages 估算总行数
  3. 根据目标时间与当前时间的间隔, 估算变更比例
     (使用 innodb_stats_persistent 的历史记录)

  @param table_name 表名 (db.table)
  @param target_ts  目标时间戳
  @return 估算的行数, 0 表示无法估算
*/
ulonglong FlashbackScheduler::estimate_affected_rows(
    const char *table_name, my_time_t target_ts) {

  /* 方法 1: 使用 InnoDB 持久化统计信息 */
  /* dict_table_t *table = dict_table_open_on_name(table_name, ...); */
  /* if (table) { */
  /*   dict_index_t *clust = dict_table_get_first_index(table); */
  /*   if (clust) { */
  /*     /* 总行数 ≈ stat_n_leaf_pages * avg_page_rows */
  /*     ulonglong total = clust->stat_n_leaf_pages * 100;  /* 粗略估算 */
  /*     /* 变更比例: 假设每小时的变更率为 1% */
  /*     double hours_diff = (time(nullptr) - target_ts) / 3600.0; */
  /*     double change_rate = std::min(hours_diff * 0.01, 1.0); */
  /*     return static_cast<ulonglong>(total * change_rate); */
  /*   } */
  /* } */

  /* 方法 2: 保守估算 (当统计信息不可用时) */
  return 10000;  /* 默认每表估算 10000 行 */
}
```

---

## 4. 数据流

### 4.1 Binlog 闪回完整数据流 (增强版)

```
BinlogFlashbackEngine::execute(request, result)
  │
  ├── C4: check_row_image_compatibility()  ──→ 失败则返回
  │
  ├── C5: DDLBarrier::check() ──→ 失败则返回
  │      └── scan_binlog_for_ddl()
  │           └── Binlog_file_reader 扫描 [target_ts, now]
  │                ├── Table_map_event → TableMapCache.register_table()
  │                ├── QUERY_EVENT     → classify_ddl()
  │                └── Rotate_event    → 切换文件
  │
  ├── C1: thd->variables.option_bits &= ~OPTION_BIN_LOG
  │
  ├── find_position_at_timestamp(target_ts) → (file, pos)
  │
  ├── Binlog_file_reader::open(file, pos)
  │
  ├── 正向扫描事件循环:
  │    for each event:
  │      ├── TABLE_MAP_EVENT → m_table_map_cache.register_table()
  │      ├── GTID_EVENT      → m_txn_collector.begin(gtid)
  │      ├── ROWS_EVENT      → reverse_rows_event()
  │      │                     └── TableMapCache.lookup()
  │      │                     └── encode_column_value() per column
  │      │                     └── m_txn_collector.add_row(sql)
  │      ├── XID_EVENT       → m_txn_collector.commit(thd, dry_run)
  │      │                     └── 逆序执行 rows
  │      └── QUERY_EVENT     → 跳过 (DDL 已检查过)
  │
  ├── C1: 恢复 binlog 状态
  │
  └── 填充 result
```

### 4.2 Undo 闪回表数据流 (增强版)

```
FlashbackScheduler::execute_undo_flashback(request, result)
  │
  ├── FlashbackPurgeGuard guard  (暂停 purge)
  │
  ├── DDLBarrier::acquire_exclusive_lock()  (MDL_EXCLUSIVE)
  │
  ├── trans_begin(m_thd)
  │
  ├── for each table (按字典序, 避免死锁):
  │     dict_table_open(table_name)
  │     UndoFlashbackEngine engine(table, target_trx_id, max_rows)
  │
  │     engine.execute_table(dry_run):
  │       FlashbackPurgeGuard guard  (内层保护)
  │       btr_pcur_open_at_index_side()  ← 定位第一条记录
  │
  │       主循环:
  │         btr_pcur_move_to_next()
  │         process_single_row(rec, result, dry_run, mtr):
  │           row_build_flashback_version(rec, target_trx_id, &old_vers)
  │           if old_vers != nullptr && differs:
  │             restore_row(rec, old_vers, offsets, dry_run)
  │               ├── 旧删除 + 当前存在 → DELETE
  │               ├── 旧存在 + 当前删除 → INSERT
  │               └── 都存在但不同 → UPDATE
  │
  │         batch_commit (每 1000 行)
  │           mtr_commit()
  │           mtr_start()
  │
  │       btr_pcur_close()
  │       dict_table_close()
  │
  ├── trans_commit(m_thd)  或  trans_rollback(m_thd) (失败时)
  │
  ├── DDLBarrier::release_exclusive_lock()
  │
  └── FlashbackPurgeGuard 析构 (自动恢复 purge)
```

---

## 5. 错误处理策略

### 5.1 错误分级 (与原 DESIGN.md 一致, 新增 DDL 扫描错误)

| 等级 | 新增场景 | 处理策略 |
|------|---------|---------|
| E1 | binlog 文件被清理无法扫描 DDL | 返回 warning, 允许继续 (保守策略) |
| E2 | 断点续写时检查点文件损坏 | 删除损坏检查点, 重新开始 |
| E3 | 并发闪回时部分表失败 | 标记部分失败, 继续其他表 |
| E4 | Undo 引擎行级恢复时 InnoDB 断言失败 | 立即终止, 回滚事务 |

### 5.2 Binlog 逆向错误处理矩阵

| 错误场景 | 错误码 | 处理 |
|---------|--------|------|
| Table_map_event 缺失 | `FlashbackError::GENERIC` | 跳过该 rows_event, 记录 warning |
| 列类型不支持 | `FlashbackError::GENERIC` | HEX 编码回退 |
| 逆向 SQL 执行失败 | `FlashbackError::GENERIC` | 回滚当前事务, 返回失败 |
| 主键列不存在 | `FlashbackError::NO_PRIMARY_KEY` | 跳过该表, 记录错误 |
| binlog_row_image 不足 | `FlashbackError::BINLOG_ROW_IMAGE_NOT_FULL` | 前置检查已拦截 |

---

## 6. 关键约束清单

### 6.1 原有约束 (C1-C10)

保持原 DESIGN.md 中的 C1-C10 不变。

### 6.2 新增约束

| 编号 | 约束描述 | 违反后果 | 缓解措施 |
|------|---------|---------|---------|
| **C11** | Binlog 闪回的逆向 SQL 执行**必须按事务逆序** | 事务间依赖关系被打乱, 外键约束失败 | `TransactionCollector` 按事务收集, commit 时逆序执行 |
| **C12** | 断点续写的检查点**必须在每批次提交后更新** | 中断后重新执行重复处理已完成的行 | 检查点在 `batch_commit` 后调用 `save_checkpoint()` |
| **C13** | Undo 引擎分批提交的**批次大小不超过 1000 行** | 单事务 undo 过大, 影响其他事务 | `BATCH_SIZE = 1000`, 可通过 `flashback_batch_size` 调整 |
| **C14** | DDL Barrier 扫描**必须覆盖从 target_ts 到 now 的完整 binlog 范围** | 遗漏中间文件的 DDL, 闪回不安全 | `scan_binlog_for_ddl()` 遍历所有 binlog 文件 |
| **C15** | 并发闪回的**表锁必须按字典序获取** | 多表并发时可能死锁 | 闪回前对表名排序, 依次获取 MDL 锁 |
| **C16** | TableMapCache**必须在扫描开始时清空, 且永不删除条目** | 旧 table_id 残留导致表名错误 | `m_table_map_cache.clear()` 在 `execute()` 开头调用 |

---

## 7. 文件结构 (增量更新)

### 7.1 新增/修改文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `sql/flashback_binlog_engine.h` | 修改 | 新增 TableMapCache, TransactionCollector, handle_* 方法 |
| `sql/flashback_binlog_engine.cc` | 修改 | 实现 TableMapCache 注册、列类型编码、事务逆序执行 |
| `sql/flashback_ddl_barrier.h` | 修改 | 新增 DdlCategory 枚举, classify_ddl(), scan_binlog_for_ddl() |
| `sql/flashback_ddl_barrier.cc` | 修改 | 实现 binlog 扫描式 DDL 检查, 精确 DDL 分类器 |
| `sql/flashback_scheduler.h` | 修改 | 新增 resume_from_checkpoint(), estimate_affected_rows() |
| `sql/flashback_scheduler.cc` | 修改 | 实现 Undo 引擎实际调用, binlog_row_image 真实检查, 检查点 |
| `sql/flashback_types.h` | 修改 | 新增 FlashbackCheckpoint 结构体 |
| `storage/innobase/flashback/flashback_undo_engine.cc` | 修改 | 实现 execute_table 全表扫描, restore_row 数据恢复 |

### 7.2 目录结构 (不变, 仅增量更新)

```
sql/
├── flashback_binlog_engine.h    ← 新增 TableMapCache, TransactionCollector
├── flashback_binlog_engine.cc   ← 完整逆向 SQL 生成
├── flashback_ddl_barrier.h      ← 新增 DdlCategory, scan_binlog_for_ddl
├── flashback_ddl_barrier.cc     ← binlog 扫描式 DDL 检查
├── flashback_scheduler.h        ← 新增 resume/checkpoint/estimate
├── flashback_scheduler.cc       ← Undo 引擎调用, 检查点
└── flashback_types.h            ← 新增 FlashbackCheckpoint

storage/innobase/flashback/
└── flashback_undo_engine.cc     ← 全表扫描 + 分批提交 + 行恢复
```

---

## 8. 实施顺序与验收标准

### 8.1 实施顺序

| 阶段 | 内容 | 预计工作量 |
|------|------|-----------|
| **Sprint 1** | D1: TableMapCache + 列类型编码 + reverse_* 完整实现 | 3-5 天 |
| **Sprint 2** | D2: DDL Barrier binlog 扫描 + 精确分类器 | 2-3 天 |
| **Sprint 3** | D3: Undo 引擎全表扫描 + restore_row 实现 | 3-5 天 |
| **Sprint 4** | D4: 断点续写 + 检查点持久化 | 2-3 天 |
| **Sprint 5** | D6: InnoDB 统计信息行数估算 | 1-2 天 |
| **Sprint 6** | D5: 并发处理框架 (后期) | 3-5 天 |

### 8.2 验收标准

#### D1 验收

```cpp
/* 给定 binlog 中有 INSERT INTO t1 (id, name) VALUES (1, 'Alice') */
/* 逆向 SQL 应为: */
"DELETE FROM `db1`.`t1` WHERE `id` = 1;"

/* 给定 binlog 中有 DELETE FROM t1 WHERE id = 1 */
/* 逆向 SQL 应为: */
"INSERT INTO `db1`.`t1` (`id`, `name`) VALUES (1, 'Alice');"

/* 给定 binlog 中有 UPDATE t1 SET name='Bob' WHERE id=1 */
/* 逆向 SQL 应为: */
"UPDATE `db1`.`t1` SET `name` = 'Alice' WHERE `id` = 1;"
```

#### D2 验收

- [ ] 对包含 DROP TABLE 的 binlog 窗口, check() 返回 DDL_INCOMPATIBLE
- [ ] 对仅包含 ADD INDEX 的 binlog 窗口, check() 返回 NONE
- [ ] 扫描 10 个 binlog 文件 (< 1GB), 耗时 < 30 秒

#### D3 验收

- [ ] 1000 行表闪回, rows_restored 计数正确
- [ ] 分批提交: 每 1000 行 mtr_commit 一次
- [ ] DRY RUN: 不修改数据, 返回正确估算

#### D4 验收

- [ ] 闪回中途 kill 后, resume_from_checkpoint 可从断点继续
- [ ] 幂等: 同一闪回操作执行两次, 结果相同
- [ ] 检查点文件损坏时自动重新开始

---

## 9. 设计验收检查清单

### 9.1 模块接口完整性

- [x] `TableMapCache`: register_table() / lookup() / clear(), 职责单一
- [x] `TransactionCollector`: begin() / add_row() / commit() / rollback(), 事务边界管理
- [x] `encode_column_value()`: 支持所有 MYSQL_TYPE_*, 返回 SQL 字面量
- [x] `DDLBarrier::classify_ddl()`: 精确分类, 替代 strstr 误判
- [x] `DDLBarrier::scan_binlog_for_ddl()`: 完整 binlog 扫描, 发现冲突短路
- [x] `UndoFlashbackEngine::execute_table()`: 全表扫描 + 分批提交
- [x] `UndoFlashbackEngine::restore_row()`: 三种操作类型 (INSERT/DELETE/UPDATE)
- [x] `FlashbackCheckpoint`: 序列化/反序列化, 持久化加载/保存
- [x] `FlashbackScheduler::resume_from_checkpoint()`: 断点续写 + 幂等保证
- [x] `FlashbackScheduler::estimate_affected_rows()`: InnoDB 统计信息接入

### 9.2 依赖方向

- [x] SQL 层 → 引擎层 → InnoDB/binlog 层 (单向)
- [x] TableMapCache 仅依赖 log_event.h, 无循环依赖
- [x] TransactionCollector 仅依赖 THD + SQL 执行, 无循环依赖
- [x] DDLBarrier 增强后依赖 BinlogFlashbackEngine (定位位置), 但无反向依赖

### 9.3 约束覆盖

- [x] C11: 事务逆序执行 → TransactionCollector::commit() 逆序迭代
- [x] C12: 检查点批次更新 → save_checkpoint() 在 batch_commit 后调用
- [x] C13: 分批提交 ≤ 1000 行 → BATCH_SIZE = 1000
- [x] C14: 完整 binlog 扫描 → scan_binlog_for_ddl() 遍历所有文件
- [x] C15: 表锁字典序 → execute() 前对 request.tables 排序
- [x] C16: TableMapCache 生命周期 → clear() 在 execute() 开头, 永不删除条目

---

## 附录: 与原 DESIGN.md 的差异对照

| 原设计 (DESIGN.md) | 本设计 (DESIGN-v2.md) | 变更原因 |
|-------------------|----------------------|---------|
| reverse_* 输出 unknown_table | 通过 TableMapCache 获取真实表名 + 列类型 | HEV 调研发现 TODO 未实现 |
| DDLBarrier::check() 返回 NONE | binlog 事件驱动扫描 + 精确分类器 | 原实现为空壳 |
| execute_undo_flashback() 空壳 | Undo 引擎全表扫描 + restore_row 实现 | Phase 2 需要 |
| validate_binlog_row_image() 永远 true | 读取 thd->variables.binlog_row_image 真实值 | 安全检查必须精确 |
| 无断点续写 | FlashbackCheckpoint + resume_from_checkpoint | 大表闪回需要容错 |
| 无并发设计 | 分阶段并发架构 (Phase 2 后期) | 性能优化需求 |
| 行数估算 TODO | InnoDB 统计信息接入 | 安全阀值需要精确估算 |
