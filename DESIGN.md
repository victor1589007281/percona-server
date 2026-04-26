# MySQL Flashback 技术设计文档 (DESIGN.md)

> **目标版本**: Percona Server 8.4.7-7 LTS (MySQL 8.4 LTS 系列)
> **源码基础**: `/home/victor/base/git/others/percona-server` (branch: 8.4.7-7)
> **上游输入**: 7 份研究文档 (`research/mysql_flashback_*.md`) + 源码验证
> **架构师**: architect
> **日期**: 2025-07-28

---

## 1. 架构概览

### 1.1 分层架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                        MySQL Server Layer                        │
│                                                                  │
│  ┌──────────────┐  ┌───────────────┐  ┌────────────────────┐    │
│  │  SQL Parser  │  │  Flashback    │  │  Flashback         │    │
│  │  (sql_yacc)  │→ │  Executor     │→ │  Result Handler    │    │
│  │              │  │  (sql_cmd.cc) │  │                    │    │
│  └──────────────┘  └───────┬───────┘  └────────────────────┘    │
│                            │                                     │
│           ┌────────────────┴────────────────┐                    │
│           │      FlashbackScheduler         │                    │
│           │  • Engine selection (Undo/Bin)  │                    │
│           │  • DDL barrier check            │                    │
│           │  • Permission check             │                    │
│           │  • DRY RUN dispatch             │                    │
│           └────────────────┬────────────────┘                    │
└────────────────────────────┼─────────────────────────────────────┘
                             │
          ┌──────────────────┴──────────────────┐
          │                                     │
┌─────────▼──────────┐              ┌──────────▼──────────────┐
│   UndoFlashback     │              │   BinlogFlashback       │
│   Engine (核心)     │              │   Engine (补充)         │
│                    │              │                         │
│ • AS OF 闪回查询   │              │ • 长窗口闪回表          │
│ • 闪回表 (分钟级)  │              │ • 闪回事务              │
│ • 闪回版本查询     │              │                         │
│                    │              │ 依赖:                    │
│ 依赖:              │              │ • Binlog_event_data_    │
│ • row_vers_build_  │              │   istream               │
│   _for_consistent_ │              │ • Rows_log_event 体系   │
│   read()           │              │ • log_event.h           │
│ • trx_purge_stop/  │              │                         │
│   run()            │              │ 约束:                    │
│                    │              │ • binlog_row_image=FULL │
└─────────┬──────────┘              │ • 表需有主键            │
          │                         └──────────┬──────────────┘
          │                                    │
          ▼                                    ▼
┌──────────────────────┐          ┌────────────────────────┐
│  InnoDB Undo Log     │          │  MySQL Binlog Files    │
│  (ibundo_*)          │          │  (mysql-bin.00000N)    │
│  row0vers.cc:1255    │          │  log_event.h           │
│  trx0purge.h:96      │          │  binlog_reader.cc      │
└──────────────────────┘          └────────────────────────┘
```

### 1.2 架构模式: Clean Architecture 变体

采用 **Clean Architecture** 变体, 以 InnoDB 存储引擎为内层核心, SQL 层为外层适配:

```
外层 (依赖内层)                     内层 (不依赖外层)
┌────────────────────┐             ┌──────────────────────┐
│ SQL Layer          │             │ InnoDB Engine        │
│ • Parser (yacc)    │  ──────→   │ • row_vers_*         │
│ • Executor         │  单向依赖   │ • trx_undo_*         │
│ • Scheduler        │             │ • trx_purge_*        │
│ • Cmd Handler      │             │ • trx_sys_*          │
└────────────────────┘             └──────────────────────┘
```

**依赖方向**: SQL 层 → InnoDB 层 (单向, 无反向依赖)

### 1.3 核心模式

| 模式 | 应用位置 | 说明 |
|------|---------|------|
| **策略模式** | FlashbackScheduler → IFlashbackEngine | Undo/Binlog 引擎可插拔 |
| **模板方法** | Sql_cmd_flashback → execute_inner() | 公共流程在基类, 差异化在子类 |
| **工厂模式** | FlashbackEngineFactory::create() | 根据时间窗口选择引擎 |
| **责任链** | 安全检测链: 权限→DDL→引擎→执行 | 每个检测点可短路返回 |

---

## 2. 组件分解

### 2.1 模块总览

| 模块名 | 目录 | 职责 | 依赖 |
|--------|------|------|------|
| `sql_cmd_flashback` | sql/ | SQL 命令分发入口 | LEX, THD |
| `flashback_scheduler` | sql/ | 引擎选择 + 路由 | IFlashbackEngine |
| `flashback_undo_engine` | storage/innobase/ | Undo-based 闪回 | row0vers, trx0purge |
| `flashback_binlog_engine` | sql/ | Binlog-based 闪回 | log_event.h |
| `flashback_purge_guard` | storage/innobase/ | Purge 暂停/恢复 | trx0purge |
| `flashback_ddl_barrier` | sql/ | DDL 兼容性检查 | MDL, dd::Table |
| `flashback_errors` | sql/share/, include/ | 错误码 + 消息 | - |
| `flashback_sysvars` | sql/ | 系统变量注册 | mysqld |

### 2.2 接口定义

#### 2.2.1 IFlashbackEngine (核心接口)

```cpp
// sql/flashback_types.h

/** 闪回操作类型 */
enum class FlashbackType {
    QUERY,       /* SELECT ... AS OF TIMESTAMP */
    TABLE,       /* FLASHBACK TABLE ... TO TIMESTAMP */
    VERSIONS,    /* SELECT ... VERSIONS BETWEEN */
    TRANSACTION  /* FLASHBACK TRANSACTION <xid> */
};

/** 闪回引擎类型 */
enum class FlashbackEngineType {
    UNDO,        /* 基于 Undo Log (分钟窗口) */
    BINLOG,      /* 基于 Binlog (天级窗口) */
    NONE         /* 无可用引擎 */
};

/** 闪回请求 */
struct FlashbackRequest {
    FlashbackType type;
    std::vector<dd::Object_id> table_ids;
    my_time_t   target_timestamp;   /* TO TIMESTAMP '...' */
    my_time_t   start_timestamp;    /* VERSIONS BETWEEN 起始 */
    my_time_t   end_timestamp;      /* VERSIONS BETWEEN 结束 */
    trx_id_t    target_trx_id;      /* TO TRX_ID <id> */
    bool        dry_run;            /* DRY RUN 模式 */
    ulonglong   max_rows;           /* 最大处理行数 */
};

/** 闪回结果 */
struct FlashbackResult {
    bool        success;
    ulonglong   rows_scanned;
    ulonglong   rows_restored;
    ulonglong   tables_processed;
    my_time_t   start_time;
    my_time_t   end_time;
    FlashbackEngineType engine_used;
    std::string error_message;      /* 失败时填充 */
};

/** 闪回引擎接口 (策略模式) */
class IFlashbackEngine {
public:
    virtual ~IFlashbackEngine() = default;

    /** 引擎类型标识 */
    virtual FlashbackEngineType type() const = 0;

    /** 检查请求是否适用于本引擎 */
    virtual bool supports(const FlashbackRequest &req) const = 0;

    /** 执行闪回操作
        @param thd       线程句柄
        @param req       闪回请求
        @param result    输出: 闪回结果
        @return true=失败, false=成功 */
    virtual bool execute(THD *thd,
                         const FlashbackRequest &req,
                         FlashbackResult &result) = 0;

    /** 获取引擎可提供的最大时间窗口(秒) */
    virtual ulonglong max_window_seconds() const = 0;

    /** 获取引擎最古老可用时间点 */
    virtual my_time_t oldest_available_time() const = 0;
};
```

#### 2.2.2 FlashbackScheduler

```cpp
// sql/flashback_scheduler.h

class FlashbackScheduler {
public:
    /** 构造函数
        @param engines 可用引擎列表 (按优先级排序) */
    explicit FlashbackScheduler(
        std::vector<IFlashbackEngine *> engines);

    /** 执行闪回 (自动选择引擎)
        @param thd       线程句柄
        @param req       闪回请求
        @param result    输出: 闪回结果
        @return true=失败, false=成功 */
    bool execute(THD *thd,
                 const FlashbackRequest &req,
                 FlashbackResult &result);

private:
    /** 选择最优引擎 */
    IFlashbackEngine *select_engine(
        const FlashbackRequest &req) const;

    /** 执行安全检测链 */
    bool run_safety_checks(
        THD *thd,
        const FlashbackRequest &req) const;

    std::vector<IFlashbackEngine *> m_engines;
};
```

#### 2.2.3 FlashbackPurgeGuard (RAII)

```cpp
// storage/innobase/include/flashback_purge_guard.h

/** RAII 风格的 Purge 保护器
    构造时暂停 purge, 析构时恢复 purge.
    闪回操作的生命周期必须完全包含在 guard 作用域内. */
class FlashbackPurgeGuard {
public:
    FlashbackPurgeGuard();
    ~FlashbackPurgeGuard();

    /** 检查是否成功暂停了 purge */
    bool is_active() const { return m_stopped; }

    /* 禁止拷贝 */
    FlashbackPurgeGuard(const FlashbackPurgeGuard &) = delete;
    FlashbackPurgeGuard &operator=(const FlashbackPurgeGuard &) = delete;

private:
    bool m_stopped;
};
```

#### 2.2.4 UndoFlashbackEngine

```cpp
// storage/innobase/include/flashback_undo_engine.h

class UndoFlashbackEngine : public IFlashbackEngine {
public:
    UndoFlashbackEngine();
    ~UndoFlashbackEngine() override = default;

    FlashbackEngineType type() const override;
    bool supports(const FlashbackRequest &req) const override;
    bool execute(THD *thd,
                 const FlashbackRequest &req,
                 FlashbackResult &result) override;
    ulonglong max_window_seconds() const override;
    my_time_t oldest_available_time() const override;

private:
    /** 闪回查询 (AS OF TIMESTAMP) */
    bool execute_query(THD *thd,
                       const FlashbackRequest &req,
                       FlashbackResult &result);

    /** 闪回表 (FLASHBACK TABLE) */
    bool execute_table(THD *thd,
                       const FlashbackRequest &req,
                       FlashbackResult &result);

    /** 闪回版本查询 (VERSIONS BETWEEN) */
    bool execute_versions(THD *thd,
                          const FlashbackRequest &req,
                          FlashbackResult &result);

    /** DRY RUN 模式: 仅统计 */
    bool execute_dry_run(THD *thd,
                         const FlashbackRequest &req,
                         FlashbackResult &result);

    /** 闪回单表核心逻辑 */
    bool flashback_single_table(
        THD *thd,
        dd::Object_id table_id,
        my_time_t target_ts,
        bool dry_run,
        ulonglong max_rows,
        ulonglong &rows_scanned,
        ulonglong &rows_restored);
};
```

#### 2.2.5 BinlogFlashbackEngine

```cpp
// sql/flashback_binlog_engine.h

class BinlogFlashbackEngine : public IFlashbackEngine {
public:
    BinlogFlashbackEngine();
    ~BinlogFlashbackEngine() override = default;

    FlashbackEngineType type() const override;
    bool supports(const FlashbackRequest &req) const override;
    bool execute(THD *thd,
                 const FlashbackRequest &req,
                 FlashbackResult &result) override;
    ulonglong max_window_seconds() const override;
    my_time_t oldest_available_time() const override;

private:
    /** 定位目标时间点的 binlog 位置 */
    bool find_position_at_timestamp(
        my_time_t target_ts,
        std::string &binlog_file,
        my_off_t &binlog_pos);

    /** 逆向单个 Rows Event */
    bool reverse_rows_event(
        Rows_log_event *event,
        const FlashbackRequest &req,
        std::string &sql_out);

    /** 逆向 UPDATE (before↔after 互换) */
    bool reverse_update_event(
        Update_rows_log_event *event,
        std::string &sql_out);

    /** 逆向 INSERT → DELETE */
    bool reverse_write_event(
        Write_rows_log_event *event,
        std::string &sql_out);

    /** 逆向 DELETE → INSERT */
    bool reverse_delete_event(
        Delete_rows_log_event *event,
        std::string &sql_out);

    /** 检查 binlog_row_image 配置 */
    bool check_row_image_compatibility();
};
```

#### 2.2.6 DDLBarrier

```cpp
// sql/flashback_ddl_barrier.h

class DDLBarrier {
public:
    /** 检查目标时间点至今是否有不兼容 DDL
        @param thd          线程句柄
        @param table_ids    表 ID 列表
        @param target_ts    目标时间戳
        @param detail_out   输出: 冲突详情
        @return true=有冲突(不可闪回), false=安全 */
    static bool check(THD *thd,
                      const std::vector<dd::Object_id> &table_ids,
                      my_time_t target_ts,
                      std::string &detail_out);

    /** 获取表级 MDL 排他锁 (阻塞 DDL)
        @param thd       线程句柄
        @param table_ids 表 ID 列表
        @param timeout   锁等待超时(秒)
        @return true=获取失败, false=成功 */
    static bool acquire_exclusive_lock(
        THD *thd,
        const std::vector<dd::Object_id> &table_ids,
        ulong timeout);

    /** 释放表级 MDL 排他锁 */
    static void release_exclusive_lock(THD *thd);
};
```

### 2.3 InnoDB 层新增接口

#### 2.3.1 row_build_flashback_version

```cpp
// storage/innobase/include/row0vers.h (新增声明)

/** 构建记录在指定时间点的历史版本.
    内部复用 row_vers_build_for_consistent_read(),
    但构造一个指向目标时间戳的自定义 read_view_t.

    @param[in]  rec        聚集索引当前记录
    @param[in]  index      索引描述符
    @param[in]  target_ts  目标时间戳
    @param[out] old_vers   输出: 历史版本记录 (nullptr 表示无变更)
    @param[in]  heap       内存堆
    @return DB_SUCCESS 成功
            DB_MISSING_HISTORY undo 已被 purge, 历史不可用
            DB_ERROR 其他错误 */
[[nodiscard]] dberr_t row_build_flashback_version(
    const rec_t *rec,
    dict_index_t *index,
    my_time_t target_ts,
    const rec_t **old_vers,
    mem_heap_t *heap);
```

#### 2.3.2 trx_sys_time_mapping

```cpp
// storage/innobase/include/trx0sys.h (新增声明)

/** 获取最老的可用 undo 记录对应的时间戳.
    用于判断目标时间是否在闪回窗口内.

    @return 最老可用时间戳 (unixtime), 0 表示无记录 */
my_time_t trx_sys_get_oldest_undo_timestamp();

/** 估算指定时间戳对应的大致 trx_id.
    用于构造 read_view_t 以查询历史版本.

    @param[in] target_ts  目标时间戳
    @return 估算的 trx_id, TRX_ID_MAX 表示不可用 */
trx_id_t trx_sys_estimate_trx_id_at_timestamp(my_time_t target_ts);
```

---

## 3. 数据流

### 3.1 闪回查询 (SELECT ... AS OF TIMESTAMP)

```
Client
  │  "SELECT * FROM t AS OF TIMESTAMP '2025-07-28 10:30:00'"
  ▼
┌─────────────────────────────────────────────┐
│ 1. SQL Parser (sql_yacc.yy)                 │
│    • 识别 AS OF TIMESTAMP 语法              │
│    • 设置 lex->flashback_query = true       │
│    • 设置 lex->flashback_timestamp = ...    │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 2. Query Optimizer (sql_select.cc)          │
│    • 检测 flashback_query 标志              │
│    • 为 InnoDB 表设置闪回上下文             │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 3. ha_innobase::rnd_next()                  │
│    • 检测 m_flashback_mode = true           │
│    • 调用 rnd_next_flashback() 分支         │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 4. row_build_flashback_version()            │
│    • 构造 read_view_t (target_ts → trx_id)  │
│    • 调用 row_vers_build_for_consistent_    │
│      read() 遍历 undo 版本链                │
│    • 返回 old_vers 或 DB_MISSING_HISTORY    │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 5. 结果返回 Client                          │
│    • 若 DB_MISSING_HISTORY → 返回错误       │
│    • 否则返回历史版本数据                   │
└─────────────────────────────────────────────┘
```

### 3.2 闪回表 (FLASHBACK TABLE ... TO TIMESTAMP)

```
Client
  │  "FLASHBACK TABLE t1 TO TIMESTAMP '2025-07-28 10:30:00'"
  ▼
┌─────────────────────────────────────────────┐
│ 1. SQL Parser                               │
│    • 解析为 SQLCOM_FLASHBACK_TABLE          │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 2. Sql_cmd_flashback_table::execute()       │
│    • 权限检查 (FLASHBACK privilege)         │
│    • 调用 FlashbackScheduler::execute()     │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 3. FlashbackScheduler                       │
│    ├─ DDLBarrier::check()                   │
│    │   └─ 检查目标时间至今是否有 DDL        │
│    ├─ select_engine(req)                    │
│    │   └─ 根据时间窗口选 Undo 或 Binlog     │
│    └─ engine->execute()                     │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 4a. Undo 引擎路径 (短窗口)                  │
│    ├─ FlashbackPurgeGuard guard             │
│    │   └─ trx_purge_stop()                  │
│    ├─ DDLBarrier::acquire_exclusive_lock()  │
│    ├─ 全表扫描聚集索引                      │
│    │   └─ 每行: row_build_flashback_        │
│    │       version() → 比对 → 更新          │
│    ├─ 同步更新二级索引                      │
│    └─ trx_purge_run() / 释放 MDL 锁         │
└──────────────────┬──────────────────────────┘
                   │
┌─────────────────────────────────────────────┐
│ 4b. Binlog 引擎路径 (长窗口)                │
│    ├─ check_row_image_compatibility()       │
│    ├─ find_position_at_timestamp()          │
│    ├─ 正向读取 binlog events                │
│    │   └─ reverse_rows_event() 逆向生成 SQL │
│    ├─ sql_log_bin = OFF (防止嵌套)          │
│    └─ 执行逆向 SQL 事务                     │
└──────────────────┬──────────────────────────┘
                   ▼
┌─────────────────────────────────────────────┐
│ 5. 结果返回 Client                          │
│    • rows_scanned / rows_restored           │
│    • engine_used (UNDO / BINLOG)            │
└─────────────────────────────────────────────┘
```

### 3.3 引擎选择决策流

```
FlashbackScheduler::select_engine(req)
  │
  ├── 计算目标时间距现在的间隔 Δt
  │
  ├── Δt ≤ innodb_flashback_retention_seconds ?
  │     ├─ YES → Undo 引擎
  │     │         └─ 验证: oldest_undo_ts ≤ target_ts ?
  │     │             ├─ YES → 返回 UNDO
  │     │             └─ NO  → 降级到 Binlog
  │     │
  │     └─ NO → Δt ≤ binlog 保留天数 × 86400 ?
  │               ├─ YES → 检查 binlog_row_image = FULL ?
  │               │         ├─ YES → 检查表有主键 ?
  │               │         │         ├─ YES → 返回 BINLOG
  │               │         │         └─ NO  → 返回 NONE (无主键)
  │               │         └─ NO  → 返回 NONE (row_image 不兼容)
  │               │
  │               └─ NO → 返回 NONE (超出所有窗口)
  │
  └─ 返回选中的引擎 (或 NONE)
```

---

## 4. 文件结构

### 4.1 目录树

```
percona-server/
│
├── include/
│   ├── my_sqlcommand.h              ← 修改: 新增 SQLCOM_FLASHBACK_*
│   └── mysql/udf_registration_types.h  ← 无需修改
│
├── sql/
│   ├── sql_yacc.yy                  ← 修改: 新增 FLASHBACK 语法 (~+250 行)
│   ├── sql_yacc.h                   ← 自动生成
│   ├── sql_lex.h                    ← 修改: LEX 新增 flashback_* 字段
│   ├── sql_class.cc                 ← 修改: LEX::cleanup() 中处理 flashback
│   ├── sql_select.cc                ← 修改: JOIN::exec() 闪回查询分支
│   ├── parse_tree_nodes.h           ← 修改: 新增 PT_flashback_* 类
│   ├── parse_tree_nodes.cc          ← 修改: PT_flashback_* 实现
│   │
│   ├── flashback_types.h            ← ★ 新增: 核心类型定义
│   ├── flashback_scheduler.h        ← ★ 新增: 调度器接口
│   ├── flashback_scheduler.cc       ← ★ 新增: 调度器实现
│   ├── flashback_binlog_engine.h    ← ★ 新增: Binlog 引擎头
│   ├── flashback_binlog_engine.cc   ← ★ 新增: Binlog 引擎实现
│   ├── flashback_ddl_barrier.h      ← ★ 新增: DDL 屏障头
│   ├── flashback_ddl_barrier.cc     ← ★ 新增: DDL 屏障实现
│   ├── flashback_sysvars.h          ← ★ 新增: 系统变量头
│   ├── flashback_sysvars.cc         ← ★ 新增: 系统变量实现
│   └── sql_flashback.cc             ← ★ 新增: 命令入口总控
│
├── storage/innobase/
│   ├── include/
│   │   ├── row0vers.h               ← 修改: 新增 row_build_flashback_version() 声明
│   │   ├── trx0sys.h                ← 修改: 新增时间映射函数声明
│   │   ├── flashback_purge_guard.h  ← ★ 新增: RAII purge 保护器
│   │   └── flashback_undo_engine.h  ← ★ 新增: Undo 引擎头
│   │
│   ├── row/
│   │   └── row0vers.cc              ← 修改: 新增 row_build_flashback_version()
│   │
│   ├── trx/
│   │   └── trx0sys.cc               ← 修改: 新增时间映射函数
│   │
│   ├── handler/
│   │   └── ha_innodb.cc             ← 修改: 闪回查询行读取分支
│   │
│   └── flashback/
│       ├── flashback_undo_engine.cc ← ★ 新增: Undo 引擎实现
│       └── flashback_purge_guard.cc ← ★ 新增: RAII 保护器实现
│
├── share/
│   └── messages_to_clients.txt      ← 修改: 新增闪回错误消息
│
├── mysql-test/
│   └── suite/flashback/
│       ├── t/
│       │   ├── flashback_basic.test ← ★ 新增: 基础功能测试
│       │   ├── flashback_query.test ← ★ 新增: 闪回查询测试
│       │   ├── flashback_table.test ← ★ 新增: 闪回表测试
│       │   ├── flashback_dry_run.test ← ★ 新增: DRY RUN 测试
│       │   ├── flashback_ddl_barrier.test ← ★ 新增: DDL 屏障测试
│       │   ├── flashback_purge_race.test ← ★ 新增: Purge 竞态测试
│       │   └── flashback_binlog.test ← ★ 新增: Binlog 引擎测试
│       └── r/
│           └── (对应 .result 文件)
│
└── CMakeLists.txt                   ← 修改: 新增源文件编译
```

### 4.2 命名约定

| 类别 | 约定 | 示例 |
|------|------|------|
| C++ 类名 | PascalCase, 前缀无 | `FlashbackScheduler` |
| C 函数名 | snake_case, 前缀 `flashback_` / `row_build_` | `flashback_scheduler_execute()` |
| 系统变量 | snake_case, 前缀 `innodb_flashback_` | `innodb_flashback_retention_seconds` |
| SQL 关键字 | 大写 | `FLASHBACK`, `AS OF`, `VERSIONS` |
| 错误码 | 大写, 前缀 `ER_FLASHBACK_` | `ER_FLASHBACK_UNDO_PURGED` |
| 测试文件 | snake_case, 前缀 `flashback_` | `flashback_basic.test` |

---

## 5. 错误处理策略

### 5.1 错误分级

| 等级 | 分类 | 含义 | 处理策略 |
|------|------|------|---------|
| **E1** | 业务错误 | 用户输入不合法 | 返回错误消息, 不修改数据 |
| **E2** | 可恢复错误 | 运行时资源不足/超时 | 回滚闪回事, 释放锁, 返回错误 |
| **E3** | 系统错误 | InnoDB 内部异常 | 终止闪回, 释放所有资源, 返回严重错误 |
| **E4** | 不可恢复错误 | 数据损坏/断言失败 | 终止线程, 记录崩溃日志, 可能触发 server 重启 |

### 5.2 新增错误码

```
// sql/share/messages_to_clients.txt (新增)

ER_FLASHBACK_UNDO_PURGED 3801
  eng "Target time %s is beyond the undo log retention window. "
      "The oldest available undo timestamp is %s."

ER_FLASHBACK_DDL_CONFLICT 3802
  eng "Incompatible DDL operation detected on table '%s' after "
      "target time %s. Flashback is not safe."

ER_FLASHBACK_NO_PRIMARY_KEY 3803
  eng "Table '%s' has no PRIMARY KEY. Binlog-based flashback "
      "requires a primary key."

ER_FLASHBACK_ROW_IMAGE_INCOMPLETE 3804
  eng "binlog_row_image is set to '%s'. Binlog-based flashback "
      "requires binlog_row_image=FULL."

ER_FLASHBACK_LOCK_TIMEOUT 3805
  eng "Failed to acquire exclusive lock for flashback within "
      "%lu seconds. Another transaction may be accessing the table."

ER_FLASHBACK_MEMORY_LIMIT 3806
  eng "Flashback operation exceeded memory limit of %lu MB."

ER_FLASHBACK_NOT_ENABLED 3807
  eng "Flashback feature is not enabled. Set "
      "flashback_enabled=ON to enable."

ER_FLASHBACK_MISSING_HISTORY 3808
  eng "Historical version for row at position %s is not available. "
      "Undo log may have been purged."

ER_FLASHBACK_ENGINE_NONE 3809
  eng "No flashback engine available for target time %s. "
      "The data is beyond both undo and binlog retention windows."

ER_FLASHBACK_PRIVILEGE 3810
  eng "Access denied; you need the FLASHBACK privilege on '%s.%s' "
      "to perform this operation."
```

### 5.3 错误处理流程

```
错误发生
  │
  ├── E1 业务错误
  │   └─ my_error(ER_FLASHBACK_*, MYF(0))
  │       └─ return true (失败)
  │
  ├── E2 可恢复错误
  │   ├─ trans_rollback(thd)          ← 回滚闪回事
  │   ├─ DDLBarrier::release_lock()   ← 释放 MDL 锁
  │   ├─ FlashbackPurgeGuard 析构     ← 自动恢复 purge
  │   └─ my_error(ER_FLASHBACK_*, MYF(0))
  │       └─ return true
  │
  ├── E3 系统错误
  │   ├─ trans_rollback(thd)
  │   ├─ 释放所有 MDL 锁
  │   ├─ 恢复 purge (trx_purge_run)
  │   ├─ push_warning + my_error
  │   └─ return true
  │
  └── E4 不可恢复错误
      ├─ ut_error 或 ut_ad 失败
      └─ 触发 server 崩溃处理
```

---

## 6. 关键约束清单

### 约束定义

| 编号 | 约束描述 | 违反后果 | 缓解措施 |
|------|---------|---------|---------|
| **C1** | 闪回操作**绝不能写入 binlog** (防止复制环和嵌套) | 从库重复执行闪回, 数据二次损坏 | 闪回事务中 `thd->variables.sql_log_bin = 0` |
| **C2** | 闪回表执行期间**必须暂停 Undo Purge** | 闪回中途 undo 被清理, `DB_MISSING_HISTORY`, 部分执行无法回退 | `FlashbackPurgeGuard` RAII, 构造时 `trx_purge_stop()`, 析构时 `trx_purge_run()` |
| **C3** | 闪回表执行期间**必须获取 MDL_EXCLUSIVE 锁** | 并发 DDL 改变表结构, 闪回数据写入错误位置 | `DDLBarrier::acquire_exclusive_lock()` 在所有数据操作前执行 |
| **C4** | Binlog 引擎**仅在 binlog_row_image=FULL 时可用** | MINIMAL 模式下 before-image 不完整, 逆向 SQL 缺少列值 | `BinlogFlashbackEngine::check_row_image_compatibility()` 前置检查 |
| **C5** | Binlog 引擎**仅对有主键的表可用** | 无主键表无法精准定位行, 逆向 SQL 无法精确匹配 | 闪回前检查 `table->s->primary_key < MAX_KEY` |
| **C6** | `AS OF TIMESTAMP` 查询**必须与当前 SELECT 相同权限** | 特权提升: 通过历史查询获取已删除的敏感数据 | 复用现有 `check_access()` 权限检查逻辑 |
| **C7** | 闪回操作**必须支持 DRY RUN 模式** | 用户无法预览影响范围, 误操作风险高 | `FlashbackRequest::dry_run` 标志, 仅统计不修改 |
| **C8** | 单次闪回**不得处理超过 flashback_max_rows 行** | 大表闪回产生巨量 redo, 影响正常业务 | 计数器检查, 超限时返回 `ER_FLASHBACK_MEMORY_LIMIT` |
| **C9** | 闪回查询**不阻塞并发写入** | 闪回查询是纯读操作, 不应影响业务 | 使用 MVCC 一致性读, 不加锁 |
| **C10** | 所有闪回操作**必须写入审计日志** (若 audit_log 插件启用) | 无法追溯闪回操作, 违反合规要求 | 在结果返回前调用 `audit_log_notify()` |

---

## 7. 系统变量

### 7.1 新增变量清单

| 变量名 | 类型 | 默认值 | 范围 | 描述 |
|--------|------|--------|------|------|
| `flashback_enabled` | BOOL | ON | ON/OFF | 闪回功能总开关 |
| `innodb_flashback_retention_seconds` | ULONG | 900 | 60-86400 | Undo 闪回窗口(秒) |
| `flashback_lock_wait_timeout` | ULONG | 300 | 1-3600 | MDL 锁等待超时(秒) |
| `flashback_max_rows` | ULONGLONG | 10000000 | 1-4294967295 | 单次闪回最大行数 |
| `flashback_redo_throttle_ms` | ULONG | 10 | 0-1000 | Redo 写入节流间隔(ms), 0=不禁流 |
| `flashback_require_full_row_image` | BOOL | ON | ON/OFF | 强制 Binlog 引擎检查 row_image |
| `flashback_audit_log` | BOOL | ON | ON/OFF | 是否写入审计日志 |

### 7.2 变量注册

```cpp
// sql/flashback_sysvars.cc

static MYSQL_SYSVAR_BOOL(enabled,
    flashback_enabled,
    PLUGIN_VAR_OPCMDARG,
    "Enable or disable the flashback feature globally",
    nullptr, nullptr, true);

static MYSQL_SYSVAR_ULONG(retention_seconds,
    innodb_flashback_retention_seconds,
    PLUGIN_VAR_OPCMDARG,
    "Maximum seconds to retain undo log for flashback queries",
    nullptr, nullptr,
    900,    /* default: 15 minutes */
    60,     /* min: 1 minute */
    86400,  /* max: 24 hours */
    0);

static MYSQL_SYSVAR_ULONG(lock_wait_timeout,
    flashback_lock_wait_timeout,
    PLUGIN_VAR_OPCMDARG,
    "Timeout in seconds for acquiring flashback exclusive lock",
    nullptr, nullptr,
    300,    /* default: 5 minutes */
    1,      /* min */
    3600,   /* max: 1 hour */
    0);

static MYSQL_SYSVAR_ULONGLONG(max_rows,
    flashback_max_rows,
    PLUGIN_VAR_OPCMDARG,
    "Maximum rows to process in a single flashback operation",
    nullptr, nullptr,
    10000000,  /* default: 10 million */
    1,         /* min */
    4294967295ULL /* max */
    0);
```

---

## 8. SQL 语法扩展

### 8.1 新增关键字 (sql_yacc.yy)

```yacc
%token<lexer.keyword> FLASHBACK_SYM  901
%token<lexer.keyword> VERSIONS_SYM   902
%token<lexer.keyword> TRX_ID_SYM     903
%token<lexer.keyword> DRY_SYM        904
%token<lexer.keyword> RUN_SYM        905
```

### 8.2 新增语句规则

```yacc
/* 在 statement 规则中新增 */
statement:
    ...
    | flashback_statement
    ...

flashback_statement:
    FLASHBACK_SYM TABLE_SYM table_list
      TO_SYM TIMESTAMP_SYM datetime
      opt_dry_run
      {
        Lex->sql_command = SQLCOM_FLASHBACK_TABLE;
        Lex->flashback_tables = $3;
        Lex->flashback_timestamp = $5;
        Lex->flashback_dry_run = $6;
      }
    | FLASHBACK_SYM TRANSACTION_SYM ulonglong_num
      {
        Lex->sql_command = SQLCOM_FLASHBACK_TRANSACTION;
        Lex->flashback_trx_id = $3;
      }

opt_dry_run:
    /* empty */     { $$ = false; }
    | DRY_SYM RUN_SYM { $$ = true; }

/* 扩展 table_factor 支持 AS OF TIMESTAMP */
table_factor:
    table_name opt_alias
    | table_name AS_SYM OF_SYM TIMESTAMP_SYM datetime
      {
        Lex->flashback_query = true;
        Lex->flashback_timestamp = $5;
        /* 标记该表为闪回查询模式 */
      }

/* 扩展 table_factor 支持 VERSIONS BETWEEN */
table_factor:
    ...
    | table_name VERSIONS_SYM BETWEEN_SYM TIMESTAMP_SYM
      datetime AND_SYM datetime
      {
        Lex->flashback_versions = true;
        Lex->flashback_start_time = $5;
        Lex->flashback_end_time = $7;
      }
```

### 8.3 LEX 扩展 (sql/sql_lex.h)

```cpp
struct LEX {
    // ... 现有字段 ...

    /* === Flashback 扩展 === */
    bool flashback_query = false;            /* SELECT ... AS OF TIMESTAMP */
    bool flashback_versions = false;         /* SELECT ... VERSIONS BETWEEN */
    my_time_t flashback_timestamp = 0;       /* 闪回目标时间戳 */
    my_time_t flashback_start_time = 0;      /* 版本查询开始 */
    my_time_t flashback_end_time = 0;        /* 版本查询结束 */
    trx_id_t flashback_trx_id = 0;           /* 按事务号闪回 */
    bool flashback_dry_run = false;          /* DRY RUN 模式 */
    List<LEX_CSTRING> flashback_tables;      /* 闪回表列表 */
};
```

---

## 9. 测试策略

### 9.1 测试分层

| 层次 | 测试类型 | 工具 | 覆盖范围 |
|------|---------|------|---------|
| L1 | 单元测试 | gtest | `row_build_flashback_version()`, `DDLBarrier::check()` |
| L2 | 集成测试 | MTR (MySQL Test Run) | 闪回查询、闪回表、DRY RUN、DDL 屏障 |
| L3 | 竞态测试 | MTR + 自定义注入 | Purge 线程并发、DDL 并发 |
| L4 | 性能测试 | sysbench 定制 | 大表(1000万行)闪回耗时 |
| L5 | 兼容测试 | MTR 参数化 | 不同 `binlog_row_image` 配置 |

### 9.2 关键测试用例

| 用例 ID | 描述 | 输入 | 预期输出 |
|---------|------|------|---------|
| TC-001 | 基础闪回查询 | `SELECT * FROM t AS OF TIMESTAMP '...'` | 返回目标时间点的行数据 |
| TC-002 | DRY RUN 闪回表 | `FLASHBACK TABLE t TO TIMESTAMP '...' DRY RUN` | 输出将恢复的行数, 不修改数据 |
| TC-003 | 闪回表实际执行 | `FLASHBACK TABLE t TO TIMESTAMP '...'` | 数据恢复到目标时间点 |
| TC-004 | Undo 过期错误 | 目标时间 < 最早 undo 时间 | 返回 ER_FLASHBACK_UNDO_PURGED |
| TC-005 | DDL 屏障冲突 | 闪回前对表执行 ALTER TABLE | 返回 ER_FLASHBACK_DDL_CONFLICT |
| TC-006 | 无主键 Binlog 闪回 | 无 PK 表 + Binlog 引擎 | 返回 ER_FLASHBACK_NO_PRIMARY_KEY |
| TC-007 | row_image 不兼容 | `binlog_row_image=MINIMAL` + Binlog 引擎 | 返回 ER_FLASHBACK_ROW_IMAGE_INCOMPLETE |
| TC-008 | Purge 竞态保护 | 闪回进行时 purge 线程运行 | 闪回成功, purge 在闪回完成后恢复 |
| TC-009 | 并发闪回同一表 | 两个会话同时闪回同一表 | 一个成功, 另一个获取锁超时 |
| TC-010 | 闪回后再次闪回 | 闪回 → 立即再次闪回 | 第二次闪回正常执行 |

---

## 10. 四阶段实施计划

### Phase 1: SQL 语法 + Undo 闪回查询原型 (2-4 周)

**交付物**:
- [ ] `include/my_sqlcommand.h`: 新增 `SQLCOM_FLASHBACK_TABLE/QUERY/VERSIONS/TRANSACTION`
- [ ] `sql/sql_yacc.yy`: FLASHBACK 语法解析 (~250 行)
- [ ] `sql/sql_lex.h`: LEX 扩展
- [ ] `sql/parse_tree_nodes.h`: PT_flashback_table 等语法树节点
- [ ] `storage/innobase/row/row0vers.cc`: `row_build_flashback_version()`
- [ ] `storage/innobase/trx/trx0sys.cc`: 时间戳↔trx_id 映射
- [ ] `storage/innobase/handler/ha_innodb.cc`: `rnd_next_flashback()`
- [ ] `FlashbackPurgeGuard`: RAII purge 保护器 (C2 约束)
- [ ] 系统变量: `flashback_enabled`, `innodb_flashback_retention_seconds`
- [ ] 错误码: ER_FLASHBACK_UNDO_PURGED, ER_FLASHBACK_MISSING_HISTORY
- [ ] MTR 测试: TC-001, TC-004

**验收标准**:
```sql
SELECT * FROM employees AS OF TIMESTAMP '2025-07-28 10:30:00';
-- 返回历史数据 或 ER_FLASHBACK_UNDO_PURGED
```

### Phase 2: Undo 闪回表 + Binlog 引擎 (4-8 周)

**交付物**:
- [ ] `sql/sql_flashback.cc`: 命令入口总控
- [ ] `sql/flashback_scheduler.cc`: 引擎选择器
- [ ] `sql/flashback_binlog_engine.cc`: Binlog 逆向引擎
- [ ] `sql/flashback_ddl_barrier.cc`: DDL 屏障 (C3 约束)
- [ ] `storage/innobase/flashback/flashback_undo_engine.cc`: Undo 闪回表
- [ ] Binlog 引擎: `binlog_row_image` 检查 (C4), 主键检查 (C5)
- [ ] 闪回操作 `sql_log_bin=OFF` (C1)
- [ ] DRY RUN 模式完整实现 (C7)
- [ ] MTR 测试: TC-002, TC-003, TC-005, TC-006, TC-007

### Phase 3: 闪回版本查询 + 事务闪回 + 监控 (4-8 周)

**交付物**:
- [ ] `sql/sql_flashback_versions.cc`: VERSIONS BETWEEN 执行器
- [ ] `sql/sql_flashback_transaction.cc`: 事务级闪回
- [ ] `performance_schema.flashback_status` 表
- [ ] `SHOW GLOBAL STATUS` 新增闪回指标
- [ ] 审计日志集成 (C10)
- [ ] MTR 测试: TC-008, TC-009, TC-010

### Phase 4: 多表一致性 + 从库闪回 + 优化 (持续)

**交付物**:
- [ ] 多表原子闪回 (单事务内)
- [ ] 从库闪回支持
- [ ] Redo 写入节流 (flashback_redo_throttle_ms)
- [ ] 分批提交 (防止 undo 膨胀)
- [ ] 性能优化: 并行闪回, Buffer Pool LRU 标记
- [ ] GDPR "不可闪回标记" 机制

---

## 11. 与 Oracle Flashback 对比

| 能力 | Oracle | 本方案 (Phase 1) | 本方案 (Phase 2+) |
|------|--------|-----------------|------------------|
| 闪回查询 (AS OF) | ✅ | ✅ | ✅ |
| 闪回表 (TO TIMESTAMP) | ✅ | ❌ | ✅ |
| 闪回版本查询 (VERSIONS BETWEEN) | ✅ | ❌ | ✅ (Phase 3) |
| 闪回事务 | ✅ | ❌ | ✅ (Phase 3) |
| DRY RUN 预览 | ⚠️ (需手动) | ✅ | ✅ |
| Undo Purge 保护 | ✅ (Guaranteed Undo) | ✅ (trx_purge_stop) | ✅ |
| DDL 屏障 | ✅ (Flashback Archive) | ❌ | ✅ (MDL lock) |
| Binlog 长窗口 | ❌ (纯 Undo) | ❌ | ✅ |
| 闪回查询不阻塞写入 | ✅ | ✅ (MVCC) | ✅ |

---

## 12. 构建与编译

### 12.1 CMake 修改

```cmake
# sql/CMakeLists.txt 新增
set(FLASHBACK_SOURCES
    flashback_types.cc
    flashback_scheduler.cc
    flashback_binlog_engine.cc
    flashback_ddl_barrier.cc
    flashback_sysvars.cc
    sql_flashback.cc
)
list(APPEND MYSQLD_SOURCE ${FLASHBACK_SOURCES})

# storage/innobase/CMakeLists.txt 新增
set(FLASHBACK_INNODB_SOURCES
    flashback/flashback_undo_engine.cc
    flashback/flashback_purge_guard.cc
)
list(APPEND innobase_sources ${FLASHBACK_INNODB_SOURCES})
```

### 12.2 编译命令

```bash
cd /home/victor/base/git/others/percona-server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_DEBUG=1
make -j$(nproc) mysqld
```

---

## 13. 设计验收检查清单

### 13.1 模块接口完整性

- [x] `IFlashbackEngine` 接口: 5 个纯虚方法, 职责单一
- [x] `FlashbackScheduler`: 引擎选择 + 安全检测链
- [x] `UndoFlashbackEngine`: 3 种操作类型 + DRY RUN
- [x] `BinlogFlashbackEngine`: 3 种 event 逆向 + 兼容性检查
- [x] `FlashbackPurgeGuard`: RAII 风格, 禁止拷贝
- [x] `DDLBarrier`: 静态方法, 无状态
- [x] `row_build_flashback_version()`: InnoDB 层入口, 返回 `dberr_t`

### 13.2 依赖方向

- [x] SQL 层 → InnoDB 层 (单向)
- [x] InnoDB 层不依赖 SQL 层类型
- [x] `flashback_types.h` 为共享类型定义, 无依赖
- [x] 无循环依赖

### 13.3 文件结构

- [x] 所有新增文件有明确位置
- [x] 修改文件已标注
- [x] 测试文件目录结构完整
- [x] CMake 构建配置已规划

### 13.4 约束覆盖

- [x] C1: sql_log_bin=OFF → BinlogFlashbackEngine::execute() 中实现
- [x] C2: trx_purge_stop → FlashbackPurgeGuard RAII
- [x] C3: MDL_EXCLUSIVE → DDLBarrier::acquire_exclusive_lock()
- [x] C4: binlog_row_image=FULL → check_row_image_compatibility()
- [x] C5: 主键检查 → supports() 方法中实现
- [x] C6: 权限检查 → Sql_cmd_flashback::execute() 中复用 check_access()
- [x] C7: DRY RUN → FlashbackRequest::dry_run 标志
- [x] C8: 最大行数 → 计数器 + ER_FLASHBACK_MEMORY_LIMIT
- [x] C9: 不阻塞写入 → MVCC 一致性读, 不加锁
- [x] C10: 审计日志 → audit_log_notify() 集成

---

## 附录 A: 关键源码行号索引

| 函数/结构 | 文件 | 行号 | 用途 |
|-----------|------|------|------|
| `row_vers_build_for_consistent_read()` | `row0vers.cc` | 1255 | 版本链构建 (复用) |
| `row_vers_build_for_consistent_read()` | `row0vers.h` | 117 | 声明 |
| `trx_purge_stop()` | `trx0purge.cc` | 2526 | 暂停 purge |
| `trx_purge_run()` | `trx0purge.cc` | 2585 | 恢复 purge |
| `trx_purge_stop()` | `trx0purge.h` | 96 | 声明 |
| `trx_purge_run()` | `trx0purge.h` | 98 | 声明 |
| `SQLCOM_CLONE` | `my_sqlcommand.h` | 200 | 参考命令位置 |
| `DB_MISSING_HISTORY` | `db0err.h` | 59 | 历史不可用错误码 |
| `sql_yacc.yy` | `sql/sql_yacc.yy` | 18574 行 | 语法文件总行数 |

## 附录 B: 风险缓解矩阵

| 风险 | 等级 | 缓解方案 | 实施阶段 |
|------|------|---------|---------|
| R1: Undo Purge 竞态 | 🔴 25/25 | FlashbackPurgeGuard RAII (C2) | Phase 1 |
| R2: DDL 并发 | 🔴 20/25 | DDLBarrier MDL 锁 (C3) | Phase 1 |
| R3: row_image 不完整 | 🔴 20/25 | check_row_image_compatibility (C4) | Phase 2 |
| R4: 无主键表 | 🟡 16/25 | 主键检查 (C5) | Phase 2 |
| R6: Binlog 嵌套 | 🟡 16/25 | sql_log_bin=OFF (C1) | Phase 2 |
| R10: GTID 冲突 | 🟡 12/25 | 闪回不分配 GTID | Phase 3 |
