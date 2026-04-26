# MySQL 内置回收站(Recycle Bin)能力实现方案

> 基于 Percona Server 8.4.7 LTS 源码分析，融合 Oracle/PostgreSQL/TiDB 等业界最佳实践
> 文档版本: v1.0  
> 日期: 2025-01-01

---

## 目录

1. [执行摘要 (Executive Summary)](#1-执行摘要)
2. [技术背景与源码分析](#2-技术背景与源码分析)
3. [业界方案对比分析](#3-业界方案对比分析)
4. [总体架构设计](#4-总体架构设计)
5. [详细实现方案](#5-详细实现方案)
6. [市场分析](#6-市场分析)
7. [风险评估](#7-风险评估)
8. [优先级建议](#8-优先级建议)
9. [附录：关键源码索引](#9-附录关键源码索引)

---

## 1. 执行摘要

- **MySQL 8.0/8.4 原生不支持 DROP TABLE 回收站功能**，误删表后仅能通过备份恢复或 binlog 回滚，恢复成本高、窗口长。
- **Oracle Recycle Bin（Flashback Drop）是最成熟的参考方案**：通过"逻辑删除 + 重命名"而非物理删除表数据，配合独立命名空间和自动清理策略，实现了秒级恢复。
- **MySQL 8.0 的原子 DDL 和数据字典(DD)体系为回收站提供了良好的基础设施**：`dd::Table` 对象、`dd::drop_table()` 接口、InnoDB 表空间管理(`fil_delete_tablespace`) 均可复用。
- **推荐采用三层拦截方案**：在 `sql_parse.cc` 中拦截 `SQLCOM_DROP_TABLE` → 将 DROP 转换为"逻辑删除"(标记 + 重命名 + 迁移到回收站 schema) → 保留 InnoDB 表空间文件不删除。
- **关键风险在于**：Foreign Key 依赖、binlog 复制、复制拓扑兼容性、InnoDB 外键缓存失效。需要逐一处理。

---

## 2. 技术背景与源码分析

### 2.1 Percona Server 8.4.7 LTS 版本基础

本方案基于 `/home/victor/base/git/others/percona-server` 源码，版本为 MySQL 8.4.7 LTS，包含：
- 完整的数据字典(Data Dictionary, DD)体系
- 原子 DDL(Atomic DDL) 支持
- InnoDB 表空间隔离管理

### 2.2 DROP TABLE 完整执行链路

通过源码分析，`DROP TABLE t` 的完整执行路径如下：

```
sql_parse.cc:3873          mysql_rm_table()              ← SQL 层入口
  └─ sql_table.cc:1622     mysql_rm_table()              ← 锁表名、触发器
     └─ sql_table.cc:1734  mysql_rm_table_no_locks()     ← 核心删除逻辑
        ├─ rm_table_sort_into_groups()                   ← 按原子/非原子引擎分组
        ├─ rm_table_eval_gtid_and_table_groups_state()   ← GTID 处理
        ├─ rm_table_check_fks()                          ← FK 依赖检查
        ├─ drop_base_table() [非原子引擎]                ← 逐个删除
        └─ drop_base_table() [原子引擎]                  ← 批量删除
           ├─ dd::drop_table()                           ← 从 DD 删除元数据
           └─ ha_innobase::delete_table()                ← InnoDB 物理删除
              └─ row_drop_table_for_mysql()              ← 删除数据字典缓存
              └─ fil_delete_tablespace()                 ← 删除 .ibd 文件
```

#### 关键函数定位

| 函数 | 文件位置 | 作用 |
|------|---------|------|
| `mysql_rm_table()` | `sql/sql_table.cc:1622` | DROP TABLE 主入口，负责锁表名和触发器 |
| `mysql_rm_table_no_locks()` | `sql/sql_table.cc:3212` | 核心删除逻辑，按引擎类型分组处理 |
| `drop_base_table()` | `sql/sql_table.cc:2879` | 删除单个表，调用 DD 和引擎层 |
| `dd::drop_table()` | `sql/dd/dd_table.cc:2480` | 从数据字典删除表定义 |
| `ha_innobase::delete_table()` | `storage/innobase/handler/ha_innodb.cc:16215` | InnoDB 存储引擎删除 |
| `innobase_basic_ddl::delete_impl()` | `storage/innobase/handler/ha_innodb.cc:15107` | InnoDB DDL 删除实现 |
| `row_drop_table_for_mysql()` | `storage/innobase/row/row0mysql.cc:4246` | InnoDB 行级删除表 |
| `fil_delete_tablespace()` | `storage/innobase/fil/fil0fil.cc:4668` | 删除 InnoDB 表空间文件 |
| `Sql_cmd_drop_table` | `sql/sql_cmd_ddl_table.h:106` | SQL 命令类，execute() 为内联返回 false |

### 2.3 数据字典结构

MySQL 8.0 使用 `dd::Table` 对象表示用户表：

```
dd::Table extends dd::Abstract_table
  ├── columns()        → dd::Column 集合
  ├── indexes()        → dd::Index 集合
  ├── foreign_keys()   → dd::Foreign_key 集合
  ├── partitions()     → dd::Partition 集合(分区表)
  ├── triggers()       → dd::Trigger 集合
  ├── se_private_id()  → InnoDB table_id
  ├── engine()         → 存储引擎名("InnoDB")
  ├── schema_id()      → 所属 schema 的 Object ID
  └── hidden()         → 可见性标记(HT_VISIBLE/HT_HIDDEN_DD/HT_HIDDEN_SE)
```

### 2.4 关键数据结构

**Drop_tables_ctx** (sql_table.cc:1806):
```cpp
class Drop_tables_ctx {
  std::vector<Table_ref *> base_atomic_tables;    // 原子引擎表
  std::vector<Table_ref *> base_non_atomic_tables; // 非原子引擎表
  std::vector<Table_ref *> tmp_trans_tables;       // 事务临时表
  // ...
};
```

---

## 3. 业界方案对比分析

### 3.1 Oracle Flashback Drop (Recycle Bin)

**核心机制**：
- `DROP TABLE` 不物理删除，而是将表重命名为 `BIN$<unique_id>$<original_name>`
- 表和所有依赖对象(索引、约束、触发器)一起移入回收站
- 存储在原始表空间，配额仍然占用
- 通过 `FLASHBACK TABLE ... TO BEFORE DROP` 恢复
- `PURGE RECYCLEBIN` 清空回收站
- 自动清理策略：当表空间不足时，按 FIFO 清除最早的回收站对象

**优点**：
- 零数据丢失(表结构+数据完整保留)
- 秒级恢复(`FLASHBACK TABLE`)
- 自动清理策略避免无限膨胀

**缺点**：
- 占用原始表空间配额
- 不适用于 `DROP TABLE ... PURGE` 和系统表

### 3.2 PostgreSQL 扩展方案 (pg_recyclebin / pg_safelimit)

**pg_recyclebin** (社区扩展):
- 通过 event trigger 拦截 `sql_drop` 事件
- 将表重命名到回收站 schema (`_recyclebin_<oid>`)
- 保留原始表数据文件不变
- 通过 `pg_recyclebin_restore()` 函数恢复

**pg_safelimit** (阿里云/腾讯云方案):
- 不拦截 DROP，而是通过 `safe_guard` 表 + trigger 实现软删除
- 应用层改为 UPDATE 标记而非物理删除

**优点**：
- 无需修改内核源码（event trigger 方式）
- 灵活可控

**缺点**：
- Event Trigger 有性能开销
- 无法保证 DDL 原子性(拦截点在执行后)
- 对 `DROP TABLE` 只能事后记录，无法在删除前拦截

### 3.3 TiDB GC 机制

**核心机制**：
- 基于 PD 的全局时间戳服务(Oracle)
- `DROP TABLE` 后表进入 GC 等待期
- 数据保留到 `tikv_gc_safe_point` 之后才真正清理
- `FLASHBACK TABLE` 利用 MVCC 快照恢复

**优点**：
- 天然支持时间点恢复(基于 MVCC)
- 分布式一致性

**缺点**：
- 仅适用于分布式架构
- 需要 PD 全局时间戳服务

### 3.4 对比总结

| 特性 | Oracle Recycle Bin | pg_recyclebin | TiDB Flashback | 本方案 |
|------|-------------------|---------------|----------------|--------|
| 拦截时机 | DROP 前拦截 | sql_drop 事件后 | DROP 后延迟GC | **DROP 前拦截** |
| 数据存储位置 | 原表空间 | 回收站 schema | 原 Region | **独立回收站 schema** |
| 恢复方式 | FLASHBACK TABLE | 函数调用 | FLASHBACK TABLE | **FLASHBACK TABLE 扩展** |
| 自动清理 | 表空间满时清理 | 手动/TTL | GC safe point | **TTL + 容量策略** |
| 复制兼容 | 支持 | 有限 | 原生支持 | **需要适配** |
| 实现复杂度 | 中 | 低 | 高 | **中** |

---

## 4. 总体架构设计

### 4.1 设计原则

1. **最小侵入**：尽量不修改 InnoDB 存储引擎代码，在 SQL 层完成拦截
2. **元数据优先**：回收站使用独立的 `mysql_recyclebin` schema，与原 schema 隔离
3. **数据保留**：保留原始 InnoDB 表空间文件(.ibd)，不执行 `fil_delete_tablespace`
4. **可恢复性**：支持 `FLASHBACK TABLE` 语法（与 Oracle 兼容）
5. **可清理性**：支持自动 TTL 清理和手动 `PURGE` 操作
6. **复制安全**：在复制拓扑中正确处理 binlog 事件

### 4.2 系统架构

```
┌──────────────────────────────────────────────────────────────┐
│                       SQL Parser                              │
│  DROP TABLE t1  ──▶ 识别 recycle_bin_enabled=ON  ──▶ 拦截     │
└────────────────────────────────────┬─────────────────────────┘
                                     │
                    ┌────────────────┼────────────────┐
                    ▼                ▼                ▼
          ┌─────────────────┐ ┌──────────────┐ ┌──────────────┐
          │ RecycleBin::    │ │   直接删除   │ │ DROP TABLE   │
          │ moveToRecycle() │ │  (原行为)    │ │ IF EXISTS    │
          │                 │ │              │ │ PURGE        │
          │ 1. 重命名表     │ └──────────────┘ └──────────────┘
          │ 2. 迁移 schema  │
          │ 3. 记录元数据   │
          │ 4. 保留 .ibd    │
          │ 5. 写入 binlog  │
          └────────┬────────┘
                   │
         ┌─────────▼─────────┐
         │ mysql.recycle_bin │  ← 回收站系统表
         │  ├── recycle_bin  │     记录回收条目
         │  └── ...          │
         └───────────────────┘
                   │
         ┌─────────▼─────────┐
         │ 回收站 schema     │
         │  __recycle_12345  │  ← 被回收的表
         │  __recycle_12346  │     原始 .ibd 文件保留
         │  ...              │
         └───────────────────┘
```

### 4.3 核心组件

#### 4.3.1 回收站系统表

在 `mysql` schema 中创建系统表 `mysql.recycle_bin`：

```sql
CREATE TABLE mysql.recycle_bin (
    id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    original_schema VARCHAR(64) NOT NULL,
    original_name   VARCHAR(64) NOT NULL,
    recycle_name    VARCHAR(256) NOT NULL,    -- 如 __recycle_1736000000_employees
    recycle_schema  VARCHAR(64) NOT NULL DEFAULT 'mysql',
    dropped_by      VARCHAR(128),             -- DROP 执行用户
    dropped_at      TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    expires_at      TIMESTAMP NULL,           -- 过期时间(TTL)
    table_def_json  JSON,                     -- 序列化的 dd::Table 定义
    tablespace_info JSON,                     -- 表空间信息
    table_rows      BIGINT UNSIGNED DEFAULT 0,
    data_length     BIGINT UNSIGNED DEFAULT 0,
    index_length    BIGINT UNSIGNED DEFAULT 0,
    engine          VARCHAR(64),
    partitioned     BOOLEAN DEFAULT FALSE,
    restore_status  ENUM('active','restored','purged') DEFAULT 'active',
    restored_at     TIMESTAMP NULL,
    restored_by     VARCHAR(128),
    
    INDEX idx_schema_name (original_schema, original_name),
    INDEX idx_expires_at (expires_at),
    INDEX idx_restore_status (restore_status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

#### 4.3.2 回收站命名规则

```
回收站表名: __recycle_{drop_timestamp_unix}_{original_name}
示例:     __recycle_1736000000_employees

命名约束:
- 使用下划线前缀避免用户冲突
- 时间戳确保唯一性
- 总长度不超过 64 字符(表名限制)
```

### 4.4 系统变量

| 变量名 | 类型 | 默认值 | 说明 |
|--------|------|--------|------|
| `recycle_bin` | BOOL | OFF | 全局/会话开关 |
| `recycle_bin_retention` | INT | 604800 | 默认保留秒数(7天) |
| `recycle_bin_max_size` | BIGINT | 0(无限制) | 回收站最大空间(bytes)，0=不限制 |
| `recycle_bin_max_entries` | INT | 10000 | 回收站最大条目数 |
| `recycle_bin_purge_interval` | INT | 3600 | 自动清理间隔(秒) |

---

## 5. 详细实现方案

### 5.1 Phase 1: DROP TABLE 拦截

#### 5.1.1 拦截点选择

**推荐拦截点：`sql_parse.cc` 中的 `SQLCOM_DROP_TABLE` case**

```cpp
// sql/sql_parse.cc (修改)
case SQLCOM_DROP_TABLE: {
  if (lex->drop_temporary) {
    // 临时表不进入回收站
    res = mysql_rm_table(thd, first_table, lex->drop_if_exists, true);
    break;
  }
  
  // 检查是否包含 PURGE 关键字
  if (lex->drop_purge) {
    // PURGE 模式：直接物理删除
    res = mysql_rm_table(thd, first_table, lex->drop_if_exists, false);
    break;
  }
  
  // 检查回收站开关
  if (opt_recycle_bin) {
    res = mysql_recycle_table(thd, first_table, lex->drop_if_exists);
  } else {
    res = mysql_rm_table(thd, first_table, lex->drop_if_exists, false);
  }
  break;
}
```

#### 5.1.2 新增语法支持

在 `sql/sql_yacc.yy` 中扩展 DROP TABLE 语法：

```yacc
drop_table:
    DROP opt_temporary TABLE opt_if_exists table_name_list
    opt_recycle_option

opt_recycle_option:
    /* empty */       { $$ = RECYCLE_DEFAULT; }
  | PURGE             { $$ = RECYCLE_PURGE; }
  | NORECYCLE         { $$ = RECYCLE_NORECYCLE; }
;
```

新增 `FLASHBACK TABLE` 语法：

```yacc
flashback_table:
    FLASHBACK TABLE table_name_list TO BEFORE DROP
  | FLASHBACK TABLE table_name_list TO table_name
;
```

### 5.2 Phase 2: moveToRecycle 核心逻辑

```cpp
// sql/sql_recyclebin.cc (新增文件)

bool mysql_recycle_table(THD *thd, Table_ref *tables, bool if_exists) {
  RecycleBinGuard guard(thd);
  
  for (Table_ref *table = tables; table; table = table->next_local) {
    // 1. 检查表是否存在（类似 DROP IF EXISTS 行为）
    const dd::Table *table_def = nullptr;
    bool exists = false;
    if (dd::table_exists(thd->dd_client(), table->db, table->table_name, &exists))
      return true;
    
    if (!exists) {
      if (if_exists) {
        push_warning(thd, Sql_condition::SL_NOTE, ER_BAD_TABLE_ERROR, ...);
        continue;
      }
      my_error(ER_BAD_TABLE_ERROR, MYF(0), table->db, table->table_name);
      return true;
    }
    
    // 2. 获取 MDL 排他锁（与原 DROP 相同）
    if (lock_table_names(thd, table, nullptr, thd->variables.lock_wait_timeout, 0))
      return true;
    
    // 3. 检查 FK 依赖（复用 rm_table_check_fks 逻辑）
    if (check_recycle_fk_deps(thd, table)) return true;
    
    // 4. 执行回收操作
    if (recycle_single_table(thd, table, table_def)) return true;
  }
  
  return false;
}

bool recycle_single_table(THD *thd, Table_ref *table, const dd::Table *table_def) {
  // Step 1: 生成回收站名称
  dd::String_type recycle_name = generate_recycle_name(table->table_name);
  
  // Step 2: 创建回收条目记录
  RecycleEntry entry;
  entry.original_schema = table->db;
  entry.original_name = table->table_name;
  entry.recycle_name = recycle_name;
  entry.table_def_json = serialize_table_def(table_def);
  entry.dropped_by = thd->security_context()->priv_user().str;
  entry.expires_at = compute_expiry();
  
  // Step 3: 在 DD 层重命名表（移动到回收站 schema）
  // 关键：不删除 DD 条目，而是更新 schema 和名称
  if (rename_table_to_recyclebin(thd, table, recycle_name)) return true;
  
  // Step 4: 记录到 mysql.recycle_bin 系统表
  if (insert_recycle_entry(thd, entry)) {
    // 回滚：撤销重命名
    rollback_rename(thd, table, recycle_name);
    return true;
  }
  
  // Step 5: 写入 binlog（RENAME TABLE 事件）
  write_recycle_binlog(thd, table->db, table->table_name, recycle_name);
  
  return false;
}
```

### 5.3 Phase 3: InnoDB 层保护

**关键设计决策：在回收站模式下，不调用 `ha_innobase::delete_table()`**

当前 DROP TABLE 流程中，`ha_innobase::delete_table()` 会被调用并执行：
```
ha_innobase::delete_table()
  → innobase_basic_ddl::delete_impl()
    → row_drop_table_for_mysql()       ← 从 InnoDB dict cache 中删除
    → fil_delete_tablespace()          ← 删除 .ibd 文件
```

**回收方案**：在 `mysql_rm_table_no_locks()` 中，增加 `is_recycle_mode` 标志：

```cpp
// sql/sql_table.cc 修改
bool mysql_rm_table_no_locks(THD *thd, Table_ref *tables, bool if_exists,
                             bool drop_temporary, bool drop_database, ...) {
  
  // 新增：回收模式跳过物理删除
  if (thd->variables.recycle_bin && !thd->lex->drop_purge) {
    return mysql_recycle_table_no_locks(thd, tables, if_exists, ...);
  }
  
  // 原有逻辑不变...
}

// 在 drop_base_table() 中：
static bool drop_base_table(THD *thd, ...) {
  // ...
  
  // 回收模式：不删除 DD 条目
  if (thd->lex->is_recycle_mode) {
    return false; // 直接返回成功，不执行任何删除
  }
  
  // 正常模式：删除 DD 条目
  bool result = dd::drop_table(thd, table->db, table->table_name, *table_def);
  
  // ...
}
```

### 5.4 Phase 4: FLASHBACK TABLE 恢复

```cpp
// sql/sql_recyclebin.cc

bool mysql_flashback_table(THD *thd, Table_ref *tables) {
  for (Table_ref *table = tables; table; table = table->next_local) {
    // 1. 从 mysql.recycle_bin 查找条目
    RecycleEntry entry;
    if (!find_recycle_entry(thd, table->db, table->table_name, &entry)) {
      my_error(ER_TABLE_NOT_IN_RECYCLEBIN, MYF(0), table->db, table->table_name);
      return true;
    }
    
    // 2. 检查目标名称是否已被占用
    bool exists = false;
    if (dd::table_exists(thd->dd_client(), entry.original_schema,
                         entry.original_name, &exists) || exists) {
      my_error(ER_TABLE_EXISTS, MYF(0), entry.original_name);
      return true;
    }
    
    // 3. 将表从回收站 schema 重命名回原 schema
    if (rename_table_from_recyclebin(thd, &entry)) return true;
    
    // 4. 从 dd::Table JSON 恢复表定义（如果需要）
    // 由于 DD 条目仍然存在，不需要重建
    
    // 5. 更新 mysql.recycle_bin 状态
    update_recycle_entry_status(thd, entry.id, RESTORED);
    
    // 6. 写入 binlog
    write_flashback_binlog(thd, entry);
  }
  
  return false;
}
```

### 5.5 Phase 5: PURGE 清理机制

```cpp
// sql/sql_recyclebin.cc

// 手动清理单个回收条目
bool mysql_purge_recycle_entry(THD *thd, const RecycleEntry &entry) {
  // 1. 获取表的 dd::Table 对象
  const dd::Table *table_def = acquire_recycle_table(thd, entry.recycle_name);
  if (!table_def) return true;
  
  // 2. 执行正常的物理删除流程
  bool result = execute_physical_drop(thd, entry.recycle_schema,
                                      entry.recycle_name, table_def);
  
  // 3. 清理 mysql.recycle_bin 记录
  delete_recycle_entry(thd, entry.id);
  
  return result;
}

// 自动清理（后台线程）
void recyclebin_purge_thread(void *arg) {
  while (!shutdown_requested) {
    // 查找过期条目
    std::vector<RecycleEntry> expired_entries = 
        find_expired_entries(current_time);
    
    for (const auto &entry : expired_entries) {
      // 检查容量约束
      if (should_purge_for_space(entry)) {
        mysql_purge_recycle_entry(thd, entry);
      }
    }
    
    // 检查总大小限制
    enforce_size_limit(opt_recycle_bin_max_size);
    
    // 检查条目数限制
    enforce_entry_limit(opt_recycle_bin_max_entries);
    
    std::this_thread::sleep_for(
        std::chrono::seconds(opt_recycle_bin_purge_interval));
  }
}
```

### 5.6 Phase 6: Binlog 与复制处理

**复制兼容性方案**：

```
主库 (Master):
  DROP TABLE t1  
    → 拦截 → 回收站  
    → 写入 binlog: "RENAME TABLE db.t1 TO mysql.__recycle_123_t1"

从库 (Slave):
  执行 binlog: RENAME TABLE
    → 如果从库也开启回收站: 正常重命名
    → 如果从库关闭回收站: 仍执行重命名（兼容）

恢复时:
  主库: FLASHBACK TABLE db.t1  
    → 写入 binlog: "RENAME TABLE mysql.__recycle_123_t1 TO db.t1"
  从库: 执行相同的 RENAME
```

**binlog 事件类型选择**：
- 使用 `Query_log_event` 记录 RENAME TABLE 语句（与现有 DDL 机制一致）
- GTID 模式下，回收操作和恢复操作共享相同的 GTID（作为同一事务）

### 5.7 Phase 7: 系统视图

```sql
-- 回收站查询视图
CREATE OR REPLACE VIEW mysql.recycle_bin_view AS
SELECT 
    id,
    original_schema AS `Schema`,
    original_name AS `Name`,
    recycle_name AS `Recycle Name`,
    dropped_by AS `Dropped By`,
    dropped_at AS `Dropped At`,
    expires_at AS `Expires At`,
    table_rows AS `Rows`,
    FORMAT_BYTES(data_length) AS `Data Size`,
    FORMAT_BYTES(index_length) AS `Index Size`,
    engine AS `Engine`,
    TIMESTAMPDIFF(HOUR, NOW(), expires_at) AS `Hours Until Purge`
FROM mysql.recycle_bin
WHERE restore_status = 'active';
```

---

## 6. 市场分析

### 6.1 需求背景

- **误删表是数据库运维中最常见的事故之一**。据行业统计，约 30-40% 的数据库紧急恢复事件由误删表触发。
- 当前 MySQL 的恢复手段：
  - 从备份恢复：耗时数小时，数据丢失窗口 = 备份间隔
  - 从 binlog 回滚：技术门槛高，依赖 binlog 完整性
  - 商业工具（如 Delphix、Actifio）：成本高，部署复杂

### 6.2 竞争格局

| 方案 | 开源/商业 | MySQL 兼容 | 成熟度 |
|------|----------|-----------|--------|
| Oracle Recycle Bin | 商业(Oracle) | 否 | ★★★★★ |
| Percona Server | 开源(AGPL) | 是(本方案) | ★★★☆☆(待实现) |
| Aliyun RDS 回收站 | 商业 | 是(云端定制) | ★★★★☆ |
| TiDB Flashback | 开源(Apache 2.0) | 否(仅 TiDB) | ★★★★☆ |
| 自建脚本方案 | 开源 | 是 | ★★☆☆☆ |

### 6.3 商业价值

- **对于 Percona Server**：作为差异化特性，提升与商业版本的竞争力
- **对于云数据库厂商**：可作为 PaaS 层增值功能
- **对于企业用户**：降低误操作风险，减少恢复成本，提升 SLA

---

## 7. 风险评估

### 7.1 技术风险

| 风险 | 严重度 | 缓解措施 |
|------|--------|----------|
| FK 依赖表无法正确处理 | 高 | 参考 `rm_table_check_fks`，实现级联回收 |
| InnoDB dict cache 未更新导致脏读 | 高 | 重命名后刷新 dict cache |
| Binlog 复制拓扑不兼容 | 中 | 使用 RENAME TABLE 事件，保持双向兼容 |
| 回收站空间无限增长 | 中 | TTL + 容量限制 + 后台清理 |
| 分区表/外键表恢复失败 | 中 | 限制初版仅支持简单 InnoDB 表 |
| 并发 DROP 产生命名冲突 | 低 | 使用时间戳 + UUID 后缀确保唯一 |
| 大表回收导致锁竞争 | 低 | 回收操作只修改 DD 元数据，不移动数据 |

### 7.2 兼容性风险

- **向下兼容**：回收站功能默认关闭，不影响现有行为
- **备份工具**：mysqldump/percona-xtrabackup 需要识别 `__recycle_*` 表并处理
- **监控工具**：回收站表不应纳入常规监控统计（通过 `hidden` 标记排除）

### 7.3 运维风险

- DBA 需要理解回收站的存储影响
- 需要明确的文档说明回收站的行为边界
- 需要提供 `SHOW RECYCLEBIN` 等诊断命令

---

## 8. 优先级建议

### Phase 1 (MVP - 最小可行产品)
- [ ] 实现 `recycle_bin` 系统变量开关
- [ ] 拦截 `DROP TABLE`，重命名到 `mysql.__recycle_*`
- [ ] 创建 `mysql.recycle_bin` 系统表
- [ ] 实现基础 `FLASHBACK TABLE ... TO BEFORE DROP`
- [ ] 支持手动 `PURGE TABLE __recycle_*`

### Phase 2 (增强)
- [ ] 实现自动 TTL 清理后台线程
- [ ] 支持 `recycle_bin_max_size` 容量限制
- [ ] 支持 `PURGE RECYCLEBIN` 批量清理
- [ ] 创建 `mysql.recycle_bin_view` 视图
- [ ] binlog 复制完整兼容

### Phase 3 (完善)
- [ ] 支持分区表回收/恢复
- [ ] 支持外键表的级联回收
- [ ] 支持 `FLASHBACK TABLE ... TO <original_name>` 重命名恢复
- [ ] 支持 `SHOW RECYCLEBIN` 命令
- [ ] Xtrabackup 兼容

### Phase 4 (高级)
- [ ] 支持 `FLASHBACK TABLE ... TO TIMESTAMP` (基于 binlog)
- [ ] 支持回收站配额管理和告警
- [ ] 支持基于角色的回收站权限控制
- [ ] 支持回收站压缩(对长时间不恢复的表)

---

## 9. 附录：关键源码索引

### SQL 层
```
sql/sql_table.cc          → mysql_rm_table, mysql_rm_table_no_locks, drop_base_table
sql/sql_parse.cc          → SQLCOM_DROP_TABLE case (line ~3873)
sql/sql_cmd_ddl_table.h   → Sql_cmd_drop_table 类
sql/dd/dd_table.cc        → dd::drop_table (line 2480)
sql/dd/dd_table.h         → DD 表操作接口
sql/dd/types/table.h      → dd::Table 定义
sql/sql_yacc.yy           → DROP TABLE 语法定义
```

### InnoDB 存储引擎
```
storage/innobase/handler/ha_innodb.cc
    → ha_innobase::delete_table (line 16215)
    → innobase_basic_ddl::delete_impl (line 15107)
storage/innobase/row/row0mysql.cc
    → row_drop_table_for_mysql (line 4246)
storage/innobase/fil/fil0fil.cc
    → fil_delete_tablespace (line 4668)
storage/innobase/dict/dict0dict.cc
    → dict 缓存管理
```

### Binlog
```
sql/binlog.cc             → DDL 事件写入
sql/log_event.cc          → Query_log_event, Gtid_log_event
```

---

## 10. 结论

基于 Percona Server 8.4.7 LTS 的现有架构，实现 MySQL 内置回收站功能是**技术可行且风险可控**的。核心方案是：

1. **在 SQL 层拦截 `DROP TABLE`**，将其转换为 RENAME + 元数据记录操作
2. **保留 InnoDB 表空间文件不删除**，确保数据完整性
3. **在 `mysql` schema 中管理回收站系统表和回收条目**
4. **通过 `FLASHBACK TABLE` 实现秒级恢复**
5. **通过 TTL + 容量策略实现自动清理**

该方案借鉴了 Oracle Flashback Drop 的核心理念，同时充分利用了 MySQL 8.0 的数据字典和原子 DDL 基础设施，是一个工程上务实的设计。

---

*文档结束*
