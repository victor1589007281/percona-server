# UPDATE 执行与提交过程中事务状态、Undo、Redo、Binlog 的深度剖析

## 备选标题

1. **MySQL内核揭秘：UPDATE语句如何优雅地修改你的数据**
2. **从源码视角看UPDATE：事务日志的"三剑客"如何协同作战**
3. **数据库工程师必备：UPDATE操作背后的Undo/Redo/Binlog全解析**

---

## 一、开篇引子

> 如果说INSERT是往白纸上写字，那么UPDATE就像在已有文字上修改——不仅要记录新内容（Redo），还要保留旧内容以便反悔（Undo），同时还要在日记本上记录这次修改（Binlog）。更复杂的是，修改可能需要"搬家"——当新数据太大放不下原来的位置时。

UPDATE是数据库中最复杂的DML操作之一。与INSERT只需记录主键不同，UPDATE的Undo Log需要保存**所有被修改字段的旧值**，以便回滚时能恢复数据。同时，UPDATE还可能触发"原地更新"或"删除+插入"两种不同的执行路径。

本文基于 **Percona Server 8.4.3** 源码，深入剖析UPDATE操作的完整执行流程，揭示其背后的复杂机制。

---

## 二、场景展示

### 2.1 UPDATE的两种执行模式

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        UPDATE 两种执行模式对比                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────────────────────┐   ┌─────────────────────────────────┐ │
│  │       原地更新 (In-Place)        │   │    删除标记+插入 (Delete-Mark)   │ │
│  ├─────────────────────────────────┤   ├─────────────────────────────────┤ │
│  │                                 │   │                                 │ │
│  │  ┌─────┐      ┌─────┐          │   │  ┌─────┐      ┌─────┐          │ │
│  │  │ Old │  ──▶ │ New │          │   │  │ Old │      │ Old │ (标记删除)│ │
│  │  │Data │      │Data │          │   │  │Data │  ──▶ │Data │          │ │
│  │  └─────┘      └─────┘          │   │  └─────┘      └──┬──┘          │ │
│  │   同一位置直接修改               │   │                  │              │ │
│  │                                 │   │                  ▼              │ │
│  │  适用条件：                      │   │            ┌─────┐             │ │
│  │  1. 字段长度不变                 │   │            │ New │ (新位置)    │ │
│  │  2. 不涉及主键更新               │   │            │Data │             │ │
│  │  3. 空间足够                    │   │            └─────┘             │ │
│  │                                 │   │                                 │ │
│  │  Undo类型: TRX_UNDO_UPD_EXIST_REC│   │  Undo类型: TRX_UNDO_UPD_DEL_REC │ │
│  │                                 │   │                                 │ │
│  └─────────────────────────────────┘   └─────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 UPDATE事务处理时序图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE 执行与提交时序图                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  时间轴 ─────────────────────────────────────────────────────────────────▶  │
│                                                                             │
│  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐ │
│  │协议 │  │定位 │  │加锁 │  │写入 │  │修改 │  │Prep │  │Binlog│  │Commit│ │
│  │解析 │─▶│记录 │─▶│X Lock│─▶│Undo │─▶│数据 │─▶│are  │─▶│Write │─▶│     │ │
│  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘ │
│     │        │        │        │        │        │        │        │      │
│     ▼        ▼        ▼        ▼        ▼        ▼        ▼        ▼      │
│  com_data  读取旧值  防止并发  保存旧值  产生Redo  状态变为  写入Row   释放锁  │
│  设置      old_rec   修改      update_  Log       PREPARED Event    资源    │
│                              undo分配                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 事务状态变迁与关键字段

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE事务状态与关键字段变化                               │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│    ┌───────────────┐                                                        │
│    │TRX_STATE_     │  关键字段初始值:                                        │
│    │NOT_STARTED    │  - trx->id = 0                                         │
│    └───────┬───────┘  - trx->state = NOT_STARTED                            │
│            │ trx_start_low()                                                │
│            ▼                                                                │
│    ┌───────────────┐  关键字段变化:                                          │
│    │TRX_STATE_     │  - trx->id = 分配唯一事务ID                             │
│    │ACTIVE         │  - trx->no = TRX_ID_MAX (未提交)                        │
│    └───────┬───────┘  - trx->undo_no = 0 → 递增                              │
│            │          - trx->rsegs.m_redo.update_undo = 分配(首次UPDATE)      │
│            │          - old_rec的roll_ptr指向undo记录                         │
│            │                                                                │
│            │ trx_prepare()                                                  │
│            ▼                                                                │
│    ┌───────────────┐  关键字段变化:                                          │
│    │TRX_STATE_     │  - trx->state = TRX_STATE_PREPARED                     │
│    │PREPARED       │  - update_undo->state = TRX_UNDO_PREPARED              │
│    └───────┬───────┘  - 刷Redo Log (lsn)                                    │
│            │                                                                │
│            │ trx_commit()                                                   │
│            ▼                                                                │
│    ┌───────────────┐  关键字段变化:                                          │
│    │TRX_STATE_     │  - trx->state = COMMITTED_IN_MEMORY                    │
│    │COMMITTED      │  - trx->no = 分配提交序号                               │
│    └───────────────┘  - update_undo加入history list                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 三、原理深入

### 3.1 UPDATE的Undo Log结构

UPDATE的Undo Log比INSERT复杂得多，需要记录：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE Undo Record 结构详解                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                        Undo Record Header                              │ │
│  ├──────────┬──────────┬──────────┬──────────┬────────────────────────────┤ │
│  │Next Ptr  │Type+     │Undo No   │Table ID  │info_bits + trx_id +        │ │
│  │(2 bytes) │Cmpl Info │(压缩)    │(压缩)    │roll_ptr(旧版本指针)         │ │
│  └──────────┴──────────┴──────────┴──────────┴────────────────────────────┘ │
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                        主键字段 + 修改字段                              │ │
│  ├──────────────────────┬─────────────────────────────────────────────────┤ │
│  │ 主键字段值           │ 被修改字段的【旧值】(用于回滚恢复)                │ │
│  │ (定位记录用)         │ n_fields + field_no + old_value ...              │ │
│  └──────────────────────┴─────────────────────────────────────────────────┘ │
│                                                                             │
│  Type值说明：                                                                │
│  ┌────────────────────────┬────────────────────────────────────────────┐   │
│  │ TRX_UNDO_UPD_EXIST_REC │ 值=12, 更新已存在的非删除记录 (原地更新)      │   │
│  │ TRX_UNDO_UPD_DEL_REC   │ 值=13, 更新被删除标记的记录 (恢复后更新)      │   │
│  │ TRX_UNDO_DEL_MARK_REC  │ 值=14, 删除标记操作                          │   │
│  └────────────────────────┴────────────────────────────────────────────┘   │
│                                                                             │
│  【Undo Log样例】UPDATE t SET name='new' WHERE id=100                       │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ next_ptr: 0x0180 | type: 0x0C (UPD_EXIST) | undo_no: 2               │  │
│  │ table_id: 1234 | info_bits: 0x00                                     │  │
│  │ old_trx_id: 1000 | old_roll_ptr: 0x... (指向更早版本)                 │  │
│  │ pk_len: 4 | pk_value: 100                                            │  │
│  │ n_updated: 1 | field_no: 1 | old_len: 4 | old_value: 'old'          │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 UPDATE的Redo Log

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE Redo Log 类型                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【原地更新场景】                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │ MLOG_REC_UPDATE_IN_PLACE                                               │ │
│  │ - space_id: 表空间ID                                                   │ │
│  │ - page_no: 页号                                                        │ │
│  │ - slot_no: 记录槽位                                                    │ │
│  │ - offset: 修改位置偏移                                                  │ │
│  │ - old_data + new_data: 修改前后的数据                                   │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  【非原地更新场景】(删除+插入)                                                │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │ 1. MLOG_REC_CLUST_DELETE_MARK  (标记删除旧记录)                         │ │
│  │ 2. MLOG_REC_INSERT             (插入新记录)                            │ │
│  │ 3. MLOG_UNDO_INSERT            (Undo页写入)                            │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  【Redo Log样例】                                                           │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ LSN: 123456800 | Type: MLOG_REC_UPDATE_IN_PLACE | Space: 5           │  │
│  │ Page: 100 | Index: PRIMARY | Slot: 5                                 │  │
│  │ Offset: 20 | Old: [4 bytes] | New: [4 bytes]                        │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.3 UPDATE的Binlog Event序列

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE的Binlog Event序列                                 │
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
│   │ table_id: 123      │     sql/binlog.cc:10005                           │
│   │ db: test           │                                                    │
│   │ table: users       │                                                    │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │UPDATE_ROWS_EVENT   │ ◀── 包含修改前后的行数据                            │
│   │ table_id: 123      │     sql/binlog.cc:11600 THD::binlog_update_row    │
│   │ before_image:      │     TYPE_CODE = UPDATE_ROWS_EVENT                 │
│   │   [(1,'old')]      │                                                    │
│   │ after_image:       │                                                    │
│   │   [(1,'new')]      │                                                    │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │ XID_EVENT          │ ◀── 事务提交                                       │
│   │ xid: 12346         │                                                    │
│   └────────────────────┘                                                    │
│                                                                             │
│  【Binlog样例】mysqlbinlog输出:                                             │
│  ### UPDATE test.users                                                      │
│  ### WHERE                                                                  │
│  ###   @1=100 /* INT meta=0 nullable=0 is_null=0 */                        │
│  ###   @2='old' /* STRING(100) meta=... */                                 │
│  ### SET                                                                    │
│  ###   @1=100 /* INT meta=0 nullable=0 is_null=0 */                        │
│  ###   @2='new' /* STRING(100) meta=... */                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.4 MVCC版本链

UPDATE操作会形成版本链，支持MVCC读取：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE形成的MVCC版本链                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌─────────────────┐    roll_ptr    ┌─────────────────┐    roll_ptr        │
│  │ 数据页最新记录   │ ──────────────▶│ Undo Log V2     │ ──────────────▶    │
│  │                 │                │                 │                    │
│  │ id=1, name='C'  │                │ name='B'        │    ┌────────────┐  │
│  │ trx_id=103      │                │ trx_id=102      │    │Undo Log V1 │  │
│  │ roll_ptr=xxx    │                │ roll_ptr=yyy    │    │name='A'    │  │
│  └─────────────────┘                └─────────────────┘    │trx_id=101  │  │
│         │                                  │               └────────────┘  │
│         │                                  │                     │         │
│         ▼                                  ▼                     ▼         │
│     当前事务看到                       trx_id=102              trx_id=101   │
│     最新值'C'                          看到'B'                  看到'A'    │
│                                                                             │
│  【版本可见性判断】                                                          │
│  1. 如果记录的trx_id < ReadView.min_trx_id → 可见                          │
│  2. 如果记录的trx_id > ReadView.max_trx_id → 不可见，沿roll_ptr找旧版本    │
│  3. 如果记录的trx_id在活跃事务列表中 → 不可见，沿roll_ptr找旧版本           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、源码根因揭秘

**源码版本：Percona Server 8.4.3-3**

### 4.1 完整函数调用链（从协议解析开始）

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              UPDATE 完整执行函数调用链（协议层 → 存储引擎层）                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【阶段1：MySQL协议解析】                                                    │
│  Protocol_classic::read_packet()           sql/protocol_classic.cc:1408    │
│  └── Protocol_classic::get_command()       sql/protocol_classic.cc:2887    │
│      └── parse_packet() for COM_QUERY      sql/protocol_classic.cc:2834    │
│          │  提取SQL文本: "UPDATE t SET name='new' WHERE id=1"              │
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
│      └── Sql_cmd_update::execute()         sql/sql_update.cc:600           │
│          │  case SQLCOM_UPDATE:                                             │
│          └── Sql_cmd_update::update_single_table()                         │
│                                            sql/sql_update.cc:800           │
│                                                                             │
│  【阶段4：行更新循环】                                                       │
│  Sql_cmd_update::update_single_table()     sql/sql_update.cc:800           │
│  └── while (iterator->Read() == 0)         // 逐行读取                     │
│      └── handler::ha_update_row()          sql/handler.cc:8350             │
│          │  字段变化:                                                       │
│          │    - table->record[0] = 新值                                     │
│          │    - table->record[1] = 旧值                                     │
│          │                                                                  │
│          └── ha_innobase::update_row()     storage/innobase/handler/       │
│              │                             ha_innodb.cc:9200               │
│              │  字段变化: prebuilt->upd_node设置                            │
│              │                                                              │
│  【阶段5：InnoDB行更新】                                                     │
│  row_update_for_mysql()                    storage/innobase/row/           │
│  │                                         row0mysql.cc:2500               │
│  │  字段变化: trx->undo_no++ (每次DML递增)                                  │
│  │                                                                          │
│  └── row_upd_clust_step()                  storage/innobase/row/           │
│      │                                     row0upd.cc:2700                 │
│      │                                                                      │
│      └── row_upd_clust_rec()               storage/innobase/row/           │
│          │                                 row0upd.cc:2852                 │
│          │                                                                  │
│          ├── 【Undo Log生成】                                               │
│          │   trx_undo_report_row_operation()                               │
│          │   │                             storage/innobase/trx/           │
│          │   │                             trx0rec.cc:2146                 │
│          │   │  op_type = TRX_UNDO_MODIFY_OP                               │
│          │   │  字段变化:                                                   │
│          │   │    - trx->rsegs.m_redo.update_undo 分配(首次)                │
│          │   │    - roll_ptr 生成并写入记录                                  │
│          │   │                                                              │
│          │   └── trx_undo_page_report_modify()                             │
│          │                                 storage/innobase/trx/           │
│          │                                 trx0rec.cc:1200                 │
│          │                                                                  │
│          ├── 【原地更新路径】                                               │
│          │   btr_cur_update_in_place()     storage/innobase/btr/           │
│          │                                 btr0cur.cc:3200                 │
│          │                                                                  │
│          └── 【非原地更新路径】                                             │
│              btr_cur_pessimistic_update()  storage/innobase/btr/           │
│              │                             btr0cur.cc:3800                 │
│              ├── btr_cur_del_mark_set_clust_rec() // 标记删除旧记录        │
│              └── btr_cur_insert_if_possible()     // 插入新记录            │
│                                                                             │
│  【阶段6：Binlog缓存写入】(DML执行时)                                        │
│  handler::ha_update_row()                  sql/handler.cc:8350             │
│  └── binlog_log_row()                      sql/handler.cc:8000             │
│      ├── THD::binlog_write_table_map()     sql/binlog.cc:10005             │
│      └── THD::binlog_update_row()          sql/binlog.cc:11600             │
│          └── binlog_prepare_pending_rows_event<Update_rows_log_event>      │
│              // before_record = record[1], after_record = record[0]        │
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
│  └── ... UPDATE执行完成 ...                                                 │
│                                                                             │
│  dispatch_sql_command()                    sql/sql_parse.cc:5521           │
│  └── trans_commit_stmt()                   sql/sql_parse.cc:5202           │
│      │                                     sql/transaction.cc:513          │
│      │  字段变化:                                                           │
│      │    - thd->transaction->is_active(STMT) 检查                         │
│      │                                                                      │
│  【进入提交流程】(与INSERT相同)                                              │
│  ha_commit_trans()                         sql/handler.cc:1663             │
│  └── MYSQL_BIN_LOG::commit()               sql/binlog.cc:8423              │
│      └── MYSQL_BIN_LOG::ordered_commit()   sql/binlog.cc:9234              │
│          │                                                                  │
│          ├── 【Stage 1】FLUSH阶段                                          │
│          │   process_flush_stage_queue()                                   │
│          │   └── flush_thread_caches()     // 写Binlog cache               │
│          │       // 写入TABLE_MAP + UPDATE_ROWS_EVENT                      │
│          │                                                                  │
│          ├── 【Stage 2】SYNC阶段                                           │
│          │   sync_binlog_file()            // fsync Binlog                 │
│          │                                                                  │
│          └── 【Stage 3】COMMIT阶段                                         │
│              ha_commit_low()               sql/handler.cc:1938             │
│              └── trx_commit_for_mysql()    storage/innobase/trx/           │
│                  │                         trx0trx.cc:2517                 │
│                  │  字段变化:                                               │
│                  │    - trx->state → COMMITTED_IN_MEMORY                   │
│                  │    - update_undo加入history list (供purge清理)          │
│                  │                                                          │
│                  └── trx_commit()          storage/innobase/trx/           │
│                                            trx0trx.cc:2300                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 UPDATE Undo Log生成核心代码

```c
// storage/innobase/trx/trx0rec.cc:1200-1350
/** Reports a modify (UPDATE/DELETE) operation to undo log.
 @return offset of the undo record if succeed, 0 if fail */
static ulint trx_undo_page_report_modify(
    page_t *undo_page,           /*!< in: undo log page */
    trx_t *trx,                  /*!< in: transaction */
    dict_index_t *index,         /*!< in: clustered index */
    const rec_t *rec,            /*!< in: record being modified */
    const ulint *offsets,        /*!< in: record offsets */
    const upd_t *update,         /*!< in: update vector */
    ulint cmpl_info,             /*!< in: complete info */
    mtr_t *mtr)                  /*!< in: mtr */
{
  byte *ptr;
  ulint first_free;
  trx_id_t trx_id;
  
  first_free = mach_read_from_2(undo_page + TRX_UNDO_PAGE_HDR + 
                                TRX_UNDO_PAGE_FREE);
  ptr = undo_page + first_free;
  
  ptr += 2;  // 预留next指针空间
  
  // 【关键】写入记录类型 (TRX_UNDO_UPD_EXIST_REC 或其他)
  ulint type_cmpl = update ? TRX_UNDO_UPD_EXIST_REC : TRX_UNDO_DEL_MARK_REC;
  *ptr++ = (byte)(type_cmpl | cmpl_info);
  
  // 【关键】写入undo_no
  ptr += mach_u64_write_much_compressed(ptr, trx->undo_no);
  
  // 写入table_id
  ptr += mach_u64_write_much_compressed(ptr, index->table->id);
  
  // 【关键】写入info_bits (记录状态位)
  *ptr++ = (byte)rec_get_info_bits(rec, dict_table_is_comp(index->table));
  
  // 【关键】写入旧的trx_id (用于版本链)
  trx_id = trx_read_trx_id(rec + DATA_TRX_ID_OFFSET);
  ptr += mach_u64_write_compressed(ptr, trx_id);
  
  // 【关键】写入旧的roll_ptr (指向更早的版本)
  roll_ptr_t roll_ptr = trx_read_roll_ptr(rec + DATA_ROLL_PTR_OFFSET);
  ptr += mach_u64_write_compressed(ptr, roll_ptr);
  
  // 【关键】写入主键字段值
  for (i = 0; i < dict_index_get_n_unique(index); i++) {
    // ... 写入主键各字段
  }
  
  // 【关键】写入被修改字段的旧值
  if (update) {
    // 写入修改字段数量
    ptr += mach_write_compressed(ptr, upd_get_n_fields(update));
    
    for (i = 0; i < upd_get_n_fields(update); i++) {
      upd_field_t *upd_field = upd_get_nth_field(update, i);
      ulint field_no = upd_field->field_no;
      
      // 写入字段编号
      ptr += mach_write_compressed(ptr, field_no);
      
      // 【关键】写入旧值
      ulint len = rec_offs_nth_size(offsets, field_no);
      ptr += mach_write_compressed(ptr, len);
      memcpy(ptr, rec_get_nth_field(rec, offsets, field_no, &len), len);
      ptr += len;
    }
  }
  
  return trx_undo_page_set_next_prev_and_add(undo_page, ptr, mtr);
}
```

### 4.4 原地更新核心代码

```c
// storage/innobase/row/row0upd.cc:2852-2916
/** Updates a clustered index record of a row when the ordering fields
do not change. */
static dberr_t row_upd_clust_rec(
    ulint flags,         /*!< in: undo logging and locking flags */
    upd_node_t *node,    /*!< in: row update node */
    dict_index_t *index, /*!< in: clustered index */
    ulint *offsets,      /*!< in: rec_get_offsets() */
    mem_heap_t **offsets_heap,
    que_thr_t *thr,      /*!< in: query thread */
    mtr_t *mtr)          /*!< in: mtr; gets committed here */
{
  btr_pcur_t *pcur = node->pcur;
  btr_cur_t *btr_cur = btr_pcur_get_btr_cur(pcur);
  dberr_t err;
  
  // 【关键】检查是否可以原地更新
  if (node->cmpl_info & UPD_NODE_NO_ORD_CHANGE) {
    // 排序字段未改变，尝试原地更新
    err = btr_cur_optimistic_update(
        flags | BTR_KEEP_SYS_FLAG, btr_cur,
        &offsets, offsets_heap, node->update,
        node->cmpl_info, thr, thr_get_trx(thr)->id, mtr);
    
    if (err == DB_SUCCESS) {
      // 【关键字段变化】更新成功，Redo Log已在MTR中生成
      return err;
    }
  }
  
  // 原地更新失败，需要悲观更新（删除+插入）
  err = btr_cur_pessimistic_update(
      flags | BTR_KEEP_SYS_FLAG, btr_cur,
      &offsets, offsets_heap, node->heap,
      &node->big_rec, node->update,
      node->cmpl_info, thr, thr_get_trx(thr)->id, mtr);
  
  return err;
}
```

### 4.5 Binlog Event生成代码位置

```c
// sql/binlog.cc:11600 THD::binlog_update_row()
int THD::binlog_update_row(TABLE *table, bool is_trans,
                           const uchar *before_record,
                           const uchar *after_record,
                           const uchar *extra_row_info) {
  // before_record = table->record[1]  (旧值)
  // after_record = table->record[0]   (新值)
  
  Rows_log_event *const ev =
      binlog_prepare_pending_rows_event<Update_rows_log_event>(
          table, server_id, len, is_trans, extra_row_info);
  
  // 写入before image和after image
  // ...
}

// sql/log_event.h:3400 Update_rows_log_event
class Update_rows_log_event : public Rows_log_event,
                              public mysql::binlog::event::Update_rows_event {
 public:
  enum {
    TYPE_CODE = mysql::binlog::event::UPDATE_ROWS_EVENT  // 值=31
  };
  // ...
};
```

---

## 五、优化与修复

### 5.1 UPDATE性能优化

| 优化点 | 说明 | 配置/方法 |
|:------|:-----|:---------|
| **减少锁竞争** | 使用合适的WHERE条件 | 确保使用索引定位记录 |
| **批量更新** | 减少事务开销 | 合并多个UPDATE到一个事务 |
| **避免大字段更新** | 减少Undo存储 | LOB字段单独表存储 |
| **索引优化** | 加速记录定位 | 确保WHERE条件有索引 |

### 5.2 UPDATE常见问题

**问题1：UPDATE导致死锁**
```
症状：并发UPDATE报错 Deadlock found
原因：不同事务以不同顺序更新同一批记录
解决：
  1. 保持一致的更新顺序
  2. 减小事务范围
  3. 使用SELECT ... FOR UPDATE预先锁定
```

**问题2：UPDATE变慢**
```
症状：UPDATE执行时间突然变长
原因：
  1. 表数据量增大，索引失效
  2. Undo Log堆积
  3. 锁等待
解决：
  1. EXPLAIN分析执行计划
  2. 检查长事务
  3. 监控锁等待
```

---

## 六、总结与反思

### 6.1 核心要点回顾

1. **UPDATE的Undo比INSERT复杂**：需要保存所有被修改字段的旧值
2. **两种更新模式**：原地更新 vs 删除标记+插入
3. **MVCC版本链**：通过roll_ptr串联历史版本
4. **Binlog记录完整变化**：UPDATE_ROWS_EVENT包含before和after image

### 6.2 关键内存字段变化总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     UPDATE全流程关键字段变化时序                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  时间轴 ──────────────────────────────────────────────────────────────────▶ │
│                                                                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │
│  │协议解析 │  │事务启动 │  │UPDATE   │  │Prepare  │  │Commit   │          │
│  │         │  │         │  │执行     │  │         │  │         │          │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘          │
│       │            │            │            │            │                 │
│       ▼            ▼            ▼            ▼            ▼                 │
│  com_data.     trx->state=   trx->undo_no trx->state=  trx->state=         │
│  com_query     ACTIVE        ++           PREPARED     COMMITTED           │
│  设置          trx->id分配   update_undo  undo->state= update_undo         │
│               trx->no=MAX   分配         PREPARED     加入history          │
│                              old_roll_ptr                                   │
│                              保存                                           │
│                              new_roll_ptr                                   │
│                              写入记录                                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

**【示意图描述】**
`![UPDATE事务日志流程图](update_transaction_log_flow.png): 展示UPDATE语句从协议解析到提交过程中，Undo Log（包含旧值）、Redo Log、Binlog（包含before/after image）的写入时序，MVCC版本链的形成，以及事务状态和关键内存字段的变迁过程。`

---

> 📝 **作者注**：本文基于Percona Server 8.4.3源码分析，不同版本实现细节可能略有差异。如有疑问，欢迎在评论区交流讨论。
