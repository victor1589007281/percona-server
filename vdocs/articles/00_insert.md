## insert语句 的生命周期

总结：


```text



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
║                       ├──▶ trx_start_if_not_started_xa()      storage/innobase/include/trx0trx.h:1406                                            ║
║                       │       │  ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓                            ║
║                       │       │  ┃ 【事务状态变化: NOT_STARTED → ACTIVE】                                           ┃                            ║
║                       │       │  ┃▶trx_start_if_not_started_xa_low() storage/innobase/trx/trx0trx.cc:3396         ┃                            ║
║                       │       │  ┃──▶trx_start_low()                  storage/innobase/trx/trx0trx.cc:1333          ┃                            ║
║                       │       │  ┃      │                                                                           ┃                            ║
║                       │       │  ┃      ├── trx->id = trx_sys_allocate_trx_id()  【分配事务ID】                     ┃                            ║
║                       │       │  ┃      ├── trx->state = TRX_STATE_ACTIVE        【设置状态为ACTIVE】               ┃                            ║
║                       │       │  ┃      ├── trx->start_time = time(NULL)         【记录启动时间】                   ┃                            ║
║                       │       │  ┃      ├── trx->undo_no = 0                     【初始化undo序号】                 ┃                            ║
║                       │       │  ┃      └── trx->no = TRX_ID_MAX                 【提交序号初始化为最大值】          ┃                            ║
║                       │       │  ┃                                                                                  ┃                            ║
║                       │       │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛                            ║
║                       │       │                                                                                                                  ║
║                       ├──▶ row_get_prebuilt_insert_row()      storage/innobase/row/row0mysql.cc:1502                                            ║
║                       │       │  【准备INSERT graph】                                                                                            ║
║                       │                                                                                                                          ║
║                       ├──▶ row_mysql_convert_row_to_innobase() storage/innobase/row/row0mysql.cc:1016                                            ║
║                       │       │  【MySQL行格式转InnoDB格式】                                                                                     ║
║                       │                                                                                                                          ║
║                       └──▶ row_ins_step()                     storage/innobase/row/row0ins.cc:3617                                              ║
║                               │  【INSERT执行图入口】                                                                                            ║
║                               │                                                                                                                  ║
║                               ├──▶ lock_table(LOCK_IX)        storage/innobase/lock/lock0lock.cc:3574                                           ║
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
│               ├──▶ btr_search_guess_on_hash()                 storage/innobase/btr/btr0sea.cc:809                                              │
│               │       │  【尝试AHI快速查找】                                                                                                     │
│               │                                                                                                                                  │
│               ├──▶ buf_page_get_gen()                         storage/innobase/buf/buf0buf.cc:4440                                              │
│               │       │  【获取B+树页面并加锁】                                                                                                  │
│               │       │  【乐观插入: 只对叶子页加X锁】                                                                                           │
│               │                                                                                                                                  │
│               └──▶ page_cur_search_with_match()               storage/innobase/page/page0cur.cc:328                                             │
│                       │  【页内二分查找定位记录位置】                                                                                            │
│                       │                                                                                                                          │
│                       └── 返回 cursor: 指向插入位置的前一条记录                                                                                  │
│                           cursor->up_match / cursor->low_match 记录匹配字段数                                                                    │
│                                                                                                                                                  │
└──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│  3.1.2 Undo Log构造调用链 (Undo Page修改)                                                                                                        │
├──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│  row_ins_clust_index_entry_low()                              storage/innobase/row/row0ins.cc:2362                                              │
├──▶ btr_cur_optimistic_insert()                                storage/innobase/btr/btr0cur.cc:2719                                              │
│       │                                                                                                                                          │
│       └──▶ btr_cur_ins_lock_and_undo()                        storage/innobase/btr/btr0cur.cc:2611                                              │
│               │  【获取锁并生成Undo日志】                                                                                                        │
│               │                                                                                                                                  │
│               ├──▶ lock_rec_insert_check_and_lock()           storage/innobase/lock/lock0lock.cc:5165                                           │
│               │       │  【检查并加行锁】                                                                                                        │
│               │                                                                                                                                  │
│               └──▶ trx_undo_report_row_operation()            storage/innobase/trx/trx0rec.cc:2112                                              │
│                       │  【Undo日志生成入口】                                                                                                    │
│                       │                                                                                                                          │
│                       ├──▶ trx_undo_assign_undo()             storage/innobase/trx/trx0undo.cc:1688                                             │
│                       │       │  【分配Undo段Page(首次INSERT时)】                                                                                  │
│                       │       │─▶trx_undo_create()            storage/innobase/trx/trx0undo.cc:1549                                            │
│                       │       │  └──▶trx_undo_mem_create()    storage/innobase/trx/trx0undo.cc:1463                                                                                                               │
│                       │       │      |  设置回滚段的状态【注意是回滚段，不是回滚记录】                                                                 │
│                       │       |      └──▶ undo->state = TRX_UNDO_ACTIVE       storage/innobase/trx/trx0undo.cc：1479                               │
│                       │       └──▶ trx->rsegs.m_redo.insert_undo = undo       storage/innobase/trx/trx0undo.cc:1767                                │
│                       │                                                                                                                          │
│                       ├──▶ buf_page_get_gen()                 storage/innobase/buf/buf0buf.cc:4440                                              │
│                       │       │  【获取Undo页面】                                                                                                │
│                       │                                                                                                                          │
│                       └──▶ trx_undo_page_report_insert()      storage/innobase/trx/trx0rec.cc:483                                               │
│                               │  【在Undo Page上构造INSERT Undo记录】【这个页面会是脏页，会被异步刷盘】                                                  │
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
│                               │  ┃  │ 3+len1+len2      │ unique_fields  │ 可变长度，主键/唯一索引字段值/Row_id(用于回滚时定位删除)           │       ┃│
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
│                               │               │  【此时写入到：MTR级别的日志缓存 (mtr->m_log) 】                                                    │
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
│                               │──▶ 【把MTR缓存中内容，批量复制到全局Redo日志缓冲区 (log_sys->buf)】    storage/innobase/trx/trx0rec.cc:2304            │
│                               │       ├── mtr_commit(&mtr);                                     storage/innobase/mtr/mtr0mtr.cc:659             │
│                               │           └──▶ mtr_t::Command::execute()                        storage/innobase/mtr/mtr0mtr.cc:839             │
│                               │                 └──▶ len = prepare_write();                     storage/innobase/mtr/mtr0mtr.cc:843             │
│                               │                 |    【Redo Buffer中预留空间】                                                                    │
│                               │                 └──▶ handle =log_buffer_reserve(*log_sys, len); storage/innobase/mtr/mtr0mtr.cc:850             │
│                               │                 |    【写入MTR 日志到Redo Buffer中】                                                               │
│                               │                 └──▶ m_impl->m_log.for_each_block(write_log);   storage/innobase/mtr/mtr0mtr.cc:855             │
│                               │                 |    【等待日志空间】                                                                              │
│                               │                 └──▶ log_wait_for_space_in_log_recent_closed(*log_sys, handle.start_lsn);                       │
│                               │                                                                 storage/innobase/mtr/mtr0mtr.cc:860             │
│                               │                 |    【把脏页添加到Flush List】                                                                    │
│                               │                 └──▶ add_dirty_blocks_to_flush_list(handle.start_lsn, handle.end_lsn);                          │
│                               │                                                                 storage/innobase/mtr/mtr0mtr.cc:864             │
│                               │                 |    【关闭日志缓冲区句柄】                                                                         │
│                               │                 └──▶ log_buffer_close(*log_sys, handle);        storage/innobase/mtr/mtr0mtr.cc:866             │
│                               │                 |    【释放资源】                                                                                 │
│                               │                 └──▶ release_all();                             storage/innobase/mtr/mtr0mtr.cc:877             │
│                               │                 └──▶ release_resources();                       storage/innobase/mtr/mtr0mtr.cc:878             │
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
│  row_ins_clust_index_entry_low()                              storage/innobase/row/row0ins.cc:2362                                              │
│──▶ btr_cur_optimistic_insert()                                storage/innobase/btr/btr0cur.cc:2719                                              │
│       │  【(Undo构造完成后) 执行Data Page插入】                                                                                                  │
│       │                                                                                                                                          │
│       └──▶ page_cur_tuple_insert()                            storage/innobase/include/page0cur.ic:187                                          │
│               │  【将dtuple转换为记录并插入】                                                                                                    │
│               │                                                                                                                                  │
│               ├──▶ rec_convert_dtuple_to_rec()                storage/innobase/rem/rem0rec.cc:1115                                              │
│               │       │  【dtuple → 物理记录格式】                                                                                               │
│               │                                                                                                                                  │
│               └──▶ page_cur_insert_rec_low()                  storage/innobase/page/page0cur.cc:1225                                            │
│                       │  【在Data Page上插入记录】                                                                                               │
│                       │                                                                                                                          │
│                       ├──▶ page_mem_alloc_heap()              storage/innobase/page/page0page.cc:223                                            │
│                       │       │  【从页堆分配空间】                                                                                              │
│                       │                                                                                                                          │
│                       ├──▶ rec_copy()                         storage/innobase/row/row0ins.cc:713                                               │
│                       │       │  【复制记录到分配的空间】                                                                                        │
│                       │                                                                                                                          │
│                       ├──▶ page_rec_set_next()                storage/innobase/include/page0page.ic:662                                         │
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
│                               │       ├──▶ mlog_open()                storage/innobase/include/mtr0log.ic:42                                    │
│                               │       │       │  【获取MTR日志缓冲区指针】                                                                       │
│                               │       │       │                                                                                                  │
│                               │       │       └──▶ mtr->get_log()->open()                                                                       │
│                               │       │                                                                                                          │
│                               │       └──▶ mlog_write_initial_log_record_fast()  storage/innobase/include/mtr0log.ic:192                         │
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
│                               └──▶ mlog_close()                       storage/innobase/include/mtr0log.ic:59                                    │
│                                       │  【关闭MTR日志缓冲区】                                                                                      │
│                                                                                                                                                  │
│  【Redo写入Log Buffer调用链】                                    storage/innobase/row/row0ins.cc:2583                                              │
│──▶mtr_t::commit()                                              storage/innobase/mtr/mtr0mtr.cc:480                                               │
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
║               ├──▶ check_table_binlog_row_based()             sql/binlog.cc:8149                                                               ║
║               │       │  【检查是否需要记录Row格式Binlog】                                                                                       ║
║               │                                                                                                                                  ║
║               ├──▶ add_pke()                                  sql/rpl_write_set_handler.cc:761                                                  ║
║               │       │  【收集Writeset用于并行复制依赖计算】                                                                                    ║
║               │       │                                                                                                                          ║
║               │       └──▶ generate_hash_pke()                sql/rpl_write_set_handler.cc:676                                                  ║
║               │               │  【计算主键hash添加到thd->rpl_thd_ctx.transaction_write_set】                                                    ║
║               │                                                                                                                                  ║
║               ├──▶ write_locked_table_maps()                  sql/handler.cc:8186                                                               ║
║               │       │                                                                                                                          ║
║               │       └──▶ thd->binlog_write_table_map()      sql/binlog.cc:9994                                                                ║
║               │               │                                                                                                                  ║
║               │               │   【构造Table_map_log_event】【构造函数位置：sql/log_event.cc：10696】                                                 ║
║               │               │──▶ Table_map_log_event the_event(this, table, table->s->table_map_id,is_transactional);                                                                                                                  ║
║               │               │                                           sql/binlog.cc:10005                                                   ║
║               │               ├──▶ binlog_start_trans_and_stmt()          sql/binlog.cc:9914                                                    ║
║               │               │       │  【首次写入事务缓存时触发】                                                                              ║
║               │               │       │                                                                                                          ║
║               │               │       ├──▶ thd->binlog_setup_trx_data()   sql/binlog.cc:9791                                                    ║
║               │               │       │       │  【初始化binlog_cache_mngr存储binlog events(trx_cache(事务缓存) + stmt_cache(语句缓存-autocommit))】 ║
║               │               │       │                                                                                                          ║
║               │               │       ├──▶ register_binlog_handler()      sql/binlog.cc:9849                                                    ║
║               │               │       │       │  【将Binlog注册为事务参与者(2PC)】                                                               ║
║               │               │       │                                                                                                          ║
║               │               │       └──▶ cache_data->write_event(&qinfo)    sql/binlog.cc:1611                                                ║
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
║               │               ├──▶ Rows_query_log_event (可选)            sql/binlog.cc:10019                                                   ║
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
║                               ├──▶ pack_row()                 storage/innobase/row/row0ins.cc:270                                              ║
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
║                                       │──▶ do_add_row_data()  sql/log_event.cc:8197                                                          ║
║                                                                                                                                                  ║
║  【此时trx_cache中的Event序列】                                                                                                                  ║
║  ┌───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐  ║
║  │ 序号 │ Event类型              │ 来源函数                                  │ 文件位置                                                      │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 1    │ Query_log_event(BEGIN) │ binlog_start_trans_and_stmt()             │ sql/binlog.cc:1611                                            │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 2    │ Rows_query_log_event   │ binlog_write_table_map() (可选)            │ sql/binlog.cc:10019                                           │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 3    │ Table_map_log_event    │ binlog_write_table_map()                  │ sql/log_event.cc:10022                                        │  ║
║  ├──────┼────────────────────────┼───────────────────────────────────────────┼───────────────────────────────────────────────────────────────┤  ║
║  │ 4    │ Write_rows_log_event   │ binlog_write_row()                        │ sql/binlog.cc:11564                                           │  ║
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
│                       └──▶ tc_log->prepare(thd, all)                             sql/handler.cc:1822                                                            │
│                       |        │  【tc_log = &mysql_bin_log (binlog开启时)】                                                                                      │
│                       |        │                                                                                                                                  │
│                       |        └──▶ MYSQL_BIN_LOG::prepare()                      sql/binlog.cc:8370                                                             │
│                       |                │                                                                                                                          │
│                       |                ├── thd->durability_property = HA_IGNORE_DURABILITY                                                                        │
│                       |                │       【设置延迟刷盘标志，redo log在flush阶段统一刷】                                                                   │
│                       |                │                                                                                                                          │
│                       |                └──▶ ha_prepare_low(thd, all)              sql/binlog.cc:8387                                                             │
│                       |                   │                                                                                                                  │
│                       |                   │  【遍历所有事务参与的存储引擎】   sql/handler.cc:2356                                                           │
│                       |                   │                                                                                                                  │
│                       |                   └──▶ ht->prepare(ht, thd, all)     sql/handler.cc:2372                                                            │
│                       |                           │  【调用InnoDB的prepare回调】                                                                             │
│                       |                           │                                                                                                          │
│                       |                           └──▶ innobase_xa_prepare() storage/innobase/handler/ha_innodb.cc:20885                                    │
│                       |                                   │                                                                                                  │
│                       |                                   ├── thd_get_xid(thd, (MYSQL_XID *)trx->xid)                                                       │
│                       |                                   │       【获取XID用于2PC恢复】                                                                    │
│                       |                                   │                                                                                                  │
│                       |                                   └──▶ trx_prepare_for_mysql(trx)                                                                   │
│                       |                                           │          storage/innobase/handler/ha_innodb.cc:20921                                    │
│                       |                                           │                                                                                          │
│                       |                                           └──▶ trx_prepare(trx)                                                                     │
│                       |                                                   │  storage/innobase/trx/trx0trx.cc:3192                                           │
│                       |                                                   │                                                                                  │
│                       |                                                   │  【trx_prepare()实现】  storage/innobase/trx/trx0trx.cc:3051                    │
│                       |                                                   │                                                                                  │
│                       |                                                   └──▶ trx_prepare_low(trx, &trx->rsegs.m_redo, false);                              │
│                       |                                                   │    |                    storage/innobase/trx/trx0trx.cc:3067                     │
│                       |                                                   │    └──▶trx_prepare_low() storage/innobase/trx/trx0trx.cc:2989                    │
│                       |                                                   │       └──▶ trx_undo_set_state_at_prepare(trx, undo_ptr->insert_undo, false, &mtr); │
│                       |                                                   |       |     |              storage/innobase/trx/trx0trx.cc:3016                     │
│                       |                                                   |       |     ├── trx_undo_set_state_at_prepare()                                     │
│                       |                                                   |       |     │       │      storage/innobase/trx/trx0undo.cc:1839                    │
│                       |                                                   |       |     │       └──▶ undo->set_prepared(trx->xid);                              │
│                       |                                                   |       |     │       |  |    【Undo内存状态调整】                                      │
│                       |                                                   |       |     │       |  └──▶ state = TRX_UNDO_PREPARED;                             │
│                       |                                                   |       |     │       |  └──▶ xid = *in_xid;                                         │
│                       |                                                   |       |     │       |  └──▶ flag |= TRX_UNDO_FLAG_XID;                             │
│                       |                                                   |       |     │       │       【Undo段状态变为PREPARED，并且记录redo】                   │
│                       |                                                   |       |     │       └──▶               storage/innobase/trx/trx0undo.cc:1872       |
                                                                                      mlog_write_ulint(seg_hdr + TRX_UNDO_STATE, undo->state, MLOG_2BYTES, mtr); │
│                       |                                                   |       |     │       │       【Undo头标识修改，并且记录redo】                           │
│                       |                                                   |       |     │       └──▶               storage/innobase/trx/trx0undo.cc:1874       |
                                                                                      mlog_write_ulint(undo_header + TRX_UNDO_FLAGS, undo->flag, MLOG_1BYTE, mtr); │                                                                                      
│                       |                                                   |       |     │       │       【Undo头XID信息修改，并且记录redo】                           │
│                       |                                                   |       |     │       └──▶               storage/innobase/trx/trx0undo.cc:1876       |
                                                                                                        trx_undo_write_xid(undo_header, &undo->xid, mtr);        │ 
│                       |                                                   |       |     │       │                                                                │
│                       |                                                   |       |     │       └── 在Undo段头部写入PREPARED标记                                │
│                       |                                                   |       |     │               (用于崩溃恢复时识别prepared事务)                        │
│                       |                                                   │       │     【把这个阶段产生的redo，写入redo buffer】                                 │
│                       |                                                   │       └──▶ mtr_commit(&mtr);   storage/innobase/trx/trx0trx.cc:3031               │
│                       |                                                   │       │                                                                           │
│                       |                                                   │                                                                                  │
│                       |                                                   ├── trx->state.store(TRX_STATE_PREPARED)                                          │
│                       |                                                   │       │  storage/innobase/trx/trx0trx.cc:3077                                   │
│                       |                                                   │       │  【事务状态变为PREPARED】                                               │
│                       |                                                   │                                                                                  │
│                       |                                                   └── trx_flush_logs(trx, lsn)                                                      │
│                       |                                                           │  storage/innobase/trx/trx0trx.cc:3185                                   │
│                       |                                                           │  【根据durability_property决定是否刷盘】                               │
│                       |                                                           │  【此时HA_IGNORE_DURABILITY，不刷盘】                                  │
│                       |                                                                                                                                      │
│  ═════════════════════|══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════    │
║                       └──▶ tc_log->commit(thd, all)            sql/handler.cc:1837                                                                ║
║                         └─▶ MYSQL_BIN_LOG::commit()            sql/binlog.cc:8423                                                                ║
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
║               │       ├──▶ ha_flush_logs(true)                sql/handler.cc:2620                                                               ║
║               │       │       │  【刷InnoDB redo log (保证Prepared状态持久化)】                                                                  ║
║               │       │       │──▶ innobase_flush_logs()      storage/innobase/handler/ha_innodb.cc:6262                                      ║
║               │       │       │    └──▶ log_buffer_flush_to_disk()      storage/innobase/include/log0buf.h:196                          ║
║               │       │       │         └──▶ log_buffer_flush_to_disk()      storage/innobase/include/log0buf.h:192                          ║
║               │       │       │            └──▶ log_buffer_flush_to_disk()      storage/innobase/log/log0buf.cc:1193                          ║
║               │       │       │              └──▶ log_write_up_to()             storage/innobase/log/log0write.cc:1086                        ║
║               │       │                                                                                                                          ║
║               │       └──▶ Commit_stage_manager::process_final_stage_for_ordered_commit_group()                                                 ║
║               │               │                               sql/rpl_commit_stage_manager.cc:457                                                ║
║               │                                                                                                                                  ║
║               ├──▶ assign_automatic_gtids_to_flush_group()    sql/binlog.cc:1661                                                                ║
║               │       │  【为组内每个事务分配GTID】                                                                                              ║
║               │       │                                                                                                                          ║
║               │       └── for (THD *head = first_seen; head; head = head->next_to_commit) {                                                      ║
║               │               │                                                                                                                  ║
║               │               ├──▶ gtid_state->specify_transaction_sidno()    sql/rpl_gtid_state.cc:494                                        ║
║               │               │       │  【确定SIDNO】                                                                                           ║
║               │               │                                                                                                                  ║
║               │               └──▶ gtid_state->generate_automatic_gtid()      sql/rpl_gtid_state.cc:514                                        ║
║               │                       │  【分配GNO，设置thd->owned_gtid = {sidno, gno}】                                                         ║
║               │           }                                                                                                                      ║
║               │                                                                                                                                  ║
║               └── for (THD *head = first_seen; head; head = head->next_to_commit) {                                                              ║
║                       │                                       sql/binlog.cc:8843                                                                ║
║                       │                                                                                                                          ║
║                       └──▶ flush_thread_caches(head)          sql/binlog.cc:8732                                                                ║
║                               │                                                                                                                  ║
║                               └──▶ cache_mngr->flush()        sql/binlog.cc:2478                                                                ║
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
║       ├──▶ change_stage(COMMIT_STAGE)                         sql/binlog.cc:9427                                                                ║
║       │                                                                                                                                          ║
║       └──▶ process_commit_stage_queue()                       sql/binlog.cc:9457                                                                ║
║               │                                                                                                                                  ║
║               └──▶ ha_commit_low()                            sql/handler.cc:1938                                                               ║
║                       │  【调用各存储引擎commit】                                                                                                ║
║                       │                                                                                                                          ║
║                       └──▶ innobase_commit()                  storage/innobase/handler/ha_innodb.cc:4449                                        ║
║                               │                                                                                                                  ║
║                               └──▶ trx_commit_for_mysql()     storage/innobase/trx/trx0trx.cc:2505                                              ║
║                                       │                                                                                                          ║
║                                       └──▶ trx_commit()       storage/innobase/trx/trx0trx.cc:2281                                              ║
║                                        └──▶trx_commit_low()                     storage/innobase/trx/trx0trx.cc:2189                          ┃ ║
║                                         └──▶trx_write_serialisation_history()   storage/innobase/trx/trx0trx.cc:1595                          ┃ ║
║                                         | │      ┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓ ║
║                                         | │      ┃ 【最终事务状态变化: PREPARED → COMMITTED_IN_MEMORY】                                     ┃ ║
║                                         | └──▶trx_undo_set_state_at_finish()    storage/innobase/trx/trx0undo.cc:1799                       ┃ ║
║                                         | │      └──▶ undo->state = TRX_UNDO_TO_PURGE          【Undo可被Purge】                                                                                         ┃ ║
║                                         | │      ┃                                                                                          ┃ ║
║                                         | └──▶trx_serialisation_number_get()    storage/innobase/trx/trx0trx.cc:1536                          ┃ ║
║                                         |   └──▶trx_add_to_serialisation_list() storage/innobase/trx/trx0trx.cc:1494                          ┃ ║
║                                         |     │  ┃      ├── trx->no = trx_sys_allocate_trx_no()      【分配提交序号】                          ┃ ║
║                                         |     │                                                                                                  ║
║                                         └──▶ttrx_commit_in_memory()   storage/innobase/trx/trx0trx.cc:1987                                  ┃ ║
║                                             └──▶ trx_release_impl_and_expl_locks() storage/innobase/trx/trx0trx.cc:1881                                                                                                          ║
║                                               │  |   【事务状态调整：PREPARED → COMMITTED_IN_MEMORY】                                          ┃ ║
║                                               │  └──▶ trx->state.store(TRX_STATE_COMMITTED_IN_MEMORY, std::memory_order_relaxed);           ┃ ║
║                                               │  ┃   │                                                                                      ┃ ║
║                                               │  |   【释放所有锁】                                                                        ┃ ║
║                                               │  └──▶ lock_trx_release_locks()                                                           ┃ ║
║                                               │  ┃                                                                                       ┃ ║
║                                               │  ┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛ ║
║                                                                                                                                                  ║
╚══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝
       │
       ▼
┏━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┓
┃                                           INSERT语句执行完成 - 返回客户端OK                                                                       ┃
┗━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━┛

```