# INSERT 执行与提交过程中事务状态、Undo、Redo、Binlog 的深度剖析

## 备选标题

1. **深入MySQL内核：一条INSERT语句背后的事务日志魔法**
2. **从零到精通：INSERT操作中Undo/Redo/Binlog的完整生命周期**
3. **数据库专家必读：揭秘INSERT执行与提交的底层机制**

---

## 一、开篇引子

> 想象你往银行账户存钱，银行不仅要记录这笔存款（Redo），还要准备好"撤销凭证"以防万一需要退款（Undo），同时还要在对账单上留下记录（Binlog）。MySQL的INSERT操作正是这样一个精密的"三重保险"机制。

当我们执行一条简单的 `INSERT INTO users VALUES (1, 'Alice')` 时，MySQL内部究竟发生了什么？这条语句从执行到提交，事务状态如何流转？Undo、Redo、Binlog这三种日志又是如何协同工作的？

本文基于 **Percona Server 8.4.3** 源码，深入剖析INSERT操作的完整生命周期，带你揭开MySQL事务机制的神秘面纱。

---

## 二、场景展示

### 2.1 一条INSERT的执行流程概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        INSERT 执行与提交完整流程                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │ 协议解析 │───▶│ SQL解析  │───▶│ 执行引擎 │───▶│ InnoDB层 │              │
│  │COM_QUERY │    │          │    │          │    │          │              │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘              │
│       │               │               │               │                     │
│       ▼               ▼               ▼               ▼                     │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐              │
│  │read_packet│   │parse_sql │    │write_record│  │row_insert│              │
│  │          │    │          │    │          │    │          │              │
│  └──────────┘    └──────────┘    └──────────┘    └──────────┘              │
│                                       │               │                     │
│                        ┌──────────────┼───────────────┤                     │
│                        ▼              ▼               ▼                     │
│                   ┌────────┐    ┌────────┐     ┌────────┐                   │
│                   │  Undo  │    │  Redo  │     │ Binlog │                   │
│                   │  Log   │    │  Log   │     │ Cache  │                   │
│                   └────────┘    └────────┘     └────────┘                   │
│                                                    │                        │
│                        ┌───────────────────────────┘                        │
│                        ▼                                                    │
│                   ┌────────┐    ┌────────┐    ┌────────┐                    │
│                   │ Group  │───▶│ Binlog │───▶│ Engine │                    │
│                   │ Commit │    │ Sync   │    │ Commit │                    │
│                   └────────┘    └────────┘    └────────┘                    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 事务状态变迁图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          事务状态流转图                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│    ┌───────────────┐                                                        │
│    │TRX_STATE_     │                                                        │
│    │NOT_STARTED    │ ◀─── 事务初始状态                                       │
│    └───────┬───────┘      trx->state = TRX_STATE_NOT_STARTED               │
│            │ trx_start_low() 被调用                                         │
│            ▼                                                                │
│    ┌───────────────┐                                                        │
│    │TRX_STATE_     │      关键字段变化:                                      │
│    │ACTIVE         │ ◀─── trx->id = 分配事务ID                              │
│    └───────┬───────┘      trx->no = TRX_ID_MAX                             │
│            │              trx->undo_no = 0                                  │
│            │ trx_prepare() 被调用                                           │
│            ▼                                                                │
│    ┌───────────────┐                                                        │
│    │TRX_STATE_     │      关键字段变化:                                      │
│    │PREPARED       │ ◀─── trx->state = TRX_STATE_PREPARED                  │
│    └───────┬───────┘      undo->state = TRX_UNDO_PREPARED                  │
│            │ trx_commit() 被调用                                            │
│            ▼                                                                │
│    ┌───────────────┐                                                        │
│    │TRX_STATE_     │      关键字段变化:                                      │
│    │COMMITTED_IN   │ ◀─── trx->state = TRX_STATE_COMMITTED_IN_MEMORY       │
│    │MEMORY         │      trx->no = trx_sys_get_new_trx_no()               │
│    └───────────────┘                                                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 事务内存关键字段说明

| 字段名 | 类型 | 含义 | 变化时机 |
|:------|:-----|:-----|:--------|
| **trx->id** | trx_id_t | 事务ID | trx_start_low()时分配 |
| **trx->no** | trx_id_t | 事务提交序号 | ACTIVE时为TRX_ID_MAX，提交时分配 |
| **trx->state** | trx_state_t | 事务状态 | 状态流转时改变 |
| **trx->undo_no** | undo_no_t | 事务内操作序号 | 每次DML递增 |
| **trx->read_only** | bool | 是否只读事务 | trx_start_low()时设置 |
| **trx->auto_commit** | bool | 是否自动提交 | trx_start_low()时设置 |
| **trx->rsegs.m_redo.rseg** | trx_rseg_t* | Redo回滚段 | 首次写入时分配 |
| **trx->rsegs.m_redo.insert_undo** | trx_undo_t* | INSERT Undo段 | 首次INSERT时分配 |

---

## 三、原理深入

### 3.1 三种日志的角色定位

| 日志类型 | 作用 | 写入时机 | 比喻 |
|:--------|:-----|:--------|:-----|
| **Undo Log** | 记录数据修改前的状态，用于回滚和MVCC | INSERT执行时 | "后悔药"——让你能撤销操作 |
| **Redo Log** | 记录数据页的物理修改，用于崩溃恢复 | INSERT执行时(先写Buffer) | "施工日志"——记录每一步修改 |
| **Binlog** | 记录逻辑操作，用于主从复制 | COMMIT时 | "对账单"——给其他人看的操作记录 |

### 3.2 INSERT操作的Undo Log

对于INSERT操作，Undo Log只需记录**主键信息**即可。因为回滚时只需要知道删除哪条记录：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     INSERT Undo Record 结构                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────┬──────────┬──────────┬──────────┬──────────┬────────────────┐ │
│  │Next Ptr  │Record    │Undo No   │Table ID  │主键字段  │Virtual Col     │ │
│  │(2 bytes) │Type      │          │          │Values    │Info (可选)     │ │
│  │          │(1 byte)  │(压缩)    │(压缩)    │          │                │ │
│  └──────────┴──────────┴──────────┴──────────┴──────────┴────────────────┘ │
│                                                                             │
│  Record Type = TRX_UNDO_INSERT_REC (值为11)                                 │
│                                                                             │
│  【Undo Log样例】INSERT INTO t(id, name) VALUES(100, 'test')               │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ next_ptr: 0x0120 | type: 0x0B | undo_no: 1 | table_id: 1234 |       │  │
│  │ pk_len: 4 | pk_value: 100                                            │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.3 INSERT操作的Redo Log

Redo Log记录的是**物理页面的修改**，包括：

1. **数据页修改**：记录在哪个页面、哪个位置插入了什么数据
2. **Undo页修改**：Undo Log本身的写入也需要Redo保护
3. **系统页修改**：如回滚段头页等

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MTR (Mini-Transaction) 工作原理                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                     一次INSERT的MTR组成                              │   │
│   ├─────────────────────────────────────────────────────────────────────┤   │
│   │                                                                     │   │
│   │  MTR Begin                                                          │   │
│   │      │                                                              │   │
│   │      ├──▶ MLOG_UNDO_INSERT (Undo页修改)                             │   │
│   │      │    space_id=undo_space, page_no=undo_page                    │   │
│   │      │                                                              │   │
│   │      ├──▶ MLOG_REC_INSERT (数据页修改)                              │   │
│   │      │    space_id=table_space, page_no=leaf_page                   │   │
│   │      │                                                              │   │
│   │      ├──▶ MLOG_COMP_PAGE_REORGANIZE (页面重组，如需要)              │   │
│   │      │                                                              │   │
│   │  MTR Commit ──▶ 写入Redo Log Buffer                                │   │
│   │                                                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  【Redo Log样例】                                                           │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ LSN: 123456789 | Type: MLOG_REC_INSERT | Space: 5 | Page: 100       │  │
│  │ Index: PRIMARY | Record: [100, 'test'] | Info_bits: 0x00           │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.4 Binlog事件类型与结构

对于Row格式的INSERT操作，会产生以下Binlog Event：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     INSERT的Binlog Event序列                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   事务开始                                                                   │
│   ┌────────────────────┐                                                    │
│   │ GTID_LOG_EVENT     │ ◀── GTID信息 (如果开启GTID)                        │
│   │ gtid: uuid:N       │     sql/binlog.cc:9460 生成                       │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │ QUERY_EVENT        │ ◀── "BEGIN" 事务开始标记                           │
│   │ query: "BEGIN"     │                                                    │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │TABLE_MAP_EVENT     │ ◀── 表结构映射                                     │
│   │ table_id: 123      │     sql/binlog.cc:10005 THD::binlog_write_table_map│
│   │ db: test           │                                                    │
│   │ table: users       │                                                    │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │WRITE_ROWS_EVENT    │ ◀── 行数据                                         │
│   │ table_id: 123      │     sql/binlog.cc:11581 THD::binlog_write_row     │
│   │ rows: [(1,'Alice')]│                                                    │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │ XID_EVENT          │ ◀── 事务提交                                       │
│   │ xid: 12345         │     sql/binlog.cc:9530 写入                       │
│   └────────────────────┘                                                    │
│                                                                             │
│  【Binlog样例】mysqlbinlog输出:                                             │
│  # at 1234                                                                  │
│  #240101 12:00:00 server id 1  end_log_pos 1400  GTID last_committed=10    │
│  #  sequence_number=11                                                      │
│  SET @@SESSION.GTID_NEXT= 'uuid:100';                                      │
│  # at 1400                                                                  │
│  BEGIN                                                                      │
│  # at 1450                                                                  │
│  # TABLE_MAP_EVENT (table_id: 123, db: test, table: users)                 │
│  # at 1520                                                                  │
│  # WRITE_ROWS_EVENT (table_id: 123)                                        │
│  ### INSERT INTO test.users VALUES (100, 'test')                           │
│  # at 1600                                                                  │
│  COMMIT;                                                                    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.5 Binlog与事务提交

Binlog在**事务提交时**才写入，记录的是**行格式(Row-based)**或**语句格式(Statement-based)**的逻辑操作：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     事务提交时的日志写入顺序                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐           │
│  │  1.     │  │  2.     │  │  3.     │  │  4.     │  │  5.     │           │
│  │ InnoDB  │─▶│ Binlog  │─▶│ Binlog  │─▶│ InnoDB  │─▶│ 释放    │           │
│  │Prepare  │  │ Write   │  │ Sync    │  │ Commit  │  │ 锁资源  │           │
│  │         │  │(到cache)│  │(到磁盘) │  │         │  │         │           │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘  └─────────┘           │
│       │            │            │            │                              │
│       ▼            ▼            ▼            ▼                              │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐                        │
│  │Undo状态 │  │写入GTID │  │刷盘保证 │  │释放Undo │                        │
│  │PREPARED │  │+INSERT  │  │持久化   │  │事务完成 │                        │
│  │         │  │Event    │  │         │  │         │                        │
│  └─────────┘  └─────────┘  └─────────┘  └─────────┘                        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、源码根因揭秘

**源码版本：Percona Server 8.4.3-3**

### 4.1 完整函数调用链（从协议解析开始）

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              INSERT 完整执行函数调用链（协议层 → 存储引擎层）                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【阶段1：MySQL协议解析】                                                    │
│  Protocol_classic::read_packet()           sql/protocol_classic.cc:1408    │
│  └── Protocol_classic::get_command()       sql/protocol_classic.cc:2887    │
│      └── parse_packet() for COM_QUERY      sql/protocol_classic.cc:2834    │
│          │  提取SQL文本: "INSERT INTO t VALUES(...)"                        │
│          │  字段变化: com_data->com_query.query = SQL文本                   │
│          │                                                                  │
│  【阶段2：命令分发】                                                         │
│  do_command()                              sql/sql_parse.cc:1374           │
│  └── dispatch_command()                    sql/sql_parse.cc:1815           │
│      │  case COM_QUERY:                    sql/sql_parse.cc:2172           │
│      └── dispatch_sql_command()            sql/sql_parse.cc:5521           │
│          │  字段变化: thd->query() = SQL文本                                │
│          │                                                                  │
│  【阶段3：SQL解析与执行】                                                    │
│  parse_sql()                               sql/sql_parse.cc:5555           │
│  └── mysql_execute_command()               sql/sql_parse.cc:3068           │
│      └── Sql_cmd_insert::execute()         sql/sql_insert.cc:611           │
│          │  case SQLCOM_INSERT:                                             │
│          └── Sql_cmd_dml::execute_inner()  sql/sql_select.cc               │
│              └── write_record()            sql/sql_insert.cc:1800          │
│                                                                             │
│  【阶段4：存储引擎接口】                                                     │
│  write_record()                            sql/sql_insert.cc:1800          │
│  └── handler::ha_write_row()               sql/handler.cc:8200             │
│      │  字段变化: thd->binlog_row_event_extra_data 设置                     │
│      └── ha_innobase::write_row()          storage/innobase/handler/       │
│          │                                 ha_innodb.cc:8500               │
│          │  字段变化: prebuilt->autoinc_last_value 更新                     │
│          │                                                                  │
│  【阶段5：InnoDB行插入】                                                     │
│  row_insert_for_mysql()                    storage/innobase/row/           │
│  │                                         row0mysql.cc:1883               │
│  │  字段变化: trx->undo_no++ (每次DML递增)                                  │
│  │                                                                          │
│  └── row_ins_clust_index_entry()           storage/innobase/row/           │
│      │                                     row0ins.cc:3088                 │
│      │                                                                      │
│      ├── 【Undo Log生成】                                                   │
│      │   trx_undo_report_row_operation()   storage/innobase/trx/           │
│      │   │                                 trx0rec.cc:2146                 │
│      │   │  字段变化:                                                       │
│      │   │    - trx->rsegs.m_redo.insert_undo 分配(首次)                    │
│      │   │    - undo_ptr->insert_undo->top_page_no 更新                     │
│      │   │                                                                  │
│      │   └── trx_undo_page_report_insert() storage/innobase/trx/           │
│      │                                     trx0rec.cc:483                  │
│      │                                                                      │
│      └── 【数据页插入+Redo Log】                                            │
│          btr_cur_optimistic_insert()       storage/innobase/btr/           │
│          │                                 btr0cur.cc:2500                 │
│          └── page_cur_insert_rec()         storage/innobase/page/          │
│              │                             page0cur.cc:1500                │
│              └── page_cur_insert_rec_write_log()                           │
│                                            storage/innobase/page/          │
│                                            page0cur.cc:853                 │
│                                                                             │
│  【阶段6：Binlog缓存写入】(DML执行时)                                        │
│  handler::ha_write_row()                   sql/handler.cc:8200             │
│  └── binlog_log_row()                      sql/handler.cc:8000             │
│      ├── THD::binlog_write_table_map()     sql/binlog.cc:10005             │
│      │   └── Table_map_log_event构造       sql/log_event.cc:10696          │
│      │                                                                      │
│      └── THD::binlog_write_row()           sql/binlog.cc:11578             │
│          └── binlog_prepare_pending_rows_event<Write_rows_log_event>       │
│                                            sql/binlog.cc:11581             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 自动提交触发的函数调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              COMMIT 自动提交函数调用链（autocommit=1 场景）                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【触发点：语句执行完成后】                                                   │
│  mysql_execute_command()                   sql/sql_parse.cc:3068           │
│  └── ... INSERT执行完成 ...                                                 │
│                                                                             │
│  dispatch_sql_command()                    sql/sql_parse.cc:5521           │
│  └── trans_commit_stmt()                   sql/sql_parse.cc:5202           │
│      │  // 语句级提交，autocommit时触发完整提交                              │
│      │                                     sql/transaction.cc:513          │
│      │  字段变化:                                                           │
│      │    - thd->transaction->is_active(STMT) 检查                         │
│      │                                                                      │
│  【进入提交流程】                                                            │
│  ha_commit_trans()                         sql/handler.cc:1663             │
│  │  字段变化:                                                               │
│  │    - thd->durability_property 检查                                       │
│  │                                                                          │
│  └── MYSQL_BIN_LOG::commit()               sql/binlog.cc:8423              │
│      │  // Binlog Group Commit入口                                          │
│      │                                                                      │
│      └── MYSQL_BIN_LOG::ordered_commit()   sql/binlog.cc:9234              │
│          │                                                                  │
│          ├── 【Stage 0】Slave Commit Order                                  │
│          │   Commit_stage_manager::          sql/rpl_commit_stage_         │
│          │     wait_for_ticket_turn()        manager.cc:168                │
│          │                                                                  │
│          ├── 【Stage 1】FLUSH阶段                                          │
│          │   change_stage(BINLOG_FLUSH_STAGE)                              │
│          │   └── process_flush_stage_queue()                               │
│          │       │  字段变化:                                               │
│          │       │    - thd->commit_error 设置                              │
│          │       │    - flush_error 记录                                    │
│          │       │                                                          │
│          │       ├── ha_flush_logs()       // 刷引擎日志                    │
│          │       ├── assign_automatic_gtids_to_flush_group()               │
│          │       │   字段变化: thd->owned_gtid 分配                         │
│          │       └── flush_thread_caches() // 写Binlog cache               │
│          │                                                                  │
│          ├── 【Stage 2】SYNC阶段                                           │
│          │   change_stage(SYNC_STAGE)                                      │
│          │   └── sync_binlog_file()        // fsync Binlog                 │
│          │                                                                  │
│          └── 【Stage 3】COMMIT阶段                                         │
│              change_stage(COMMIT_STAGE)                                    │
│              └── process_commit_stage_queue()                              │
│                  └── ha_commit_low()       sql/handler.cc:1938             │
│                      │  字段变化:                                           │
│                      │    - trn_ctx->ha_trx_info 遍历提交                   │
│                      │                                                      │
│                      └── innobase_commit()  storage/innobase/handler/      │
│                          │                  ha_innodb.cc:4600              │
│                          └── trx_commit_for_mysql()                        │
│                              │              storage/innobase/trx/          │
│                              │              trx0trx.cc:2517                │
│                              │  字段变化:                                   │
│                              │    - trx->state → TRX_STATE_COMMITTED       │
│                              │    - trx->no = 分配提交序号                  │
│                              │                                              │
│                              └── trx_commit()                              │
│                                             storage/innobase/trx/          │
│                                             trx0trx.cc:2300                │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 Undo Log生成核心代码

```c
// storage/innobase/trx/trx0rec.cc:483-550
/** Reports in the undo log of an insert of a clustered index record.
 @return offset of the inserted entry on the page if succeed, 0 if fail */
static ulint trx_undo_page_report_insert(
    page_t *undo_page,           /*!< in: undo log page */
    trx_t *trx,                  /*!< in: transaction */
    dict_index_t *index,         /*!< in: clustered index */
    const dtuple_t *clust_entry, /*!< in: index entry which will be
                                 inserted to the clustered index */
    mtr_t *mtr)                  /*!< in: mtr */
{
  ulint first_free;
  byte *ptr;
  ulint i;

  // 确认是聚簇索引
  ut_ad(index->is_clustered());
  // 确认是INSERT类型的Undo页
  ut_ad(mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) ==
        TRX_UNDO_INSERT);

  // 获取当前Undo页的空闲位置
  first_free =
      mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_FREE);
  ptr = undo_page + first_free;

  // 预留2字节给下一条记录的指针
  ptr += 2;

  // 写入记录类型：TRX_UNDO_INSERT_REC
  *ptr++ = TRX_UNDO_INSERT_REC;
  
  // 写入undo_no（事务内的操作序号）
  // 【关键字段】trx->undo_no 在此使用并递增
  ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);
  
  // 写入table_id
  ptr += mach_u64_write_much_compressed(ptr, index->table->id);

  // 【关键】只存储主键字段，用于回滚时删除记录
  for (i = 0; i < dict_index_get_n_unique(index); i++) {
    const dfield_t *field = dtuple_get_nth_field(clust_entry, i);
    ulint flen = dfield_get_len(field);

    ptr += mach_write_compressed(ptr, flen);

    if (flen != UNIV_SQL_NULL && flen != 0) {
      ut_memcpy(ptr, dfield_get_data(field), flen);
      ptr += flen;
    }
  }

  // 处理虚拟列（如果有）
  if (index->table->n_v_cols) {
    if (!trx_undo_report_insert_virtual(undo_page, index->table, 
                                        clust_entry, &ptr)) {
      return (0);
    }
  }

  return (trx_undo_page_set_next_prev_and_add(undo_page, ptr, mtr));
}
```

### 4.4 事务启动核心代码（关键字段初始化）

```c
// storage/innobase/trx/trx0trx.cc:1338-1410
/** Starts a transaction. */
static void trx_start_low(
    trx_t *trx,      /*!< in: transaction */
    bool read_write) /*!< in: true if read-write transaction */
{
  ut_ad(!trx->in_rollback);
  ut_ad(!trx->is_recovered);
  ut_ad(trx_state_eq(trx, TRX_STATE_NOT_STARTED));

  // 【关键】版本号递增
  ++trx->version;

  // 【关键】检测是否自动提交
  trx->auto_commit = (trx->api_trx && trx->api_auto_commit) ||
                     thd_trx_is_auto_commit(trx->mysql_thd);

  // 【关键】检测是否只读事务
  trx->read_only = (trx->api_trx && !trx->read_write) ||
                   (!trx->internal && thd_trx_is_read_only(trx->mysql_thd)) ||
                   srv_read_only_mode;

  if (!trx->auto_commit) {
    ++trx->will_lock;
  } else if (trx->will_lock == 0) {
    trx->read_only = true;
  }
  
  // 【关键】GTID持久化标记初始化
  trx->persists_gtid = false;

  // 【关键】设置事务开始时间
  if (trx->mysql_thd != nullptr) {
    trx->start_time.store(thd_start_time(trx->mysql_thd),
                          std::memory_order_relaxed);
  }

  // 【关键】事务序号初始化为最大值（提交时才分配真实值）
  trx->no = TRX_ID_MAX;
  
  // 【关键】根据读写类型分配事务ID
  if (read_write) {
    // 读写事务：分配trx_id并加入trx_sys
    trx_sys_mutex_enter();
    trx->id = trx_sys_allocate_trx_id();
    trx->state.store(TRX_STATE_ACTIVE, std::memory_order_relaxed);
    trx_sys_rw_trx_add(trx);
    trx_sys_mutex_exit();
  } else {
    // 只读事务：不分配trx_id
    trx->state.store(TRX_STATE_ACTIVE, std::memory_order_relaxed);
  }
}
```

### 4.5 Redo Log生成核心代码

```c
// storage/innobase/page/page0cur.cc:853-918
/** Writes the log record of a record insert on a page. */
static void page_cur_insert_rec_write_log(
    rec_t *insert_rec,   /*!< in: inserted physical record */
    ulint rec_size,      /*!< in: insert_rec size */
    rec_t *cursor_rec,   /*!< in: record the cursor is pointing to */
    dict_index_t *index, /*!< in: record descriptor */
    mtr_t *mtr)          /*!< in: mini-transaction handle */
{
  ulint cur_rec_size;
  ulint extra_size;
  ulint cur_extra_size;
  const byte *ins_ptr;

  // 【关键】临时表不写Redo Log（崩溃恢复不需要）
  if (index->table->is_temporary()) {
    byte *log_ptr = nullptr;
    if (!mlog_open(mtr, 0, log_ptr)) {
      return;
    }
    mlog_close(mtr, log_ptr);
    return;
  }

  // 计算记录的额外空间和完整大小
  {
    mem_heap_t *heap = nullptr;
    ulint cur_offs_[REC_OFFS_NORMAL_SIZE];
    ulint ins_offs_[REC_OFFS_NORMAL_SIZE];

    cur_offs = rec_get_offsets(cursor_rec, index, cur_offs_, 
                               ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);
    ins_offs = rec_get_offsets(insert_rec, index, ins_offs_, 
                               ULINT_UNDEFINED, UT_LOCATION_HERE, &heap);

    extra_size = rec_offs_extra_size(ins_offs);
    cur_rec_size = rec_offs_size(cur_offs);
    cur_extra_size = rec_offs_extra_size(cur_offs);

    if (heap != nullptr) {
      mem_heap_free(heap);
    }
  }

  // 【关键】写入MLOG_REC_INSERT类型的Redo记录
  byte *log_ptr = nullptr;
  if (!mlog_open_and_write_index(mtr, insert_rec, index, MLOG_REC_INSERT,
                                 2 + 5 + 1 + 5 + 5 + MLOG_BUF_MARGIN,
                                 log_ptr)) {
    return;
  }
  
  // 写入cursor_rec的页内偏移（2字节）
  mach_write_to_2(log_ptr, page_offset(cursor_rec));
  log_ptr += 2;
  
  // ... 后续写入记录详情
}
```

### 4.6 InnoDB Prepare阶段核心代码

```c
// storage/innobase/trx/trx0trx.cc:3063-3111
/** Prepares a transaction.
@param[in]     trx the transction to prepare. */
static void trx_prepare(trx_t *trx) {
  lsn_t lsn = 0;

  // 只有新事务可以prepare，恢复的事务不行
  ut_a(!trx->is_recovered);

  // 如果有Redo回滚段且已更新
  if (trx->rsegs.m_redo.rseg != nullptr && trx_is_redo_rseg_updated(trx)) {
    // 【关键】将Undo Log状态改为PREPARED
    lsn = trx_prepare_low(trx, &trx->rsegs.m_redo, false);
  }

  // 临时表的Undo处理（不需要Redo）
  if (trx->rsegs.m_noredo.rseg != nullptr && trx_is_temp_rseg_updated(trx)) {
    trx_prepare_low(trx, &trx->rsegs.m_noredo, true);
  }

  // 【关键字段变化】状态从ACTIVE变为PREPARED
  ut_a(trx->state.load(std::memory_order_relaxed) == TRX_STATE_ACTIVE);
  trx_sys_mutex_enter();
  trx->state.store(TRX_STATE_PREPARED, std::memory_order_relaxed);
  trx_sys->n_prepared_trx++;
  trx_sys_mutex_exit();

  // RC隔离级别下释放GAP锁
  if (trx->releases_gap_locks_at_prepare()) {
    trx->skip_lock_inheritance = true;
    lock_trx_release_read_locks(trx, true);
  }

  // 【关键】刷新Redo Log确保持久化
  if (lsn > 0) {
    trx_flush_logs(trx, lsn);
  }
}
```

---

## 五、优化与修复

### 5.1 性能优化建议

| 优化点 | 说明 | 配置建议 |
|:------|:-----|:--------|
| **innodb_flush_log_at_trx_commit** | 控制Redo刷盘策略 | 性能优先设为2，安全优先设为1 |
| **sync_binlog** | 控制Binlog刷盘策略 | 与上面配合使用 |
| **innodb_log_buffer_size** | Redo Log缓冲区大小 | 大事务时适当增大 |
| **binlog_group_commit_sync_delay** | Group Commit等待时间 | 高并发时适当设置 |

### 5.2 常见问题与解决

**问题1：INSERT性能下降**
```
症状：大量INSERT时TPS下降
原因：Undo/Redo竞争、Binlog写入瓶颈
解决：
  1. 批量INSERT代替单条
  2. 使用LOAD DATA INFILE
  3. 调整innodb_flush_log_at_trx_commit
```

**问题2：崩溃恢复时间长**
```
症状：重启后恢复时间过长
原因：Redo Log过大、未提交事务多
解决：
  1. 适当减小innodb_log_file_size
  2. 优化长事务
```

### 5.3 两阶段提交的容错机制

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     崩溃恢复场景分析                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  崩溃点          │  InnoDB状态      │  Binlog状态  │  恢复动作               │
│  ───────────────────────────────────────────────────────────────────────── │
│  Prepare前崩溃  │  ACTIVE          │  无          │  InnoDB回滚            │
│  Prepare后崩溃  │  PREPARED        │  无          │  InnoDB回滚            │
│  Binlog写入后   │  PREPARED        │  有          │  InnoDB提交(XA恢复)    │
│  Commit后崩溃   │  COMMITTED       │  有          │  无需恢复              │
│                                                                             │
│  【核心原则】Binlog有记录 = 事务必须提交；Binlog无记录 = 事务必须回滚        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 六、总结与反思

### 6.1 核心要点回顾

1. **Undo Log在INSERT执行时生成**：只记录主键信息，用于回滚时删除记录
2. **Redo Log通过MTR机制写入**：保证数据页修改的原子性和持久性
3. **Binlog在COMMIT时写入**：通过两阶段提交保证与InnoDB数据一致
4. **事务状态流转**：NOT_STARTED → ACTIVE → PREPARED → COMMITTED_IN_MEMORY

### 6.2 关键内存字段变化总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     INSERT全流程关键字段变化时序                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  时间轴 ──────────────────────────────────────────────────────────────────▶ │
│                                                                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │
│  │协议解析 │  │事务启动 │  │INSERT   │  │Prepare  │  │Commit   │          │
│  │         │  │         │  │执行     │  │         │  │         │          │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘          │
│       │            │            │            │            │                 │
│       ▼            ▼            ▼            ▼            ▼                 │
│  com_data.     trx->state=   trx->undo_no trx->state=  trx->state=         │
│  com_query.    ACTIVE        ++           PREPARED     COMMITTED           │
│  query设置     trx->id分配   insert_undo  undo->state= trx->no分配         │
│               trx->no=MAX   分配         PREPARED                          │
│                              roll_ptr                                       │
│                              生成                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.3 架构设计思考

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MySQL日志系统设计哲学                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐   │
│   │                                                                     │   │
│   │    【分层设计】                                                       │   │
│   │    ┌──────────┐                                                     │   │
│   │    │  Binlog  │ ◀── Server层：逻辑日志，用于复制                      │   │
│   │    └──────────┘                                                     │   │
│   │         │                                                           │   │
│   │         ▼                                                           │   │
│   │    ┌──────────┐                                                     │   │
│   │    │Redo/Undo │ ◀── 存储引擎层：物理日志，用于恢复                     │   │
│   │    └──────────┘                                                     │   │
│   │                                                                     │   │
│   │    【协调机制】                                                       │   │
│   │    两阶段提交(2PC)确保跨层一致性                                       │   │
│   │                                                                     │   │
│   │    【性能权衡】                                                       │   │
│   │    Group Commit批量处理，减少fsync次数                                │   │
│   │                                                                     │   │
│   └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.4 延伸阅读建议

- 深入理解MTR(Mini-Transaction)机制
- 学习InnoDB Buffer Pool与日志的协作
- 研究Binlog的Row/Statement/Mixed格式差异

---

**【示意图描述】**
`![INSERT事务日志流程图](insert_transaction_log_flow.png): 展示INSERT语句从协议解析到提交过程中，Undo Log、Redo Log、Binlog的写入时序和数据流向，以及事务状态和关键内存字段的变迁过程。`

---

> 📝 **作者注**：本文基于Percona Server 8.4.3源码分析，不同版本实现细节可能略有差异。如有疑问，欢迎在评论区交流讨论。

---

## 七、大量INSERT语句的瓶颈分析

### 7.1 瓶颈点全景图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   大量INSERT语句时的资源竞争瓶颈全景                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【应用层】                                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  线程1: INSERT INTO t VALUES(1,'a');  ──┐                            │  │
│  │  线程2: INSERT INTO t VALUES(2,'b');  ──┼─▶ 并发请求                  │  │
│  │  线程3: INSERT INTO t VALUES(3,'c');  ──┘                            │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                              │                                              │
│                              ▼                                              │
│  【MySQL Server层 - 瓶颈点1~2】                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  ┌─────────────────┐      ┌─────────────────┐                        │  │
│  │  │  MDL Lock       │      │ Table Cache     │                        │  │
│  │  │  (SHARED_WRITE) │      │  Lock           │                        │  │
│  │  │  ★瓶颈点1       │      │  ★瓶颈点2       │                        │  │
│  │  └─────────────────┘      └─────────────────┘                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                              │                                              │
│                              ▼                                              │
│  【InnoDB存储引擎层 - 瓶颈点3~7】                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │  ┌─────────────┐   ┌─────────────┐   ┌─────────────┐                │  │
│  │  │Auto-Inc Lock│   │B+Tree Index │   │ Buffer Pool │                │  │
│  │  │dict_table_  │   │  sx-latch   │   │    Mutex    │                │  │
│  │  │autoinc_lock │   │             │   │             │                │  │
│  │  │★瓶颈点3     │   │★瓶颈点4     │   │★瓶颈点5     │                │  │
│  │  └─────────────┘   └─────────────┘   └─────────────┘                │  │
│  │                                                                      │  │
│  │  ┌─────────────┐   ┌─────────────┐                                  │  │
│  │  │ Redo Log    │   │ Undo Log    │                                  │  │
│  │  │ Buffer      │   │ Segment     │                                  │  │
│  │  │ sn_lock     │   │  rseg Mutex │                                  │  │
│  │  │★瓶颈点6     │   │★瓶颈点7     │                                  │  │
│  │  └─────────────┘   └─────────────┘                                  │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                              │                                              │
│                              ▼                                              │
│  【二进制日志层 - 瓶颈点8~9】                                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  ┌─────────────────┐      ┌─────────────────┐                        │  │
│  │  │  Binlog Cache   │      │   LOCK_log      │                        │  │
│  │  │  Memory         │      │  (Group Commit) │                        │  │
│  │  │  ★瓶颈点8       │      │  ★瓶颈点9       │                        │  │
│  │  └─────────────────┘      └─────────────────┘                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.2 各瓶颈点详细分析

#### 瓶颈点1：MDL Lock竞争

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   瓶颈点1: MDL Lock 竞争分析                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【源码位置】                                                                │
│  sql/mdl.cc:MDL_context::acquire_lock()                                    │
│  sql/sql_base.cc:open_table()                                              │
│                                                                             │
│  【竞争场景】                                                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   Thread1: INSERT ──┐                                                │  │
│  │   Thread2: INSERT ──┼─▶ 同时请求 MDL_SHARED_WRITE ──▶ 兼容，无阻塞  │  │
│  │   Thread3: INSERT ──┘                                                │  │
│  │                                                                      │  │
│  │   ThreadN: ALTER TABLE ──▶ 请求 MDL_EXCLUSIVE ──▶ 阻塞所有INSERT!   │  │
│  │                                                                      │  │
│  │   【问题】一个DDL就会阻塞所有INSERT                                  │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【锁等待队列】                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   MDL_lock::m_waiting (等待队列)                                     │  │
│  │   ┌─────┬─────┬─────┬─────┬─────┐                                   │  │
│  │   │ T1  │ T2  │ T3  │ ... │ Tn  │  等待MDL_EXCLUSIVE的锁            │  │
│  │   └─────┴─────┴─────┴─────┴─────┘                                   │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化建议】                                                                │
│  - 使用 Online DDL (ALGORITHM=INPLACE)                                     │
│  - 避免在高峰期执行DDL                                                      │
│  - 使用 pt-online-schema-change 等工具                                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 瓶颈点3：Auto-Increment Lock竞争（关键瓶颈）

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   瓶颈点3: Auto-Increment Lock 竞争分析                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【源码位置】                                                                │
│  storage/innobase/handler/ha_innodb.cc:9411-9473                           │
│  storage/innobase/row/row0mysql.cc:1620-1682 (row_lock_table_autoinc)      │
│                                                                             │
│  【三种自增锁模式】innodb_autoinc_lock_mode                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │  值=0: AUTOINC_OLD_STYLE_LOCKING (传统模式)                          │  │
│  │  ├── 每个INSERT都持有表级AUTO-INC锁直到语句结束                      │  │
│  │  └── 并发性最差，但保证连续自增值                                    │  │
│  │                                                                      │  │
│  │  值=1: AUTOINC_NEW_STYLE_LOCKING (默认，折中模式)                    │  │
│  │  ├── Simple INSERT: 只锁定分配自增值的瞬间(mutex)                   │  │
│  │  ├── Bulk INSERT: 仍使用表级AUTO-INC锁                              │  │
│  │  └── 并发性好，基本保证连续                                          │  │
│  │                                                                      │  │
│  │  值=2: AUTOINC_NO_LOCKING (交错模式)                                 │  │
│  │  ├── 所有INSERT都只使用轻量mutex                                    │  │
│  │  ├── 并发性最好                                                      │  │
│  │  └── 自增值可能不连续，对SBR复制不安全                               │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【源码调用链】                                                              │
│                                                                             │
│  handler::ha_write_row()                         sql/handler.cc:8200       │
│  │                                                                          │
│  └── ha_innobase::write_row()                    ha_innodb.cc:9000         │
│      │                                                                      │
│      ├── get_auto_increment()                    ha_innodb.cc:20560        │
│      │   │                                                                  │
│      │   └── innobase_get_autoinc()              ha_innodb.cc:20516        │
│      │       │                                                              │
│      │       └── innobase_lock_autoinc()         ha_innodb.cc:9411         │
│      │           │                                                          │
│      │           │  switch (innobase_autoinc_lock_mode) {                  │
│      │           │                                                          │
│      │           ├── case AUTOINC_NO_LOCKING:                              │
│      │           │   └── dict_table_autoinc_lock(table)  // 只加mutex      │
│      │           │                                                          │
│      │           ├── case AUTOINC_NEW_STYLE_LOCKING:                       │
│      │           │   ├── if (SQLCOM_INSERT || SQLCOM_REPLACE)              │
│      │           │   │   └── dict_table_autoinc_lock(table)  // 只加mutex  │
│      │           │   └── else  // LOAD DATA, INSERT...SELECT等            │
│      │           │       └── row_lock_table_autoinc_for_mysql()  // 表锁!  │
│      │           │                                                          │
│      │           └── case AUTOINC_OLD_STYLE_LOCKING:                       │
│      │               └── row_lock_table_autoinc_for_mysql()  // 表锁!      │
│      │                   │                       row0mysql.cc:1620         │
│      │                   │                                                  │
│      │                   └── lock_table(LOCK_AUTO_INC)                     │
│      │                       // 持有直到语句结束!                           │
│      │                                                                      │
│      └── row_insert_for_mysql()                  row0mysql.cc:1760         │
│                                                                             │
│  【竞争热点可视化】                                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   lock_mode=0 (OLD):                                                 │  │
│  │   T1: [========AUTO-INC LOCK========]                                │  │
│  │   T2:                                 [========AUTO-INC LOCK========]│  │
│  │   T3:                                                                 │  │
│  │       完全串行! 吞吐量极低                                            │  │
│  │                                                                      │  │
│  │   lock_mode=1 (NEW), Simple INSERT:                                  │  │
│  │   T1: [M]──────────────────                                          │  │
│  │   T2:   [M]────────────────                                          │  │
│  │   T3:     [M]──────────────                                          │  │
│  │       mutex只在分配值瞬间，并发好                                     │  │
│  │                                                                      │  │
│  │   lock_mode=1 (NEW), Bulk INSERT (INSERT...SELECT):                  │  │
│  │   T1: [========AUTO-INC LOCK========]                                │  │
│  │   T2:                                 [========AUTO-INC LOCK========]│  │
│  │       退化为串行!                                                     │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 瓶颈点4：B+Tree索引sx-latch竞争

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   瓶颈点4: B+Tree Index sx-latch 竞争分析                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【源码位置】                                                                │
│  storage/innobase/row/row0ins.cc:3088-3160 (row_ins_clust_index_entry)     │
│  storage/innobase/btr/btr0cur.cc (B-tree cursor operations)                │
│                                                                             │
│  【索引页分裂导致的锁升级】                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │  正常INSERT: BTR_MODIFY_LEAF (乐观插入，只锁叶子页)                  │  │
│  │  ┌─────────────┐                                                     │  │
│  │  │   ROOT      │  无锁                                               │  │
│  │  └─────┬───────┘                                                     │  │
│  │        │                                                              │  │
│  │  ┌─────┴───────┐                                                     │  │
│  │  │  Internal   │  无锁                                               │  │
│  │  └─────┬───────┘                                                     │  │
│  │        │                                                              │  │
│  │  ┌─────┴───────┐                                                     │  │
│  │  │   LEAF ★   │  X-latch (只锁这一页)                                │  │
│  │  └─────────────┘                                                     │  │
│  │                                                                      │  │
│  │  页满需分裂: BTR_MODIFY_TREE (悲观插入，锁整棵树!)                   │  │
│  │  ┌─────────────┐                                                     │  │
│  │  │   ROOT ★   │  SX-latch (整棵树!)                                  │  │
│  │  └─────┬───────┘                                                     │  │
│  │        │                                                              │  │
│  │  ┌─────┴───────┐                                                     │  │
│  │  │ Internal ★ │  X-latch                                             │  │
│  │  └─────┬───────┘                                                     │  │
│  │        │                                                              │  │
│  │  ┌─────┴───────┐                                                     │  │
│  │  │   LEAF ★   │  X-latch                                             │  │
│  │  └─────────────┘                                                     │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【热点页问题】                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │  场景: AUTO_INCREMENT主键，顺序插入                                   │  │
│  │                                                                      │  │
│  │  所有INSERT都插入到B+Tree的最右边叶子页:                             │  │
│  │                                                                      │  │
│  │  ┌────┬────┬────┬────┬────┬────┬────┐                               │  │
│  │  │ 1  │ 2  │ 3  │ ...│ n  │n+1 │HOT │ ◀── 所有线程竞争这一页!        │  │
│  │  └────┴────┴────┴────┴────┴────┴────┘                               │  │
│  │                                       ▲                               │  │
│  │                        T1,T2,T3...Tn 全部等待                        │  │
│  │                                                                      │  │
│  │  【解决方案】                                                         │  │
│  │  - 使用UUID/GUID作为主键 (随机分布)                                  │  │
│  │  - 使用分区表                                                        │  │
│  │  - 使用组合主键 (时间戳+序号)                                        │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【源码关键代码】row0ins.cc:3107-3134                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │  // 首先尝试乐观插入(只锁叶子页)                                     │  │
│  │  err = row_ins_clust_index_entry_low(                                │  │
│  │      flags, BTR_MODIFY_LEAF, index, ...);                            │  │
│  │                                                                      │  │
│  │  if (err != DB_FAIL) {                                               │  │
│  │      return err;  // 成功，返回                                      │  │
│  │  }                                                                   │  │
│  │                                                                      │  │
│  │  // 乐观失败(页满)，需要悲观插入(锁整棵树!)                          │  │
│  │  log_free_check();  // 检查redo log空间                              │  │
│  │                                                                      │  │
│  │  // BTR_MODIFY_TREE会锁定从root到leaf的整条路径!                     │  │
│  │  err = row_ins_clust_index_entry_low(                                │  │
│  │      flags, BTR_MODIFY_TREE, index, ...);                            │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 瓶颈点6：Redo Log Buffer竞争

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   瓶颈点6: Redo Log Buffer 竞争分析                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【源码位置】                                                                │
│  storage/innobase/log/log0buf.cc:60-165                                    │
│  storage/innobase/include/log0sys.h:67-126                                 │
│                                                                             │
│  【Redo Log Buffer架构】                                                     │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   log_t 结构体关键字段:                                              │  │
│  │   ┌──────────────────────────────────────────────────────────────┐  │  │
│  │   │  atomic_sn_t sn;              // 当前序列号(原子变量)         │  │  │
│  │   │  ib_mutex_t sn_x_lock_mutex;  // 排他锁获取的互斥量           │  │  │
│  │   │  byte* buf;                   // Log Buffer指针              │  │  │
│  │   │  atomic_sn_t buf_size_sn;     // Buffer大小                   │  │  │
│  │   └──────────────────────────────────────────────────────────────┘  │  │
│  │                                                                      │  │
│  │   【空间预留过程】log0buf.cc:107-165                                 │  │
│  │   ┌──────────────────────────────────────────────────────────────┐  │  │
│  │   │                                                              │  │  │
│  │   │  1. 原子操作预留空间:                                         │  │  │
│  │   │     start_sn = log.sn.fetch_add(len)  // 原子递增            │  │  │
│  │   │     end_sn = start_sn + len                                  │  │  │
│  │   │                                                              │  │  │
│  │   │  2. 等待空间可用:                                             │  │  │
│  │   │     if (end_lsn - log.write_lsn > log.buf_size)              │  │  │
│  │   │         wait_for_log_writer()  // ★瓶颈点: 等待写入完成      │  │  │
│  │   │                                                              │  │  │
│  │   │  3. 写入数据到Buffer                                          │  │  │
│  │   │                                                              │  │  │
│  │   └──────────────────────────────────────────────────────────────┘  │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【竞争可视化】                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   Redo Log Buffer: [====已写入====][====空闲====]                   │  │
│  │                     ↑              ↑             ↑                   │  │
│  │                  write_lsn       sn           buf_end                │  │
│  │                                                                      │  │
│  │   高并发时:                                                           │  │
│  │   T1: sn.fetch_add(100) ──▶ [0,100)                                 │  │
│  │   T2: sn.fetch_add(50)  ──▶ [100,150)                               │  │
│  │   T3: sn.fetch_add(80)  ──▶ [150,230)                               │  │
│  │                                                                      │  │
│  │   如果Buffer满了:                                                     │  │
│  │   T4: sn.fetch_add(100) ──▶ 等待log_writer线程写入释放空间          │  │
│  │       ★这里会阻塞!                                                   │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化建议】                                                                │
│  - 增大 innodb_log_buffer_size (默认16MB)                                  │
│  - 使用更快的存储设备                                                       │
│  - 批量提交减少日志写入次数                                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

#### 瓶颈点9：Binlog LOCK_log竞争

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   瓶颈点9: Binlog LOCK_log 竞争分析                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【源码位置】                                                                │
│  sql/binlog.cc:8830-8864 (process_flush_stage_queue)                       │
│  sql/binlog.cc:9234-9383 (ordered_commit)                                  │
│                                                                             │
│  【Group Commit流程中的锁】                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   每个INSERT事务提交时都需要经过:                                     │  │
│  │                                                                      │  │
│  │   Stage 0: COMMIT_ORDER_FLUSH (从库)                                 │  │
│  │   Stage 1: FLUSH (写binlog cache到文件)                              │  │
│  │   │        ├── 获取 LOCK_log                                        │  │
│  │   │        ├── flush_thread_caches() for each thd                   │  │
│  │   │        └── 释放 LOCK_log                                        │  │
│  │   │                                                                  │  │
│  │   Stage 2: SYNC (fsync binlog文件)                                   │  │
│  │   │        ├── 获取 LOCK_sync                                       │  │
│  │   │        ├── sync_binlog_file()                                   │  │
│  │   │        └── 释放 LOCK_sync                                       │  │
│  │   │                                                                  │  │
│  │   Stage 3: COMMIT (引擎提交)                                         │  │
│  │            ├── 获取 LOCK_commit                                     │  │
│  │            ├── ha_commit_low() for each thd                         │  │
│  │            └── 释放 LOCK_commit                                     │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【Group Commit的优化效果】                                                  │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   无Group Commit (每个事务单独fsync):                                │  │
│  │   T1: [FLUSH][SYNC][COMMIT]                                         │  │
│  │   T2:                      [FLUSH][SYNC][COMMIT]                    │  │
│  │   T3:                                            [FLUSH][SYNC][COMMIT]│
│  │                                                                      │  │
│  │   有Group Commit (合并多个事务):                                      │  │
│  │   T1,T2,T3: [FLUSH(3个)][SYNC(1次)][COMMIT(3个)]                     │  │
│  │                                                                      │  │
│  │   但问题是: LOCK_log仍是串行获取的瓶颈                               │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化建议】                                                                │
│  - 设置 binlog_group_commit_sync_delay (微秒级延迟收集更多事务)            │
│  - 设置 binlog_group_commit_sync_no_delay_count (达到数量立即提交)        │
│  - 使用 sync_binlog=0 或更大值减少fsync次数(牺牲持久性)                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 7.3 单条INSERT vs 批量INSERT的开销对比

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   单条INSERT vs 批量INSERT 开销对比                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【单条INSERT执行1000次】                                                    │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   for i in 1..1000:                                                  │  │
│  │       INSERT INTO t VALUES(i, 'data')                               │  │
│  │                                                                      │  │
│  │   开销分析:                                                           │  │
│  │   ┌───────────────────────────────────────────────────────────────┐ │  │
│  │   │  操作                    │ 次数    │ 说明                     │ │  │
│  │   ├───────────────────────────────────────────────────────────────┤ │  │
│  │   │  SQL解析                 │ 1000    │ 每条都要解析              │ │  │
│  │   │  获取MDL锁               │ 1000    │ 每条都要获取              │ │  │
│  │   │  获取Auto-Inc锁/mutex   │ 1000    │ 每条都要获取              │ │  │
│  │   │  B+Tree定位              │ 1000    │ 每条都要定位插入点        │ │  │
│  │   │  写Undo Log              │ 1000    │ 每条都要写                │ │  │
│  │   │  写Redo Log              │ 1000    │ 每条都要写                │ │  │
│  │   │  写Binlog Cache          │ 1000    │ 每条都要写                │ │  │
│  │   │  事务提交(Group Commit)  │ 1000    │ 每条都是独立事务          │ │  │
│  │   │  Binlog Sync             │ ~1000   │ 取决于group commit效果    │ │  │
│  │   └───────────────────────────────────────────────────────────────┘ │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【批量INSERT (Multi-Row INSERT)】                                          │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   INSERT INTO t VALUES(1,'a'),(2,'b'),...,(1000,'xxx')              │  │
│  │                                                                      │  │
│  │   开销分析:                                                           │  │
│  │   ┌───────────────────────────────────────────────────────────────┐ │  │
│  │   │  操作                    │ 次数    │ 说明                     │ │  │
│  │   ├───────────────────────────────────────────────────────────────┤ │  │
│  │   │  SQL解析                 │ 1       │ 只解析一次!              │ │  │
│  │   │  获取MDL锁               │ 1       │ 只获取一次!              │ │  │
│  │   │  获取Auto-Inc锁/mutex   │ 1~少量  │ 批量预分配!              │ │  │
│  │   │  B+Tree定位              │ 1000    │ 每行仍需定位              │ │  │
│  │   │  写Undo Log              │ 1000    │ 每行仍需写                │ │  │
│  │   │  写Redo Log              │ 1000    │ 每行仍需写(但连续高效)    │ │  │
│  │   │  写Binlog Cache          │ 1       │ 一个Write_rows_event!    │ │  │
│  │   │  事务提交(Group Commit)  │ 1       │ 只提交一次!              │ │  │
│  │   │  Binlog Sync             │ 1       │ 只sync一次!              │ │  │
│  │   └───────────────────────────────────────────────────────────────┘ │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【节省比例估算】                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   - SQL解析:      节省 ~99.9%                                        │  │
│  │   - MDL锁获取:    节省 ~99.9%                                        │  │
│  │   - Auto-Inc锁:   节省 ~95%+ (批量预分配)                            │  │
│  │   - 事务提交:     节省 ~99.9%                                        │  │
│  │   - Binlog Sync:  节省 ~99.9%                                        │  │
│  │   - 网络往返:     节省 ~99.9% (如果是远程数据库)                     │  │
│  │                                                                      │  │
│  │   综合性能提升: 通常 10x ~ 100x                                      │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 八、Multi INSERT 与 LOAD DATA INFILE 优化原理

### 8.1 Multi-Row INSERT 源码调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Multi-Row INSERT 完整调用链 (从命令行开始)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【客户端】                                                                  │
│  mysql> INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c');                       │
│                                                                             │
│  【网络协议层】                                                              │
│  Protocol_classic::read_packet()             sql/protocol_classic.cc:1408  │
│  │                                                                          │
│  │  // 读取COM_QUERY命令包                                                  │
│  └── Protocol_classic::get_command()         sql/protocol_classic.cc:2887  │
│      │                                                                      │
│      └── command = COM_QUERY                                               │
│                                                                             │
│  【命令分发层】                                                              │
│  do_command()                                sql/sql_parse.cc:1374         │
│  │                                                                          │
│  └── dispatch_command()                      sql/sql_parse.cc:1828         │
│      │                                                                      │
│      │  // 解析SQL                                                          │
│      ├── mysql_parse()                       sql/sql_parse.cc:6100         │
│      │   │                                                                  │
│      │   └── LEX解析器识别为:                                               │
│      │       lex->sql_command = SQLCOM_INSERT                              │
│      │       insert_many_values.size() = 3  // 识别为多行INSERT             │
│      │                                                                      │
│      │  // 执行                                                              │
│      └── mysql_execute_command()             sql/sql_parse.cc:3068         │
│          │                                                                  │
│          │  case SQLCOM_INSERT:                                            │
│          └── Sql_cmd_insert::execute()       sql/sql_insert.cc:630         │
│                                                                             │
│  【INSERT执行层】                                                            │
│  Sql_cmd_insert_values::execute_inner()      sql/sql_insert.cc:460         │
│  │                                                                          │
│  │  // ★关键: 开启Bulk Insert优化                                          │
│  ├── insert_table->file->ha_start_bulk_insert(insert_many_values.size())   │
│  │   │                                       sql/sql_insert.cc:562         │
│  │   │                                                                      │
│  │   │  // InnoDB层处理                                                     │
│  │   └── ha_innobase::start_bulk_insert()    ha_innodb.cc:9300            │
│  │       │                                                                  │
│  │       │  // 预分配Auto-Inc值!                                           │
│  │       └── m_prebuilt->autoinc_increment = rows_to_insert;              │
│  │           // 一次性获取足够的自增值，避免反复获取锁                       │
│  │                                                                          │
│  │  // ★关键: 循环插入每一行                                                │
│  ├── for (const List_item *values : insert_many_values) {                  │
│  │   │                                       sql/sql_insert.cc:581         │
│  │   │                                                                      │
│  │   │  // 填充记录                                                         │
│  │   ├── fill_record_n_invoke_before_triggers()                           │
│  │   │                                                                      │
│  │   │  // 写入行                                                           │
│  │   └── write_record()                      sql/sql_insert.cc:1800        │
│  │       │                                                                  │
│  │       └── table->file->ha_write_row()     sql/handler.cc:8200          │
│  │           │                                                              │
│  │           └── ha_innobase::write_row()                                  │
│  │               │                                                          │
│  │               │  // 使用预分配的Auto-Inc值，无需再获取锁!               │
│  │               └── row_insert_for_mysql()                                │
│  │   }                                                                      │
│  │                                                                          │
│  │  // ★关键: 结束Bulk Insert                                               │
│  └── insert_table->file->ha_end_bulk_insert()                             │
│      │                                       sql/sql_insert.cc:656         │
│      │                                                                      │
│      └── ha_innobase::end_bulk_insert()      ha_innodb.cc:9350            │
│          │                                                                  │
│          └── // 清理Bulk Insert状态                                        │
│                                                                             │
│  【提交阶段 - 只提交一次!】                                                  │
│  trans_commit_stmt()                         sql/transaction.cc:500        │
│  │                                                                          │
│  └── MYSQL_BIN_LOG::commit()                 sql/binlog.cc:8423           │
│      │                                                                      │
│      └── ordered_commit()                                                  │
│          │                                                                  │
│          ├── Stage 1: FLUSH                                                │
│          │   └── 写一个包含所有行的Write_rows_log_event                    │
│          │                                                                  │
│          ├── Stage 2: SYNC                                                 │
│          │   └── 只sync一次!                                               │
│          │                                                                  │
│          └── Stage 3: COMMIT                                               │
│              └── 只commit一次!                                             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 LOAD DATA INFILE 源码调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              LOAD DATA INFILE 完整调用链 (从命令行开始)                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【客户端】                                                                  │
│  mysql> LOAD DATA INFILE '/path/to/data.csv' INTO TABLE t;                 │
│                                                                             │
│  【命令分发层】                                                              │
│  dispatch_command() → mysql_execute_command()                              │
│  │                                                                          │
│  │  case SQLCOM_LOAD:                                                       │
│  └── Sql_cmd_load_table::execute()           sql/sql_load.cc:2620          │
│                                                                             │
│  【LOAD DATA执行层】                                                         │
│  Sql_cmd_load_table::execute_inner()         sql/sql_load.cc:860           │
│  │                                                                          │
│  │  // ★关键: 开启Bulk Insert优化 (行数设为0表示未知)                       │
│  ├── table->file->ha_start_bulk_insert((ha_rows)0)                        │
│  │   │                                       sql/sql_load.cc:1020          │
│  │   │                                                                      │
│  │   │  // InnoDB特殊处理: 禁用唯一键检查等优化                            │
│  │   └── ha_innobase::start_bulk_insert(0)                                │
│  │       │                                                                  │
│  │       ├── // 设置批量插入标志                                            │
│  │       │   m_prebuilt->bulk_insert_active = true;                        │
│  │       │                                                                  │
│  │       ├── // 调整自增值预分配策略                                        │
│  │       │   // 从1开始指数增长: 1,2,4,8,16,...                            │
│  │       │                                                                  │
│  │       └── // 可能禁用部分一致性检查以提速                                │
│  │                                                                          │
│  │  // ★关键: 额外优化标志                                                  │
│  ├── table->file->ha_extra(HA_EXTRA_IGNORE_DUP_KEY)  // 忽略重复键         │
│  ├── table->file->ha_extra(HA_EXTRA_WRITE_CAN_REPLACE)  // 可替换          │
│  │                                                                          │
│  │  // 根据文件格式选择读取函数                                              │
│  ├── if (filetype == FILETYPE_XML)                                         │
│  │   │   read_xml_field()                                                  │
│  │   │                                                                      │
│  │   ├── else if (fixed_length)                                            │
│  │   │   read_fixed_length()                 sql/sql_load.cc:1200          │
│  │   │                                                                      │
│  │   └── else                                                               │
│  │       read_sep_field()                    sql/sql_load.cc:1356          │
│  │                                                                          │
│  │  // ★核心循环: 读取文件并逐行插入                                        │
│  │  read_sep_field():                                                       │
│  │  │                                                                       │
│  │  │  while (!read_info.error) {                                          │
│  │  │      │                                                                │
│  │  │      │  // 读取一行数据                                               │
│  │  │      ├── 解析CSV/TSV行                                               │
│  │  │      │                                                                │
│  │  │      │  // 填充记录                                                   │
│  │  │      ├── fill_record_n_invoke_before_triggers()                      │
│  │  │      │                                                                │
│  │  │      │  // 写入行 (使用Bulk Insert优化)                               │
│  │  │      └── write_record()                sql/sql_load.cc:1334          │
│  │  │          │                                                            │
│  │  │          └── table->file->ha_write_row()                             │
│  │  │              │                                                        │
│  │  │              │  // ★优化: 延迟刷新                                    │
│  │  │              │  // 数据先积累在Buffer中                               │
│  │  │              │  // 减少页面写入次数                                   │
│  │  │              │                                                        │
│  │  │              └── ha_innobase::write_row_with_bulk_optimization()     │
│  │  │  }                                                                    │
│  │                                                                          │
│  │  // ★关键: 结束Bulk Insert                                               │
│  └── table->file->ha_end_bulk_insert()       sql/sql_load.cc:1033          │
│      │                                                                      │
│      └── ha_innobase::end_bulk_insert()                                    │
│          │                                                                  │
│          ├── // 刷新所有积累的数据                                          │
│          ├── // 重建/合并延迟构建的索引                                     │
│          └── // 恢复正常插入模式                                            │
│                                                                             │
│  【提交阶段】                                                                │
│  trans_commit_stmt() / trans_commit_implicit()                             │
│  │                                                                          │
│  └── // 整个LOAD DATA作为一个事务提交                                       │
│      // Binlog记录为一个LOAD DATA事件或多个Row事件                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 8.3 为什么Multi INSERT和LOAD DATA更高效

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Multi INSERT 和 LOAD DATA 的优化原理对比                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【优化点1: Bulk Insert Auto-Increment优化】                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   源码: ha_innodb.cc:9300-9350 (start_bulk_insert)                  │  │
│  │                                                                      │  │
│  │   单条INSERT:                                                        │  │
│  │   ┌─────┐  ┌─────┐  ┌─────┐                                         │  │
│  │   │Lock │──│Get 1│──│Lock │──│Get 1│──│Lock │──│Get 1│              │  │
│  │   └─────┘  └─────┘  └─────┘                                         │  │
│  │   每次获取1个值，每次都要加锁                                         │  │
│  │                                                                      │  │
│  │   Multi INSERT / LOAD DATA:                                          │  │
│  │   ┌─────┐  ┌───────────┐                                            │  │
│  │   │Lock │──│Get N values│──│使用N个值无需再锁│                       │  │
│  │   └─────┘  └───────────┘                                            │  │
│  │   一次获取N个值，大幅减少锁竞争                                       │  │
│  │                                                                      │  │
│  │   实现代码:                                                           │  │
│  │   ┌────────────────────────────────────────────────────────────┐    │  │
│  │   │  // ha_innodb.cc:20578-20620 (get_auto_increment)          │    │  │
│  │   │  // 批量INSERT时，estimation_rows_to_insert > 1             │    │  │
│  │   │  // 会一次性预分配多个自增值                                 │    │  │
│  │   │                                                            │    │  │
│  │   │  if (estimation_rows_to_insert > 1) {                      │    │  │
│  │   │      // 预分配: 从当前值分配nb_desired_values个             │    │  │
│  │   │      *first_value = autoinc;                               │    │  │
│  │   │      *nb_reserved_values = nb_desired_values;              │    │  │
│  │   │  }                                                         │    │  │
│  │   └────────────────────────────────────────────────────────────┘    │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化点2: 减少事务提交次数】                                               │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   单条INSERT (autocommit=1):                                         │  │
│  │   T1: [INSERT][COMMIT] [INSERT][COMMIT] [INSERT][COMMIT] ...        │  │
│  │        1000次INSERT = 1000次COMMIT = 1000次fsync                     │  │
│  │                                                                      │  │
│  │   Multi INSERT:                                                       │  │
│  │   T1: [INSERT 1000行][COMMIT]                                        │  │
│  │        1次COMMIT = 1次fsync                                          │  │
│  │                                                                      │  │
│  │   LOAD DATA:                                                          │  │
│  │   T1: [LOAD 100万行][COMMIT]                                         │  │
│  │        1次COMMIT = 1次fsync                                          │  │
│  │                                                                      │  │
│  │   【fsync是最昂贵的操作!】                                            │  │
│  │   HDD: ~10ms/次 → 100次/秒                                          │  │
│  │   SSD: ~0.1ms/次 → 10000次/秒                                       │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化点3: 减少Binlog写入】                                                 │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   单条INSERT (ROW格式):                                               │  │
│  │   ┌───────────────┐  ┌───────────────┐  ┌───────────────┐           │  │
│  │   │GTID│TABLE_MAP │  │GTID│TABLE_MAP │  │GTID│TABLE_MAP │           │  │
│  │   │    │WRITE_ROWS│  │    │WRITE_ROWS│  │    │WRITE_ROWS│ ...       │  │
│  │   └───────────────┘  └───────────────┘  └───────────────┘           │  │
│  │   每条INSERT都有完整的Event头开销                                    │  │
│  │                                                                      │  │
│  │   Multi INSERT:                                                       │  │
│  │   ┌─────────────────────────────────────────────────────────┐       │  │
│  │   │GTID│TABLE_MAP│WRITE_ROWS(包含1000行数据)                 │       │  │
│  │   └─────────────────────────────────────────────────────────┘       │  │
│  │   只有一个Event头，数据连续存储                                      │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化点4: B+Tree批量插入优化 (LOAD DATA特有)】                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   普通INSERT: 每行都要定位B+Tree插入点                               │  │
│  │   ┌──────────────────────────────────────────────────────┐          │  │
│  │   │  for each row:                                       │          │  │
│  │   │      search_tree_position()  // O(log N)             │          │  │
│  │   │      insert_at_position()                            │          │  │
│  │   └──────────────────────────────────────────────────────┘          │  │
│  │                                                                      │  │
│  │   LOAD DATA (sorted input): 利用顺序插入优化                         │  │
│  │   ┌──────────────────────────────────────────────────────┐          │  │
│  │   │  // InnoDB检测到顺序插入时:                          │          │  │
│  │   │  if (next_key > last_inserted_key) {                 │          │  │
│  │   │      // 直接在最右边叶子页追加，无需搜索!            │          │  │
│  │   │      append_to_rightmost_leaf()  // O(1)             │          │  │
│  │   │  }                                                   │          │  │
│  │   └──────────────────────────────────────────────────────┘          │  │
│  │                                                                      │  │
│  │   源码: row/row0ins.cc (row_ins_sorted_clust_index_entry)           │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【优化点5: 延迟二级索引构建 (LOAD DATA ALGORITHM=BULK)】                   │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   普通INSERT: 每插入一行，都要更新所有二级索引                       │  │
│  │   ┌──────────────────────────────────────────────────────┐          │  │
│  │   │  for each row:                                       │          │  │
│  │   │      insert_clustered_index()                        │          │  │
│  │   │      for each secondary_index:                       │          │  │
│  │   │          insert_secondary_index()  // 随机I/O!       │          │  │
│  │   └──────────────────────────────────────────────────────┘          │  │
│  │                                                                      │  │
│  │   LOAD DATA ALGORITHM=BULK: 先聚集索引，后批量构建二级索引           │  │
│  │   ┌──────────────────────────────────────────────────────┐          │  │
│  │   │  Phase 1: 只插入聚集索引 (顺序I/O)                   │          │  │
│  │   │  Phase 2: 排序二级索引键                             │          │  │
│  │   │  Phase 3: 批量构建二级索引 (顺序I/O)                 │          │  │
│  │   └──────────────────────────────────────────────────────┘          │  │
│  │                                                                      │  │
│  │   源码: sql/sql_load.cc:440-475 (execute_bulk)                      │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 8.4 性能优化建议总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                   大量INSERT场景的性能优化建议                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【场景1: 应用程序批量插入】                                                 │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   ❌ 错误方式:                                                        │  │
│  │   for row in data:                                                   │  │
│  │       execute("INSERT INTO t VALUES (%s)", row)                     │  │
│  │                                                                      │  │
│  │   ✅ 正确方式 (Multi-Row INSERT):                                    │  │
│  │   batch = []                                                         │  │
│  │   for row in data:                                                   │  │
│  │       batch.append(row)                                              │  │
│  │       if len(batch) >= 1000:                                        │  │
│  │           execute("INSERT INTO t VALUES " + ",".join(batch))        │  │
│  │           batch = []                                                 │  │
│  │                                                                      │  │
│  │   ✅ 更好方式 (executemany):                                         │  │
│  │   cursor.executemany("INSERT INTO t VALUES (%s,%s)", data)          │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【场景2: 大文件导入】                                                       │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   ✅ 使用LOAD DATA INFILE:                                           │  │
│  │   LOAD DATA INFILE '/path/data.csv'                                 │  │
│  │   INTO TABLE t                                                       │  │
│  │   FIELDS TERMINATED BY ','                                          │  │
│  │   LINES TERMINATED BY '\n';                                         │  │
│  │                                                                      │  │
│  │   ✅ 8.0.21+ 使用BULK算法:                                           │  │
│  │   LOAD DATA INFILE '/path/data.csv'                                 │  │
│  │   INTO TABLE t                                                       │  │
│  │   ALGORITHM = BULK;  -- 延迟索引构建，更快!                          │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【参数优化建议】                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   -- 自增锁模式 (如果不依赖连续自增值)                               │  │
│  │   SET GLOBAL innodb_autoinc_lock_mode = 2;                          │  │
│  │                                                                      │  │
│  │   -- 增大Redo Log Buffer                                             │  │
│  │   SET GLOBAL innodb_log_buffer_size = 64*1024*1024;  -- 64MB        │  │
│  │                                                                      │  │
│  │   -- 批量导入时可临时调整                                            │  │
│  │   SET GLOBAL innodb_flush_log_at_trx_commit = 2;  -- 性能vs持久性   │  │
│  │   SET GLOBAL sync_binlog = 0;  -- 性能vs持久性                      │  │
│  │                                                                      │  │
│  │   -- 禁用唯一键检查 (确保数据无重复时)                               │  │
│  │   SET unique_checks = 0;                                            │  │
│  │   SET foreign_key_checks = 0;                                       │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【性能对比参考】                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   插入100万行数据对比 (参考值):                                       │  │
│  │   ┌────────────────────────────────┬─────────────┬─────────────┐    │  │
│  │   │  方式                          │ 耗时        │ 相对性能    │    │  │
│  │   ├────────────────────────────────┼─────────────┼─────────────┤    │  │
│  │   │  单条INSERT (autocommit=1)     │ ~30分钟     │ 1x          │    │  │
│  │   │  单条INSERT (事务批量提交)     │ ~5分钟      │ 6x          │    │  │
│  │   │  Multi-Row INSERT (1000行/批)  │ ~1分钟      │ 30x         │    │  │
│  │   │  LOAD DATA INFILE              │ ~30秒       │ 60x         │    │  │
│  │   │  LOAD DATA ALGORITHM=BULK      │ ~15秒       │ 120x        │    │  │
│  │   └────────────────────────────────┴─────────────┴─────────────┘    │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

**【示意图描述】**
`![INSERT瓶颈与优化示意图](insert_bottleneck_optimization.png): 展示大量INSERT语句时的各层瓶颈点(MDL锁、Auto-Inc锁、B+Tree锁、Redo Log Buffer、Binlog锁)，以及Multi-Row INSERT和LOAD DATA INFILE如何通过批量操作优化来缓解这些瓶颈，包含源码调用链和性能对比数据。`

---

> 📝 **补充说明**：本节内容基于Percona Server 8.4.3源码分析。实际性能数据会因硬件配置、数据分布、索引结构等因素而异。建议在生产环境前进行充分测试。

---

## 附录：B+树插入完整调用链（btr_cur_optimistic_insert → page_cur_insert_rec_write_log）

**源码版本：Percona Server 8.4.3-3**

```
btr_cur_optimistic_insert() - storage/innobase/btr/btr0cur.cc:2719
│
├── 【参数验证与初始化】
│   ├── btr_cur_get_block() - storage/innobase/include/btr0cur.ic:45
│   ├── buf_block_get_frame() - storage/innobase/include/buf0buf.ic:120
│   └── rec_get_converted_size() - storage/innobase/rem/rem0rec.cc:680
│
├── 【空间检查】
│   ├── page_zip_rec_needs_ext() - storage/innobase/include/page0zip.ic:95
│   ├── page_get_max_insert_size_after_reorganize() - storage/innobase/page/page0page.cc:750
│   └── page_has_garbage() - storage/innobase/include/page0page.ic:320
│
├── 【锁与Undo日志处理】
│   └── btr_cur_ins_lock_and_undo() - storage/innobase/btr/btr0cur.cc:2400
│       │
│       ├── lock_rec_insert_check_and_lock() - storage/innobase/lock/lock0lock.cc:2150
│       │   └── 【锁状态变化】
│       │       └── ┌──────────────────┬────────────────────┬──────────────────────────────────┐
│       │           │ 字段              │ 操作                │ 说明                              │
│       │           ├──────────────────┼────────────────────┼──────────────────────────────────┤
│       │           │ trx->lock.n_rec  │ 递增                │ 记录锁计数+1                       │
│       │           └──────────────────┴────────────────────┴──────────────────────────────────┘
│       │
│       └── trx_undo_report_row_operation() - storage/innobase/trx/trx0rec.cc:2112
│           │
│           ├── trx_undo_assign_undo() - storage/innobase/trx/trx0undo.cc:1757
│           │   └── 【事务Undo状态变化】
│           │       └── ┌──────────────────────┬──────────────────┬─────────────────────────────┐
│           │           │ 字段                  │ 操作              │ 说明                         │
│           │           ├──────────────────────┼──────────────────┼─────────────────────────────┤
│           │           │ trx->rsegs.m_redo    │ 分配              │ 获取回滚段                    │
│           │           ├──────────────────────┼──────────────────┼─────────────────────────────┤
│           │           │ undo->state          │ TRX_UNDO_ACTIVE  │ Undo段激活                    │
│           │           └──────────────────────┴──────────────────┴─────────────────────────────┘
│           │
│           └── trx_undo_page_report_insert() - storage/innobase/trx/trx0rec.cc:480
│               │
│               ├── 【构造Undo记录】
│               │   └── ┌────────────┬────────────────┬─────────────────────┬──────────────────────────┐
│               │       │ 字节段      │ 内容            │ 赋值来源             │ 说明                      │
│               │       ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
│               │       │ 1          │ 操作类型        │ 直接赋值             │ TRX_UNDO_INSERT_REC      │
│               │       ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
│               │       │ 可变        │ undo_no        │ trx->undo_no        │ 压缩编码的操作序列号       │
│               │       ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
│               │       │ 可变        │ table_id       │ index->table->id    │ 压缩编码的表ID            │
│               │       ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
│               │       │ 可变        │ 唯一标识字段    │ clust_entry         │ 聚集索引主键字段值         │
│               │       ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
│               │       │ 2          │ 前向指针        │ first_free          │ 指向上一个undo记录偏移量   │
│               │       └────────────┴────────────────┴─────────────────────┴──────────────────────────┘
│               │
│               └── 【事务状态更新】
│                   └── ┌──────────────────────┬──────────────────┬─────────────────────────────┐
│                       │ 字段                  │ 操作              │ 说明                         │
│                       ├──────────────────────┼──────────────────┼─────────────────────────────┤
│                       │ trx->undo_no         │ trx->undo_no++   │ 事务内操作序列号+1            │
│                       ├──────────────────────┼──────────────────┼─────────────────────────────┤
│                       │ undo->empty          │ false            │ 标记undo段非空               │
│                       ├──────────────────────┼──────────────────┼─────────────────────────────┤
│                       │ undo->top_page_no    │ page_no          │ 最新undo记录页号             │
│                       ├──────────────────────┼──────────────────┼─────────────────────────────┤
│                       │ undo->top_offset     │ offset           │ 最新undo记录偏移量           │
│                       └──────────────────────┴──────────────────┴─────────────────────────────┘
│
└── 【记录插入到B+树页面】
    └── page_cur_tuple_insert() - storage/innobase/include/page0cur.ic:187
        │
        ├── rec_get_converted_size() - storage/innobase/rem/rem0rec.cc:680
        │   └── 计算记录转换后的大小
        │
        ├── mem_heap_create() - storage/innobase/include/mem0mem.ic:210
        │   └── 创建内存堆用于临时存储
        │
        ├── rec_convert_dtuple_to_rec() - storage/innobase/rem/rem0rec.cc:1050
        │   └── 将dtuple转换为物理记录格式
        │
        ├── rec_get_offsets() - storage/innobase/rem/rem0rec.cc:870
        │   └── 获取记录各字段的偏移量数组
        │
        └── page_cur_insert_rec_low() - storage/innobase/page/page0cur.cc:1226
            │
            ├── 【步骤1: 获取记录大小】
            │   └── rec_offs_size() - storage/innobase/include/rem0rec.ic:540
            │
            ├── 【步骤2: 从空闲链表或堆中分配空间】
            │   ├── page_header_get_ptr(PAGE_FREE) - storage/innobase/include/page0page.ic:180
            │   ├── page_mem_alloc_free() - storage/innobase/page/page0page.cc:650
            │   └── page_mem_alloc_heap() - storage/innobase/page/page0page.cc:590
            │
            ├── 【步骤3: 复制记录到页面】
            │   └── rec_copy() - storage/innobase/rem/rem0rec.cc:450
            │
            ├── 【步骤4: 插入记录到链表】
            │   ├── page_rec_get_next() - storage/innobase/include/page0page.ic:710
            │   ├── page_rec_set_next(insert_rec, next_rec) - storage/innobase/include/page0page.ic:750
            │   └── page_rec_set_next(current_rec, insert_rec) - storage/innobase/include/page0page.ic:750
            │
            ├── 【步骤5: 更新页头记录计数】
            │   └── page_header_set_field(PAGE_N_RECS, n+1) - storage/innobase/include/page0page.ic:220
            │
            ├── 【步骤6: 设置记录的n_owned和heap_no】
            │   ├── rec_set_n_owned_new() - storage/innobase/include/rem0rec.ic:380
            │   └── rec_set_heap_no_new() - storage/innobase/include/rem0rec.ic:320
            │
            ├── 【步骤7: 更新页头最后插入信息】
            │   ├── page_header_get_ptr(PAGE_LAST_INSERT) - storage/innobase/include/page0page.ic:180
            │   ├── page_header_set_field(PAGE_DIRECTION) - storage/innobase/include/page0page.ic:220
            │   ├── page_header_set_field(PAGE_N_DIRECTION) - storage/innobase/include/page0page.ic:220
            │   └── page_header_set_ptr(PAGE_LAST_INSERT) - storage/innobase/include/page0page.ic:240
            │
            ├── 【步骤8: 更新目录槽owner记录】
            │   ├── page_rec_find_owner_rec() - storage/innobase/page/page0page.cc:540
            │   ├── rec_set_n_owned_new(n_owned + 1) - storage/innobase/include/rem0rec.ic:380
            │   └── page_dir_split_slot() - storage/innobase/page/page0page.cc:1050
            │       └── (当n_owned超过PAGE_DIR_SLOT_MAX_N_OWNED时)
            │
            └── 【步骤9: 写入Redo日志】
                └── page_cur_insert_rec_write_log() - storage/innobase/page/page0cur.cc:854
                    │
                    ├── 【临时表检查 - 跳过Redo】
                    │   └── index->table->is_temporary() → 直接返回
                    │
                    ├── 【获取记录偏移信息】
                    │   ├── rec_get_offsets(cursor_rec) - storage/innobase/rem/rem0rec.cc:870
                    │   ├── rec_get_offsets(insert_rec) - storage/innobase/rem/rem0rec.cc:870
                    │   ├── rec_offs_extra_size() - storage/innobase/include/rem0rec.ic:480
                    │   └── rec_offs_size() - storage/innobase/include/rem0rec.ic:540
                    │
                    ├── 【计算差异优化】
                    │   └── 比较insert_rec与cursor_rec，找出第一个不同字节位置i
                    │
                    ├── 【打开Redo日志缓冲区】
                    │   └── mlog_open_and_write_index() - storage/innobase/mtr/mtr0log.cc:800
                    │       │
                    │       ├── mlog_open() - storage/innobase/include/mtr0log.ic:35
                    │       │   └── mtr->get_log()->open(size) → 获取日志缓冲区指针
                    │       │
                    │       ├── mlog_write_initial_log_record_fast() - storage/innobase/include/mtr0log.ic:95
                    │       │   │
                    │       │   └── 【写入Redo日志头部】
                    │       │       └── ┌────────────┬────────────────┬─────────────────────┬──────────────────────────┐
                    │       │           │ 字节段      │ 内容            │ 赋值来源             │ 说明                      │
                    │       │           ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
                    │       │           │ 1          │ 日志类型        │ MLOG_REC_INSERT     │ 值=67，插入记录日志类型    │
                    │       │           ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
                    │       │           │ 可变(1-5)  │ space_id       │ page_id.space()     │ 压缩编码的表空间ID         │
                    │       │           ├────────────┼────────────────┼─────────────────────┼──────────────────────────┤
                    │       │           │ 可变(1-5)  │ page_no        │ page_id.page_no()   │ 压缩编码的页号             │
                    │       │           └────────────┴────────────────┴─────────────────────┴──────────────────────────┘
                    │       │
                    │       ├── log_index_log_version() - storage/innobase/mtr/mtr0log.cc:720
                    │       │   └── 写入索引日志版本号(1字节)
                    │       │
                    │       ├── log_index_flag() - storage/innobase/mtr/mtr0log.cc:730
                    │       │   └── 写入索引标志位(1字节): instant/versioned/compact
                    │       │
                    │       ├── log_index_column_counts() - storage/innobase/mtr/mtr0log.cc:740
                    │       │   └── 写入索引列数信息
                    │       │
                    │       └── log_index_fields() - storage/innobase/mtr/mtr0log.cc:760
                    │           └── 写入索引字段定义信息
                    │
                    ├── 【写入Redo日志体】
                    │   └── ┌────────────────┬──────────────────┬───────────────────────────┬────────────────────────────────┐
                    │       │ 字节段          │ 内容              │ 赋值来源                   │ 说明                            │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ 2              │ cursor_rec偏移   │ page_offset(cursor_rec)   │ 游标记录在页内的偏移量          │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ 可变(1-5)      │ 记录尾段长度      │ 2*(rec_size-i)+flag       │ 压缩编码，flag=1表示有额外信息   │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ 1 (可选)       │ info_bits        │ rec_get_info_and_status   │ 记录信息位(仅当与cursor不同时)   │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ 可变 (可选)    │ extra_size       │ rec_offs_extra_size       │ 记录额外信息大小                 │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ 可变 (可选)    │ mismatch_index   │ i                         │ 第一个不匹配字节的位置           │
                    │       ├────────────────┼──────────────────┼───────────────────────────┼────────────────────────────────┤
                    │       │ rec_size-i     │ 记录内容          │ insert_rec+i              │ 从不匹配位置开始的记录内容       │
                    │       └────────────────┴──────────────────┴───────────────────────────┴────────────────────────────────┘
                    │
                    └── 【关闭Redo日志缓冲区】
                        ├── (短记录) mlog_close() - storage/innobase/include/mtr0log.ic:55
                        │   └── mtr->get_log()->close(log_ptr) → 标记缓冲区使用完成
                        │
                        └── (长记录) mlog_catenate_string() - storage/innobase/mtr/mtr0log.cc:61
                            └── mtr->get_log()->push(str, len) → 追加到日志缓冲区
```

### Redo日志完整结构示意图

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    MLOG_REC_INSERT Redo日志完整结构                                       │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                         │
│  【日志头部 - mlog_write_initial_log_record_fast()】                                                     │
│  ┌───────────────┬────────────────┬────────────────┬────────────────────────────────────────────────┐  │
│  │ 偏移           │ 长度            │ 内容            │ 说明                                            │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 0             │ 1字节           │ 67             │ MLOG_REC_INSERT日志类型                         │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 1             │ 1-5字节         │ space_id       │ 表空间ID(压缩编码)                               │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 1-5字节         │ page_no        │ 页号(压缩编码)                                  │  │
│  └───────────────┴────────────────┴────────────────┴────────────────────────────────────────────────┘  │
│                                                                                                         │
│  【索引信息 - mlog_open_and_write_index()】                                                              │
│  ┌───────────────┬────────────────┬────────────────┬────────────────────────────────────────────────┐  │
│  │ 偏移           │ 长度            │ 内容            │ 说明                                            │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 1字节           │ log_version    │ 索引日志版本 (INDEX_LOG_VERSION_CURRENT=0)      │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 1字节           │ flag           │ 标志位: bit0=instant, bit1=versioned, bit2=comp│  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 2字节           │ n_fields       │ 索引字段数                                      │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 2字节           │ n_uniq         │ 唯一字段数 (仅聚集索引)                          │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ n*2字节         │ field_lens[]   │ 各字段长度数组                                   │  │
│  └───────────────┴────────────────┴────────────────┴────────────────────────────────────────────────┘  │
│                                                                                                         │
│  【记录数据 - page_cur_insert_rec_write_log()】                                                          │
│  ┌───────────────┬────────────────┬────────────────┬────────────────────────────────────────────────┐  │
│  │ 偏移           │ 长度            │ 内容            │ 说明                                            │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 2字节           │ cursor_offset  │ 游标记录在页内偏移                               │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ 1-5字节         │ end_seg_len    │ 2*(rec_size-mismatch)+extra_info_flag          │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长(可选)     │ 1字节           │ info_bits      │ 记录info和status位 (当extra_info_flag=1)        │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长(可选)     │ 1-5字节         │ origin_offset  │ 记录原点偏移 (当extra_info_flag=1)              │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长(可选)     │ 1-5字节         │ mismatch_idx   │ 首个不匹配字节索引 (当extra_info_flag=1)         │  │
│  ├───────────────┼────────────────┼────────────────┼────────────────────────────────────────────────┤  │
│  │ 变长           │ rec_size-i字节  │ rec_data       │ 从mismatch位置开始的记录数据                     │  │
│  └───────────────┴────────────────┴────────────────┴────────────────────────────────────────────────┘  │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Redo日志写入缓存流程图

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    Redo日志写入缓存流程                                                   │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                         │
│  page_cur_insert_rec_write_log()                                                                        │
│         │                                                                                               │
│         ▼                                                                                               │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐              │
│  │ mlog_open_and_write_index()  - storage/innobase/mtr/mtr0log.cc:800                   │              │
│  │      │                                                                                │              │
│  │      ├── mlog_open(mtr, size, log_ptr)                                               │              │
│  │      │      │                                                                         │              │
│  │      │      └── mtr->get_log()->open(size)                                           │              │
│  │      │             │                                                                  │              │
│  │      │             └── 【mtr_buf_t 缓冲区状态变化】                                     │              │
│  │      │                 └── ┌─────────────────┬─────────────────────────────────────┐ │              │
│  │      │                     │ 字段             │ 变化                                 │ │              │
│  │      │                     ├─────────────────┼─────────────────────────────────────┤ │              │
│  │      │                     │ m_buf->m_ptr    │ 返回当前可写位置                      │ │              │
│  │      │                     ├─────────────────┼─────────────────────────────────────┤ │              │
│  │      │                     │ m_buf->m_size   │ 剩余可写空间                          │ │              │
│  │      │                     └─────────────────┴─────────────────────────────────────┘ │              │
│  │      │                                                                                │              │
│  │      ├── mlog_write_initial_log_record_fast()                                        │              │
│  │      │      └── 写入: type(1B) + space_id(压缩) + page_no(压缩)                       │              │
│  │      │                                                                                │              │
│  │      ├── log_index_log_version() → 写入版本号                                         │              │
│  │      ├── log_index_flag() → 写入标志位                                                │              │
│  │      ├── log_index_column_counts() → 写入列数                                         │              │
│  │      └── log_index_fields() → 写入字段信息                                            │              │
│  │                                                                                       │              │
│  └──────────────────────────────────────────────────────────────────────────────────────┘              │
│         │                                                                                               │
│         ▼                                                                                               │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐              │
│  │ 写入记录数据                                                                          │              │
│  │      │                                                                                │              │
│  │      ├── mach_write_to_2(log_ptr, cursor_offset)  → 写入游标偏移                      │              │
│  │      ├── mach_write_compressed(log_ptr, end_seg_len)  → 写入尾段长度                  │              │
│  │      ├── (可选) mach_write_to_1(log_ptr, info_bits)  → 写入info位                     │              │
│  │      ├── (可选) mach_write_compressed(log_ptr, extra_size)  → 写入额外大小            │              │
│  │      ├── (可选) mach_write_compressed(log_ptr, mismatch_idx)  → 写入不匹配索引        │              │
│  │      └── memcpy(log_ptr, ins_ptr, rec_size) / mlog_catenate_string()  → 写入记录内容 │              │
│  │                                                                                       │              │
│  └──────────────────────────────────────────────────────────────────────────────────────┘              │
│         │                                                                                               │
│         ▼                                                                                               │
│  ┌──────────────────────────────────────────────────────────────────────────────────────┐              │
│  │ mlog_close(mtr, log_ptr)  - storage/innobase/include/mtr0log.ic:55                   │              │
│  │      │                                                                                │              │
│  │      └── mtr->get_log()->close(log_ptr)                                              │              │
│  │             │                                                                         │              │
│  │             └── 【mtr_buf_t 缓冲区状态变化】                                           │              │
│  │                 └── ┌─────────────────┬─────────────────────────────────────────────┐│              │
│  │                     │ 字段             │ 变化                                         ││              │
│  │                     ├─────────────────┼─────────────────────────────────────────────┤│              │
│  │                     │ m_buf->m_size   │ 更新已写入大小                                ││              │
│  │                     ├─────────────────┼─────────────────────────────────────────────┤│              │
│  │                     │ mtr->m_log_mode │ 保持MTR_LOG_ALL                              ││              │
│  │                     └─────────────────┴─────────────────────────────────────────────┘│              │
│  │                                                                                       │              │
│  └──────────────────────────────────────────────────────────────────────────────────────┘              │
│         │                                                                                               │
│         ▼                                                                                               │
│  【后续在mtr_commit时刷新到redo log buffer】                                                             │
│  mtr_t::commit() → log_buffer_write() → 写入全局redo log buffer                                         │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---


---

## 附录补充：回滚指针构造、事务状态设置、Undo Redo日志及并发写入

**源码版本：Percona Server 8.4.3-3**

### 1. 事务状态设置为ACTIVE的调用链

```
【事务状态激活调用链】

row_insert_for_mysql() - storage/innobase/row/row0mysql.cc:1450
│
└── trx_start_if_not_started() - storage/innobase/include/trx0trx.h:850
    │
    └── trx_start_if_not_started_low() - storage/innobase/trx/trx0trx.cc:3440
        │
        └── trx_start_low() - storage/innobase/trx/trx0trx.cc:1338
            │
            ├── 【初始化事务时间】
            │   └── trx->start_time.store(thd_start_time(trx->mysql_thd))
            │
            ├── 【分配回滚段】(读写事务)
            │   └── trx_assign_rseg_durable() - storage/innobase/trx/trx0trx.cc:1284
            │
            ├── 【分配事务ID】(读写事务)
            │   ├── trx_sys_mutex_enter()
            │   ├── trx_assign_id_for_rw() - storage/innobase/trx/trx0trx.cc:1290
            │   └── trx_add_to_rw_trx_list() - storage/innobase/trx/trx0trx.cc:1300
            │
            └── 【设置事务状态为ACTIVE】
                │                              storage/innobase/trx/trx0trx.cc:1452
                │
                └── trx->state.store(TRX_STATE_ACTIVE, std::memory_order_relaxed)
                    │
                    └── 【事务状态变化表】
                        └── ┌────────────────────┬─────────────────────┬───────────────────────────────┐
                            │ 字段                │ 变化                 │ 说明                           │
                            ├────────────────────┼─────────────────────┼───────────────────────────────┤
                            │ trx->state         │ TRX_STATE_ACTIVE    │ 事务进入活跃状态                 │
                            ├────────────────────┼─────────────────────┼───────────────────────────────┤
                            │ trx->id            │ 分配唯一事务ID       │ 仅读写事务分配                   │
                            ├────────────────────┼─────────────────────┼───────────────────────────────┤
                            │ trx->no            │ TRX_ID_MAX          │ 初始值，提交时更新               │
                            ├────────────────────┼─────────────────────┼───────────────────────────────┤
                            │ trx->rsegs.m_redo  │ 分配回滚段           │ 用于存储undo日志                │
                            ├────────────────────┼─────────────────────┼───────────────────────────────┤
                            │ trx->start_time    │ 当前时间             │ 事务开始时间戳                   │
                            └────────────────────┴─────────────────────┴───────────────────────────────┘
```

### 2. 回滚指针(roll_ptr)构造与使用调用链

```
【回滚指针构造调用链 - 在trx_undo_report_row_operation内部】

trx_undo_report_row_operation() - storage/innobase/trx/trx0rec.cc:2112
│
├── trx_undo_page_report_insert() - storage/innobase/trx/trx0rec.cc:480
│   │
│   │  【返回值】
│   └── offset = 写入undo记录后的页内偏移量
│
└── 【构造回滚指针】
    │                                      storage/innobase/trx/trx0rec.cc:2317
    │
    └── *roll_ptr = trx_undo_build_roll_ptr() - storage/innobase/include/trx0undo.ic:45
        │
        ├── 【函数参数】
        │   ├── is_insert = (op_type == TRX_UNDO_INSERT_OP)  // 是否插入操作
        │   ├── space_id  = undo_ptr->rseg->space_id         // 回滚段空间ID
        │   ├── page_no   = undo记录所在页号                  // 从undo段获取
        │   └── offset    = undo记录在页内偏移量              // trx_undo_page_report_insert返回值
        │
        └── 【回滚指针位结构】
            └── ┌────────────┬────────────┬────────────────────┬──────────────────────────────────┐
                │ 位段        │ 长度        │ 内容                │ 计算公式/来源                     │
                ├────────────┼────────────┼────────────────────┼──────────────────────────────────┤
                │ bit 55     │ 1位         │ is_insert标志       │ (roll_ptr_t)is_insert << 55      │
                ├────────────┼────────────┼────────────────────┼──────────────────────────────────┤
                │ bit 48-54  │ 7位         │ 回滚段ID            │ undo::id2num(space_id) << 48     │
                ├────────────┼────────────┼────────────────────┼──────────────────────────────────┤
                │ bit 16-47  │ 32位        │ 页号                │ (roll_ptr_t)page_no << 16        │
                ├────────────┼────────────┼────────────────────┼──────────────────────────────────┤
                │ bit 0-15   │ 16位        │ 页内偏移量          │ offset (直接使用)                 │
                ├────────────┼────────────┼────────────────────┼──────────────────────────────────┤
                │ 完整公式    │ 56位        │ roll_ptr            │ is_insert<<55 | id<<48 |         │
                │            │            │                    │ page_no<<16 | offset             │
                └────────────┴────────────┴────────────────────┴──────────────────────────────────┘

【回滚指针使用位置 - 写入聚集索引记录】

btr_cur_optimistic_insert() - storage/innobase/btr/btr0cur.cc:2719
│
├── btr_cur_ins_lock_and_undo() - storage/innobase/btr/btr0cur.cc:2400
│   │
│   └── trx_undo_report_row_operation()
│       └── 返回 roll_ptr (见上面构造过程)
│
└── 【将roll_ptr写入聚集索引记录】
    │                                      storage/innobase/btr/btr0cur.cc:2683
    │
    └── row_upd_index_entry_sys_field(entry, index, DATA_ROLL_PTR, roll_ptr)
        │                                  storage/innobase/row/row0upd.cc:1150
        │
        └── 【写入位置】
            └── ┌────────────────────┬────────────────────────┬────────────────────────────────┐
                │ 字段                │ 写入位置                │ 说明                            │
                ├────────────────────┼────────────────────────┼────────────────────────────────┤
                │ DATA_TRX_ID        │ 聚集索引记录系统列1     │ 事务ID (6字节)                  │
                ├────────────────────┼────────────────────────┼────────────────────────────────┤
                │ DATA_ROLL_PTR      │ 聚集索引记录系统列2     │ 回滚指针 (7字节)                │
                ├────────────────────┼────────────────────────┼────────────────────────────────┤
                │ 用途                │ MVCC多版本读取          │ 通过roll_ptr定位历史版本        │
                ├────────────────────┼────────────────────────┼────────────────────────────────┤
                │ 用途                │ 事务回滚                │ 通过roll_ptr找到undo记录回滚    │
                └────────────────────┴────────────────────────┴────────────────────────────────┘
```

### 3. Undo状态(undo->state)说明

```
【undo->state 的设置与用途】

【为什么构造Undo记录时不直接使用undo->state？】

undo->state 是Undo段的状态，不是单条Undo记录的状态。
它用于管理Undo段的生命周期，而不是编码到Undo记录内容中。

trx_undo_assign_undo() - storage/innobase/trx/trx0undo.cc:1757
│
├── 【首次分配时】
│   └── trx_undo_create() - storage/innobase/trx/trx0undo.cc:1534
│       │
│       └── undo->state = TRX_UNDO_ACTIVE
│           │
│           └── 【Undo段状态说明】
│               └── ┌────────────────────┬────────────────────────┬────────────────────────────────┐
│                   │ 状态值              │ 含义                    │ 说明                            │
│                   ├────────────────────┼────────────────────────┼────────────────────────────────┤
│                   │ TRX_UNDO_ACTIVE    │ Undo段正在被事务使用    │ 事务执行DML时设置               │
│                   ├────────────────────┼────────────────────────┼────────────────────────────────┤
│                   │ TRX_UNDO_CACHED    │ Undo段已缓存可复用      │ 小事务提交后缓存                │
│                   ├────────────────────┼────────────────────────┼────────────────────────────────┤
│                   │ TRX_UNDO_PREPARED  │ Undo段处于Prepare状态   │ XA事务Prepare后设置             │
│                   ├────────────────────┼────────────────────────┼────────────────────────────────┤
│                   │ TRX_UNDO_TO_FREE   │ Undo段等待释放          │ Insert Undo提交后设置           │
│                   ├────────────────────┼────────────────────────┼────────────────────────────────┤
│                   │ TRX_UNDO_TO_PURGE  │ Undo段等待Purge         │ Update Undo提交后设置           │
│                   └────────────────────┴────────────────────────┴────────────────────────────────┘
│
├── 【复用缓存Undo段时】
│   └── trx_undo_reuse_cached() - storage/innobase/trx/trx0undo.cc:1450
│       │
│       └── undo->state = TRX_UNDO_ACTIVE  // 从CACHED变为ACTIVE
│
└── 【状态使用场景】
    │
    ├── 事务提交时
    │   └── trx_undo_set_state_at_finish() - storage/innobase/trx/trx0undo.cc:1650
    │       └── undo->state = TRX_UNDO_CACHED / TRX_UNDO_TO_FREE / TRX_UNDO_TO_PURGE
    │
    ├── 事务Prepare时
    │   └── trx_undo_set_state_at_prepare() - storage/innobase/trx/trx0undo.cc:1610
    │       └── undo->state = TRX_UNDO_PREPARED
    │
    └── 崩溃恢复时
        └── trx_resurrect_insert() / trx_resurrect_update()
            └── 根据undo->state恢复事务状态
```

### 4. Undo记录写入Redo日志的调用链

```
【Undo记录写入Redo日志完整调用链】

trx_undo_page_report_insert() - storage/innobase/trx/trx0rec.cc:480
│
├── 【步骤1: 构造Undo记录】
│   └── (写入undo_page的过程，见前面的Undo记录构造表)
│
├── 【步骤2: 更新页头的空闲指针】
│   └── mach_write_to_2(ptr_to_first_free, end_of_rec)
│
└── 【步骤3: 写入Redo日志记录Undo变更】
    │                                      storage/innobase/trx/trx0rec.cc:221
    │
    └── trx_undof_page_add_undo_rec_log() - storage/innobase/trx/trx0rec.cc:70
        │
        ├── 【打开Redo缓冲区】
        │   └── mlog_open(mtr, 11 + 13 + MLOG_BUF_MARGIN, log_ptr)
        │
        ├── 【写入Redo日志头】
        │   └── mlog_write_initial_log_record_fast(undo_page, MLOG_UNDO_INSERT, log_ptr, mtr)
        │       │                              storage/innobase/include/mtr0log.ic:95
        │       │
        │       └── 【Redo日志头结构】
        │           └── ┌────────────┬────────────┬────────────────────┬────────────────────────────┐
        │               │ 字节段      │ 长度        │ 内容                │ 说明                        │
        │               ├────────────┼────────────┼────────────────────┼────────────────────────────┤
        │               │ 0          │ 1字节       │ MLOG_UNDO_INSERT   │ 值=20，Undo插入日志类型     │
        │               ├────────────┼────────────┼────────────────────┼────────────────────────────┤
        │               │ 1          │ 1-5字节     │ space_id           │ Undo表空间ID (压缩编码)     │
        │               ├────────────┼────────────┼────────────────────┼────────────────────────────┤
        │               │ 变长        │ 1-5字节     │ page_no            │ Undo页号 (压缩编码)         │
        │               └────────────┴────────────┴────────────────────┴────────────────────────────┘
        │
        ├── 【写入Undo记录长度】
        │   │                              storage/innobase/trx/trx0rec.cc:89
        │   │
        │   ├── len = new_free - old_free - 4  // 计算Undo记录长度(不含前后指针)
        │   └── mach_write_to_2(log_ptr, len)
        │
        ├── 【写入Undo记录内容】
        │   │                              storage/innobase/trx/trx0rec.cc:92-98
        │   │
        │   ├── (短记录) memcpy(log_ptr, undo_page + old_free + 2, len)
        │   └── (长记录) mlog_catenate_string(mtr, undo_page + old_free + 2, len)
        │
        └── 【关闭Redo缓冲区】
            └── mlog_close(mtr, log_ptr + len)
            
        【Undo的Redo日志完整结构】
        └── ┌────────────────┬────────────┬────────────────────┬────────────────────────────────────┐
            │ 偏移            │ 长度        │ 内容                │ 说明                                │
            ├────────────────┼────────────┼────────────────────┼────────────────────────────────────┤
            │ 0              │ 1字节       │ 20                 │ MLOG_UNDO_INSERT日志类型            │
            ├────────────────┼────────────┼────────────────────┼────────────────────────────────────┤
            │ 1              │ 1-5字节     │ space_id           │ Undo表空间ID                        │
            ├────────────────┼────────────┼────────────────────┼────────────────────────────────────┤
            │ 变长            │ 1-5字节     │ page_no            │ Undo页号                            │
            ├────────────────┼────────────┼────────────────────┼────────────────────────────────────┤
            │ 变长            │ 2字节       │ len                │ Undo记录长度(new_free-old_free-4)   │
            ├────────────────┼────────────┼────────────────────┼────────────────────────────────────┤
            │ 变长            │ len字节     │ undo_record        │ Undo记录内容(跳过2字节后向指针)      │
            └────────────────┴────────────┴────────────────────┴────────────────────────────────────┘
```

### 5. Redo Log Buffer 并发写入机制

```
【Redo Log Buffer 并发写入完整流程】

mtr_t::commit() - storage/innobase/mtr/mtr0mtr.cc:750
│
└── mtr_t::Command::execute() - storage/innobase/mtr/mtr0mtr.cc:680
    │
    └── add_dirty_blocks_to_flush_list() - storage/innobase/mtr/mtr0mtr.cc:600
        │
        └── log_buffer_x_lock_own_lsn(log, start_lsn, end_lsn)
            │
            ├── 【阶段1: 预留空间】
            │   └── log_buffer_reserve() - storage/innobase/log/log0buf.cc:859
            │       │
            │       ├── 【原子预留sn范围】
            │       │   └── log_buffer_s_lock_enter_reserve(log, len)
            │       │       │                          storage/innobase/log/log0buf.cc:780
            │       │       │
            │       │       └── start_sn = log.sn.fetch_add(len)  // 原子操作
            │       │           │
            │       │           └── 【并发控制机制】
            │       │               └── ┌────────────────────┬─────────────────────────────────────────┐
            │       │                   │ 变量                │ 说明                                     │
            │       │                   ├────────────────────┼─────────────────────────────────────────┤
            │       │                   │ log.sn             │ 全局sn计数器，原子递增                    │
            │       │                   ├────────────────────┼─────────────────────────────────────────┤
            │       │                   │ start_sn           │ 当前mtr分配的起始sn                      │
            │       │                   ├────────────────────┼─────────────────────────────────────────┤
            │       │                   │ end_sn             │ start_sn + len                          │
            │       │                   ├────────────────────┼─────────────────────────────────────────┤
            │       │                   │ log.sn_lock_inst   │ S锁保证并发写入期间buffer不被回收         │
            │       │                   └────────────────────┴─────────────────────────────────────────┘
            │       │
            │       ├── 【计算LSN】
            │       │   ├── end_sn = start_sn + len
            │       │   ├── handle.start_lsn = log_translate_sn_to_lsn(start_sn)
            │       │   └── handle.end_lsn = log_translate_sn_to_lsn(end_sn)
            │       │
            │       └── 【检查buffer空间】
            │           └── if (end_sn > log.buf_limit_sn)
            │                   log_wait_for_space_after_reserving(log, handle)
            │
            ├── 【阶段2: 并发写入数据】
            │   └── log_buffer_write() - storage/innobase/log/log0buf.cc:922
            │       │
            │       ├── 【计算写入位置】
            │       │   └── ptr = log.buf + (start_lsn % log.buf_size)
            │       │
            │       ├── 【处理跨块写入】
            │       │   └── while (str_len > 0) {
            │       │           offset = lsn % OS_FILE_LOG_BLOCK_SIZE
            │       │           left = OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE - offset
            │       │           len = min(left, str_len)
            │       │           memcpy(ptr, str, len)  // 关键：并发memcpy
            │       │           ...
            │       │       }
            │       │
            │       └── 【并发写入示意图】
            │           └── ┌─────────────────────────────────────────────────────────────────────────┐
            │               │              Redo Log Buffer 并发写入示意                                │
            │               ├─────────────────────────────────────────────────────────────────────────┤
            │               │                                                                         │
            │               │  log.buf:                                                               │
            │               │  ┌──────────────────────────────────────────────────────────────────┐  │
            │               │  │░░░░░░░░│████████│████████│████████│░░░░░░░░│░░░░░░░░│░░░░░░░░│  │  │
            │               │  └──────────────────────────────────────────────────────────────────┘  │
            │               │           ↑        ↑        ↑        ↑                                 │
            │               │           │        │        │        │                                 │
            │               │       Thread1  Thread2  Thread3  Thread4                               │
            │               │       (sn=100) (sn=150) (sn=200) (sn=250)                              │
            │               │                                                                         │
            │               │  每个线程独占自己的sn范围，可并发写入不同位置                              │
            │               │                                                                         │
            │               │  ░░░ = 已提交   ████ = 正在写入   空白 = 未使用                          │
            │               │                                                                         │
            │               └─────────────────────────────────────────────────────────────────────────┘
            │
            └── 【阶段3: 完成写入并链接】
                └── log_buffer_write_completed() - storage/innobase/log/log0buf.cc:1061
                    │
                    ├── 【内存屏障】
                    │   └── std::atomic_thread_fence(std::memory_order_release)
                    │
                    ├── 【添加链接到recent_written】
                    │   └── log.recent_written.add_link_advance_tail(start_lsn, end_lsn)
                    │       │
                    │       └── 【Link_buf 链接机制】
                    │           └── ┌────────────────────┬─────────────────────────────────────────┐
                    │               │ 结构                │ 说明                                     │
                    │               ├────────────────────┼─────────────────────────────────────────┤
                    │               │ recent_written     │ 环形缓冲区，记录已写入的LSN范围           │
                    │               ├────────────────────┼─────────────────────────────────────────┤
                    │               │ slot计算            │ slot = start_lsn % capacity             │
                    │               ├────────────────────┼─────────────────────────────────────────┤
                    │               │ 链接值              │ slot[start_lsn] = end_lsn               │
                    │               ├────────────────────┼─────────────────────────────────────────┤
                    │               │ 遍历方式            │ log_writer线程从write_lsn开始遍历链接    │
                    │               └────────────────────┴─────────────────────────────────────────┘
                    │
                    └── 【通知log_writer线程】
                        └── log.recent_written.advance_tail()
                            │
                            └── 【log_writer线程行为】
                                └── ┌─────────────────────────────────────────────────────────────────┐
                                    │              Log Writer 线程工作流程                              │
                                    ├─────────────────────────────────────────────────────────────────┤
                                    │                                                                 │
                                    │  log_writer_thread():                                           │
                                    │  while (true) {                                                 │
                                    │      // 等待有新数据写入                                          │
                                    │      ready_lsn = log_buffer_ready_for_write_lsn(log)            │
                                    │                                                                 │
                                    │      // 遍历recent_written链接，找连续的已写入范围               │
                                    │      while (link = recent_written[write_lsn]) {                 │
                                    │          write_lsn = link  // 跟随链接前进                       │
                                    │      }                                                          │
                                    │                                                                 │
                                    │      // 写入磁盘                                                  │
                                    │      log_files_write_buffer(log, write_lsn)                     │
                                    │  }                                                              │
                                    │                                                                 │
                                    │  【关键点】如果某个mtr还未完成写入，链接断开，                     │
                                    │            log_writer会停在断点等待，确保顺序写入               │
                                    │                                                                 │
                                    └─────────────────────────────────────────────────────────────────┘
```

### 6. 并发写入完整时序图

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                              Redo Log Buffer 并发写入时序图                                               │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                         │
│  时间轴 ─────────────────────────────────────────────────────────────────────────────────────────────▶ │
│                                                                                                         │
│  Thread1 (mtr1):                                                                                        │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │ reserve(len=100)    write(sn=100~200)                    write_completed()                         ││
│  │ ───●─────────────────●●●●●●●●●●●●●●●●●●●●●●●●●────────────●─────────────────────────────────────── ││
│  │    │                 │←──────memcpy──────→│               │                                        ││
│  │    │                                                      │                                        ││
│  │    └── sn=100                                             └── link: 100→200                        ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                         │
│  Thread2 (mtr2):                                                                                        │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │      reserve(len=50)       write(sn=200~250)    write_completed()                                  ││
│  │ ─────●───────────────────────●●●●●●●●●●●●●────────●─────────────────────────────────────────────── ││
│  │      │                       │←──memcpy──→│        │                                               ││
│  │      │                                             │                                               ││
│  │      └── sn=200                                    └── link: 200→250                               ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                         │
│  Thread3 (mtr3):                                                                                        │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │           reserve(len=80)              write(sn=250~330)           write_completed()               ││
│  │ ──────────●────────────────────────────●●●●●●●●●●●●●●●●●────────────●───────────────────────────── ││
│  │           │                            │←────memcpy────→│           │                              ││
│  │           │                                                         │                              ││
│  │           └── sn=250                                                └── link: 250→330              ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                         │
│  Log Writer:                                                                                            │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │                                                              遍历链接    写入磁盘                   ││
│  │ ─────────────────────────────────────────────────────────────●──●──●────●●●●●●●●●●●●●●●●●●●●●●●●── ││
│  │                                                              │  │  │    │←─────write to disk─────→│││
│  │                                                              │  │  │                               ││
│  │                                              等待Thread1完成→│  │  └── 330                         ││
│  │                                                              │  └── 250                            ││
│  │                                                              └── 200                               ││
│  │                                              write_lsn: 100 → 200 → 250 → 330                       ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                         │
│  recent_written 状态变化:                                                                                │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │  初始:    [ _ | _ | _ | _ | _ | _ | _ | _ ]                                                        ││
│  │  Thread1: [100→200| _ | _ | _ | _ | _ | _ ]   // 添加链接                                           ││
│  │  Thread2: [100→200|200→250| _ | _ | _ | _ ]   // 添加链接                                           ││
│  │  Thread3: [100→200|200→250|250→330| _ | _ ]   // 添加链接                                           ││
│  │  Writer:  [ √ | √ | √ | _ | _ | _ | _ | _ ]   // 遍历并清除                                         ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

---


---

## 附录补充：Binlog Event生成与B+树查找上锁调用链

**源码版本：Percona Server 8.4.3-3**


### 1. INSERT语句从执行到提交的完整Binlog Event生成调用链

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                     INSERT语句完整Binlog Event序列 (Row格式, 自动提交)                                   │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                         │
│   【执行阶段 - 写入事务缓存】                【提交阶段 - Group Commit写入binlog文件】                     │
│                                                                                                         │
│   ┌─────────────────────────┐               ┌─────────────────────────┐                                 │
│   │ 1. Query_log_event      │               │ 0. Gtid_log_event       │  ← 在flush阶段生成              │
│   │    ("BEGIN")            │               │    (GTID + 时间戳)       │                                 │
│   │    写入trx_cache        │               │    写入binlog文件首位    │                                 │
│   └───────────┬─────────────┘               └─────────────────────────┘                                 │
│               │                                          ↑                                              │
│               ▼                                          │                                              │
│   ┌─────────────────────────┐                           │                                              │
│   │ 2. Rows_query_log_event │  (可选)                    │                                              │
│   │    (原始SQL文本)         │                           │                                              │
│   └───────────┬─────────────┘                           │                                              │
│               │                                          │                                              │
│               ▼                                          │                                              │
│   ┌─────────────────────────┐                           │                                              │
│   │ 3. Table_map_log_event  │                           │   trx_cache内容                               │
│   │    (表结构映射)          │  ─────────────────────────┼──────────────────▶ flush到binlog文件          │
│   └───────────┬─────────────┘                           │                                              │
│               │                                          │                                              │
│               ▼                                          │                                              │
│   ┌─────────────────────────┐                           │                                              │
│   │ 4. Write_rows_log_event │                           │                                              │
│   │    (实际行数据)          │                           │                                              │
│   └───────────┬─────────────┘                           │                                              │
│               │                                          │                                              │
│               ▼                                          │                                              │
│   ┌─────────────────────────┐                           │                                              │
│   │ 5. Xid_log_event        │                           │                                              │
│   │    (事务ID/提交标记)     │                           │                                              │
│   └─────────────────────────┘                           │                                              │
│                                                                                                         │
│   【最终binlog文件中的Event顺序】                                                                        │
│   ┌─────────┬─────────────────────┬───────────────────────────────────────────────────────────────┐    │
│   │ 序号     │ Event类型            │ 内容                                                          │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 1       │ Gtid_log_event      │ GTID + last_committed + sequence_number + 时间戳              │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 2       │ Query_log_event     │ "BEGIN"                                                       │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 3       │ Rows_query_log_event│ 原始INSERT SQL (如果binlog_rows_query_log_events=ON)          │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 4       │ Table_map_log_event │ 表ID + 数据库名 + 表名 + 列信息                                │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 5       │ Write_rows_log_event│ 插入的行数据                                                  │    │
│   ├─────────┼─────────────────────┼───────────────────────────────────────────────────────────────┤    │
│   │ 6       │ Xid_log_event       │ 事务XID (2PC提交标记)                                         │    │
│   └─────────┴─────────────────────┴───────────────────────────────────────────────────────────────┘    │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 1.1 执行阶段：BEGIN Event写入事务缓存

```
【BEGIN Event生成调用链】

binlog_start_trans_and_stmt() - sql/binlog.cc:9914
│
│  【触发时机】首次写入事务缓存时自动调用
│  【调用来源】binlog_write_table_map() / write_locked_table_maps()
│
├── 【初始化缓存管理器】
│   └── thd->binlog_setup_trx_data() - sql/binlog.cc:9920
│       │
│       └── 【创建binlog_cache_mngr】
│           ├── trx_cache  : 事务性DML缓存
│           └── stmt_cache : 非事务性语句缓存
│
├── 【获取事务缓存】
│   └── cache_mngr->get_binlog_cache_data(is_transactional)
│       │                                  sql/binlog.cc:9927
│       │
│       └── 返回 trx_cache (对于InnoDB表)
│
├── 【注册binlog处理器】
│   └── register_binlog_handler(thd, in_multi_stmt_transaction)
│       │                                  sql/binlog.cc:9936
│       │
│       └── 【将binlog注册为事务参与者】
│           └── 后续commit时会调用binlog的prepare/commit
│
└── 【如果缓存为空，写入BEGIN】
    │                                      sql/binlog.cc:9946
    │
    └── if (cache_data->is_binlog_empty()) {
        │
        ├── 【构造BEGIN Query_log_event】
        │   └── Query_log_event qinfo(thd, "BEGIN", 5, is_transactional, 
        │       │                     false, true, 0, true)
        │       │                          sql/binlog.cc:9967
        │       │
        │       └── 【Query_log_event 结构】
        │           └── ┌──────────────────┬──────────────────┬─────────────────────────────┐
        │               │ 字段              │ 值                │ 说明                         │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ event_type       │ QUERY_EVENT (2)  │ 事件类型                      │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ thread_id        │ thd->thread_id   │ 线程ID                        │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ exec_time        │ 0                │ 执行时间                      │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ db_len           │ 数据库名长度       │                             │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ error_code       │ 0                │ 错误码                        │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ status_vars      │ 变长              │ 状态变量                      │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ db               │ 数据库名          │                             │
        │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │ query            │ "BEGIN"          │ SQL语句                      │
        │               └──────────────────┴──────────────────┴─────────────────────────────┘
        │
        └── 【写入事务缓存】
            └── cache_data->write_event(&qinfo)
                │                          sql/binlog.cc:9969
                │
                └── 【追加到IO_CACHE】
                    └── my_b_write(&cache_log, event_data, len)
        }
```

#### 1.2 执行阶段：Table_map + Write_rows Event写入事务缓存

```
【Table_map + Write_rows Event生成调用链】

handler::ha_write_row(buf) - sql/handler.cc:8427
│
├── 【执行存储引擎写入】
│   └── write_row(buf)  // ha_innobase::write_row()
│
└── 【Binlog日志记录】
    │                                          sql/handler.cc:8450
    │
    └── binlog_log_row(table, nullptr, buf, log_func) - sql/handler.cc:8285
        │
        │  【log_func = Write_rows_log_event::binlog_row_logging_function】
        │
        ├── 【收集Writeset (用于并行复制)】
        │   └── add_pke(table, thd, rec) - sql/rpl_write_set_handler.cc:761
        │
        ├── 【写入Table_map Event】
        │   │                                  sql/handler.cc:8338
        │   │
        │   └── write_locked_table_maps(thd)
        │       │
        │       └── thd->binlog_write_table_map() - sql/binlog.cc:9994
        │           │
        │           ├── 【先调用binlog_start_trans_and_stmt (写入BEGIN)】
        │           │   └── binlog_start_trans_and_stmt(this, &the_event)
        │           │       │                      sql/binlog.cc:10008
        │           │       └── (如上所述，写入BEGIN到缓存)
        │           │
        │           ├── 【可选：写入Rows_query_log_event】
        │           │   │                          sql/binlog.cc:10015
        │           │   │
        │           │   └── if (binlog_rows_query && this->query().str) {
        │           │           Rows_query_log_event rows_query_ev(this, query, len);
        │           │           cache_data->write_event(&rows_query_ev);
        │           │       }
        │           │
        │           └── 【写入Table_map_log_event】
        │               │                          sql/binlog.cc:10022
        │               │
        │               ├── 【构造Table_map Event】
        │               │   └── Table_map_log_event the_event(thd, table, table_map_id, is_trans)
        │               │       │                      sql/log_event.cc:10696
        │               │       │
        │               │       └── 【Table_map Event 结构】
        │               │           └── ┌──────────────────┬──────────────────┬─────────────────────────────┐
        │               │               │ 字段              │ 大小              │ 说明                         │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ event_type       │ 1字节             │ TABLE_MAP_EVENT (19)        │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ table_id         │ 6字节             │ 表ID                         │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ flags            │ 2字节             │ 表标志位                      │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ schema_name_len  │ 1字节             │ 数据库名长度                  │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ schema_name      │ 变长              │ 数据库名                      │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ table_name_len   │ 1字节             │ 表名长度                      │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ table_name       │ 变长              │ 表名                         │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ column_count     │ 变长              │ 列数(packed integer)         │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ column_types     │ column_count字节  │ 各列类型                      │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ metadata_length  │ 变长              │ 元数据长度                    │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ metadata         │ 变长              │ 列元数据(精度等)              │
        │               │               ├──────────────────┼──────────────────┼─────────────────────────────┤
        │               │               │ null_bitmap      │ (column_count+7)/8│ NULL位图                     │
        │               │               └──────────────────┴──────────────────┴─────────────────────────────┘
        │               │
        │               └── cache_data->write_event(&the_event)
        │
        └── 【写入Write_rows Event】
            │                                  sql/handler.cc:8349
            │
            └── (*log_func)(thd, table, has_trans, nullptr, buf)
                │
                │  【即 Write_rows_log_event::binlog_row_logging_function】
                │                                  sql/log_event.cc:12018
                │
                └── thd->binlog_write_row() - sql/binlog.cc:11564
                    │
                    ├── 【打包行数据】
                    │   └── pack_row(table, write_set, row_data, record, WRITE_AI)
                    │       │                          sql/log_event.cc:2800
                    │       │
                    │       └── 【行数据格式】
                    │           └── ┌──────────────────┬──────────────────┐
                    │               │ 字段              │ 说明              │
                    │               ├──────────────────┼──────────────────┤
                    │               │ null_bitmap      │ NULL列位图        │
                    │               ├──────────────────┼──────────────────┤
                    │               │ column_values    │ 各列值            │
                    │               └──────────────────┴──────────────────┘
                    │
                    ├── 【获取或创建Write_rows Event】
                    │   └── binlog_prepare_pending_rows_event<Write_rows_log_event>()
                    │       │                      sql/binlog.cc:11268
                    │       │
                    │       └── 【Write_rows Event 结构】
                    │           └── ┌──────────────────┬──────────────────┬─────────────────────────────┐
                    │               │ 字段              │ 大小              │ 说明                         │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ event_type       │ 1字节             │ WRITE_ROWS_EVENT (30)       │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ table_id         │ 6字节             │ 表ID (与Table_map对应)       │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ flags            │ 2字节             │ STMT_END_F等标志            │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ extra_data_len   │ 2字节             │ 额外数据长度                  │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ columns_width    │ 变长              │ 列数                         │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ columns_bitmap   │ (width+7)/8      │ 使用的列位图                  │
                    │               ├──────────────────┼──────────────────┼─────────────────────────────┤
                    │               │ rows_data        │ 变长              │ 多行数据                      │
                    │               └──────────────────┴──────────────────┴─────────────────────────────┘
                    │
                    └── 【添加行数据到Event】
                        └── ev->add_row_data(row_data, len)
                            │                      sql/log_event.cc:11300
                            │
                            └── 【追加到rows_buf缓冲区】
                                └── 同一INSERT多行复用同一个Write_rows_log_event
```

#### 1.3 提交阶段：Xid Event写入事务缓存

```
【Xid_log_event生成调用链】

MYSQL_BIN_LOG::commit() - sql/binlog.cc:8423
│
│  【触发时机】事务提交时调用
│
├── 【检查是否需要记录事务日志】
│   └── trx_stuff_logged = cache_mngr->trx_cache.has_content()
│
└── 【根据事务类型选择结束Event】
    │                                      sql/binlog.cc:8596
    │
    ├── 【XA事务 - 写入XA_prepare_log_event】
    │   └── if (is_loggable_xa) {
    │           XA_prepare_log_event end_evt(thd, xs->get_xid(), one_phase);
    │           cache_mngr->trx_cache.finalize(thd, &end_evt, xs);
    │       }
    │
    ├── 【2PC事务 (多引擎) - 写入Xid_log_event】
    │   │                                  sql/binlog.cc:8596-8599
    │   │
    │   └── else if (real_trans && xid && rw_ha_count > 1 && !no_2pc) {
    │       │
    │       ├── 【构造Xid_log_event】
    │       │   └── Xid_log_event end_evt(thd, xid)
    │       │       │                      sql/binlog.cc:8598
    │       │       │
    │       │       └── 【Xid_log_event 结构】
    │       │           └── ┌──────────────────┬──────────────────┬─────────────────────────────┐
    │       │               │ 字段              │ 大小              │ 说明                         │
    │       │               ├──────────────────┼──────────────────┼─────────────────────────────┤
    │       │               │ event_type       │ 1字节             │ XID_EVENT (16)              │
    │       │               ├──────────────────┼──────────────────┼─────────────────────────────┤
    │       │               │ xid              │ 8字节             │ 事务XID (用于2PC恢复)        │
    │       │               └──────────────────┴──────────────────┴─────────────────────────────┘
    │       │
    │       └── 【写入事务缓存并结束】
    │           └── cache_mngr->trx_cache.finalize(thd, &end_evt)
    │               │                      sql/binlog.cc:8599
    │               │
    │               └── 【finalize操作】
    │                   ├── 将end_evt写入trx_cache
    │                   └── 标记事务缓存完成
    │       }
    │
    └── 【普通事务 - 写入COMMIT Query_log_event】
        │                                  sql/binlog.cc:8608-8611
        │
        └── else {
                Query_log_event end_evt(thd, "COMMIT", 6, true, false, true, 0, true);
                cache_mngr->trx_cache.finalize(thd, &end_evt);
            }
```

#### 1.4 提交阶段：Group Commit - GTID Event生成并写入binlog文件

```
【Group Commit阶段 - GTID生成与binlog写入】

MYSQL_BIN_LOG::ordered_commit() - sql/binlog.cc:9234
│
├── 【Stage #0: 从库提交顺序控制】
│   └── Commit_order_manager::wait_for_its_turn_before_flush_stage(thd)
│
├── 【Stage #1: Flush阶段】
│   │                                      sql/binlog.cc:9286
│   │
│   ├── 【进入Flush队列成为Leader/Follower】
│   │   └── change_stage(thd, BINLOG_FLUSH_STAGE, thd, nullptr, &LOCK_log)
│   │
│   └── 【Leader执行Flush操作】
│       │
│       └── process_flush_stage_queue(&total_bytes, &wait_queue)
│           │                          sql/binlog.cc:9309
│           │
│           └── process_flush_stage_queue() - sql/binlog.cc:8826
│               │
│               ├── 【获取Flush队列所有线程】
│               │   └── fetch_and_process_flush_stage_queue()
│               │       │                      sql/binlog.cc:8838
│               │       │
│               │       └── 【同时刷新InnoDB redo log】
│               │           └── ha_flush_logs(true)  // flush all prepared
│               │
│               ├── 【为每个事务分配GTID】
│               │   │                          sql/binlog.cc:8841
│               │   │
│               │   └── assign_automatic_gtids_to_flush_group(first_seen)
│               │       │                      sql/binlog.cc:1661
│               │       │
│               │       └── for (THD *head = first_seen; head; head = head->next_to_commit) {
│               │           │
│               │           ├── 【获取SIDNO】
│               │           │   └── gtid_state->specify_transaction_sidno(head, locked_sidno_set)
│               │           │
│               │           └── 【生成GNO (自动递增)】
│               │               └── gtid_state->generate_automatic_gtid(head, sidno, gno)
│               │                   │                  sql/binlog.cc:1691
│               │                   │
│               │                   └── 【设置thd->owned_gtid】
│               │                       └── head->owned_gtid = {sidno, gno}
│               │           }
│               │
│               └── 【遍历每个事务，写入binlog文件】
│                   │                          sql/binlog.cc:8843
│                   │
│                   └── for (THD *head = first_seen; head; head = head->next_to_commit) {
│                       │
│                       └── flush_thread_caches(head)
│                           │                  sql/binlog.cc:8732
│                           │
│                           └── cache_mngr->flush(thd, &bytes, &wrote_xid)
│                               │              sql/binlog.cc:8736
│                               │
│                               └── binlog_cache_data::flush() - sql/binlog.cc:1568
│                                   │
│                                   └── 【调用write_transaction写入GTID + 事务内容】
│                                       │
│                                       └── mysql_bin_log.write_transaction(thd, cache_data, &writer, barrier)
│                                           │                  sql/binlog.cc:1726
│                                           │
│                                           ├── 【生成MTS并行复制依赖】
│                                           │   └── m_dependency_tracker.get_dependency(thd, barrier,
│                                           │       │                  sequence_number, last_committed)
│                                           │       │                  sql/binlog.cc:1740
│                                           │       │
│                                           │       └── 【Writeset算法计算last_committed】
│                                           │           └── 返回 sequence_number 和 last_committed
│                                           │
│                                           ├── 【构造Gtid_log_event】
│                                           │   │                  sql/binlog.cc:1838
│                                           │   │
│                                           │   └── Gtid_log_event gtid_event(
│                                           │       │   thd,
│                                           │       │   cache_data->is_trx_cache(),
│                                           │       │   last_committed,
│                                           │       │   sequence_number,
│                                           │       │   may_have_sbr_stmts,
│                                           │       │   original_commit_timestamp,
│                                           │       │   immediate_commit_timestamp,
│                                           │       │   original_server_version,
│                                           │       │   immediate_server_version)
│                                           │       │
│                                           │       └── 【Gtid_log_event 结构】
│                                           │           └── ┌──────────────────┬──────────────────┬─────────────────────────────┐
│                                           │               │ 字段              │ 大小              │ 说明                         │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ event_type       │ 1字节             │ GTID_LOG_EVENT (33)         │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ flags            │ 1字节             │ 事务标志                      │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ uuid             │ 16字节            │ 服务器UUID                    │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ gno              │ 8字节             │ 事务序列号                    │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ logical_clock_ts │ 1字节             │ 逻辑时钟类型标志              │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ last_committed   │ 8字节             │ 并行复制依赖(可并行边界)      │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ sequence_number  │ 8字节             │ 提交序列号                    │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ immediate_ts     │ 7字节             │ 直接提交时间戳(微秒)          │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ original_ts      │ 7字节             │ 原始提交时间戳(微秒)          │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ trx_length       │ 变长              │ 事务总长度(packed)            │
│                                           │               ├──────────────────┼──────────────────┼─────────────────────────────┤
│                                           │               │ server_version   │ 4+4字节           │ 服务器版本(immediate+origin) │
│                                           │               └──────────────────┴──────────────────┴─────────────────────────────┘
│                                           │
│                                           ├── 【写入GTID Event到binlog文件】
│                                           │   └── gtid_event.write(writer)
│                                           │       │                  sql/binlog.cc:1860
│                                           │       │
│                                           │       └── 【GTID是事务第一个Event】
│                                           │
│                                           └── 【写入事务缓存内容到binlog文件】
│                                               └── mysql_bin_log.write_cache(thd, cache_data, writer)
│                                                   │                  sql/binlog.cc:1867
│                                                   │
│                                                   └── 【顺序写入: BEGIN → Table_map → Write_rows → Xid】
│                   }
│
├── 【Stage #2: Sync阶段】
│   │                                      sql/binlog.cc:9352
│   │
│   └── sync_binlog_file(false)  // fsync binlog文件
│
└── 【Stage #3: Commit阶段】
    │                                      sql/binlog.cc:9406
    │
    └── 【调用存储引擎提交】
        └── ha_commit_low(thd, all, run_after_commit)
```


### 2. B+树查找上锁确认插入位置完整调用链

```
【INSERT B+树查找上锁完整调用链】

row_ins_clust_index_entry() - storage/innobase/row/row0ins.cc:3085
│
│  【尝试乐观插入 (只锁叶子页)】
│
└── row_ins_clust_index_entry_low() - storage/innobase/row/row0ins.cc:2362
    │
    │  参数: flags=0, mode=BTR_MODIFY_LEAF
    │
    ├── 【启动Mini-Transaction】
    │   └── mtr_start(&mtr)
    │
    ├── 【设置搜索模式】
    │   └── search_mode = BTR_MODIFY_LEAF | BTR_INSERT
    │       │
    │       └── 【搜索模式说明】
    │           └── ┌────────────────────┬─────────────────────────────────────────────────────┐
    │               │ 模式                │ 说明                                                 │
    │               ├────────────────────┼─────────────────────────────────────────────────────┤
    │               │ BTR_MODIFY_LEAF    │ 获取叶子页X锁，不锁上层                               │
    │               ├────────────────────┼─────────────────────────────────────────────────────┤
    │               │ BTR_INSERT         │ 标记为插入操作，可使用Insert Buffer                   │
    │               ├────────────────────┼─────────────────────────────────────────────────────┤
    │               │ BTR_MODIFY_TREE    │ 获取索引SX锁 + 路径页X锁 (悲观插入)                   │
    │               └────────────────────┴─────────────────────────────────────────────────────┘
    │
    └── 【B+树搜索并定位插入位置】
        │                                  storage/innobase/row/row0ins.cc:2918
        │
        └── btr_cur_search_to_nth_level() - storage/innobase/btr/btr0cur.cc:638
            │
            │  参数: index, level=0, entry, PAGE_CUR_LE, search_mode, &cursor
            │
            ├── 【初始化游标】
            │   ├── cursor->flag = BTR_CUR_BINARY
            │   └── cursor->index = index
            │
            ├── 【尝试AHI (Adaptive Hash Index) 快速查找】
            │   │                              storage/innobase/btr/btr0cur.cc:805
            │   │
            │   └── btr_search_guess_on_hash(tuple, mode, latch_mode, cursor, ...)
            │       │                          storage/innobase/btr/btr0sea.cc:1220
            │       │
            │       ├── 【计算hash值】
            │       │   └── rec_fold = dtuple_fold(tuple, n_fields, n_bytes, index_id)
            │       │
            │       ├── 【在AHI中查找】
            │       │   └── ha_search_and_get_data(table, fold)
            │       │
            │       └── 【如果AHI命中】
            │           ├── 获取页面latch
            │           ├── 验证记录有效性
            │           └── 返回true (跳过B+树遍历)
            │
            ├── 【获取索引根页】
            │   │                              storage/innobase/btr/btr0cur.cc:900
            │   │
            │   ├── 【获取索引锁】(根据latch_mode)
            │   │   └── ┌────────────────────┬─────────────────────────────────────────────────────┐
            │   │       │ latch_mode          │ 索引锁类型                                           │
            │   │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │   │       │ BTR_MODIFY_LEAF    │ 无索引锁(只锁叶子页)                                  │
            │   │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │   │       │ BTR_MODIFY_TREE    │ SX锁(允许读，阻塞写)                                  │
            │   │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │   │       │ BTR_SEARCH_LEAF    │ 无索引锁                                             │
            │   │       └────────────────────┴─────────────────────────────────────────────────────┘
            │   │
            │   └── buf_page_get_gen(space, root_page_no, rw_latch, ...)
            │       │                          storage/innobase/buf/buf0buf.cc:4400
            │       │
            │       └── 【获取根页并加锁】
            │           └── ┌────────────────────┬─────────────────────────────────────────────────────┐
            │               │ 页面操作            │ 说明                                                 │
            │               ├────────────────────┼─────────────────────────────────────────────────────┤
            │               │ buf_pool查找        │ 先在buffer pool中查找页面                            │
            │               ├────────────────────┼─────────────────────────────────────────────────────┤
            │               │ 磁盘读取(如需)      │ 页面不在内存则从磁盘读取                              │
            │               ├────────────────────┼─────────────────────────────────────────────────────┤
            │               │ 加锁(S或X)         │ 根据rw_latch参数决定锁类型                           │
            │               ├────────────────────┼─────────────────────────────────────────────────────┤
            │               │ fix页面            │ 增加页面pin count防止被淘汰                          │
            │               └────────────────────┴─────────────────────────────────────────────────────┘
            │
            ├── 【从根到叶遍历B+树】
            │   │                              storage/innobase/btr/btr0cur.cc:1050
            │   │
            │   └── while (height > 0) {  // height从root_height递减到0
            │       │
            │       ├── 【在当前页内二分查找】
            │       │   └── page_cur_search_with_match(block, index, tuple, mode,
            │       │       │   &up_match, &up_bytes, &low_match, &low_bytes, page_cursor)
            │       │       │                      storage/innobase/page/page0cur.cc:450
            │       │       │
            │       │       ├── 【二分查找过程】
            │       │       │   └── ┌────────────────────┬─────────────────────────────────────────────────────┐
            │       │       │       │ 步骤                │ 说明                                                 │
            │       │       │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │       │       │       │ 1. 获取目录槽       │ page_dir_get_nth_slot()获取页目录                    │
            │       │       │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │       │       │       │ 2. 二分定位槽       │ 在目录槽中二分查找定位范围                            │
            │       │       │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │       │       │       │ 3. 线性扫描记录     │ 在槽范围内线性扫描找到精确位置                         │
            │       │       │       ├────────────────────┼─────────────────────────────────────────────────────┤
            │       │       │       │ 4. 记录匹配信息     │ up_match/low_match记录匹配的字段数                   │
            │       │       │       └────────────────────┴─────────────────────────────────────────────────────┘
            │       │       │
            │       │       └── 【返回游标位置】
            │       │           └── page_cursor指向 <= tuple 的最大记录
            │       │
            │       ├── 【获取子页指针】(非叶子节点)
            │       │   └── node_ptr = page_cur_get_rec(page_cursor)
            │       │       └── child_page_no = btr_node_ptr_get_child_page_no(node_ptr)
            │       │
            │       ├── 【释放当前页锁】(乐观插入模式)
            │       │   └── mtr_release_block_at_savepoint(mtr, savepoint, block)
            │       │
            │       └── 【获取子页并加锁】
            │           └── buf_page_get_gen(space, child_page_no, rw_latch, ...)
            │               │
            │               └── 【锁coupling机制】
            │                   └── ┌────────────────────┬─────────────────────────────────────────────────────┐
            │                       │ 遍历阶段            │ 锁行为                                               │
            │                       ├────────────────────┼─────────────────────────────────────────────────────┤
            │                       │ 非叶子页(乐观)      │ 获取子页锁后释放父页锁(lock coupling)                 │
            │                       ├────────────────────┼─────────────────────────────────────────────────────┤
            │                       │ 非叶子页(悲观)      │ 保持路径上所有页的X锁                                 │
            │                       ├────────────────────┼─────────────────────────────────────────────────────┤
            │                       │ 叶子页              │ 获取X锁并保持                                        │
            │                       └────────────────────┴─────────────────────────────────────────────────────┘
            │       }
            │
            ├── 【叶子页内最终定位】
            │   │                              storage/innobase/btr/btr0cur.cc:1200
            │   │
            │   └── page_cur_search_with_match(block, index, tuple, mode,
            │           &up_match, &up_bytes, &low_match, &low_bytes, page_cursor)
            │       │
            │       └── 【记录匹配结果到游标】
            │           ├── cursor->up_match = up_match      // 向后匹配字段数
            │           ├── cursor->up_bytes = up_bytes
            │           ├── cursor->low_match = low_match    // 向前匹配字段数
            │           └── cursor->low_bytes = low_bytes
            │
            └── 【搜索完成，返回游标】
                │
                └── 【游标状态】
                    └── ┌────────────────────┬─────────────────────────────────────────────────────┐
                        │ 游标字段            │ 说明                                                 │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->page_cur   │ 页内游标，指向插入位置的前一条记录                     │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->block      │ 叶子页block(已X锁)                                   │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->index      │ 索引对象指针                                         │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->up_match   │ 与后一条记录的匹配字段数                              │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->low_match  │ 与当前记录的匹配字段数(用于判重)                      │
                        ├────────────────────┼─────────────────────────────────────────────────────┤
                        │ cursor->flag       │ BTR_CUR_BINARY (二分查找完成)                        │
                        └────────────────────┴─────────────────────────────────────────────────────┘
```

### 3. B+树查找上锁流程示意图

```
┌─────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                              B+树查找上锁流程示意图 (乐观插入 BTR_MODIFY_LEAF)                             │
├─────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                         │
│                                    ┌─────────────────┐                                                  │
│                                    │   Root Page     │                                                  │
│                                    │   (level=2)     │                                                  │
│                                    │   [暂时S锁]     │                                                  │
│                                    └────────┬────────┘                                                  │
│                                             │ page_cur_search_with_match()                              │
│                                             │ 二分查找 → 找到child_page_no                               │
│                                             │ 释放S锁 ↓                                                  │
│                    ┌────────────────────────┼────────────────────────┐                                  │
│                    ▼                        ▼                        ▼                                  │
│          ┌─────────────────┐      ┌─────────────────┐      ┌─────────────────┐                          │
│          │  Internal Page  │      │  Internal Page  │      │  Internal Page  │                          │
│          │   (level=1)     │      │   (level=1)     │      │   (level=1)     │                          │
│          │   [暂时S锁]     │      │   [暂时S锁]     │      │   [暂时S锁]     │                          │
│          └────────┬────────┘      └────────┬────────┘      └────────┬────────┘                          │
│                   │                        │                        │                                   │
│                   │ 二分查找 → 释放S锁      │                        │                                   │
│                   ▼                        ▼                        ▼                                   │
│  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐                   │
│  │ Leaf 0  │ │ Leaf 1  │ │ Leaf 2  │ │ Leaf 3  │ │ Leaf 4  │ │ Leaf 5  │ │ Leaf 6  │                   │
│  │(level=0)│ │(level=0)│ │(level=0)│ │(level=0)│ │(level=0)│ │(level=0)│ │(level=0)│                   │
│  └─────────┘ └─────────┘ └────┬────┘ └─────────┘ └─────────┘ └─────────┘ └─────────┘                   │
│                               │                                                                         │
│                               ▼                                                                         │
│                    ┌──────────────────────────────────────────────┐                                     │
│                    │          目标叶子页 (Leaf 2)                   │                                     │
│                    │          【持有X锁，直到mtr_commit】           │                                     │
│                    ├──────────────────────────────────────────────┤                                     │
│                    │                                              │                                     │
│                    │  page_cur_search_with_match() 二分查找        │                                     │
│                    │                                              │                                     │
│                    │  ┌─────┬─────┬─────┬─────┬─────┬─────┐      │                                     │
│                    │  │infim│rec1 │rec2 │rec3 │rec4 │supre│      │                                     │
│                    │  └─────┴─────┴──┬──┴─────┴─────┴─────┘      │                                     │
│                    │                 │                            │                                     │
│                    │                 └── cursor定位到rec2          │                                     │
│                    │                     (新记录将插入rec2之后)     │                                     │
│                    │                                              │                                     │
│                    │  up_match = 0 (rec3与entry不匹配)            │                                     │
│                    │  low_match = 2 (rec2与entry匹配2个字段)      │                                     │
│                    │                                              │                                     │
│                    └──────────────────────────────────────────────┘                                     │
│                                                                                                         │
│  【锁持有时间线】                                                                                        │
│  ────────────────────────────────────────────────────────────────────────────────▶ 时间                 │
│  Root:    [==S==]                                                                                       │
│  Internal:[      ]==S==]                                                                                │
│  Leaf:    [            ]===================X==================] ← mtr_commit时释放                      │
│                                                                                                         │
└─────────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

### 4. 唯一性检查与行锁调用链

```
【INSERT唯一性检查与行锁调用链】

row_ins_clust_index_entry_low() - storage/innobase/row/row0ins.cc:2362
│
├── 【B+树搜索定位】(见上面的调用链)
│   └── btr_cur_search_to_nth_level()
│
├── 【检查是否存在重复键】
│   │                                      storage/innobase/row/row0ins.cc:2480
│   │
│   └── if (cursor.up_match >= n_unique || cursor.low_match >= n_unique)
│       │
│       │  【有可能重复，需要进一步检查】
│       │
│       └── row_ins_duplicate_error_in_clust() - storage/innobase/row/row0ins.cc:2200
│           │
│           ├── 【获取当前记录】
│           │   └── rec = btr_cur_get_rec(cursor)
│           │
│           ├── 【检查记录是否被删除标记】
│           │   └── rec_get_deleted_flag(rec)
│           │
│           ├── 【对冲突记录加锁】
│           │   │                              storage/innobase/row/row0ins.cc:2280
│           │   │
│           │   └── lock_rec_lock(true, LOCK_S | LOCK_REC_NOT_GAP,
│           │       │             block, heap_no, index, thr)
│           │       │                          storage/innobase/lock/lock0lock.cc:2000
│           │       │
│           │       └── 【行锁类型说明】
│           │           └── ┌──────────────────────┬─────────────────────────────────────────┐
│           │               │ 锁类型                │ 说明                                     │
│           │               ├──────────────────────┼─────────────────────────────────────────┤
│           │               │ LOCK_S               │ 共享锁(检查重复时使用)                   │
│           │               ├──────────────────────┼─────────────────────────────────────────┤
│           │               │ LOCK_X               │ 排他锁(实际插入时使用)                   │
│           │               ├──────────────────────┼─────────────────────────────────────────┤
│           │               │ LOCK_REC_NOT_GAP     │ 只锁记录本身，不锁间隙                   │
│           │               ├──────────────────────┼─────────────────────────────────────────┤
│           │               │ LOCK_GAP             │ 只锁间隙，不锁记录                       │
│           │               ├──────────────────────┼─────────────────────────────────────────┤
│           │               │ LOCK_ORDINARY        │ Next-Key Lock(记录+前向间隙)            │
│           │               └──────────────────────┴─────────────────────────────────────────┘
│           │
│           └── 【返回重复错误或成功】
│               └── return DB_DUPLICATE_KEY 或 DB_SUCCESS
│
└── 【执行插入】(无重复时)
    │                                      storage/innobase/row/row0ins.cc:2540
    │
    └── btr_cur_optimistic_insert() - storage/innobase/btr/btr0cur.cc:2719
        │
        ├── 【检查并加行锁】
        │   └── btr_cur_ins_lock_and_undo() - storage/innobase/btr/btr0cur.cc:2400
        │       │
        │       └── lock_rec_insert_check_and_lock() - storage/innobase/lock/lock0lock.cc:2150
        │           │
        │           ├── 【检查是否需要等待前一条记录的锁】
        │           │   └── lock_rec_other_has_conflicting(LOCK_X | LOCK_GAP, block, heap_no)
        │           │
        │           ├── 【对下一条记录加插入意向锁】(如果需要)
        │           │   └── lock_rec_enqueue_waiting(LOCK_X | LOCK_GAP | LOCK_INSERT_INTENTION,
        │           │       │                        block, next_rec_heap_no)
        │           │       │
        │           │       └── 【等待锁释放】
        │           │           └── return DB_LOCK_WAIT
        │           │
        │           └── 【加锁成功】
        │               └── return DB_SUCCESS
        │
        └── 【执行页内插入】
            └── page_cur_tuple_insert() → page_cur_insert_rec_low()
```

---


---


## 附录：INSERT语句从命令到提交的完整函数调用链整合图（修订版）

**源码版本：Percona Server 8.4.3-3**

> 本图整合了INSERT语句从网络协议解析到最终提交的全流程。
> 所有函数均标注相对路径和行号，事务状态变化、Undo/Redo/Binlog构造均内嵌在流程中。

```
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
┃                                INSERT 语句完整函数调用链整合图 (Percona Server 8.4.3-3)                                                          ┃
┃                                                                                                                                                  ┃
┃  【事务状态】 ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━▶ 时间轴   ┃
┃  trx->state:   [NOT_STARTED]━━━━━━━━━━━━━━━━━[ACTIVE]━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━[PREPARED]━━━━[COMMITTED_IN_MEMORY]           ┃
┃                     ↑                            ↑                                                      ↑              ↑                        ┃
┃                     │                            │                                                      │              │                        ┃
┃                  初始状态              trx_start_low()                                           trx_prepare()    trx_commit()                  ┃
┃                                                                                                                                                  ┃
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛


╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段1: 网络协议解析 (MySQL Protocol Layer)                                                                                                      ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  客户端发送: [COM_QUERY] + "INSERT INTO t VALUES(1,'abc')"                                                                                       ║
║       │                                                                                                                                          ║
║       ▼                                                                                                                                          ║
║  Protocol_classic::read_packet()                              sql/protocol_classic.cc:1408                                                      ║
║       │  【从socket读取数据包到net->read_pos】                                                                                                   ║
║       │                                                                                                                                          ║
║       └──▶ Protocol_classic::get_command()                    sql/protocol_classic.cc:2887                                                      ║
║               │  【解析命令类型: command = net->read_pos[0] = COM_QUERY(0x03)】                                                                  ║
║               │                                                                                                                                  ║
║               └──▶ parse_packet()                             sql/protocol_classic.cc:2834                                                      ║
║                       │  【提取SQL文本】                                                                                                         ║
║                       └── com_data->com_query.query = "INSERT INTO t VALUES(1,'abc')"                                                            ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段2: 命令分发与SQL解析 (Server Layer)                                                                                                         ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  do_command()                                                 sql/sql_parse.cc:1374                                                             ║
║       │  【THD命令处理入口】                                                                                                                     ║
║       │                                                                                                                                          ║
║       └──▶ dispatch_command()                                 sql/sql_parse.cc:1815                                                             ║
║               │                                                                                                                                  ║
║               └──▶ case COM_QUERY:                            sql/sql_parse.cc:2172                                                             ║
║                       │                                                                                                                          ║
║                       └──▶ dispatch_sql_command()             sql/sql_parse.cc:5521                                                             ║
║                               │  【设置 thd->m_query_string】                                                                                    ║
║                               │                                                                                                                  ║
║                               └──▶ parse_sql()                sql/sql_parse.cc:5555                                                             ║
║                                       │  【Bison语法解析，生成AST】                                                                               ║
║                                       │  【生成 lex->sql_command = SQLCOM_INSERT】                                                               ║
║                                       │                                                                                                          ║
║                                       └──▶ mysql_execute_command()           sql/sql_parse.cc:3068                                              ║
║                                               │                                                                                                  ║
║                                               └──▶ case SQLCOM_INSERT:       sql/sql_parse.cc:3876                                              ║
║                                                       │                                                                                          ║
║                                                       └──▶ lex->m_sql_cmd->execute()                                                            ║
║                                                               │  【多态调用 Sql_cmd_insert_values::execute()】                                  ║
║                                                               │                                                                                  ║
║                                                               └──▶ Sql_cmd_dml::execute()            sql/sql_select.cc:676                     ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段3: INSERT执行 (Server Layer → Storage Engine)           【事务状态: NOT_STARTED → ACTIVE】                                                  ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  Sql_cmd_dml::execute()                                       sql/sql_select.cc:676                                                             ║
║       │                                                                                                                                          ║
║       └──▶ Sql_cmd_dml::execute_inner()                       sql/sql_select.cc:1031                                                            ║
║               │                                                                                                                                  ║
║               └──▶ Sql_cmd_insert_values::execute_inner()     sql/sql_insert.cc:478                                                             ║
║                       │                                                                                                                          ║
║                       └──▶ write_record()                     sql/sql_insert.cc:1800                                                            ║
║                               │  【Server层写入记录入口】                                                                                        ║
║                               │                                                                                                                  ║
║                               └──▶ handler::ha_write_row()    sql/handler.cc:8427                                                               ║
║                                       │                                                                                                          ║
║                                       │  ┌───────────────────────────────────────────────────────────────────────────────────────┐              ║
║                                       │  │ 【存储引擎写入 + Binlog记录 并行分支】                                                 │              ║
║                                       │  └───────────────────────────────────────────────────────────────────────────────────────┘              ║
║                                       │                                                                                                          ║
║                                       ├──▶ write_row() [虚函数]                                                                                  ║
║                                       │       │  【调用InnoDB存储引擎】                                                                          ║
║                                       │       │                                                                                                  ║
║                                       │       └──▶ ha_innobase::write_row()   storage/innobase/handler/ha_innodb.cc:9734                        ║
║                                       │               │                                                                                          ║
║                                       │               │  【进入阶段3.1: InnoDB插入流程】                                                         ║
║                                       │               ▼                                                                                          ║
║                                       │       ┌──────────────────────────────────────────────────────────────────────────────────────────────┐  ║
║                                       │       │                      InnoDB 插入流程 (阶段3.1)                                                │  ║
║                                       │       └──────────────────────────────────────────────────────────────────────────────────────────────┘  ║
║                                       │                                                                                                          ║
║                                       └──▶ binlog_log_row()   sql/handler.cc:8285   (执行后调用，见阶段4)                                        ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段3.1: InnoDB插入流程 (事务启动 → B+树定位 → Undo构造 → 数据页插入 → Redo构造)                                                                 ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  ha_innobase::write_row()                                     storage/innobase/handler/ha_innodb.cc:9734                                        ║
║       │                                                                                                                                          ║
║       └──▶ row_insert_for_mysql()                             storage/innobase/row/row0mysql.cc:2183                                            ║
║               │                                                                                                                                  ║
║               └──▶ row_insert_for_mysql_using_ins_graph()     storage/innobase/row/row0mysql.cc:1977                                            ║
║                       │                                                                                                                          ║
║                       ├──▶ trx_start_if_not_started_xa()      storage/innobase/include/trx0trx.h:820                                            ║
║                       │       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓                            ║
║                       │       │  ┃ 【事务状态变化: NOT_STARTED → ACTIVE】                                           ┃                            ║
║                       │       │  ┃                                                                                  ┃                            ║
║                       │       │  ┃  trx_start_low()                  storage/innobase/trx/trx0trx.cc:1338          ┃                            ║
║                       │       │  ┃      │                                                                           ┃                            ║
║                       │       │  ┃      ├── trx->id = trx_sys_allocate_trx_id()  【分配事务ID】                     ┃                            ║
║                       │       │  ┃      ├── trx->state = TRX_STATE_ACTIVE        【设置状态为ACTIVE】               ┃                            ║
║                       │       │  ┃      ├── trx->start_time = time(NULL)         【记录启动时间】                   ┃                            ║
║                       │       │  ┃      ├── trx->undo_no = 0                     【初始化undo序号】                 ┃                            ║
║                       │       │  ┃      └── trx->no = TRX_ID_MAX                 【提交序号初始化为最大值】          ┃                            ║
║                       │       │  ┃                                                                                  ┃                            ║
║                       │       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛                            ║
║                       │       │                                                                                                                  ║
║                       ├──▶ row_get_prebuilt_insert_row()      storage/innobase/row/row0mysql.cc:1700                                            ║
║                       │       │  【准备INSERT graph】                                                                                            ║
║                       │                                                                                                                          ║
║                       ├──▶ row_mysql_convert_row_to_innobase() storage/innobase/row/row0mysql.cc:700                                            ║
║                       │       │  【MySQL行格式转InnoDB格式】                                                                                     ║
║                       │                                                                                                                          ║
║                       └──▶ row_ins_step()                     storage/innobase/row/row0ins.cc:3617                                              ║
║                               │  【INSERT执行图入口】                                                                                            ║
║                               │                                                                                                                  ║
║                               ├──▶ lock_table(LOCK_IX)        storage/innobase/lock/lock0lock.cc:4800                                           ║
║                               │       │  【对表加意向排他锁IX】                                                                                  ║
║                               │                                                                                                                  ║
║                               └──▶ row_ins()                  storage/innobase/row/row0ins.cc:3549                                              ║
║                                       │                                                                                                          ║
║                                       ├──▶ row_ins_alloc_row_id_step()   storage/innobase/row/row0ins.cc:3470                                   ║
║                                       │       │  【如果没有主键，分配隐藏row_id】                                                                ║
║                                       │                                                                                                          ║
║                                       └──▶ row_ins_index_entry_step()    storage/innobase/row/row0ins.cc:3443                                   ║
║                                               │  【对每个索引执行插入】                                                                          ║
║                                               │                                                                                                  ║
║                                               └──▶ row_ins_index_entry() storage/innobase/row/row0ins.cc:3299                                   ║
║                                                       │                                                                                          ║
║                                                       └──▶ row_ins_clust_index_entry()                                                          ║
║                                                               │          storage/innobase/row/row0ins.cc:3085                                   ║
║                                                               │  【聚簇索引插入入口】                                                           ║
║                                                               │                                                                                  ║
║                                                               └──▶ row_ins_clust_index_entry_low()                                              ║
║                                                                       │  storage/innobase/row/row0ins.cc:2362                                   ║
║                                                                       │                                                                          ║
║               ┌───────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────┐  ║
║               │                                                       │                                                                       │  ║
║               ▼                                                       ▼                                                                       ▼  ║
║  ┌─────────────────────────────────────┐  ┌─────────────────────────────────────────────────┐  ┌─────────────────────────────────────────────────┐║
║  │  3.1.1 B+树搜索定位                  │  │  3.1.2 Undo Log构造 (Undo Page修改)             │  │  3.1.3 Data Page插入 + Redo Log构造             │║
║  └─────────────────────────────────────┘  └─────────────────────────────────────────────────┘  └─────────────────────────────────────────────────┘║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝

┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  3.1.1 B+树搜索定位调用链                                                                                                                        │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                                                  │
│  row_ins_clust_index_entry_low()                              storage/innobase/row/row0ins.cc:2362                                              │
│       │                                                                                                                                          │
│       └──▶ btr_cur_search_to_nth_level()                      storage/innobase/btr/btr0cur.cc:638                                               │
│               │  【B+树从根到叶搜索，定位插入位置】                                                                                               │
│               │  参数: index, level=0, entry, PAGE_CUR_LE, BTR_MODIFY_LEAF                                                                       │
│               │                                                                                                                                  │
│               ├──▶ btr_search_guess_on_hash()                 storage/innobase/btr/btr0sea.cc:1220                                              │
│               │       │  【尝试AHI快速查找】                                                                                                     │
│               │                                                                                                                                  │
│               ├──▶ buf_page_get_gen()                         storage/innobase/buf/buf0buf.cc:4400                                              │
│               │       │  【获取B+树页面并加锁】                                                                                                  │
│               │       │  【乐观插入: 只对叶子页加X锁】                                                                                           │
│               │                                                                                                                                  │
│               └──▶ page_cur_search_with_match()               storage/innobase/page/page0cur.cc:450                                             │
│                       │  【页内二分查找定位记录位置】                                                                                            │
│                       │                                                                                                                          │
│                       └── 返回 cursor: 指向插入位置的前一条记录                                                                                  │
│                           cursor->up_match / cursor->low_match 记录匹配字段数                                                                    │
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  3.1.2 Undo Log构造调用链 (Undo Page修改)                                                                                                        │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                                                  │
│  btr_cur_optimistic_insert()                                  storage/innobase/btr/btr0cur.cc:2719                                              │
│       │                                                                                                                                          │
│       └──▶ btr_cur_ins_lock_and_undo()                        storage/innobase/btr/btr0cur.cc:2400                                              │
│               │  【获取锁并生成Undo日志】                                                                                                        │
│               │                                                                                                                                  │
│               ├──▶ lock_rec_insert_check_and_lock()           storage/innobase/lock/lock0lock.cc:2150                                           │
│               │       │  【检查并加行锁】                                                                                                        │
│               │                                                                                                                                  │
│               └──▶ trx_undo_report_row_operation()            storage/innobase/trx/trx0rec.cc:2112                                              │
│                       │  【Undo日志生成入口】                                                                                                    │
│                       │                                                                                                                          │
│                       ├──▶ trx_undo_assign_undo()             storage/innobase/trx/trx0undo.cc:1757                                             │
│                       │       │  【分配Undo段(首次INSERT时)】                                                                                    │
│                       │       │                                                                                                                  │
│                       │       ├── undo->state = TRX_UNDO_ACTIVE                                                                                  │
│                       │       └── trx->rsegs.m_redo.insert_undo = undo                                                                           │
│                       │                                                                                                                          │
│                       ├──▶ buf_page_get_gen()                 storage/innobase/buf/buf0buf.cc:4400                                              │
│                       │       │  【获取Undo页面】                                                                                                │
│                       │                                                                                                                          │
│                       └──▶ trx_undo_page_report_insert()      storage/innobase/trx/trx0rec.cc:480                                               │
│                               │  【在Undo Page上构造INSERT Undo记录】                                                                            │
│                               │                                                                                                                  │
│                               │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓│
│                               │  ┃ 【INSERT Undo Record 结构】                                                                                ┃│
│                               │  ┃                                                                                                            ┃│
│                               │  ┃  ┌──────────────────┬────────────────┬────────────────────────────────────────────────────────────┐       ┃│
│                               │  ┃  │ 字节偏移          │ 内容            │ 说明                                                      │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 0                │ next_record_ptr│ 2字节，指向下一条undo记录                                  │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 2                │ type_cmpl      │ 1字节，TRX_UNDO_INSERT_REC (11)                            │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 3                │ undo_no        │ 可变长度，压缩编码的事务内操作序号                          │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 3+len1           │ table_id       │ 可变长度，压缩编码的表ID                                   │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 3+len1+len2      │ unique_fields  │ 可变长度，主键/唯一索引字段值(用于回滚时定位删除)           │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 末尾-2           │ prev_rec_ptr   │ 2字节，指向前一条undo记录(形成链表)                        │       ┃│
│                               │  ┃  └──────────────────┴────────────────┴────────────────────────────────────────────────────────────┘       ┃│
│                               │  ┃                                                                                                            ┃│
│                               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛│
│                               │                                                                                                                  │
│                               ├──▶ trx_undo_page_set_next_prev_and_add()  storage/innobase/trx/trx0rec.cc:184                                   │
│                               │       │  【维护Undo记录双向链表】                                                                                │
│                               │       │                                                                                                          │
│                               │       └──▶ trx_undof_page_add_undo_rec_log()  storage/innobase/trx/trx0rec.cc:70                                │
│                               │               │  【将Undo Page修改写入Redo Log (MLOG_UNDO_INSERT)】                                              │
│                               │               │                                                                                                  │
│                               │               │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓│
│                               │               │  ┃ 【MLOG_UNDO_INSERT Redo Record 结构】                                                      ┃│
│                               │               │  ┃                                                                                            ┃│
│                               │               │  ┃  ┌──────────────────┬────────────────┬────────────────────────────────────────────┐       ┃│
│                               │               │  ┃  │ 字节偏移          │ 内容            │ 说明                                      │       ┃│
│                               │               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────┤       ┃│
│                               │               │  ┃  │ 0                │ type           │ 1字节，MLOG_UNDO_INSERT (13)              │       ┃│
│                               │               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────┤       ┃│
│                               │               │  ┃  │ 1                │ space_id       │ 可变长度，Undo表空间ID                     │       ┃│
│                               │               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────┤       ┃│
│                               │               │  ┃  │ 1+len1           │ page_no        │ 可变长度，Undo页号                         │       ┃│
│                               │               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────┤       ┃│
│                               │               │  ┃  │ 1+len1+len2      │ len            │ 2字节，Undo记录长度                        │       ┃│
│                               │               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────┤       ┃│
│                               │               │  ┃  │ 3+len1+len2      │ undo_rec       │ 可变长度，Undo记录内容                     │       ┃│
│                               │               │  ┃  └──────────────────┴────────────────┴────────────────────────────────────────────┘       ┃│
│                               │               │  ┃                                                                                            ┃│
│                               │               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛│
│                               │                                                                                                                  │
│                               ├──▶ 【更新事务Undo状态】                                                                                          │
│                               │       ├── trx->undo_no++                      【事务内操作序号递增】                                             │
│                               │       ├── undo->empty = false                 【标记Undo段非空】                                                 │
│                               │       ├── undo->top_page_no = page_no         【记录最新Undo页号】                                               │
│                               │       └── undo->top_offset = offset           【记录最新Undo偏移】                                               │
│                               │                                                                                                                  │
│                               └──▶ trx_undo_build_roll_ptr()              storage/innobase/include/trx0undo.ic:45                               │
│                                       │  【构造回滚指针roll_ptr】                                                                                │
│                                       │                                                                                                          │
│                                       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓│
│                                       │  ┃ 【回滚指针 roll_ptr 结构 (8字节/56位使用)】                                                        ┃│
│                                       │  ┃                                                                                                    ┃│
│                                       │  ┃  ┌──────────────┬────────────┬─────────────────────────────────────────────────────────────┐      ┃│
│                                       │  ┃  │ 位范围        │ 长度        │ 内容说明                                                   │      ┃│
│                                       │  ┃  ├──────────────┼────────────┼─────────────────────────────────────────────────────────────┤      ┃│
│                                       │  ┃  │ 0            │ 1位         │ is_insert标志: 1=INSERT, 0=UPDATE/DELETE                   │      ┃│
│                                       │  ┃  ├──────────────┼────────────┼─────────────────────────────────────────────────────────────┤      ┃│
│                                       │  ┃  │ 1-7          │ 7位         │ rseg_id: 回滚段ID                                          │      ┃│
│                                       │  ┃  ├──────────────┼────────────┼─────────────────────────────────────────────────────────────┤      ┃│
│                                       │  ┃  │ 8-39         │ 32位        │ page_no: Undo页号                                          │      ┃│
│                                       │  ┃  ├──────────────┼────────────┼─────────────────────────────────────────────────────────────┤      ┃│
│                                       │  ┃  │ 40-55        │ 16位        │ offset: Undo记录在页内偏移                                  │      ┃│
│                                       │  ┃  └──────────────┴────────────┴─────────────────────────────────────────────────────────────┘      ┃│
│                                       │  ┃                                                                                                    ┃│
│                                       │  ┃  roll_ptr = (is_insert<<55) | (rseg_id<<48) | (page_no<<16) | offset                              ┃│
│                                       │  ┃                                                                                                    ┃│
│                                       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛│
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  3.1.3 Data Page插入 + Redo Log构造调用链                                                                                                        │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                                                  │
│  btr_cur_optimistic_insert()                                  storage/innobase/btr/btr0cur.cc:2719                                              │
│       │  【(Undo构造完成后) 执行Data Page插入】                                                                                                  │
│       │                                                                                                                                          │
│       └──▶ page_cur_tuple_insert()                            storage/innobase/include/page0cur.ic:187                                          │
│               │  【将dtuple转换为记录并插入】                                                                                                    │
│               │                                                                                                                                  │
│               ├──▶ rec_convert_dtuple_to_rec()                storage/innobase/rem/rem0rec.cc:1050                                              │
│               │       │  【dtuple → 物理记录格式】                                                                                               │
│               │                                                                                                                                  │
│               └──▶ page_cur_insert_rec_low()                  storage/innobase/page/page0cur.cc:1226                                            │
│                       │  【在Data Page上插入记录】                                                                                               │
│                       │                                                                                                                          │
│                       ├──▶ page_mem_alloc_heap()              storage/innobase/page/page0page.cc:590                                            │
│                       │       │  【从页堆分配空间】                                                                                              │
│                       │                                                                                                                          │
│                       ├──▶ rec_copy()                         storage/innobase/rem/rem0rec.cc:450                                               │
│                       │       │  【复制记录到分配的空间】                                                                                        │
│                       │                                                                                                                          │
│                       ├──▶ page_rec_set_next()                storage/innobase/include/page0page.ic:750                                         │
│                       │       │  【更新记录链表指针】                                                                                            │
│                       │                                                                                                                          │
│                       ├──▶ page_header_set_field(PAGE_N_RECS) storage/innobase/include/page0page.ic:220                                         │
│                       │       │  【更新页头记录计数】                                                                                            │
│                       │                                                                                                                          │
│                       └──▶ page_cur_insert_rec_write_log()    storage/innobase/page/page0cur.cc:854                                             │
│                               │  【写入Data Page修改的Redo Log】                                                                                 │
│                               │                                                                                                                  │
│                               ├──▶ mlog_open_and_write_index()    storage/innobase/mtr/mtr0log.cc:800                                           │
│                               │       │  【打开Redo缓冲区，写入索引信息】                                                                        │
│                               │       │                                                                                                          │
│                               │       ├──▶ mlog_open()                storage/innobase/include/mtr0log.ic:35                                    │
│                               │       │       │  【获取MTR日志缓冲区指针】                                                                       │
│                               │       │       │                                                                                                  │
│                               │       │       └──▶ mtr->get_log()->open()                                                                       │
│                               │       │                                                                                                          │
│                               │       └──▶ mlog_write_initial_log_record_fast()  storage/innobase/include/mtr0log.ic:95                         │
│                               │               │  【写入Redo日志头部: type + space_id + page_no】                                                 │
│                               │                                                                                                                  │
│                               │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓│
│                               │  ┃ 【MLOG_REC_INSERT Redo Record 结构】                                                                       ┃│
│                               │  ┃                                                                                                            ┃│
│                               │  ┃  ┌──────────────────┬────────────────┬────────────────────────────────────────────────────────────┐       ┃│
│                               │  ┃  │ 字节偏移          │ 内容            │ 说明                                                      │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 0                │ type           │ 1字节，MLOG_REC_INSERT (67)                               │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 1                │ space_id       │ 可变长度，表空间ID                                        │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 1+len1           │ page_no        │ 可变长度，Data Page页号                                   │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ 1+len1+len2      │ index_info     │ 索引版本、标志、列数等                                     │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ ...              │ cursor_offset  │ 2字节，游标记录在页内偏移                                  │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ ...              │ rec_tail_len   │ 可变长度，记录尾部长度(差异优化)                           │       ┃│
│                               │  ┃  ├──────────────────┼────────────────┼────────────────────────────────────────────────────────────┤       ┃│
│                               │  ┃  │ ...              │ rec_data       │ 可变长度，插入记录的内容(从第一个差异位置开始)             │       ┃│
│                               │  ┃  └──────────────────┴────────────────┴────────────────────────────────────────────────────────────┘       ┃│
│                               │  ┃                                                                                                            ┃│
│                               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛│
│                               │                                                                                                                  │
│                               └──▶ mlog_close()                       storage/innobase/include/mtr0log.ic:55                                    │
│                                       │  【关闭Redo缓冲区】                                                                                      │
│                                                                                                                                                  │
│  【Redo写入Log Buffer调用链】                                                                                                                    │
│  mtr_t::commit()                                              storage/innobase/mtr/mtr0mtr.cc:480                                               │
│       │                                                                                                                                          │
│       └──▶ mtr_t::Command::execute()                          storage/innobase/mtr/mtr0mtr.cc:900                                               │
│               │                                                                                                                                  │
│               └──▶ log_buffer_reserve()                       storage/innobase/log/log0buf.cc:859                                               │
│                       │  【原子获取Log Buffer空间: log.sn.fetch_add(len)】                                                                       │
│                       │  【支持多线程并发写入】                                                                                                  │
│                       │                                                                                                                          │
│                       └──▶ log_buffer_write()                 storage/innobase/log/log0buf.cc:922                                               │
│                               │  【将MTR日志复制到Log Buffer】                                                                                   │
│                               │                                                                                                                  │
│                               └──▶ log_buffer_write_completed()   storage/innobase/log/log0buf.cc:1061                                          │
│                                       │  【标记写入完成，通知Log Writer线程】                                                                    │
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
       │
       │  【InnoDB写入完成，返回到ha_write_row】
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段4: Binlog Event写入事务缓存 (回到Server Layer)                                                                                              ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  handler::ha_write_row()                                      sql/handler.cc:8427                                                               ║
║       │  【InnoDB写入完成后，继续执行Binlog记录】                                                                                                ║
║       │                                                                                                                                          ║
║       └──▶ binlog_log_row()                                   sql/handler.cc:8285                                                               ║
║               │  log_func = Write_rows_log_event::binlog_row_logging_function  (sql/handler.cc:8429)                                             ║
║               │                                                                                                                                  ║
║               ├──▶ check_table_binlog_row_based()             sql/binlog.cc:10874                                                               ║
║               │       │  【检查是否需要记录Row格式Binlog】                                                                                       ║
║               │                                                                                                                                  ║
║               ├──▶ add_pke()                                  sql/rpl_write_set_handler.cc:761                                                  ║
║               │       │  【收集Writeset用于并行复制依赖计算】                                                                                    ║
║               │       │                                                                                                                          ║
║               │       └──▶ generate_hash_pke()                sql/rpl_write_set_handler.cc:676                                                  ║
║               │               │  【计算主键hash添加到thd->rpl_thd_ctx.transaction_write_set】                                                    ║
║               │                                                                                                                                  ║
║               ├──▶ write_locked_table_maps()                  sql/handler.cc:8338                                                               ║
║               │       │                                                                                                                          ║
║               │       └──▶ thd->binlog_write_table_map()      sql/binlog.cc:9994                                                                ║
║               │               │                                                                                                                  ║
║               │               ├──▶ binlog_start_trans_and_stmt()          sql/binlog.cc:9914                                                    ║
║               │               │       │  【首次写入事务缓存时触发】                                                                              ║
║               │               │       │                                                                                                          ║
║               │               │       ├──▶ thd->binlog_setup_trx_data()   sql/binlog.cc:9920                                                    ║
║               │               │       │       │  【初始化binlog_cache_mngr(trx_cache + stmt_cache)】                                             ║
║               │               │       │                                                                                                          ║
║               │               │       ├──▶ register_binlog_handler()      sql/binlog.cc:9936                                                    ║
║               │               │       │       │  【将Binlog注册为事务参与者(2PC)】                                                               ║
║               │               │       │                                                                                                          ║
║               │               │       └──▶ cache_data->write_event(&qinfo)    sql/binlog.cc:9969                                                ║
║               │               │               │  【写入BEGIN Query_log_event到trx_cache】                                                        ║
║               │               │               │                                                                                                  ║
║               │               │               │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓   ║
║               │               │               │  ┃ 【Query_log_event "BEGIN" 结构】                                                          ┃   ║
║               │               │               │  ┃                                                                                           ┃   ║
║               │               │               │  ┃  ┌────────────────┬────────────────┬─────────────────────────────────────────────┐        ┃   ║
║               │               │               │  ┃  │ 字段            │ 大小            │ 内容                                        │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ event_type     │ 1字节           │ QUERY_EVENT (2)                             │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ timestamp      │ 4字节           │ 事件时间戳                                  │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ server_id      │ 4字节           │ 服务器ID                                    │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ event_length   │ 4字节           │ 事件总长度                                  │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ thread_id      │ 4字节           │ 线程ID                                      │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ exec_time      │ 4字节           │ 执行时间(秒)                                │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ db_len         │ 1字节           │ 数据库名长度                                │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ error_code     │ 2字节           │ 错误码(通常0)                               │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ status_vars    │ 可变            │ 状态变量                                    │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ db             │ 可变            │ 数据库名                                    │        ┃   ║
║               │               │               │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤        ┃   ║
║               │               │               │  ┃  │ query          │ 5字节           │ "BEGIN"                                     │        ┃   ║
║               │               │               │  ┃  └────────────────┴────────────────┴─────────────────────────────────────────────┘        ┃   ║
║               │               │               │  ┃                                                                                           ┃   ║
║               │               │               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛   ║
║               │               │                                                                                                                  ║
║               │               ├──▶ Rows_query_log_event (可选)            sql/binlog.cc:10017                                                   ║
║               │               │       │  【binlog_rows_query_log_events=ON时记录原始SQL】                                                        ║
║               │               │                                                                                                                  ║
║               │               └──▶ Table_map_log_event()                  sql/log_event.cc:10696                                                ║
║               │                       │  cache_data->write_event(&the_event)                                                                     ║
║               │                       │                                                                                                          ║
║               │                       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓   ║
║               │                       │  ┃ 【Table_map_log_event 结构】                                                                      ┃   ║
║               │                       │  ┃                                                                                                   ┃   ║
║               │                       │  ┃  ┌────────────────┬────────────────┬─────────────────────────────────────────────┐                ┃   ║
║               │                       │  ┃  │ 字段            │ 大小            │ 内容                                        │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ event_type     │ 1字节           │ TABLE_MAP_EVENT (19)                        │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ table_id       │ 6字节           │ 表ID (本次会话内唯一)                        │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ flags          │ 2字节           │ 表标志位                                    │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ schema_name_len│ 1字节           │ 数据库名长度                                │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ schema_name    │ 可变            │ 数据库名 + '\0'                             │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ table_name_len │ 1字节           │ 表名长度                                    │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ table_name     │ 可变            │ 表名 + '\0'                                 │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ column_count   │ packed int     │ 列数                                        │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ column_types   │ column_count   │ 各列数据类型                                │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ metadata_len   │ packed int     │ 元数据长度                                  │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ metadata       │ 可变            │ 列元数据(精度/长度等)                       │                ┃   ║
║               │                       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                ┃   ║
║               │                       │  ┃  │ null_bitmap    │ (col_cnt+7)/8  │ 可NULL列位图                                │                ┃   ║
║               │                       │  ┃  └────────────────┴────────────────┴─────────────────────────────────────────────┘                ┃   ║
║               │                       │  ┃                                                                                                   ┃   ║
║               │                       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛   ║
║               │                                                                                                                                  ║
║               └──▶ (*log_func)()                              sql/handler.cc:8349                                                               ║
║                       │  【调用 Write_rows_log_event::binlog_row_logging_function】                                                              ║
║                       │                                                                                                                          ║
║                       └──▶ thd->binlog_write_row()            sql/binlog.cc:11564                                                               ║
║                               │                                                                                                                  ║
║                               ├──▶ pack_row()                 sql/log_event.cc:2800                                                             ║
║                               │       │  【打包行数据: null_bitmap + column_values】                                                             ║
║                               │                                                                                                                  ║
║                               ├──▶ binlog_prepare_pending_rows_event<Write_rows_log_event>()                                                    ║
║                               │       │                       sql/binlog.cc:11268                                                               ║
║                               │       │                                                                                                          ║
║                               │       └──▶ new Write_rows_log_event()     sql/log_event.cc:12008                                                ║
║                               │                                                                                                                  ║
║                               │       ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓   ║
║                               │       ┃ 【Write_rows_log_event 结构】                                                                      ┃   ║
║                               │       ┃                                                                                                    ┃   ║
║                               │       ┃  ┌────────────────┬────────────────┬─────────────────────────────────────────────┐                 ┃   ║
║                               │       ┃  │ 字段            │ 大小            │ 内容                                        │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ event_type     │ 1字节           │ WRITE_ROWS_EVENT (30)                       │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ table_id       │ 6字节           │ 表ID (与Table_map对应)                       │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ flags          │ 2字节           │ STMT_END_F等标志                            │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ extra_data_len │ 2字节           │ 额外数据长度                                │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ extra_data     │ 可变            │ 分区ID等额外信息                            │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ columns_width  │ packed int     │ 列数                                        │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ columns_bitmap │ (width+7)/8    │ 写入列位图                                  │                 ┃   ║
║                               │       ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤                 ┃   ║
║                               │       ┃  │ rows_data      │ 可变            │ 行数据(可多行): null_bitmap + values        │                 ┃   ║
║                               │       ┃  └────────────────┴────────────────┴─────────────────────────────────────────────┘                 ┃   ║
║                               │       ┃                                                                                                    ┃   ║
║                               │       ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛   ║
║                               │                                                                                                                  ║
║                               └──▶ ev->add_row_data()         sql/log_event.cc:11300                                                            ║
║                                       │  【将打包的行数据追加到event】                                                                           ║
║                                                                                                                                                  ║
║  【此时trx_cache中的Event序列】                                                                                                                  ║
║  ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ 序号 │ Event类型              │ 来源函数                                  │ 文件位置                                                      │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 1    │ Query_log_event(BEGIN) │ binlog_start_trans_and_stmt()             │ sql/binlog.cc:9967                                            │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 2    │ Rows_query_log_event   │ binlog_write_table_map() (可选)           │ sql/binlog.cc:10017                                           │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 3    │ Table_map_log_event    │ binlog_write_table_map()                  │ sql/binlog.cc:10022                                           │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 4    │ Write_rows_log_event   │ binlog_write_row()                        │ sql/binlog.cc:11581                                           │  ║
║  └──────┴────────────────────────┴───────────────────────────────────────────┴───────────────────────────────────────────────────────────────┘  ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       │  【INSERT执行完成，返回到dispatch_sql_command】
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段5: 事务提交触发 (autocommit=1)                           【事务状态: ACTIVE → (进入提交流程)】                                              ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  dispatch_sql_command()                                       sql/sql_parse.cc:5521                                                             ║
║       │  【INSERT执行完成后，语句级提交】                                                                                                        ║
║       │                                                                                                                                          ║
║       └──▶ trans_commit_stmt()                                sql/transaction.cc:513                                                            ║
║               │  【autocommit=1时触发完整事务提交】                                                                                              ║
║               │                                                                                                                                  ║
║               └──▶ ha_commit_trans()                          sql/handler.cc:1663                                                               ║
║                       │  【事务提交入口，协调各存储引擎】                                                                                        ║
║                       │                                                                                                                          ║
║                       └──▶ MYSQL_BIN_LOG::commit()            sql/binlog.cc:8423                                                                ║
║                               │                                                                                                                  ║
║                               ├──▶ 【写入Xid_log_event到trx_cache】                                                                              ║
║                               │       │                       sql/binlog.cc:8598                                                                ║
║                               │       │                                                                                                          ║
║                               │       ├── Xid_log_event end_evt(thd, xid)                                                                        ║
║                               │       │                                                                                                          ║
║                               │       │   ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓   ║
║                               │       │   ┃ 【Xid_log_event 结构】                                                                          ┃   ║
║                               │       │   ┃                                                                                                 ┃   ║
║                               │       │   ┃  ┌────────────────┬────────────────┬─────────────────────────────────────────────┐              ┃   ║
║                               │       │   ┃  │ 字段            │ 大小            │ 内容                                        │              ┃   ║
║                               │       │   ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤              ┃   ║
║                               │       │   ┃  │ event_type     │ 1字节           │ XID_EVENT (16)                              │              ┃   ║
║                               │       │   ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤              ┃   ║
║                               │       │   ┃  │ timestamp      │ 4字节           │ 事件时间戳                                  │              ┃   ║
║                               │       │   ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤              ┃   ║
║                               │       │   ┃  │ server_id      │ 4字节           │ 服务器ID                                    │              ┃   ║
║                               │       │   ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤              ┃   ║
║                               │       │   ┃  │ event_length   │ 4字节           │ 事件总长度                                  │              ┃   ║
║                               │       │   ┃  ├────────────────┼────────────────┼─────────────────────────────────────────────┤              ┃   ║
║                               │       │   ┃  │ xid            │ 8字节           │ 事务XID (用于2PC崩溃恢复)                   │              ┃   ║
║                               │       │   ┃  └────────────────┴────────────────┴─────────────────────────────────────────────┘              ┃   ║
║                               │       │   ┃                                                                                                 ┃   ║
║                               │       │   ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛   ║
║                               │       │                                                                                                          ║
║                               │       └── cache_mngr->trx_cache.finalize(thd, &end_evt)       sql/binlog.cc:8599                                ║
║                               │               │  【将Xid_log_event写入trx_cache，标记事务缓存完成】                                              ║
║                               │                                                                                                                  ║
║                               └──▶ ordered_commit()                       sql/binlog.cc:9234                                                   ║
║                                       │  【进入Group Commit流程】                                                                                ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
╔══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗
║  阶段6: Group Commit (GTID分配 → Flush → Sync → Engine Commit)  【事务状态: ACTIVE → PREPARED → COMMITTED_IN_MEMORY】                           ║
╠══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╣
║                                                                                                                                                  ║
║  MYSQL_BIN_LOG::ordered_commit()                              sql/binlog.cc:9234                                                                ║
║       │                                                                                                                                          ║
║       │                                                                                                                                          ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║  ║ Stage #0: 从库提交顺序控制 (仅Replica适用)                                                                                                ║    ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║       │                                                                                                                                          ║
║       └──▶ Commit_order_manager::wait_for_its_turn_before_flush_stage()                                                                          ║
║               │                                               sql/rpl_replica_commit_order_manager.cc:148                                        ║
║               │  【从库Worker等待提交顺序】                                                                                                      ║
║       │                                                                                                                                          ║
║       ▼                                                                                                                                          ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║  ║ Stage #1: FLUSH阶段 (刷引擎日志 + 分配GTID + 写Binlog文件)                                                                                ║    ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║       │                                                                                                                                          ║
║       ├──▶ change_stage(BINLOG_FLUSH_STAGE)                   sql/binlog.cc:9286                                                                ║
║       │       │  【Leader等待Follower加入队列，形成组】                                                                                          ║
║       │                                                                                                                                          ║
║       └──▶ process_flush_stage_queue()                        sql/binlog.cc:8826                                                                ║
║               │                                                                                                                                  ║
║               ├──▶ fetch_and_process_flush_stage_queue()      sql/binlog.cc:8784                                                                ║
║               │       │                                                                                                                          ║
║               │       ├──▶ ha_flush_logs(true)                sql/handler.cc:1156                                                               ║
║               │       │       │  【刷InnoDB redo log (保证Prepared状态持久化)】                                                                  ║
║               │       │       │                                                                                                                  ║
║               │       │       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓ ║
║               │       │       │  ┃ 【此时事务状态变化: ACTIVE → PREPARED】                                                                  ┃ ║
║               │       │       │  ┃                                                                                                          ┃ ║
║               │       │       │  ┃  在 innobase_xa_prepare() 中:                                                                            ┃ ║
║               │       │       │  ┃      storage/innobase/handler/ha_innodb.cc:4400                                                          ┃ ║
║               │       │       │  ┃                                                                                                          ┃ ║
║               │       │       │  ┃  └──▶ trx_prepare()                        storage/innobase/trx/trx0trx.cc:3061                          ┃ ║
║               │       │       │  ┃          │                                                                                               ┃ ║
║               │       │       │  ┃          ├── trx->state = TRX_STATE_PREPARED                                                             ┃ ║
║               │       │       │  ┃          ├── undo->state = TRX_UNDO_PREPARED                                                             ┃ ║
║               │       │       │  ┃          └── 写Undo段头部标记为PREPARED                                                                  ┃ ║
║               │       │       │  ┃                                                                                                          ┃ ║
║               │       │       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛ ║
║               │       │                                                                                                                          ║
║               │       └──▶ Commit_stage_manager::process_final_stage_for_ordered_commit_group()                                                 ║
║               │               │                               sql/rpl_commit_stage_manager.cc:350                                                ║
║               │                                                                                                                                  ║
║               ├──▶ assign_automatic_gtids_to_flush_group()    sql/binlog.cc:1661                                                                ║
║               │       │  【为组内每个事务分配GTID】                                                                                              ║
║               │       │                                                                                                                          ║
║               │       └── for (THD *head = first_seen; head; head = head->next_to_commit) {                                                      ║
║               │               │                                                                                                                  ║
║               │               ├──▶ gtid_state->specify_transaction_sidno()    sql/rpl_gtid_state.cc:1150                                        ║
║               │               │       │  【确定SIDNO】                                                                                           ║
║               │               │                                                                                                                  ║
║               │               └──▶ gtid_state->generate_automatic_gtid()      sql/rpl_gtid_state.cc:1200                                        ║
║               │                       │  【分配GNO，设置thd->owned_gtid = {sidno, gno}】                                                         ║
║               │           }                                                                                                                      ║
║               │                                                                                                                                  ║
║               └── for (THD *head = first_seen; head; head = head->next_to_commit) {                                                              ║
║                       │                                       sql/binlog.cc:8843                                                                ║
║                       │                                                                                                                          ║
║                       └──▶ flush_thread_caches(head)          sql/binlog.cc:8732                                                                ║
║                               │                                                                                                                  ║
║                               └──▶ cache_mngr->flush()        sql/binlog.cc:1568                                                                ║
║                                       │                                                                                                          ║
║                                       └──▶ write_transaction()            sql/binlog.cc:1726                                                    ║
║                                               │                                                                                                  ║
║                                               ├──▶ m_dependency_tracker.get_dependency()      sql/rpl_trx_tracking.cc:212                       ║
║                                               │       │  【Writeset算法计算 last_committed, sequence_number】                                   ║
║                                               │                                                                                                  ║
║                                               ├──▶ Gtid_log_event gtid_event(...)             sql/binlog.cc:1838                                ║
║                                               │       │                                                                                          ║
║                                               │       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓   ║
║                                               │       │  ┃ 【Gtid_log_event 结构】                                                          ┃   ║
║                                               │       │  ┃                                                                                  ┃   ║
║                                               │       │  ┃  ┌────────────────┬────────────────┬─────────────────────────────────────────┐   ┃   ║
║                                               │       │  ┃  │ 字段            │ 大小            │ 内容                                    │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ event_type     │ 1字节           │ GTID_LOG_EVENT (33)                     │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ flags          │ 1字节           │ 事务标志(是否包含SBR等)                 │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ uuid           │ 16字节          │ 服务器UUID                              │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ gno            │ 8字节           │ 事务序列号                              │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ ts_type        │ 1字节           │ 时间戳类型标志                          │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ last_committed │ 8字节           │ 并行复制依赖边界                        │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ sequence_number│ 8字节           │ 提交序列号                              │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ immediate_ts   │ 7字节           │ 直接提交时间戳(微秒)                    │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ original_ts    │ 7字节           │ 原始提交时间戳(微秒)                    │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ trx_length     │ packed int     │ 事务总长度                              │   ┃   ║
║                                               │       │  ┃  ├────────────────┼────────────────┼─────────────────────────────────────────┤   ┃   ║
║                                               │       │  ┃  │ server_version │ 4+4字节         │ immediate + original 版本               │   ┃   ║
║                                               │       │  ┃  └────────────────┴────────────────┴─────────────────────────────────────────┘   ┃   ║
║                                               │       │  ┃                                                                                  ┃   ║
║                                               │       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛   ║
║                                               │                                                                                                  ║
║                                               ├──▶ gtid_event.write(writer)               sql/binlog.cc:1860                                    ║
║                                               │       │  【写入GTID Event到binlog文件(事务第一个Event)】                                         ║
║                                               │                                                                                                  ║
║                                               └──▶ write_cache()                          sql/binlog.cc:1867                                    ║
║                                                       │  【写入trx_cache内容到binlog文件】                                                       ║
║                                                       │  【顺序: BEGIN → Table_map → Write_rows → Xid】                                          ║
║                   }                                                                                                                              ║
║       │                                                                                                                                          ║
║       ├──▶ flush_cache_to_file()                              sql/binlog.cc:9312                                                                ║
║       │       │  【将内存中的binlog缓冲区写入文件】                                                                                              ║
║       │                                                                                                                                          ║
║       ▼                                                                                                                                          ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║  ║ Stage #2: SYNC阶段 (fsync Binlog文件)                                                                                                     ║    ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║       │                                                                                                                                          ║
║       ├──▶ change_stage(SYNC_STAGE)                           sql/binlog.cc:9356                                                                ║
║       │                                                                                                                                          ║
║       └──▶ sync_binlog_file()                                 sql/binlog.cc:9381                                                                ║
║               │  【根据sync_binlog设置决定是否fsync】                                                                                            ║
║               │  【sync_binlog=1时每个组都fsync，保证持久化】                                                                                    ║
║       │                                                                                                                                          ║
║       ▼                                                                                                                                          ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║  ║ Stage #3: COMMIT阶段 (存储引擎提交)                        【事务状态: PREPARED → COMMITTED_IN_MEMORY】                                   ║    ║
║  ════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    ║
║       │                                                                                                                                          ║
║       ├──▶ change_stage(COMMIT_STAGE)                         sql/binlog.cc:9420                                                                ║
║       │                                                                                                                                          ║
║       └──▶ process_commit_stage_queue()                       sql/binlog.cc:9428                                                                ║
║               │                                                                                                                                  ║
║               └──▶ ha_commit_low()                            sql/handler.cc:1938                                                               ║
║                       │  【调用各存储引擎commit】                                                                                                ║
║                       │                                                                                                                          ║
║                       └──▶ innobase_commit()                  storage/innobase/handler/ha_innodb.cc:4600                                        ║
║                               │                                                                                                                  ║
║                               └──▶ trx_commit_for_mysql()     storage/innobase/trx/trx0trx.cc:2517                                              ║
║                                       │                                                                                                          ║
║                                       └──▶ trx_commit()       storage/innobase/trx/trx0trx.cc:2300                                              ║
║                                               │                                                                                                  ║
║                                               │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓ ║
║                                               │  ┃ 【最终事务状态变化: PREPARED → COMMITTED_IN_MEMORY】                                     ┃ ║
║                                               │  ┃                                                                                          ┃ ║
║                                               │  ┃  trx_commit_low()                     storage/innobase/trx/trx0trx.cc:2200              ┃ ║
║                                               │  ┃      │                                                                                   ┃ ║
║                                               │  ┃      ├── trx->no = trx_sys_allocate_trx_no()      【分配提交序号】                       ┃ ║
║                                               │  ┃      ├── trx->state = TRX_STATE_COMMITTED_IN_MEMORY                                      ┃ ║
║                                               │  ┃      ├── undo->state = TRX_UNDO_TO_PURGE          【Undo可被Purge】                      ┃ ║
║                                               │  ┃      │                (或TRX_UNDO_CACHED如果可复用)                                      ┃ ║
║                                               │  ┃      └── lock_trx_release_locks()                 【释放所有锁】                         ┃ ║
║                                               │  ┃                                                                                          ┃ ║
║                                               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛ ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
┃                                           INSERT语句执行完成 - 返回客户端OK                                                                       ┃
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛


┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                        【最终Binlog文件中的Event序列】                                                                           │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                                                  │
│  ┌──────┬────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ 序号 │ Event类型                   │ 产生函数及文件位置                                                                                   │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 1    │ Gtid_log_event             │ write_transaction()                                           sql/binlog.cc:1838                    │   │
│  │      │                            │ UUID:GNO + last_committed + sequence_number + timestamps                                             │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 2    │ Query_log_event("BEGIN")   │ binlog_start_trans_and_stmt()                                 sql/binlog.cc:9967                    │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 3    │ Rows_query_log_event (可选)│ binlog_write_table_map()                                      sql/binlog.cc:10017                   │   │
│  │      │                            │ 原始SQL: "INSERT INTO t VALUES(1,'abc')"                                                             │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 4    │ Table_map_log_event        │ binlog_write_table_map()                                      sql/binlog.cc:10022                   │   │
│  │      │                            │ 表ID + 数据库名 + 表名 + 列类型 + 元数据                                                              │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 5    │ Write_rows_log_event       │ binlog_write_row()                                            sql/binlog.cc:11581                   │   │
│  │      │                            │ 表ID + 列位图 + 行数据(null_bitmap + 列值)                                                            │   │
│  ├──────┼────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────┤   │
│  │ 6    │ Xid_log_event              │ MYSQL_BIN_LOG::commit() → finalize()                          sql/binlog.cc:8599                    │   │
│  │      │                            │ 事务XID (用于2PC崩溃恢复识别)                                                                         │   │
│  └──────┴────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                        【事务状态完整流转图】                                                                                    │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                                                                  │
│  ┌─────────────────────┬─────────────────────┬─────────────────────┬──────────────────────────────────────────────────────────────────────────┐ │
│  │ 阶段                 │ trx->state          │ undo->state         │ 触发函数及位置                                                          │ │
│  ├─────────────────────┼─────────────────────┼─────────────────────┼──────────────────────────────────────────────────────────────────────────┤ │
│  │ 初始状态             │ TRX_STATE_NOT_      │ -                   │ 事务对象创建时                                                          │ │
│  │                     │ STARTED             │                     │                                                                          │ │
│  ├─────────────────────┼─────────────────────┼─────────────────────┼──────────────────────────────────────────────────────────────────────────┤ │
│  │ 事务启动             │ TRX_STATE_ACTIVE    │ TRX_UNDO_ACTIVE     │ trx_start_low()        storage/innobase/trx/trx0trx.cc:1338             │ │
│  │ (首次DML时)          │                     │ (首次分配Undo后)    │ trx_undo_assign_undo() storage/innobase/trx/trx0undo.cc:1757            │ │
│  ├─────────────────────┼─────────────────────┼─────────────────────┼──────────────────────────────────────────────────────────────────────────┤ │
│  │ 2PC Prepare         │ TRX_STATE_PREPARED  │ TRX_UNDO_PREPARED   │ trx_prepare()          storage/innobase/trx/trx0trx.cc:3061             │ │
│  │ (Binlog Flush前)     │                     │                     │                                                                          │ │
│  ├─────────────────────┼─────────────────────┼─────────────────────┼──────────────────────────────────────────────────────────────────────────┤ │
│  │ 事务提交             │ TRX_STATE_COMMITTED │ TRX_UNDO_TO_PURGE   │ trx_commit_low()       storage/innobase/trx/trx0trx.cc:2200             │ │
│  │ (Engine Commit后)    │ _IN_MEMORY          │ (或CACHED)          │                                                                          │ │
│  └─────────────────────┴─────────────────────┴─────────────────────┴──────────────────────────────────────────────────────────────────────────┘ │
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘


### 关键函数速查表

| 阶段 | 函数名 | 文件路径 | 行号 | 功能说明 |
|:-----|:-------|:---------|:-----|:---------|
| **协议解析** | `Protocol_classic::read_packet()` | sql/protocol_classic.cc | 1408 | 读取网络数据包 |
| | `Protocol_classic::get_command()` | sql/protocol_classic.cc | 2887 | 解析命令类型 |
| | `parse_packet()` | sql/protocol_classic.cc | 2834 | 提取SQL文本 |
| **命令分发** | `do_command()` | sql/sql_parse.cc | 1374 | 命令处理入口 |
| | `dispatch_command()` | sql/sql_parse.cc | 1815 | 命令分发 |
| | `dispatch_sql_command()` | sql/sql_parse.cc | 5521 | SQL命令分发 |
| | `parse_sql()` | sql/sql_parse.cc | 5555 | SQL语法解析 |
| | `mysql_execute_command()` | sql/sql_parse.cc | 3068 | SQL执行入口 |
| **INSERT执行** | `Sql_cmd_dml::execute()` | sql/sql_select.cc | 676 | DML执行入口 |
| | `Sql_cmd_insert_values::execute_inner()` | sql/sql_insert.cc | 478 | INSERT执行 |
| | `write_record()` | sql/sql_insert.cc | 1800 | 写入记录 |
| | `handler::ha_write_row()` | sql/handler.cc | 8427 | 存储引擎写入接口 |
| **InnoDB写入** | `ha_innobase::write_row()` | storage/innobase/handler/ha_innodb.cc | 9734 | InnoDB写入入口 |
| | `row_insert_for_mysql()` | storage/innobase/row/row0mysql.cc | 2183 | MySQL插入入口 |
| | `row_insert_for_mysql_using_ins_graph()` | storage/innobase/row/row0mysql.cc | 1977 | INSERT graph执行 |
| | `row_ins_step()` | storage/innobase/row/row0ins.cc | 3617 | INSERT执行图 |
| | `row_ins()` | storage/innobase/row/row0ins.cc | 3549 | 行插入 |
| | `row_ins_index_entry_step()` | storage/innobase/row/row0ins.cc | 3443 | 索引项插入 |
| | `row_ins_index_entry()` | storage/innobase/row/row0ins.cc | 3299 | 索引入口 |
| | `row_ins_clust_index_entry()` | storage/innobase/row/row0ins.cc | 3085 | 聚簇索引插入 |
| | `row_ins_clust_index_entry_low()` | storage/innobase/row/row0ins.cc | 2362 | 低层聚簇索引插入 |
| **事务启动** | `trx_start_if_not_started_xa()` | storage/innobase/include/trx0trx.h | 820 | 事务启动入口 |
| | `trx_start_low()` | storage/innobase/trx/trx0trx.cc | 1338 | 启动事务，设置ACTIVE |
| **B+树操作** | `btr_cur_search_to_nth_level()` | storage/innobase/btr/btr0cur.cc | 638 | B+树搜索定位 |
| | `btr_search_guess_on_hash()` | storage/innobase/btr/btr0sea.cc | 1220 | AHI快速查找 |
| | `page_cur_search_with_match()` | storage/innobase/page/page0cur.cc | 450 | 页内二分查找 |
| | `btr_cur_optimistic_insert()` | storage/innobase/btr/btr0cur.cc | 2719 | 乐观插入 |
| | `btr_cur_ins_lock_and_undo()` | storage/innobase/btr/btr0cur.cc | 2400 | 加锁+Undo |
| | `page_cur_tuple_insert()` | storage/innobase/include/page0cur.ic | 187 | 元组插入 |
| | `page_cur_insert_rec_low()` | storage/innobase/page/page0cur.cc | 1226 | 页内记录插入 |
| **Undo Log** | `trx_undo_report_row_operation()` | storage/innobase/trx/trx0rec.cc | 2112 | Undo记录入口 |
| | `trx_undo_assign_undo()` | storage/innobase/trx/trx0undo.cc | 1757 | 分配Undo段 |
| | `trx_undo_page_report_insert()` | storage/innobase/trx/trx0rec.cc | 480 | 构造INSERT Undo |
| | `trx_undo_page_set_next_prev_and_add()` | storage/innobase/trx/trx0rec.cc | 184 | 维护Undo链表 |
| | `trx_undof_page_add_undo_rec_log()` | storage/innobase/trx/trx0rec.cc | 70 | Undo的Redo日志 |
| | `trx_undo_build_roll_ptr()` | storage/innobase/include/trx0undo.ic | 45 | 构造回滚指针 |
| **Redo Log** | `page_cur_insert_rec_write_log()` | storage/innobase/page/page0cur.cc | 854 | 写入INSERT Redo |
| | `mlog_open_and_write_index()` | storage/innobase/mtr/mtr0log.cc | 800 | 打开Redo缓冲区 |
| | `mlog_write_initial_log_record_fast()` | storage/innobase/include/mtr0log.ic | 95 | 写Redo头部 |
| | `log_buffer_reserve()` | storage/innobase/log/log0buf.cc | 859 | 预留Log Buffer |
| | `log_buffer_write()` | storage/innobase/log/log0buf.cc | 922 | 写入Log Buffer |
| | `log_buffer_write_completed()` | storage/innobase/log/log0buf.cc | 1061 | 标记写入完成 |
| **Binlog写入** | `binlog_log_row()` | sql/handler.cc | 8285 | Binlog行日志入口 |
| | `add_pke()` | sql/rpl_write_set_handler.cc | 761 | 收集Writeset |
| | `binlog_start_trans_and_stmt()` | sql/binlog.cc | 9914 | 写入BEGIN Event |
| | `binlog_write_table_map()` | sql/binlog.cc | 9994 | 写入Table_map |
| | `binlog_write_row()` | sql/binlog.cc | 11564 | 写入Write_rows |
| **事务提交** | `trans_commit_stmt()` | sql/transaction.cc | 513 | 语句提交 |
| | `ha_commit_trans()` | sql/handler.cc | 1663 | 事务提交入口 |
| | `MYSQL_BIN_LOG::commit()` | sql/binlog.cc | 8423 | Binlog提交入口 |
| **Group Commit** | `ordered_commit()` | sql/binlog.cc | 9234 | Group Commit主流程 |
| | `process_flush_stage_queue()` | sql/binlog.cc | 8826 | Flush阶段处理 |
| | `assign_automatic_gtids_to_flush_group()` | sql/binlog.cc | 1661 | 分配GTID |
| | `write_transaction()` | sql/binlog.cc | 1726 | 写入GTID+事务 |
| | `sync_binlog_file()` | sql/binlog.cc | 9381 | fsync binlog |
| | `ha_commit_low()` | sql/handler.cc | 1938 | 存储引擎提交 |
| **InnoDB提交** | `innobase_commit()` | storage/innobase/handler/ha_innodb.cc | 4600 | InnoDB提交入口 |
| | `trx_commit_for_mysql()` | storage/innobase/trx/trx0trx.cc | 2517 | MySQL层事务提交 |
| | `trx_commit()` | storage/innobase/trx/trx0trx.cc | 2300 | 事务提交核心 |
| | `trx_commit_low()` | storage/innobase/trx/trx0trx.cc | 2200 | 低层提交 |
| | `trx_prepare()` | storage/innobase/trx/trx0trx.cc | 3061 | 2PC Prepare |
| | `lock_trx_release_locks()` | storage/innobase/lock/lock0lock.cc | 5800 | 释放事务锁 |

---



---

## 深入分析：XID Event和GTID Event的生成时机设计

### 问题背景

从前面的事务提交流程分析中可以看到：
- **XID Event** 是在 `MYSQL_BIN_LOG::commit()` 阶段（ordered_commit之前）写入trx_cache
- **GTID Event** 是在 Group Commit 的 **Flush阶段** 才通过 `assign_automatic_gtids_to_flush_group()` 分配并生成

这与其他binlog events（如 `Write_rows_log_event`、`Table_map_log_event`）在数据页修改后立即写入trx_cache的时机不同。**为什么要这样设计？**

---

### 一、XID Event 为什么在 ordered_commit 前才生成？

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│                              **XID Event 生成时机分析**                                      │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ **时间线对比**                                                                       │   │
│  │                                                                                     │   │
│  │  数据页修改 → BEGIN Event → Table_map → Write_rows → **[此处不能写XID]** → commit()  │   │
│  │       ↓                                                     ↓                        │   │
│  │  (事务可能回滚)                               (确认提交后才写 **Xid_log_event**)       │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因1：事务可能回滚

```c++
// sql/binlog.cc:8598 - XID Event 在确认提交后才写入
Xid_log_event end_evt(thd, xid);
cache_mngr->trx_cache.finalize(thd, &end_evt);
```

如果在数据页修改完成后、两阶段提交前就生成 XID Event：
- 事务在 Prepare 阶段可能因为任何原因失败（死锁、约束冲突、用户取消等）
- 已经写入的 XID Event 会成为"垃圾数据"
- 必须实现复杂的回滚逻辑来清理这个事件

#### 原因2：XID Event 是事务边界标记

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **Binlog 事务结构**                                                                        │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  ┌─────────────────┐                                                                       │
│  │ GTID_log_event  │ ← 事务开始标记                                                        │
│  ├─────────────────┤                                                                       │
│  │ Query("BEGIN")  │                                                                       │
│  ├─────────────────┤                                                                       │
│  │ Table_map_event │                                                                       │
│  ├─────────────────┤                                                                       │
│  │ Write_rows_event│                                                                       │
│  ├─────────────────┤                                                                       │
│  │ **Xid_log_event** │ ← **事务结束标记（必须最后写入）**                                     │
│  └─────────────────┘                                                                       │
│                                                                                            │
│  XID Event 标志着事务的完整结束，必须在所有其他事件之后                                        │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因3：崩溃恢复依赖 XID Event

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **2PC 崩溃恢复逻辑**                                                                       │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  崩溃恢复时的判断逻辑（sql/binlog/recovery.h）：                                             │
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                                                                                     │   │
│  │  InnoDB Prepared 事务  ──┬── binlog 中有对应 XID Event ──► **COMMIT**               │   │
│  │                         │                                                           │   │
│  │                         └── binlog 中无对应 XID Event ──► **ROLLBACK**              │   │
│  │                                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  **如果提前写入 XID Event 会导致：**                                                        │
│  - 事务实际未提交（还在 Prepare 阶段）                                                      │
│  - 但 binlog 中已有 XID Event                                                              │
│  - 崩溃后恢复时会错误地 COMMIT 这个事务                                                      │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 为什么不在数据页修改后、Prepare前写入？

这里需要区分两个概念：
- **XID 值**：在事务开始时就已存在（通过 `query_id` 生成）
- **Xid_log_event**：是一个 binlog 事件，代表事务提交

提前写入 Xid_log_event 的问题：
1. 打破了 binlog 事件的逻辑顺序
2. 无法正确标识事务边界
3. 崩溃恢复逻辑会失效

---

### 二、GTID Event 为什么在 Flush 阶段才分配？

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│                              **GTID 分配时机分析**                                           │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  **关键代码路径** (sql/binlog.cc:8838-8841)：                                               │
│                                                                                            │
│  process_flush_stage_queue()                                                               │
│       │                                                                                    │
│       ├──▶ fetch_and_process_flush_stage_queue()     // 获取队列中的所有事务                │
│       │                                                                                    │
│       ├──▶ **assign_automatic_gtids_to_flush_group()**  // 此时才分配 GTID                 │
│       │         │                                                                          │
│       │         └── for each THD in queue:                                                 │
│       │               gtid_state->generate_automatic_gtid()  // 生成 GTID                  │
│       │                                                                                    │
│       └──▶ flush_thread_caches()                     // 写入 binlog 文件                   │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因1：**GTID 必须连续无空洞**

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **GTID 空洞问题示例**                                                                      │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  **假设：GTID 在事务执行阶段就分配**                                                        │
│                                                                                            │
│  时刻T1: 事务A 开始执行 → 分配 GTID = server_uuid:1                                         │
│  时刻T2: 事务B 开始执行 → 分配 GTID = server_uuid:2                                         │
│  时刻T3: 事务A 遇到死锁 → **回滚**                                                          │
│  时刻T4: 事务B 成功提交 → GTID:2 写入 binlog                                               │
│                                                                                            │
│  **结果：GTID 序列出现空洞（缺少 GTID:1）**                                                 │
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ gtid_executed = {server_uuid:2}     ← 缺少 1，形成空洞                               │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  **影响：**                                                                                 │
│  1. 从库复制时 GTID auto-skip 功能异常                                                      │
│  2. GTID 复制定位失败                                                                      │
│  3. 数据一致性校验失败                                                                      │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **正确做法：Flush 阶段分配 GTID**                                                          │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  时刻T1: 事务A 开始执行（不分配 GTID）                                                       │
│  时刻T2: 事务B 开始执行（不分配 GTID）                                                       │
│  时刻T3: 事务A 遇到死锁 → **回滚**（从未分配过 GTID，无影响）                                │
│  时刻T4: 事务B 进入 Flush 阶段 → **此时才分配 GTID = server_uuid:1**                        │
│  时刻T5: 事务B 成功提交 → GTID:1 写入 binlog                                               │
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │ gtid_executed = {server_uuid:1}     ← 连续无空洞                                     │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因2：**binlog 写入顺序必须与 GTID 顺序一致**

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **GTID 顺序与 Binlog 顺序的关系**                                                          │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  **错误示例：提前分配 GTID**                                                                │
│                                                                                            │
│  事务执行阶段:                    Flush 阶段:                   Binlog 文件:                │
│  ┌──────────────────────┐        ┌──────────────────────┐      ┌──────────────────────┐   │
│  │ T1 → GTID:1          │        │ T2 先进入队列        │      │ GTID:2 (T2)          │   │
│  │ T2 → GTID:2          │  ───►  │ T1 后进入队列        │ ───► │ GTID:1 (T1)          │   │
│  └──────────────────────┘        └──────────────────────┘      └──────────────────────┘   │
│                                                                                            │
│  **问题：GTID 顺序 (1,2) 与 Binlog 顺序 (2,1) 不一致！**                                    │
│                                                                                            │
│  ══════════════════════════════════════════════════════════════════════════════════════    │
│                                                                                            │
│  **正确做法：Flush 阶段分配**                                                               │
│                                                                                            │
│  事务执行阶段:                    Flush 阶段:                   Binlog 文件:                │
│  ┌──────────────────────┐        ┌──────────────────────┐      ┌──────────────────────┐   │
│  │ T1 → (无 GTID)       │        │ T2 先进入队列 → GTID:1│      │ GTID:1 (T2)          │   │
│  │ T2 → (无 GTID)       │  ───►  │ T1 后进入队列 → GTID:2│ ───► │ GTID:2 (T1)          │   │
│  └──────────────────────┘        └──────────────────────┘      └──────────────────────┘   │
│                                                                                            │
│  **队列顺序 = GTID 分配顺序 = Binlog 写入顺序 ✓**                                           │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因3：**并行复制依赖 GTID Event 中的元信息**

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **GTID Event 中的并行复制信息**                                                            │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  Gtid_log_event 结构中包含：                                                               │
│  ┌────────────────────────────────────────────────────────────────────────────────────┐    │
│  │  last_committed   │ 8字节 │ 该事务依赖的最后一个已提交事务的 sequence_number        │    │
│  ├───────────────────┼───────┼────────────────────────────────────────────────────────┤    │
│  │  sequence_number  │ 8字节 │ 该事务自身的序列号                                      │    │
│  └────────────────────────────────────────────────────────────────────────────────────┘    │
│                                                                                            │
│  **计算时机**（sql/binlog.cc:2499-2511）：                                                  │
│                                                                                            │
│  ```                                                                                       │
│  trn_ctx->sequence_number = mysql_bin_log.m_dependency_tracker.step();                     │
│  if (trn_ctx->last_committed == SEQ_UNINIT)                                               │
│      trn_ctx->last_committed = trn_ctx->sequence_number - 1;                              │
│  ```                                                                                       │
│                                                                                            │
│  **这些值只有在确定提交顺序后才能计算，因此必须在 Flush 阶段处理**                           │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

#### 原因4：**Group Commit 性能优化**

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  **批量分配 GTID 的性能优势**                                                               │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  **如果每个事务执行时单独分配 GTID：**                                                       │
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  T1: global_tsid_lock->rdlock() → generate_gtid → unlock()                          │   │
│  │  T2: global_tsid_lock->rdlock() → generate_gtid → unlock()   ← 大量锁竞争           │   │
│  │  T3: global_tsid_lock->rdlock() → generate_gtid → unlock()                          │   │
│  │  ...                                                                                │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  **Flush 阶段批量分配（当前实现）：**                                                        │
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │  assign_automatic_gtids_to_flush_group():                                           │   │
│  │      global_tsid_lock->rdlock()           // 只加一次锁                              │   │
│  │      for each THD in queue:                                                         │   │
│  │          generate_gtid(THD)               // 批量处理                               │   │
│  │      global_tsid_lock->unlock()           // 一次性释放                              │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  **优势：显著减少锁竞争，提高并发吞吐量**                                                    │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 三、如果提前分配会有什么问题？

#### 问题汇总表

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│                              **提前分配的风险分析**                                          │
├──────────────────────┬─────────────────────────────────────────────────────────────────────┤
│ **问题类型**          │ **具体影响**                                                        │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ **XID 提前写入**      │                                                                    │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 崩溃恢复错误          │ InnoDB prepared 事务被错误 commit（binlog 有 XID 但实际未提交）     │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 事务边界混乱          │ XID Event 不再是事务结束标记，无法正确识别事务范围                   │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 回滚处理复杂          │ 需要额外逻辑清理已写入但事务回滚的 XID Event                        │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ **GTID 提前分配**     │                                                                    │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ GTID 空洞             │ 事务回滚导致已分配的 GTID 序列出现间断                              │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 顺序不一致            │ GTID 分配顺序与 binlog 写入顺序不匹配                               │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 并行复制失效          │ last_committed/sequence_number 计算错误，从库并行回放异常           │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 复制定位失败          │ GTID auto-skip 无法正确工作                                        │
├──────────────────────┼─────────────────────────────────────────────────────────────────────┤
│ 锁竞争加剧            │ 失去批量分配的性能优势                                              │
└──────────────────────┴─────────────────────────────────────────────────────────────────────┘
```

#### 关键代码引用

```c++
// sql/sql_class.h:3918-3921 - GTID 所有权生命周期说明
/*
  Generally, transaction ownership starts when the transaction is
  assigned its GTID and ends when the transaction commits or rolls
  back. On a master (GTID_NEXT=AUTOMATIC), the GTID is assigned
  just before binlog flush; on a slave (GTID_NEXT=UUID:NUMBER or
  GTID_NEXT=ANONYMOUS) it is assigned before starting the
  transaction.
*/
```

---

### 四、设计总结

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│                              **Event 生成时机设计原则**                                      │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                         **事务执行阶段**                                             │   │
│  │                                                                                     │   │
│  │  可以写入的 Event:                                                                  │   │
│  │  ✓ Query_log_event("BEGIN")   - 事务开始，可安全回滚                                │   │
│  │  ✓ Table_map_log_event        - 表元信息，可安全回滚                                │   │
│  │  ✓ Write_rows_log_event       - 行数据，可安全回滚                                  │   │
│  │                                                                                     │   │
│  │  **这些 Event 在事务回滚时整个 trx_cache 被丢弃，不会进入 binlog 文件**               │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                 │                                                          │
│                                 ▼                                                          │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                         **Commit 阶段**                                              │   │
│  │                                                                                     │   │
│  │  必须在此阶段写入的 Event:                                                          │   │
│  │  ✓ Xid_log_event              - 事务提交标记，确认提交后才写入                       │   │
│  │                                                                                     │   │
│  │  **此时事务已通过所有检查，确定会提交**                                               │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                 │                                                          │
│                                 ▼                                                          │
│  ┌─────────────────────────────────────────────────────────────────────────────────────┐   │
│  │                         **Flush 阶段**                                               │   │
│  │                                                                                     │   │
│  │  必须在此阶段生成的 Event:                                                          │   │
│  │  ✓ Gtid_log_event             - 确定提交顺序后才分配 GTID                            │   │
│  │                                                                                     │   │
│  │  **此时 Group Commit 队列已形成，可以：**                                             │   │
│  │  - 保证 GTID 连续无空洞                                                             │   │
│  │  - 保证 GTID 顺序与 binlog 顺序一致                                                  │   │
│  │  - 批量分配减少锁竞争                                                               │   │
│  │  - 正确计算 last_committed/sequence_number                                          │   │
│  └─────────────────────────────────────────────────────────────────────────────────────┘   │
│                                                                                            │
│  **核心原则：只有在确定事务一定会提交且顺序已确定后，才能生成影响全局状态的标识符**            │
│                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

---

### 五、相关源码位置索引

| 功能 | 函数 | 文件位置 |
|:-----|:-----|:---------|
| XID Event 写入 | `MYSQL_BIN_LOG::commit()` | sql/binlog.cc:8598 |
| GTID 分配入口 | `assign_automatic_gtids_to_flush_group()` | sql/binlog.cc:1661 |
| GTID 生成 | `Gtid_state::generate_automatic_gtid()` | sql/rpl_gtid_state.cc |
| 并行复制元信息计算 | `binlog_cache_data::flush()` | sql/binlog.cc:2499 |
| 崩溃恢复逻辑 | `binlog::Binlog_recovery::recover()` | sql/binlog/recovery.h |
| GTID 所有权说明 | `THD::owned_gtid` 注释 | sql/sql_class.h:3893 |

