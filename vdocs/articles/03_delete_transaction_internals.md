# DELETE 执行与提交过程中事务状态、Undo、Redo、Binlog 的深度剖析

## 备选标题

1. **MySQL删除的真相：DELETE语句并非真的删除数据**
2. **深入InnoDB：DELETE操作中的"标记删除"与Purge机制详解**
3. **数据库专家必读：DELETE背后的事务日志与MVCC机制**

---

## 一、开篇引子

> 在MySQL中，DELETE就像给文件打上"待删除"标签——文件并没有立即从磁盘消失，而是等到没有人再使用它时，才由"清洁工"（Purge线程）真正清理。这种设计让回滚和MVCC成为可能。

DELETE操作在InnoDB中的实现与我们直觉中的"删除"大不相同。它**并不立即物理删除数据**，而是采用"标记删除"（Delete Mark）的方式。这条被标记的记录在事务提交后，由Purge线程异步清理。

本文基于 **Percona Server 8.4.3** 源码，深入剖析DELETE操作的完整执行流程，揭示标记删除、Purge机制以及日志协同的底层原理。

---

## 二、场景展示

### 2.1 DELETE的两阶段删除

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        DELETE 两阶段删除机制                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【阶段1：Delete Mark（事务执行时）】                                        │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                     │   │
│  │   ┌─────────────┐           ┌─────────────┐                        │   │
│  │   │   Record    │   DELETE  │   Record    │                        │   │
│  │   │ id=1        │  ──────▶  │ id=1        │                        │   │
│  │   │ name='Tom'  │           │ name='Tom'  │                        │   │
│  │   │ deleted=0   │           │ deleted=1   │ ◀── 设置删除标记       │   │
│  │   │ trx_id=100  │           │ trx_id=200  │ ◀── 更新trx_id        │   │
│  │   └─────────────┘           └─────────────┘                        │   │
│  │                                                                     │   │
│  │   记录仍在原位置，只是被标记为"待删除"                                │   │
│  │   Undo Log: TRX_UNDO_DEL_MARK_REC (类型14)                         │   │
│  │                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
│  【阶段2：Purge（事务提交后，异步执行）】                                    │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                                                                     │   │
│  │   ┌─────────────┐           ┌─────────────┐                        │   │
│  │   │   Record    │   PURGE   │   (空闲     │                        │   │
│  │   │ id=1        │  ──────▶  │    空间)    │                        │   │
│  │   │ deleted=1   │           │             │                        │   │
│  │   └─────────────┘           └─────────────┘                        │   │
│  │                                                                     │   │
│  │   当没有活跃事务需要访问此记录时，Purge线程真正删除                    │   │
│  │   释放存储空间，清理二级索引记录                                      │   │
│  │                                                                     │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 DELETE事务处理时序图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE 执行与提交时序图                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  时间轴 ─────────────────────────────────────────────────────────────────▶  │
│                                                                             │
│  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐  ┌─────┐ │
│  │协议 │  │定位 │  │加锁 │  │写入 │  │标记 │  │Prep │  │Binlog│  │Commit│ │
│  │解析 │─▶│记录 │─▶│X Lock│─▶│Undo │─▶│删除 │─▶│are  │─▶│Write │─▶│     │ │
│  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘  └─────┘ │
│     │        │        │        │        │        │        │        │      │
│     ▼        ▼        ▼        ▼        ▼        ▼        ▼        ▼      │
│  com_data  读取完整  防止并发  保存完整  设置     状态变为  写入Row   释放锁  │
│  设置      记录值    修改      记录数据  info_bit PREPARED  Event    资源    │
│                              (回滚用)   deleted                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 事务状态与关键字段变化

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE事务状态与关键字段变化                              │
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
│            │          - trx->rsegs.m_redo.update_undo = 分配(首次DELETE)     │
│            │          - rec->info_bits |= REC_INFO_DELETED_FLAG             │
│            │          - rec->trx_id = trx->id                                │
│            │          - rec->roll_ptr = 新Undo记录指针                       │
│            │                                                                │
│            │ trx_prepare()                                                  │
│            ▼                                                                │
│    ┌───────────────┐  关键字段变化:                                          │
│    │TRX_STATE_     │  - trx->state = TRX_STATE_PREPARED                     │
│    │PREPARED       │  - update_undo->state = TRX_UNDO_PREPARED              │
│    └───────┬───────┘                                                        │
│            │                                                                │
│            │ trx_commit()                                                   │
│            ▼                                                                │
│    ┌───────────────┐  关键字段变化:                                          │
│    │TRX_STATE_     │  - trx->state = COMMITTED_IN_MEMORY                    │
│    │COMMITTED      │  - trx->no = 分配提交序号                               │
│    └───────────────┘  - update_undo加入history list (供purge)               │
│            │                                                                │
│            │ 【异步】Purge线程                                               │
│            ▼                                                                │
│    ┌───────────────┐  Purge执行:                                            │
│    │真正删除记录   │  - 物理删除聚簇索引记录                                  │
│    │(当无事务引用) │  - 物理删除二级索引记录                                  │
│    └───────────────┘  - 释放Undo Log空间                                    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 三、原理深入

### 3.1 DELETE的Undo Log结构

DELETE的Undo Log需要保存**完整记录数据**（与INSERT只保存主键不同），因为回滚时需要恢复整条记录：

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE Undo Record 结构详解                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                        Undo Record Header                              │ │
│  ├──────────┬──────────┬──────────┬──────────┬────────────────────────────┤ │
│  │Next Ptr  │Type      │Undo No   │Table ID  │info_bits + old_trx_id +    │ │
│  │(2 bytes) │(1 byte)  │(压缩)    │(压缩)    │old_roll_ptr                │ │
│  └──────────┴──────────┴──────────┴──────────┴────────────────────────────┘ │
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                  主键字段 + 完整记录数据                                │ │
│  ├──────────────────────┬─────────────────────────────────────────────────┤ │
│  │ 主键字段值           │ 所有字段的【完整值】(用于回滚恢复)                │ │
│  │ (定位记录用)         │ 因为回滚DELETE需要重新INSERT整条记录              │ │
│  └──────────────────────┴─────────────────────────────────────────────────┘ │
│                                                                             │
│  Type值: TRX_UNDO_DEL_MARK_REC = 14                                        │
│                                                                             │
│  【Undo Log样例】DELETE FROM t WHERE id=100                                 │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ next_ptr: 0x0200 | type: 0x0E (DEL_MARK) | undo_no: 3                │  │
│  │ table_id: 1234 | old_info_bits: 0x00 (未删除状态)                     │  │
│  │ old_trx_id: 1000 | old_roll_ptr: 0x... (指向更早版本)                 │  │
│  │ pk_len: 4 | pk_value: 100                                            │  │
│  │ 【完整记录数据】n_fields: 3                                           │  │
│  │   field_0: id=100                                                    │  │
│  │   field_1: name='test' (len=4)                                       │  │
│  │   field_2: age=25 (len=4)                                            │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【与INSERT/UPDATE的对比】                                                  │
│  ┌────────────────┬────────────────────────────────────────────────────┐   │
│  │ INSERT Undo    │ 只存主键值（回滚时DELETE）                          │   │
│  │ UPDATE Undo    │ 存主键值+被修改字段的旧值（回滚时UPDATE回去）       │   │
│  │ DELETE Undo    │ 存完整记录（回滚时需要INSERT回去）                  │   │
│  └────────────────┴────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 DELETE的Redo Log

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE Redo Log 类型                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【Delete Mark阶段（事务执行时）】                                           │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │ MLOG_REC_CLUST_DELETE_MARK                                             │ │
│  │ - space_id: 表空间ID                                                   │ │
│  │ - page_no: 页号                                                        │ │
│  │ - offset: 记录偏移                                                     │ │
│  │ - flag: 1 (设置删除标记)                                               │ │
│  │ - trx_id: 执行删除的事务ID                                             │ │
│  │ - roll_ptr: 指向Undo记录                                               │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  【Purge阶段（异步清理时）】                                                 │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │ MLOG_REC_DELETE                                                        │ │
│  │ - space_id: 表空间ID                                                   │ │
│  │ - page_no: 页号                                                        │ │
│  │ - offset: 记录偏移                                                     │ │
│  │ // 真正从页面删除记录，释放空间                                         │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  【Redo Log样例】                                                           │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │ LSN: 123456900 | Type: MLOG_REC_CLUST_DELETE_MARK | Space: 5         │  │
│  │ Page: 100 | Offset: 200 | Flag: 1 (deleted)                         │  │
│  │ Trx_id: 200 | Roll_ptr: 0x000001234567                              │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.3 DELETE的Binlog Event序列

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE的Binlog Event序列                                 │
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
│   │DELETE_ROWS_EVENT   │ ◀── 被删除行的数据                                  │
│   │ table_id: 123      │     sql/binlog.cc:11620 THD::binlog_delete_row    │
│   │ rows:              │     TYPE_CODE = DELETE_ROWS_EVENT (值=32)         │
│   │   [(100,'test',25)]│     // 只包含被删除行的before image               │
│   └────────┬───────────┘                                                    │
│            ▼                                                                │
│   ┌────────────────────┐                                                    │
│   │ XID_EVENT          │ ◀── 事务提交                                       │
│   │ xid: 12347         │                                                    │
│   └────────────────────┘                                                    │
│                                                                             │
│  【Binlog样例】mysqlbinlog输出:                                             │
│  ### DELETE FROM test.users                                                 │
│  ### WHERE                                                                  │
│  ###   @1=100 /* INT meta=0 nullable=0 is_null=0 */                        │
│  ###   @2='test' /* STRING(100) meta=... */                                │
│  ###   @3=25 /* INT meta=0 nullable=1 is_null=0 */                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.4 Purge机制详解

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Purge线程工作流程                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────────────────────────────────────────────────┐ │
│  │                         Purge调度流程                                   │ │
│  │                                                                        │ │
│  │   ┌─────────────┐                                                      │ │
│  │   │ Coordinator │  // srv_purge_coordinator_thread                    │ │
│  │   │   Thread    │  // storage/innobase/srv/srv0srv.cc                 │ │
│  │   └──────┬──────┘                                                      │ │
│  │          │                                                             │ │
│  │          │ 1. 计算oldest_view (最老的活跃ReadView)                     │ │
│  │          │ 2. 扫描history list找到可purge的Undo                       │ │
│  │          │                                                             │ │
│  │          ▼                                                             │ │
│  │   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐            │ │
│  │   │Purge Worker 1│    │Purge Worker 2│    │Purge Worker N│            │ │
│  │   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘            │ │
│  │          │                   │                   │                     │ │
│  │          ▼                   ▼                   ▼                     │ │
│  │   ┌─────────────────────────────────────────────────────────────────┐ │ │
│  │   │                 并行处理Delete-Marked记录                        │ │ │
│  │   │  1. row_purge_del_mark()  // 处理聚簇索引                        │ │ │
│  │   │  2. row_purge_remove_sec_if_poss() // 处理二级索引              │ │ │
│  │   │  3. trx_purge_add_undo_to_history() // 清理Undo空间             │ │ │
│  │   └─────────────────────────────────────────────────────────────────┘ │ │
│  │                                                                        │ │
│  └────────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
│  【Purge条件】                                                              │
│  一条Delete-Marked记录可以被Purge当且仅当:                                  │
│  1. 该记录的trx_id < 所有活跃事务的ReadView.min_trx_id                      │
│  2. 即：没有任何活跃事务可能需要访问该记录的历史版本                          │
│                                                                             │
│  【配置参数】                                                                │
│  - innodb_purge_threads: Purge线程数量 (默认4)                              │
│  - innodb_purge_batch_size: 每批处理的Undo数量 (默认300)                    │
│  - innodb_max_purge_lag: Purge延迟阈值，超过会限制DML                       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、源码根因揭秘

**源码版本：Percona Server 8.4.3-3**

### 4.1 完整函数调用链（从协议解析开始）

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              DELETE 完整执行函数调用链（协议层 → 存储引擎层）                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【阶段1：MySQL协议解析】                                                    │
│  Protocol_classic::read_packet()           sql/protocol_classic.cc:1408    │
│  └── Protocol_classic::get_command()       sql/protocol_classic.cc:2887    │
│      └── parse_packet() for COM_QUERY      sql/protocol_classic.cc:2834    │
│          │  提取SQL文本: "DELETE FROM t WHERE id=1"                         │
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
│      └── Sql_cmd_delete::execute()         sql/sql_delete.cc:500           │
│          │  case SQLCOM_DELETE:                                             │
│          └── Sql_cmd_delete::delete_from_single_table()                    │
│                                            sql/sql_delete.cc:700           │
│                                                                             │
│  【阶段4：行删除循环】                                                       │
│  Sql_cmd_delete::delete_from_single_table()                                │
│  └── while (iterator->Read() == 0)         // 逐行读取                     │
│      └── handler::ha_delete_row()          sql/handler.cc:8420             │
│          │  字段变化:                                                       │
│          │    - table->record[0] = 被删除记录                               │
│          │                                                                  │
│          └── ha_innobase::delete_row()     storage/innobase/handler/       │
│              │                             ha_innodb.cc:9400               │
│              │  字段变化: prebuilt->upd_node设置                            │
│              │                                                              │
│  【阶段5：InnoDB行删除（标记删除）】                                         │
│  row_update_for_mysql()                    storage/innobase/row/           │
│  │                                         row0mysql.cc:2500               │
│  │  // DELETE在InnoDB内部复用UPDATE流程                                    │
│  │  字段变化: trx->undo_no++ (每次DML递增)                                  │
│  │                                                                          │
│  └── row_upd_clust_step()                  storage/innobase/row/           │
│      │                                     row0upd.cc:2700                 │
│      │                                                                      │
│      └── btr_cur_del_mark_set_clust_rec()  storage/innobase/btr/           │
│          │                                 btr0cur.cc:4344                 │
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
│          │       // type = TRX_UNDO_DEL_MARK_REC                           │
│          │                                 storage/innobase/trx/           │
│          │                                 trx0rec.cc:1200                 │
│          │                                                                  │
│          └── 【设置Delete Mark】                                            │
│              btr_rec_set_deleted_flag()    storage/innobase/btr/           │
│              │                             btr0cur.cc:4380                 │
│              │  // rec->info_bits |= REC_INFO_DELETED_FLAG                 │
│              │                                                              │
│              └── page_cur_delete_rec_write_log()  // Redo Log              │
│                                            storage/innobase/page/          │
│                                            page0cur.cc:1100                │
│                                                                             │
│  【阶段6：Binlog缓存写入】(DML执行时)                                        │
│  handler::ha_delete_row()                  sql/handler.cc:8420             │
│  └── binlog_log_row()                      sql/handler.cc:8000             │
│      ├── THD::binlog_write_table_map()     sql/binlog.cc:10005             │
│      └── THD::binlog_delete_row()          sql/binlog.cc:11620             │
│          └── binlog_prepare_pending_rows_event<Delete_rows_log_event>      │
│              // before_record = table->record[0]                           │
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
│  └── ... DELETE执行完成 ...                                                 │
│                                                                             │
│  dispatch_sql_command()                    sql/sql_parse.cc:5521           │
│  └── trans_commit_stmt()                   sql/sql_parse.cc:5202           │
│      │                                     sql/transaction.cc:513          │
│      │                                                                      │
│  【进入提交流程】(与INSERT/UPDATE相同)                                       │
│  ha_commit_trans()                         sql/handler.cc:1663             │
│  └── MYSQL_BIN_LOG::commit()               sql/binlog.cc:8423              │
│      └── MYSQL_BIN_LOG::ordered_commit()   sql/binlog.cc:9234              │
│          │                                                                  │
│          ├── 【Stage 1】FLUSH阶段                                          │
│          │   process_flush_stage_queue()                                   │
│          │   └── flush_thread_caches()                                     │
│          │       // 写入TABLE_MAP + DELETE_ROWS_EVENT                      │
│          │                                                                  │
│          ├── 【Stage 2】SYNC阶段                                           │
│          │   sync_binlog_file()                                            │
│          │                                                                  │
│          └── 【Stage 3】COMMIT阶段                                         │
│              ha_commit_low()               sql/handler.cc:1938             │
│              └── trx_commit_for_mysql()                                    │
│                  │  字段变化:                                               │
│                  │    - trx->state → COMMITTED_IN_MEMORY                   │
│                  │    - update_undo加入history list                         │
│                  │    - 通知Purge协调器有新任务                              │
│                  └── trx_commit()                                          │
│                                                                             │
│  【异步Purge流程】(事务提交后)                                               │
│  srv_purge_coordinator_thread()            storage/innobase/srv/           │
│  │                                         srv0srv.cc:2800                 │
│  └── trx_purge()                           storage/innobase/trx/           │
│      │                                     trx0purge.cc:1500               │
│      └── row_purge_step()                  storage/innobase/row/           │
│          │                                 row0purge.cc:800                │
│          ├── row_purge_del_mark()          // 删除聚簇索引记录             │
│          └── row_purge_remove_sec_if_poss()// 删除二级索引记录             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 Delete Mark核心代码

```c
// storage/innobase/btr/btr0cur.cc:4344-4400
/** Sets a secondary index record delete mark to TRUE or FALSE.
@param[in,out] block          buffer block
@param[in,out] rec            record
@param[in]     index          secondary index
@param[in]     offsets        rec_get_offsets(rec, index)
@param[in]     flag           nonzero if delete mark should be set
@param[in]     thr            query thread
@param[in,out] mtr            mini-transaction
@return DB_SUCCESS, or DB_LOCK_WAIT */
dberr_t btr_cur_del_mark_set_clust_rec(
    ulint flags,
    buf_block_t *block,
    rec_t *rec,
    dict_index_t *index,
    const ulint *offsets,
    que_thr_t *thr,
    const dtuple_t *entry,
    mtr_t *mtr) {
  
  trx_t *trx = thr_get_trx(thr);
  
  // 【关键】检查锁
  dberr_t err = lock_clust_rec_modify_check_and_lock(
      flags, block, rec, index, offsets, thr);
  if (err != DB_SUCCESS) {
    return err;
  }
  
  // 【关键】写入Undo Log
  roll_ptr_t roll_ptr;
  err = trx_undo_report_row_operation(
      flags, TRX_UNDO_MODIFY_OP, thr,
      index, entry, nullptr, rec, offsets, &roll_ptr);
  if (err != DB_SUCCESS) {
    return err;
  }
  
  // 【关键字段变化】设置删除标记
  // rec->info_bits |= REC_INFO_DELETED_FLAG
  btr_rec_set_deleted_flag(rec, page_is_comp(block->frame), TRUE);
  
  // 【关键】更新记录的trx_id和roll_ptr
  row_upd_rec_sys_fields(rec, nullptr, index, offsets, trx, roll_ptr);
  
  // 【关键】写入Redo Log
  btr_cur_del_mark_set_clust_rec_log(rec, index, trx->id, roll_ptr, mtr);
  
  return DB_SUCCESS;
}
```

### 4.4 Purge核心代码

```c
// storage/innobase/row/row0purge.cc:800-900
/** Purges a delete marking of a record.
@param[in,out] node purge node
@param[in,out] thr query thread
@return true if purged, false if skipped */
static bool row_purge_del_mark(purge_node_t *node) {
  mtr_t mtr;
  dict_index_t *index = node->index;
  
  mtr_start(&mtr);
  
  // 获取聚簇索引页
  btr_pcur_open_on_user_rec(index, node->ref, PAGE_CUR_LE,
                            BTR_MODIFY_LEAF, &node->pcur, &mtr);
  
  rec_t *rec = btr_pcur_get_rec(&node->pcur);
  
  // 检查是否是我们要purge的记录
  if (/* 条件检查 */) {
    // 【关键】物理删除记录
    page_cur_delete_rec(btr_pcur_get_page_cur(&node->pcur), 
                        index, offsets, &mtr);
  }
  
  mtr_commit(&mtr);
  
  // 【关键】删除二级索引中的对应记录
  row_purge_remove_sec_if_poss(node, thr);
  
  return true;
}

// storage/innobase/row/row0purge.cc:500-600
/** Remove a secondary index entry if possible. */
static void row_purge_remove_sec_if_poss(purge_node_t *node, que_thr_t *thr) {
  // 遍历所有二级索引
  for (dict_index_t *index = node->table->first_index(); 
       index != nullptr; 
       index = index->next()) {
    if (index->is_clustered()) continue;
    
    // 构建二级索引entry
    dtuple_t *entry = row_build_index_entry(node->row, nullptr, 
                                            index, node->heap);
    
    // 删除二级索引记录
    row_purge_remove_sec_if_poss_leaf(node, index, entry);
  }
}
```

---

## 五、优化与修复

### 5.1 DELETE性能优化

| 优化点 | 说明 | 配置/方法 |
|:------|:-----|:---------|
| **批量删除** | 减少锁竞争和日志量 | 分批删除，每批1000-10000条 |
| **使用TRUNCATE** | 快速清空整表 | TRUNCATE TABLE (DDL操作) |
| **避免大事务** | 减少Undo堆积 | 分批提交 |
| **调整Purge线程** | 加快清理速度 | 增大innodb_purge_threads |

### 5.2 DELETE常见问题

**问题1：大量DELETE后空间未释放**
```
症状：删除大量数据后，表空间文件大小不变
原因：InnoDB不会自动收缩表空间文件
解决：
  1. OPTIMIZE TABLE (需要长时间锁表)
  2. ALTER TABLE ... ENGINE=InnoDB (在线重建)
  3. 使用pt-online-schema-change
```

**问题2：Purge Lag过大**
```
症状：History list length持续增长
原因：长事务阻止Purge、Purge速度跟不上
解决：
  1. 查找并处理长事务
  2. 增大innodb_purge_threads
  3. 减小innodb_purge_batch_size
```

**问题3：DELETE导致锁等待**
```
症状：DELETE执行缓慢，其他事务等待
原因：DELETE需要X锁，与其他DML冲突
解决：
  1. 减小DELETE范围
  2. 在业务低峰期执行
  3. 使用LIMIT分批删除
```

---

## 六、总结与反思

### 6.1 核心要点回顾

1. **DELETE采用标记删除**：不立即物理删除，设置info_bits中的deleted标记
2. **Undo Log保存完整记录**：回滚需要重新INSERT整条记录
3. **Purge异步清理**：由专门的Purge线程在无事务引用时清理
4. **二级索引也需清理**：Purge负责删除所有二级索引中的对应记录

### 6.2 关键内存字段变化总结

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     DELETE全流程关键字段变化时序                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  时间轴 ──────────────────────────────────────────────────────────────────▶ │
│                                                                             │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐          │
│  │协议解析 │  │事务启动 │  │DELETE   │  │Commit   │  │Purge    │          │
│  │         │  │         │  │执行     │  │         │  │(异步)   │          │
│  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘          │
│       │            │            │            │            │                 │
│       ▼            ▼            ▼            ▼            ▼                 │
│  com_data.     trx->state=   rec->info_   trx->state=  记录被              │
│  com_query     ACTIVE        bits|=       COMMITTED    物理删除            │
│  设置          trx->id分配   DELETED      update_undo  空间释放            │
│               trx->no=MAX   rec->trx_id= 加入history  二级索引             │
│                              trx->id                   也删除              │
│                              update_undo                                   │
│                              分配                                          │
│                              roll_ptr                                      │
│                              生成                                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 6.3 三种DML操作的Undo对比

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     INSERT/UPDATE/DELETE Undo对比                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────┬──────────────┬──────────────────┬────────────────────────┐   │
│  │ 操作类型 │ Undo类型     │ 存储内容         │ 回滚操作                │   │
│  ├──────────┼──────────────┼──────────────────┼────────────────────────┤   │
│  │ INSERT   │ INSERT_REC   │ 主键值           │ DELETE该记录           │   │
│  │          │ (类型11)     │                  │                        │   │
│  ├──────────┼──────────────┼──────────────────┼────────────────────────┤   │
│  │ UPDATE   │ UPD_EXIST_   │ 主键值+修改字段  │ UPDATE回旧值           │   │
│  │          │ REC (类型12) │ 的旧值           │                        │   │
│  ├──────────┼──────────────┼──────────────────┼────────────────────────┤   │
│  │ DELETE   │ DEL_MARK_    │ 完整记录数据     │ 清除删除标记           │   │
│  │          │ REC (类型14) │                  │ (实际上是UPDATE回去)   │   │
│  └──────────┴──────────────┴──────────────────┴────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

**【示意图描述】**
`![DELETE事务日志流程图](delete_transaction_log_flow.png): 展示DELETE语句从协议解析到提交，以及异步Purge过程中，Undo Log（完整记录）、Redo Log（删除标记）、Binlog的写入时序，标记删除和物理删除的两阶段机制，以及事务状态和关键内存字段的变迁过程。`

---

> 📝 **作者注**：本文基于Percona Server 8.4.3源码分析，不同版本实现细节可能略有差异。如有疑问，欢迎在评论区交流讨论。
