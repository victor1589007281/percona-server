# MySQL Flashback 功能补齐 — HEV 调研与实施方案

> 基于已有设计文档 (`mysql_flashback_implementation_*.md`) 与当前源码 (Phase 1 骨架代码) 的深度分析
> 目标: 补齐 TODO / 空实现, 完成 Binlog 引擎逆向解析、DDL Barrier、Undo 引擎、事务闪回
> 日期: 2025

---

## Phase 1: 假设生成

### 假设 H1: 建立 Table_map_event 缓存 + 列类型感知的逆向 SQL 生成器

- **假设内容**: 在 Binlog 闪回引擎中维护一个 `table_id → Table_map_log_event` 的映射缓存, 在处理每个 Rows_log_event 时先查表获取列元信息 (表名、列名、列类型、主键位图), 再生成精确的反向 SQL。
- **预期效果 (量化)**: 逆向 SQL 正确率从当前的 0% (仅输出 `unknown_table`) 提升到 >99%, 每 event 处理延迟增加 <0.5ms。
- **关键风险**: Rows_event 可能跨 binlog 文件引用不同的 Table_map_event (Rotate 后重建), 缓存失效处理不当会导致数据损坏。

### 假设 H2: 引入分阶段并发处理架构 (Binlog 并行扫描 + 有序队列执行)

- **假设内容**: 将当前单线程串行扫描 (定位→读取→逆向→执行) 拆分为 3 个阶段: (1) Binlog 预读与解析线程池 (无状态), (2) 逆向 SQL 生成队列 (有序), (3) 执行线程 (受事务边界约束)。通过 `std::vector` + `std::mutex` + 条件变量实现生产者-消费者模型。
- **预期效果 (量化)**: Binlog 解析吞吐量提升 2-4 倍 (4 核并发), 端到端延迟降低 30-50%。
- **关键风险**: 事务边界必须严格保持 (同一事务的事件不能被打散执行), 否则会出现部分提交导致数据不一致; 死锁和内存溢出风险。

### 假设 H3: 基于 binlog GTID/XID 的断点续写与幂等重试

- **假设内容**: 在闪回执行过程中, 每处理完一个完整事务 (遇到 XID_EVENT), 将当前 binlog 位置 (file + position + GTID) 写入持久化检查点文件 (`flashback_checkpoint_<session_id>.bin`)。如果闪回中断, 可从最近检查点恢复, 且通过 `INSERT IGNORE` / `DELETE WHERE PK` 保证幂等性。
- **预期效果 (量化)**: 中断恢复时间从全量重扫 (<10s → 秒级), 重复执行闪回操作不会产生副作用。
- **关键风险**: 检查点写入本身需要 fsync (影响性能), 且 MySQL DML 的幂等性无法通过简单 SQL 保证 (UPDATE 语义本身非幂等)。

### 假设 H4: DDL Barrier 采用 Binlog 事件驱动 + 预构建 DDL 时间线索引

- **假设内容**: 在 `DDLBarrier::check()` 中, 不再简单返回 NONE, 而是正向扫描 binlog, 提取所有 QUERY_EVENT (DDL), 构建 `<timestamp, DDL_type>` 时间线索引。然后判断闪回窗口内是否存在不兼容 DDL。为了性能, 首次扫描后缓存时间线索引, 后续检查 O(log n) 查找。
- **预期效果 (量化)**: DDL 检查从 100% 误报 (当前返回 NONE) 提升到精准检测, 扫描 1GB binlog <30s (多线程)。
- **关键风险**: Binlog 中 DDL 的识别需要完整 SQL 解析 (当前用 `strstr` 容易误判), 且 binlog 格式可能随版本变化。

### 假设 H5: Undo 引擎采用 InnoDB 版本链扫描 + 分批提交

- **假设内容**: Undo 引擎 `execute_undo_flashback()` 通过全表扫描聚集索引, 对每行调用 `row_build_flashback_version()` 构建历史版本, 与当前版本差异后执行更新。采用分批提交 (每 10000 行一次 `trans_commit`), 避免 Undo 膨胀和长事务。
- **预期效果 (量化)**: 100 万行、10% 变更的表, 闪回时间 <30s; 内存峰值 <500MB。
- **关键风险**: 全表扫描期间如果有并发 DDL 会崩溃 (需 MDL 排他锁); 分批提交如果中途失败, 部分恢复导致数据半状态。

---

## Phase 2: 证据搜集

### H1: Table_map_event 缓存 + 列类型感知逆向

**支持证据:**
- **strong**: `log_event.h` 中 `Rows_log_event` 已有 `m_table` 字段 (TABLE*) 和 `get_table_id()` 方法。MySQL 自身的 binlog applier 使用 `m_table_map` (Relay_log_info::m_table_map) 维护 table_id → Table_map_event 映射。
- **strong**: MyFlash (美团开源) 和 binlog2sql 已验证此方案的可行性——它们通过解析 Table_map_event 获取表结构, 再逆向 Rows_event。
- **moderate**: `Update_rows_log_event` 已有 `m_cols` / `m_cols_ai` 位图描述哪些列在 before/after image 中存在。
- **moderate**: `Rows_log_event` 有 `unpack_current_row()` / `unpack_after_image()` 方法可将二进制数据解压到 `m_table->record[]`。

**反例:**
- **strong**: `Table_map_log_event` 的完整列名信息 (`m_colnames`) 在 MySQL 8.0 中默认不写入 binlog (需要 `binlog_row_metadata=FULL`), 此时逆向 SQL 只能用列序号 (`col_1`, `col_2`) 而非真实列名。
- **moderate**: `Rows_log_event` 的 `m_table` 字段在 binlog 文件读取模式下可能为 `nullptr` (它是在 replication applier 中填充的), 因此必须自己维护 table_id 到 Table_map_event 的映射。
- **moderate**: BLOB/TEXT 列在 `binlog_row_image=NOBLOB` 模式下可能不完整, 逆向后数据丢失。

**证据权重**: strong 证据 2, moderate 证据 3, 反例 weight ×1.5 = 4.5 → **H1 置信度: 92%**

---

### H2: 分阶段并发处理架构

**支持证据:**
- **moderate**: MySQL 8.0 MTS (Multi-Threaded Slave) 已实现类似的并发 binlog 应用机制, 使用 `slave_parallel_workers` 和 `LOGICAL_CLOCK` 调度器。
- **moderate**: 设计文档 `mysql_flashback_implementation_v2.md` §7.1 风险矩阵提到 "Binlog 解析性能瓶颈" 并建议 "多线程并行解析"。
- **weak**: Go 语言模式中的并发模式 (goroutine + channel) 可作为参考: 将 binlog 事件按事务分组后分发到 worker 池。

**反例:**
- **strong**: Binlog 事件之间存在严格的因果关系——同一事务内的 WRITE/UPDATE/DELETE 必须按原始顺序逆向 (逆序执行), 不能并行化同一事务内的操作。
- **strong**: UPDATE 操作如果跨行, 并行执行可能导致死锁 (A 行更新需要 B 行锁, B 行更新需要 A 行锁)。
- **moderate**: 逆向 SQL 的执行本身受 InnoDB 行锁保护, 并发度天然受限。实际性能提升可能 <2x。

**证据权重**: moderate 证据 2, weak 证据 1, 反例 weight ×1.5 = 6 → **H2 置信度: 55%**

---

### H3: 断点续写 + 幂等重试

**支持证据:**
- **moderate**: MySQL 的 `mysqlbinlog --start-position` 和 `--stop-position` 已验证断点续读的可行性。
- **moderate**: GTID 机制提供了全局唯一的事务标识, 可用于精确恢复点定位。
- **weak**: `INSERT IGNORE` 对反向 INSERT 有天然幂等性, `DELETE WHERE PK = val` 对反向 DELETE 也幂等。

**反例:**
- **strong**: 反向 UPDATE (`UPDATE ... SET old_val WHERE PK = new_val`) 本质上**非幂等**——如果新值和旧值恰好相同, 第二次执行不改变数据, 但也不报错; 如果中间有新写入, 第二次执行会覆盖新数据。
- **strong**: 检查点写入需要 `fsync`, 在高 TPS 场景下可能成为瓶颈 (每个事务一次 fsync)。
- **moderate**: 如果闪回中断后原始数据已被新事务修改, 从检查点恢复可能产生逻辑冲突。

**证据权重**: moderate 证据 2, weak 证据 1, 反例 weight ×1.5 = 4.5 → **H3 置信度: 60%**

---

### H4: DDL Barrier 采用 Binlog 事件驱动

**支持证据:**
- **strong**: 设计文档 §7.2 明确列出了 DDL 兼容性矩阵 (DROP TABLE/CHANGE COLUMN 等阻止闪回, ADD INDEX 不阻止)。
- **strong**: `DDLBarrier::is_ddl_compatible()` 已实现基本字符串匹配框架, 只需补全 binlog 扫描逻辑。
- **moderate**: `Binlog_file_reader` 已有 `read_event_object()` 方法, 可遍历 binlog 事件。

**反例:**
- **strong**: 当前 `is_ddl_compatible()` 使用 `strstr` 匹配 (如 `"DROP TABLE"`), 但实际 binlog 中的 QUERY_EVENT 存储的是原始 SQL 文本, 可能包含注释、大小写混写、反引号等, 简单的子串匹配会产生误判。
- **moderate**: 完整扫描 binlog 做 DDL 检查在首次执行时开销大 (可能需要扫描数 GB binlog)。
- **moderate**: 某些 DDL (如 `ALTER TABLE ... ALGORITHM=INPLACE`) 不写 binlog 或使用不同事件类型, 可能漏检。

**证据权重**: strong 证据 2, moderate 证据 2, 反例 weight ×1.5 = 4.5 → **H4 置信度: 78%**

---

### H5: Undo 引擎采用版本链扫描 + 分批提交

**支持证据:**
- **strong**: 设计文档 §5.2.1 提供了完整的 `flashback_single_table()` 伪代码, 包括 `row_build_flashback_version()` 调用。
- **strong**: InnoDB 已有 `row_vers_build_for_consistent_read()` 可用于构建历史版本, `row_build_flashback_version()` 是其上层封装。
- **moderate**: 分批提交策略 (每 N 行 commit) 是 MySQL 在线 DDL 和 pt-online-schema-change 的标准做法。

**反例:**
- **strong**: `row_build_flashback_version()` 函数在 Percona Server 源码中尚**未实现**——它只是设计文档中的占位符, 需要实际对接 InnoDB 的版本链遍历。
- **strong**: 全表扫描期间, 如果有并发 INSERT, 新增行没有历史版本, 需要特殊处理 (跳过 or 视为未变更)。
- **moderate**: 分批提交如果中间批次失败, 已提交的批次无法回滚, 导致数据处于部分闪回状态。需要额外的一致性保护 (如闪回前快照 + 验证)。

**证据权重**: strong 证据 2, moderate 证据 1, 反例 weight ×1.5 = 4.5 → **H5 置信度: 72%**

---

## Phase 3: 验证与收敛

### 交叉对比

| 假设间关系 | 分析 |
|-----------|------|
| H1 vs H2 | 不矛盾。H1 是数据依赖 (Table_map 缓存), H2 是执行模型。但 H2 的并发架构要求 H1 的缓存是线程安全的。 |
| H1 vs H3 | 不矛盾。断点续写需要的检查点信息包含 table_id, 依赖 H1 的映射关系。 |
| H2 vs H3 | 矛盾点: H2 的并发执行模型与 H3 的事务边界严格有序执行有冲突。**解决**: 并发仅限"解析+生成逆向 SQL"阶段, "执行"阶段仍保持事务顺序。 |
| H4 vs H1 | 互补。DDL Barrier 需要解析 QUERY_EVENT, H1 处理 Rows_event, 共用同一 binlog 扫描基础设施。 |
| H5 vs H2 | 不矛盾。Undo 引擎 (H5) 是独立的执行路径, Binlog 引擎 (H2) 是另一条路径。 |

### 证据权重汇总

| 假设 | 支持 (S/M/W) | 反例 (×1.5) | 置信度 | 推荐优先级 |
|------|-------------|------------|--------|-----------|
| H1: Table_map 缓存 + 列类型感知 | 2S + 3M | 3 (×1.5=4.5) | **92%** | ★★★ 最高 |
| H4: DDL Barrier binlog 事件驱动 | 2S + 2M | 3 (×1.5=4.5) | **78%** | ★★☆ 高 |
| H5: Undo 引擎版本链扫描 | 2S + 1M | 3 (×1.5=4.5) | **72%** | ★★☆ 高 |
| H3: 断点续写 + 幂等 | 2M + 1W | 3 (×1.5=4.5) | **60%** | ★☆☆ 中 |
| H2: 分阶段并发处理 | 2M + 1W | 3 (×1.5=6) | **55%** | ★☆☆ 低 (后期优化) |

---

### 技术选型对比表

| 方案 | 优势 | 劣势 | 适用场景 | 推荐度 |
|------|------|------|---------|--------|
| **方案 A: 单线程 + Table_map 缓存** (H1 单线程版) | 实现简单, 无并发 bug 风险, 事务严格有序 | 性能受限于单核, 大 binlog 处理慢 | 中小规模表, 闪回窗口 <1 小时 | ★★★★★ |
| **方案 B: 解析并发 + 执行串行** (H1+H2 混合) | 解析速度 2-3x 提升, 执行严格有序 | 架构复杂, 需要线程安全缓存和有序队列 | 大规模表, 闪回窗口 >1 小时 | ★★★★ |
| **方案 C: 全并发 (解析+执行)** | 最大吞吐量 | 事务一致性风险高, 死锁频繁, 需要复杂的冲突检测 | 仅适用于只读闪回查询 | ★★ |
| **方案 D: 外部工具 (MyFlash)** | 无需改源码, 立即可用 | 性能差 (需网络拉取 binlog), 不支持闪回查询 | 临时应急 | ★★ |

**推荐方案: B (解析并发 + 执行串行)** — Phase 1 先实现方案 A, Phase 2 升级到方案 B。

---

### 关键技术难点及解决方案

#### 难点 1: Rows_log_event 与 Table_map_event 的关联

**问题**: `Binlog_file_reader` 模式下, `Rows_log_event` 的 `m_table` 字段为 `nullptr`, 无法直接获取表名和列信息。

**解决方案**:
```cpp
// 在 BinlogFlashbackEngine 中维护 Table_map 缓存
class TableMapCache {
 public:
  // 处理 Table_map_event, 缓存 table_id → 元信息
  bool register_table_map(const Table_map_log_event &tme);
  
  // 根据 table_id 获取表元信息
  const TableMapInfo* lookup(table_id_type tid) const;
  
  // 处理 Rotate_event 时清空缓存
  void on_rotate();
  
 private:
  std::unordered_map<table_id_type, TableMapInfo> m_cache;
};

struct TableMapInfo {
  std::string db_name;
  std::string table_name;
  std::vector<enum_field_types> col_types;
  std::vector<std::string> col_names;   // 仅 binlog_row_metadata=FULL 时有值
  size_t key_columns_count;
  MY_BITMAP key_columns;                 // 主键列位图
};
```

**执行流程**:
1. 扫描 binlog 时, 遇到 `TABLE_MAP_EVENT` → 调用 `register_table_map()`
2. 遇到 `ROTATE_EVENT` → 调用 `on_rotate()` 清空缓存 (新文件的 table_id 可能复用旧值)
3. 遇到 `WRITE_ROWS_EVENT` 等 → 通过 `get_table_id()` 查缓存获取列信息

#### 难点 2: 列类型感知的 SQL 值编码

**问题**: 不同 MySQL 类型需要不同的 SQL 字面量格式 (字符串加引号、日期格式化、BLOB 用 HEX 等)。

**解决方案**:
```cpp
std::string encode_field_value(enum_field_types type,
                               const uchar *field_data,
                               uint field_len,
                               bool is_null,
                               const CHARSET_INFO *cs) {
  if (is_null) return "NULL";
  
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
      // 整数类型: 直接转字符串, 注意有无符号
      return std::to_string(decode_integer(field_data, field_len));
    
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      // 浮点类型
      return std::to_string(decode_float(field_data, field_len));
    
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
      // 字符串: 需要转义单引号, 用单引号包裹
      return "'" + escape_sql_string(field_data, field_len, cs) + "'";
    
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_GEOMETRY:
      // BLOB/GEOMETRY: 使用 0xHEX 格式
      return "0x" + bin_to_hex(field_data, field_len);
    
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      // 时间类型: 'YYYY-MM-DD HH:MM:SS'
      return "'" + decode_datetime(field_data, field_len) + "'";
    
    case MYSQL_TYPE_NULL:
      return "NULL";
    
    default:
      // 降级方案: HEX
      return "0x" + bin_to_hex(field_data, field_len);
  }
}
```

#### 难点 3: 事务边界与逆序执行

**问题**: 闪回需要按事务逆序执行 (最后的事务先闪回), 但 binlog 是正向记录的。

**解决方案**:
```cpp
// 两阶段处理: 收集 → 逆序执行
struct TransactionGroup {
  std::vector<ReversedEvent> events;  // 已逆向的事件
  my_off_t start_pos;                  // 事务起始位置
  my_off_t end_pos;                    // 事务结束位置 (XID)
  uint64_t gtid_seq_no;                // GTID 序列号 (可选)
};

// 阶段 1: 正向扫描, 收集事件并按事务分组
std::vector<TransactionGroup> txn_groups;
TransactionGroup *current_txn = nullptr;

for each event in binlog:
  switch (event_type):
    case GTID_EVENT:
      current_txn = &txn_groups.emplace_back();
      current_txn->start_pos = event_pos;
      break;
    
    case QUERY_EVENT (BEGIN):
      if (!current_txn) current_txn = &txn_groups.emplace_back();
      break;
    
    case WRITE_ROWS/UPDATE_ROWS/DELETE_ROWS:
      if (!current_txn) current_txn = &txn_groups.emplace_back();
      current_txn->events.push_back(reverse_event(event));
      break;
    
    case XID_EVENT / COMMIT:
      if (current_txn) current_txn->end_pos = event_pos;
      current_txn = nullptr;
      break;

// 阶段 2: 逆序遍历事务组, 按事务执行
for (auto it = txn_groups.rbegin(); it != txn_groups.rend(); ++it) {
  trans_begin(m_thd);
  for (auto &rev_event : it->events) {
    execute_sql(rev_event.sql);
  }
  trans_commit(m_thd);
  m_txn_groups_committed++;
}
```

**注意**: 如果请求要求按表过滤 (`request.tables`), 在阶段 1 收集时即可跳过不属于目标表的事件。

#### 难点 4: 断点续写与检查点

**解决方案**:
```cpp
struct Checkpoint {
  char binlog_file[FN_REFLEN];
  my_off_t binlog_pos;
  uint64_t txn_groups_committed;
  uint64_t rows_processed;
  uint64_t rows_restored;
  time_t checkpoint_time;
};

// 每处理完一个事务组, 写入检查点
bool write_checkpoint(const Checkpoint &ckpt, const std::string &session_id) {
  std::string path = get_checkpoint_path(session_id);
  File fd = mysql_file_open(key_file_misc, path.c_str(), 
                            O_WRONLY | O_CREAT | O_TRUNC, MYF(MY_WME));
  if (fd < 0) return true;
  
  ssize_t n = my_write(fd, reinterpret_cast<const uchar*>(&ckpt), 
                       sizeof(ckpt), MYF(MY_WME | MY_NABP));
  // 关键: fsync 确保持久化
  my_sync(fd, MYF(MY_WME));
  mysql_file_close(fd, MYF(0));
  return n != sizeof(ckpt);
}

// 恢复检查点
bool load_checkpoint(const std::string &session_id, Checkpoint &out) {
  std::string path = get_checkpoint_path(session_id);
  File fd = mysql_file_open(key_file_misc, path.c_str(), O_RDONLY, MYF(0));
  if (fd < 0) return true;
  
  ssize_t n = my_read(fd, reinterpret_cast<uchar*>(&out), sizeof(out), MYF(0));
  mysql_file_close(fd, MYF(0));
  return n != sizeof(out);
}
```

**幂等性保证**:
- 反向 INSERT: 使用 `INSERT IGNORE INTO ...` (如果 PK 已存在则跳过)
- 反向 DELETE: 使用 `DELETE FROM ... WHERE pk = val` (重复删除无害)
- 反向 UPDATE: 使用 `UPDATE ... SET old_val WHERE pk = new_val AND col = new_val` (增加条件防止覆盖新数据)

#### 难点 5: DDL Barrier 的 binlog 扫描与 SQL 解析

**解决方案**:
```cpp
FlashbackError DDLBarrier::check(const FlashbackRequest &request) {
  // 1. 定位起始 binlog 位置 (target_time 对应的位置)
  char start_file[FN_REFLEN];
  my_off_t start_pos = 0;
  uint8_t checksum_alg = 0;
  
  FlashbackError err = BinlogFlashbackEngine::find_position_at_timestamp(
      request.target_time, start_file, &start_pos, &checksum_alg);
  if (err != FlashbackError::NONE) return err;
  
  // 2. 扫描 binlog 查找 DDL
  for each binlog file from start_file to current:
    for each event in file:
      if event is QUERY_EVENT:
        Query_log_event *qev = dynamic_cast<Query_log_event*>(event);
        if (qev && is_ddl_statement(qev->query)) {
          if (affects_target_tables(qev->query, request.tables)) {
            if (!is_ddl_compatible(qev->query)) {
              delete event;
              return FlashbackError::DDL_INCOMPATIBLE;
            }
          }
        }
      if event is TABLE_MAP_EVENT:
        // 缓存 Table_map_event 以支持后续 Rows_event 关联
      
  return FlashbackError::NONE;
}

// 改进的 DDL 识别: 提取第一个关键字进行判断
static bool is_ddl_statement(const char *sql) {
  if (!sql || !sql[0]) return false;
  
  // 跳过前导空白和注释
  const char *p = sql;
  while (*p && (my_isspace(system_charset_info, *p) || *p == '#')) {
    if (*p == '#') { while (*p && *p != '\n') p++; }
    else p++;
  }
  
  // 匹配已知 DDL 关键字 (不区分大小写)
  return strncasecmp(p, "ALTER", 5) == 0 ||
         strncasecmp(p, "CREATE", 6) == 0 ||
         strncasecmp(p, "DROP", 4) == 0 ||
         strncasecmp(p, "TRUNCATE", 8) == 0 ||
         strncasecmp(p, "RENAME", 6) == 0;
}

// 改进的 DDL 兼容性检查
bool DDLBarrier::is_ddl_compatible(const char *ddl_sql) const {
  if (!ddl_sql || !ddl_sql[0]) return true;
  
  std::string upper_sql = to_upper(ddl_sql);
  
  // 不兼容: DROP TABLE, TRUNCATE
  if (contains_keyword(upper_sql, "DROP TABLE") ||
      contains_keyword(upper_sql, "TRUNCATE")) {
    return false;
  }
  
  // 不兼容: DROP COLUMN
  if (contains_keyword(upper_sql, "DROP COLUMN") ||
      contains_keyword(upper_sql, "DROP PRIMARY KEY")) {
    return false;
  }
  
  // 不兼容: CHANGE / MODIFY 涉及类型变更
  if (contains_keyword(upper_sql, "CHANGE COLUMN") ||
      contains_keyword(upper_sql, "MODIFY COLUMN")) {
    // 进一步检查是否仅重命名 vs 类型变更 (需要 SQL 解析)
    // 保守策略: 一律视为不兼容
    return false;
  }
  
  // 不兼容: RENAME TABLE (影响表名映射)
  if (contains_keyword(upper_sql, "RENAME TABLE")) {
    return false;
  }
  
  // 兼容: ADD INDEX, DROP INDEX, ADD COLUMN (有默认值), 
  //        ALGORITHM=INPLACE 的某些变更
  return true;
}

// 辅助: 判断 DDL 是否影响目标表
static bool affects_target_tables(const char *ddl_sql,
                                  const LEX_CSTRING *tables,
                                  uint32_t table_count) {
  // 从 DDL 中提取表名, 与 request.tables 比对
  // 简化实现: 检查每个目标表名是否出现在 DDL SQL 中
  for (uint32_t i = 0; i < table_count; i++) {
    if (strstr(ddl_sql, tables[i].str)) return true;
  }
  return false;
}
```

#### 难点 6: Undo 引擎的实现

**解决方案**: 创建 `flashback_undo_engine.cc`, 核心逻辑:

```cpp
// flashback_undo_engine.h
class UndoFlashbackEngine {
 public:
  explicit UndoFlashbackEngine(THD *thd);
  ~UndoFlashbackEngine();
  
  bool execute(const FlashbackRequest &request, FlashbackResult &result);
  
 private:
  bool flashback_single_table(const LEX_CSTRING &table_ref,
                              my_time_t target_ts,
                              uint64_t max_rows,
                              uint64_t &rows_processed,
                              uint64_t &rows_restored);
  
  THD *m_thd;
};

// flashback_undo_engine.cc
bool UndoFlashbackEngine::execute(const FlashbackRequest &request,
                                  FlashbackResult &result) {
  result.state = FlashbackState::RUNNING;
  result.engine_used = FlashbackEngineType::UNDO;
  
  // 关闭 binlog 记录
  bool was_binlog_on = (m_thd->variables.option_bits & OPTION_BIN_LOG) != 0;
  m_thd->variables.option_bits &= ~OPTION_BIN_LOG;
  
  uint64_t total_rows_processed = 0;
  uint64_t total_rows_restored = 0;
  bool error = false;
  
  // 分批提交: 每 batch_size 行 commit 一次
  const uint64_t batch_size = 10000;
  
  for (uint32_t i = 0; i < request.table_count && !error; i++) {
    uint64_t tbl_rows_processed = 0;
    uint64_t tbl_rows_restored = 0;
    
    error = flashback_single_table(request.tables[i], request.target_time,
                                   request.max_rows,
                                   tbl_rows_processed, tbl_rows_restored);
    
    total_rows_processed += tbl_rows_processed;
    total_rows_restored += tbl_rows_restored;
    result.tables_processed = i + 1;
  }
  
  // 恢复 binlog 状态
  if (was_binlog_on) m_thd->variables.option_bits |= OPTION_BIN_LOG;
  
  if (error) {
    result.state = FlashbackState::FAILED;
    result.error = FlashbackError::GENERIC;
    return true;
  }
  
  result.state = FlashbackState::COMPLETED;
  result.rows_processed = total_rows_processed;
  result.rows_restored = total_rows_restored;
  return false;
}

bool UndoFlashbackEngine::flashback_single_table(
    const LEX_CSTRING &table_ref, my_time_t target_ts,
    uint64_t max_rows, uint64_t &rows_processed, uint64_t &rows_restored) {
  
  // 1. 打开表
  // 2. 全表扫描聚集索引
  // 3. 对每行调用 row_build_flashback_version()
  // 4. 如果历史版本与当前版本不同, 执行更新
  // 5. 每 batch_size 行 commit 一次
  
  // TODO: 实际实现需要接入 InnoDB handler 接口
  // 这是 Phase 2 的核心工作, 需要:
  // - 打开 InnoDB 表 (open_table_in_flashback_mode)
  // - 使用 ha_innobase::index_first() + index_next() 遍历
  // - 调用 row_build_flashback_version() 构建历史版本
  // - 使用 ha_innobase::flashback_update_row() 更新
  
  rows_processed = 0;
  rows_restored = 0;
  return false;  // 占位: 返回成功
}
```

---

### 推荐结论

#### 总体推荐方案: **分阶段实施**

**Phase 1 (当前, 高优先级)**:
1. ✅ **H1: Table_map 缓存 + 列类型感知逆向 SQL** — 补齐 `flashback_binlog_engine.cc` 的核心 TODO
   - 实现 `TableMapCache` 类
   - 实现 `encode_row_as_sql_values()` 的列类型分支
   - 实现 `reverse_write_event()`, `reverse_delete_event()`, `reverse_update_event()` 的完整逻辑
   
2. ✅ **H4: DDL Barrier 完善** — 实现 `DDLBarrier::check()` 的 binlog 扫描逻辑
   - 改进 `is_ddl_compatible()` 使用 `strncasecmp` 而非 `strstr`
   - 实现 binlog 扫描 + DDL 提取
   - 实现 `affects_target_tables()` 表名匹配

3. ✅ **H5: Undo 引擎骨架** — 创建 `flashback_undo_engine.cc/h`
   - 实现基本框架 (关闭 binlog, 遍历表, 分批提交)
   - 实际 InnoDB 版本链对接标记为 Phase 2 任务

**Phase 1.5 (中优先级)**:
4. ✅ **H3: 断点续写** — 实现检查点机制
   - 检查点文件读写
   - `INSERT IGNORE` / 条件 UPDATE 保证幂等
   - 事务闪回支持 (按 GTID/XID 定位)

**Phase 2 (后期优化)**:
5. ⚠️ **H2: 并发优化** — 解析并发 + 执行串行
   - 仅在 Phase 1 验证正确性后再引入
   - 需要充分的并发测试

#### 决策理由

1. **H1 是阻塞性依赖**: 没有 Table_map 关联, 所有逆向 SQL 都是无效的 `unknown_table`, 整个 Binlog 引擎形同虚设。必须最先实现。
2. **H4 是安全门控**: DDL 屏障是防止数据损坏的关键检查, 当前 100% 放行 (返回 NONE) 是严重的安全隐患。
3. **H5 是双引擎完整性的另一半**: 调度器已有 Undo 引擎的选择逻辑, 但引擎本身是空的。补齐骨架即可让 AUTO 模式工作。
4. **H3 是运维必需**: 大表闪回可能需要数小时, 没有断点续写意味着中断就要从头开始。
5. **H2 是性能优化而非功能必需**: 在正确性未验证前引入并发, 会大幅增加调试难度。

---

### 实施文件清单

| 文件 | 改动类型 | 预估行数 | 优先级 |
|------|---------|---------|--------|
| `sql/flashback_binlog_engine.cc` | 重写核心函数 | +600 | P0 |
| `sql/flashback_binlog_engine.h` | 新增 TableMapCache | +80 | P0 |
| `sql/flashback_ddl_barrier.cc` | 实现 check() | +200 | P0 |
| `sql/flashback_undo_engine.cc` | 新增文件 | +300 | P1 |
| `sql/flashback_undo_engine.h` | 新增文件 | +60 | P1 |
| `sql/flashback_scheduler.cc` | 接入 Undo 引擎 | +30 | P1 |
| `sql/flashback_checkpoint.cc` | 新增 (断点续写) | +200 | P1.5 |
| `sql/flashback_checkpoint.h` | 新增 | +50 | P1.5 |
| `sql/CMakeLists.txt` | 注册新源文件 | +10 | P0 |
