# MTS (Multi-Threaded Slave) 多线程复制深度解析

## 备选标题

1. **MySQL MTS揭秘：让从库复制性能飙升的多线程魔法**
2. **深入理解MySQL并行复制：从单线程到多Worker的进化之路**
3. **数据库专家必读：MTS并行复制的调度策略与崩溃恢复机制**

---

## 一、开篇引子

> 想象一下高速公路：如果所有车辆都只能走一条车道，再快的车也会被堵住。MySQL的多线程复制(MTS)就像把单车道扩展成多车道——让多个事务可以并行回放，大幅提升从库的复制速度。

传统的单线程复制(STS)模式下，从库只有一个SQL线程负责回放所有事件，这成为了复制延迟的主要瓶颈。MTS通过引入**Coordinator**和多个**Worker**线程，实现了事务的并行回放。

本文基于 **Percona Server 8.4.3** 源码，深入剖析MTS的工作原理，包括事务调度、进度持久化和崩溃恢复机制。

---

## 二、场景展示

### 2.1 MTS架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MTS (Multi-Threaded Slave) 架构                          │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────────────────────────────────────────────┐  │
│   │                           Relay Log                                 │  │
│   │  ┌─────────────────────────────────────────────────────────────┐   │  │
│   │  │ T1(lc=10,sq=11) T2(lc=10,sq=12) T3(lc=11,sq=13) T4(lc=12,sq=14)│  │
│   │  └─────────────────────────────────────────────────────────────┘   │  │
│   │                              │                                      │  │
│   │                              ▼                                      │  │
│   │                    ┌─────────────────┐                              │  │
│   │                    │   Coordinator   │ ◀── 读取事件，分配给Worker   │  │
│   │                    │   (SQL Thread)  │                              │  │
│   │                    └────────┬────────┘                              │  │
│   │                             │                                       │  │
│   │         ┌───────────────────┼───────────────────┐                   │  │
│   │         ▼                   ▼                   ▼                   │  │
│   │    ┌─────────┐         ┌─────────┐         ┌─────────┐             │  │
│   │    │Worker 0 │         │Worker 1 │         │Worker 2 │             │  │
│   │    │  T1,T2  │         │   T3    │         │   T4    │             │  │
│   │    │(可并行) │         │         │         │         │             │  │
│   │    └────┬────┘         └────┬────┘         └────┬────┘             │  │
│   │         │                   │                   │                   │  │
│   │         ▼                   ▼                   ▼                   │  │
│   │    ┌──────────────────────────────────────────────────┐            │  │
│   │    │              Commit Order Queue                  │            │  │
│   │    │        (保持与主库相同的提交顺序)                  │            │  │
│   │    └──────────────────────────────────────────────────┘            │  │
│   │                                                                     │  │
│   └─────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  关键参数:                                                                  │
│  - replica_parallel_workers: Worker数量                                    │
│  - replica_parallel_type: 并行类型(DATABASE/LOGICAL_CLOCK)                 │
│  - replica_preserve_commit_order: 是否保持提交顺序                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 两种并行类型对比

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MTS 并行类型对比                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌────────────────────────────────┬────────────────────────────────────┐   │
│  │     DATABASE 模式               │       LOGICAL_CLOCK 模式          │   │
│  │  (MTS_PARALLEL_TYPE_DB_NAME)   │  (MTS_PARALLEL_TYPE_LOGICAL_CLOCK) │   │
│  ├────────────────────────────────┼────────────────────────────────────┤   │
│  │                                │                                    │   │
│  │  并行依据: 不同数据库          │  并行依据: last_committed相同      │   │
│  │                                │                                    │   │
│  │  db1.t1 ──▶ Worker 0          │  lc=10 ──▶ Worker 0 (T1,T2,T3)    │   │
│  │  db2.t1 ──▶ Worker 1          │  lc=11 ──▶ Worker 1 (T4,T5)       │   │
│  │  db1.t2 ──▶ Worker 0          │  lc=12 ──▶ Worker 2 (T6)          │   │
│  │                                │                                    │   │
│  │  优点: 简单，兼容性好          │  优点: 并行度高，支持单库          │   │
│  │  缺点: 单库无并行              │  缺点: 需要主库支持                │   │
│  │                                │                                    │   │
│  │  适用: 多库架构                │  适用: 单库高并发                  │   │
│  │                                │                                    │   │
│  └────────────────────────────────┴────────────────────────────────────┘   │
│                                                                             │
│  【LOGICAL_CLOCK原理】                                                      │
│  - last_committed: 事务开始时已提交的最大sequence_number                    │
│  - sequence_number: 事务的唯一序号                                         │
│  - 如果T2.last_committed <= T1.sequence_number，则T2可以与T1并行           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.3 Worker状态机与进度跟踪

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Worker 状态机与Checkpoint机制                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【Worker运行状态】                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                                                                    │    │
│  │   NOT_RUNNING ────▶ RUNNING ────▶ ERROR_LEAVING                   │    │
│  │        │               │               │                           │    │
│  │        │               │               ▼                           │    │
│  │        │               └──────▶ STOP ────▶ STOP_ACCEPTED          │    │
│  │        │                         │                                 │    │
│  │        └─────────────────────────┘                                 │    │
│  │                                                                    │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  【进度跟踪关键字段】                                                        │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │ Worker 结构体关键字段:                                              │    │
│  │                                                                    │    │
│  │   checkpoint_relay_log_name   // checkpoint时的relay log文件名     │    │
│  │   checkpoint_relay_log_pos    // checkpoint时的relay log位置       │    │
│  │   checkpoint_master_log_name  // checkpoint时的master log文件名    │    │
│  │   checkpoint_master_log_pos   // checkpoint时的master log位置      │    │
│  │   group_executed              // bitmap: 已执行的事务组             │    │
│  │   worker_checkpoint_seqno     // 最新checkpoint的序号              │    │
│  │                                                                    │    │
│  │ Relay_log_info 结构体关键字段:                                      │    │
│  │                                                                    │    │
│  │   rli_checkpoint_seqno        // 全局checkpoint序号计数器          │    │
│  │   gaq (Group Assigned Queue)  // 已分配事务组队列                   │    │
│  │   lwm (Low Water Mark)        // 已完成事务的最低水位               │    │
│  │                                                                    │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                                                             │
│  【Checkpoint工作原理】                                                      │
│  ┌────────────────────────────────────────────────────────────────────┐    │
│  │                                                                    │    │
│  │   时间 ───────────────────────────────────────────────────────▶   │    │
│  │                                                                    │    │
│  │   GAQ: [T1|T2|T3|T4|T5|T6|T7|T8]                                  │    │
│  │              ↑              ↑                                      │    │
│  │             LWM         最新分配                                   │    │
│  │         (T1,T2完成)                                               │    │
│  │                                                                    │    │
│  │   Checkpoint触发条件:                                              │    │
│  │   1. 定时 (每opt_mta_checkpoint_period毫秒)                        │    │
│  │   2. 完成事务数达到opt_mta_checkpoint_group                        │    │
│  │                                                                    │    │
│  │   Checkpoint内容:                                                  │    │
│  │   - 更新rli的group_master_log_pos到LWM位置                         │    │
│  │   - 更新各Worker的checkpoint信息                                   │    │
│  │   - 持久化到slave_relay_log_info表                                 │    │
│  │                                                                    │    │
│  └────────────────────────────────────────────────────────────────────┘    │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 三、原理深入

### 3.1 事务调度机制

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     Coordinator 事务调度流程                                  │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                    Coordinator 调度决策                              │  │
│  │                                                                      │  │
│  │   读取Event                                                          │  │
│  │       │                                                              │  │
│  │       ▼                                                              │  │
│  │   是GTID/BEGIN Event?                                               │  │
│  │       │ 是                                                           │  │
│  │       ▼                                                              │  │
│  │   解析last_committed和sequence_number                               │  │
│  │       │                                                              │  │
│  │       ▼                                                              │  │
│  │   ┌─────────────────────────────────────────────────────────┐       │  │
│  │   │           等待条件检查                                    │       │  │
│  │   │                                                         │       │  │
│  │   │   if (last_committed > LWM.sequence_number)            │       │  │
│  │   │       // 有依赖，需要等待                                │       │  │
│  │   │       wait until LWM >= last_committed                  │       │  │
│  │   │   else                                                  │       │  │
│  │   │       // 无依赖，可以立即分配                            │       │  │
│  │   │                                                         │       │  │
│  │   └─────────────────────────────────────────────────────────┘       │  │
│  │       │                                                              │  │
│  │       ▼                                                              │  │
│  │   选择Worker (get_least_occupied_worker)                            │  │
│  │       │                                                              │  │
│  │       ▼                                                              │  │
│  │   将事务加入Worker的工作队列                                         │  │
│  │   register_trx() // 注册到Commit Order                              │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【Worker选择策略】                                                         │
│  - DATABASE模式: 根据database名hash选择                                    │
│  - LOGICAL_CLOCK模式: 选择队列最短的Worker                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 GAQ (Group Assigned Queue) 机制

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     GAQ (Group Assigned Queue) 工作原理                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【GAQ结构】                                                                 │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   GAQ是环形队列，存储已分配给Worker但未完成的事务组信息              │  │
│  │                                                                      │  │
│  │   ┌─────┬─────┬─────┬─────┬─────┬─────┬─────┬─────┐               │  │
│  │   │ G0  │ G1  │ G2  │ G3  │ G4  │ G5  │ G6  │ G7  │               │  │
│  │   │done │done │exec │exec │wait │wait │ --  │ --  │               │  │
│  │   └─────┴─────┴─────┴─────┴─────┴─────┴─────┴─────┘               │  │
│  │       ↑                   ↑                   ↑                    │  │
│  │      LWM              assigned             tail                    │  │
│  │   (已完成)           (最新分配)           (队尾)                   │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【Slave_job_group结构】                                                    │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   struct Slave_job_group {                                          │  │
│  │     char *group_master_log_name;  // 主库binlog文件名               │  │
│  │     my_off_t group_master_log_pos; // 主库binlog位置                │  │
│  │     char *group_relay_log_name;   // relay log文件名                │  │
│  │     my_off_t group_relay_log_pos; // relay log位置                  │  │
│  │     ulong worker_id;              // 分配的Worker ID                 │  │
│  │     longlong sequence_number;     // 事务序号                       │  │
│  │     longlong last_committed;      // 依赖的最大已提交序号            │  │
│  │     bool done;                    // 是否完成                       │  │
│  │   };                                                                 │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【LWM更新流程】                                                            │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   Worker完成事务 ──▶ 标记GAQ[i].done = true                         │  │
│  │                           │                                          │  │
│  │                           ▼                                          │  │
│  │   Coordinator检查 ──▶ 从LWM开始连续done的个数                       │  │
│  │                           │                                          │  │
│  │                           ▼                                          │  │
│  │   移动LWM ──▶ LWM += count_done                                     │  │
│  │                           │                                          │  │
│  │                           ▼                                          │  │
│  │   触发Checkpoint ──▶ 持久化新的进度位置                              │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 3.3 崩溃恢复机制

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MTS 崩溃恢复流程                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【崩溃场景】                                                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   Checkpoint位置                 Worker实际执行位置                   │  │
│  │        ↓                              ↓                              │  │
│  │   ┌────┬────┬────┬────┬────┬────┬────┬────┐                        │  │
│  │   │ T1 │ T2 │ T3 │ T4 │ T5 │ T6 │ T7 │ T8 │                        │  │
│  │   └────┴────┴────┴────┴────┴────┴────┴────┘                        │  │
│  │        ↑                    ↑                                        │  │
│  │       持久化               崩溃点                                    │  │
│  │                                                                      │  │
│  │   崩溃时: T1-T4已checkpoint持久化，T5-T6已执行但未持久化             │  │
│  │   恢复时: 从T4开始，但T5,T6需要跳过(已执行)                          │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【恢复流程】                                                                │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │                                                                      │  │
│  │   1. init_recovery() 入口                                            │  │
│  │          │                                                           │  │
│  │          ▼                                                           │  │
│  │   2. mts_recovery_groups() 计算GAP                                   │  │
│  │          │                                                           │  │
│  │          │   a. 读取每个Worker的checkpoint信息                       │  │
│  │          │   b. 比较checkpoint位置与Worker实际位置                   │  │
│  │          │   c. 构建recovery_groups bitmap                           │  │
│  │          │                                                           │  │
│  │          ▼                                                           │  │
│  │   3. fill_mts_gaps_and_recover() 填充GAP                            │  │
│  │          │                                                           │  │
│  │          │   a. 设置UNTIL_SQL_AFTER_MTS_GAPS                         │  │
│  │          │   b. 启动SQL线程执行到GAP填充完成                         │  │
│  │          │   c. 跳过已执行的事务(通过bitmap检查)                     │  │
│  │          │                                                           │  │
│  │          ▼                                                           │  │
│  │   4. mts_finalize_recovery() 完成恢复                                │  │
│  │          │                                                           │  │
│  │          │   a. 重置Worker状态                                       │  │
│  │          │   b. 清理恢复相关数据结构                                  │  │
│  │          │   c. 持久化新的checkpoint                                  │  │
│  │          │                                                           │  │
│  │          ▼                                                           │  │
│  │   5. 正常启动MTS                                                     │  │
│  │                                                                      │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  【GTID模式的优化】                                                         │
│  当GTID_MODE=ON且使用AUTO_POSITION时:                                       │
│  - 无需计算GAP                                                              │
│  - 依靠GTID自动跳过已执行事务                                               │
│  - 恢复更快更简单                                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 四、源码根因揭秘

**源码版本：Percona Server 8.4.3-3**

### 4.1 Coordinator调度函数调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Coordinator 事务调度完整调用链                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【SQL线程主循环】                                                           │
│  handle_slave_sql()                        sql/rpl_replica.cc:4500         │
│  └── exec_relay_log_event()                sql/rpl_replica.cc:4600         │
│      │                                                                      │
│      │  【读取事件】                                                         │
│      ├── Log_event::read_event()           sql/log_event.cc               │
│      │                                                                      │
│      │  【LOGICAL_CLOCK模式调度】                                           │
│      └── Mts_submode_logical_clock::schedule_next_event()                  │
│          │                                 sql/rpl_mta_submode.cc:578      │
│          │                                                                  │
│          │  【解析GTID Event中的依赖信息】                                   │
│          ├── 解析last_committed和sequence_number                           │
│          │   ptr_group->sequence_number = ev->sequence_number;             │
│          │   ptr_group->last_committed = ev->last_committed;               │
│          │                                                                  │
│          │  【等待依赖事务完成】                                             │
│          ├── if (last_committed > LWM)                                     │
│          │   └── wait_for_workers_to_finish()                              │
│          │       │                         sql/rpl_mta_submode.cc:480      │
│          │       │                                                          │
│          │       └── while (lwm < last_committed && !killed)               │
│          │           mysql_cond_wait(&rli->logical_clock_cond)             │
│          │                                                                  │
│          │  【选择Worker】                                                   │
│          ├── get_least_occupied_worker()   sql/rpl_mta_submode.cc:700      │
│          │   │                                                              │
│          │   │  // 选择curr_jobs最少的Worker                               │
│          │   └── for each worker:                                          │
│          │       if (worker->curr_jobs < min_jobs)                         │
│          │           selected = worker;                                    │
│          │                                                                  │
│          │  【分配事务】                                                     │
│          └── append_item_to_jobs()         sql/rpl_rli_pdb.cc:1300        │
│              │                                                              │
│              │  【注册到Commit Order Queue】                                 │
│              ├── Commit_order_manager::register_trx()                      │
│              │                             sql/rpl_replica_commit_         │
│              │                             order_manager.cc:60             │
│              │                                                              │
│              │  【加入Worker工作队列】                                       │
│              └── worker->jobs.push(job_item)                               │
│                  mysql_cond_signal(&worker->jobs_cond) // 唤醒Worker       │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Checkpoint持久化调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              Checkpoint 持久化完整调用链                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【触发Checkpoint】                                                          │
│  mta_checkpoint_routine()                  sql/rpl_replica.cc:6515         │
│  │                                                                          │
│  │  【计算连续完成的事务数】                                                 │
│  ├── cnt = rli->gaq->count_done(rli)       sql/rpl_rli_pdb.cc:800         │
│  │   │                                                                      │
│  │   │  // 从LWM开始计算连续done=true的数量                                 │
│  │   └── while (ptr->done && ptr != assigned_end)                          │
│  │       cnt++;                                                             │
│  │                                                                          │
│  │  【移动LWM指针】                                                          │
│  ├── rli->gaq->move_queue_head()           sql/rpl_rli_pdb.cc:850         │
│  │   │                                                                      │
│  │   │  // 更新LWM位置信息                                                  │
│  │   ├── lwm.group_master_log_pos = done_group->group_master_log_pos      │
│  │   ├── lwm.group_relay_log_pos = done_group->group_relay_log_pos        │
│  │   │                                                                      │
│  │   │  // 通知等待的Coordinator                                            │
│  │   └── mysql_cond_signal(&rli->logical_clock_cond)                       │
│  │                                                                          │
│  │  【更新RLI位置】                                                          │
│  ├── rli->set_group_master_log_pos(lwm.group_master_log_pos)              │
│  ├── rli->set_group_relay_log_pos(lwm.group_relay_log_pos)                │
│  │                                                                          │
│  │  【持久化到信息表】                                                       │
│  └── rli->flush_info()                     sql/rpl_rli.cc:1200            │
│      │                                                                      │
│      │  【更新slave_relay_log_info表】                                      │
│      ├── handler->flush_info()                                             │
│      │   │                                                                  │
│      │   │  // 持久化字段:                                                  │
│      │   │  // - Relay_log_name                                            │
│      │   │  // - Relay_log_pos                                             │
│      │   │  // - Master_log_name                                           │
│      │   │  // - Master_log_pos                                            │
│      │   │  // - Sql_delay                                                 │
│      │   │  // - Number_of_workers                                         │
│      │   │                                                                  │
│      │   └── Rpl_info_table::do_flush_info()                              │
│      │       // UPDATE mysql.slave_relay_log_info SET ...                  │
│      │                                                                      │
│      │  【更新Worker信息表】                                                │
│      └── for each worker:                                                  │
│          worker->flush_info()              sql/rpl_rli_pdb.cc:500         │
│          │                                                                  │
│          │  // 持久化到mysql.slave_worker_info表                           │
│          │  // - Checkpoint_relay_log_name                                 │
│          │  // - Checkpoint_relay_log_pos                                  │
│          │  // - Checkpoint_master_log_name                                │
│          │  // - Checkpoint_master_log_pos                                 │
│          │  // - Checkpoint_seqno                                          │
│          │  // - Checkpoint_group_bitmap                                   │
│          └── Rpl_info_table::do_flush_info()                              │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.3 崩溃恢复调用链

```
┌─────────────────────────────────────────────────────────────────────────────┐
│              MTS 崩溃恢复完整调用链                                           │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  【恢复入口】                                                                │
│  init_recovery()                           sql/rpl_replica.cc:1162         │
│  │                                                                          │
│  │  【计算恢复GAP】                                                          │
│  └── mts_recovery_groups()                 sql/rpl_replica.cc:6259         │
│      │                                                                      │
│      │  【GTID模式快速路径】                                                 │
│      ├── if (GTID_MODE == ON && auto_position)                             │
│      │   return false;  // 无需计算GAP                                     │
│      │                                                                      │
│      │  【读取每个Worker的Checkpoint信息】                                   │
│      ├── for each worker:                                                  │
│      │   │                                                                  │
│      │   │  // 从slave_worker_info读取                                     │
│      │   ├── w = Rpl_info_factory::create_worker(...)                      │
│      │   │                                                                  │
│      │   │  // 比较worker位置与RLI checkpoint位置                          │
│      │   ├── if (mts_event_coord_cmp(w_pos, rli_pos) > 0)                  │
│      │   │   above_lwm_jobs.push_back(w);  // 此Worker有未持久化事务       │
│      │   │                                                                  │
│      │   └── 按master_log_pos排序above_lwm_jobs                            │
│      │                                                                      │
│      │  【构建recovery_groups bitmap】                                       │
│      ├── for each worker in above_lwm_jobs:                                │
│      │   │                                                                  │
│      │   │  // 扫描relay log找到worker执行的事务                           │
│      │   ├── while (ev = read_event())                                     │
│      │   │   if (ev_coord == w_last)                                       │
│      │   │       // 将worker的group_executed复制到recovery_groups          │
│      │   │       bitmap_copy(recovery_groups, w->group_executed)           │
│      │   │                                                                  │
│      │   └── recovery_group_cnt = max(recovery_group_cnt, w_count)         │
│      │                                                                      │
│      └── rli->mts_recovery_group_cnt = recovery_group_cnt                  │
│                                                                             │
│  【填充GAP】                                                                 │
│  fill_mts_gaps_and_recover()               sql/rpl_replica.cc:1219         │
│  │                                                                          │
│  │  【设置UNTIL条件】                                                        │
│  ├── rli->until_condition = UNTIL_SQL_AFTER_MTS_GAPS                       │
│  │                                                                          │
│  │  【启动SQL线程】                                                          │
│  ├── start_slave_thread(..., handle_slave_sql)                             │
│  │                                                                          │
│  │  【SQL线程执行GAP填充】                                                   │
│  │  exec_relay_log_event() 中:                                             │
│  │  ├── if (rli->is_mts_recovery())                                        │
│  │  │   │                                                                  │
│  │  │   │  // 检查事务是否需要跳过                                          │
│  │  │   └── if (bitmap_is_set(recovery_groups, recovery_index))            │
│  │  │       // 跳过此事务，它在崩溃前已执行                                 │
│  │  │       skip = true;                                                   │
│  │  │                                                                      │
│  │  │  // 更新恢复进度                                                      │
│  │  └── rli->mts_recovery_index++;                                         │
│  │      if (--rli->mts_recovery_group_cnt == 0)                            │
│  │          // GAP填充完成                                                  │
│  │          rli->until_condition = UNTIL_DONE;                             │
│  │                                                                          │
│  │  【等待SQL线程完成】                                                      │
│  ├── mysql_cond_wait(&rli->stop_cond)                                      │
│  │                                                                          │
│  │  【恢复relay log】                                                        │
│  ├── recover_relay_log(mi)                                                 │
│  │                                                                          │
│  │  【持久化新状态】                                                         │
│  └── rli->flush_info()                                                     │
│                                                                             │
│  【完成恢复】                                                                │
│  mts_finalize_recovery()                   sql/rpl_rli.cc:440             │
│  │                                                                          │
│  │  【重置Worker状态】                                                       │
│  ├── for each worker:                                                      │
│  │   w->reset_recovery_info()                                              │
│  │                                                                          │
│  │  【清理多余Worker记录】                                                   │
│  └── for (i = recovery_parallel_workers; i > current_workers; i--)         │
│      w->remove_info();  // 删除slave_worker_info中多余记录                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.4 Worker执行和进度更新核心代码

```c
// sql/rpl_rli_pdb.cc:1390-1465
/** Worker执行事务后更新进度 */
bool Slave_worker::commit_positions(Log_event *ev, Slave_job_group *ptr_g,
                                    bool force) {
  DBUG_TRACE;
  
  // 【关键】更新checkpoint相关位置
  strmake(checkpoint_relay_log_name, ptr_g->checkpoint_relay_log_name,
          sizeof(checkpoint_relay_log_name) - 1);
  checkpoint_relay_log_pos = ptr_g->checkpoint_relay_log_pos;
  
  strmake(checkpoint_master_log_name, ptr_g->checkpoint_master_log_name,
          sizeof(checkpoint_master_log_name) - 1);
  checkpoint_master_log_pos = ptr_g->checkpoint_master_log_pos;
  
  // 【关键】更新group_executed bitmap
  // 标记此事务组已执行
  if (ptr_g->checkpoint_seqno > worker_checkpoint_seqno) {
    // 需要移动bitmap
    bitmap_shift(group_executed, 
                 ptr_g->checkpoint_seqno - worker_checkpoint_seqno);
    worker_checkpoint_seqno = ptr_g->checkpoint_seqno;
  }
  
  // 设置对应bit表示已执行
  bitmap_set_bit(group_executed, 
                 ptr_g->checkpoint_seqno - c_rli->rli_checkpoint_seqno);
  
  // 【关键】标记GAQ中此事务组完成
  ptr_g->done = true;
  
  return false;
}

// sql/rpl_replica.cc:6515-6620
bool mta_checkpoint_routine(Relay_log_info *rli, bool force) {
  ulong cnt;
  bool error = false;
  
  DBUG_TRACE;
  
  // 【关键】计算连续完成的事务数
  do {
    cnt = rli->gaq->count_done(rli);
  } while (!sql_slave_killed(rli->info_thd, rli) && cnt == 0 && force);
  
  if (cnt == 0) goto end;
  
  // 【关键】更新jobs_done计数
  if (!is_mts_worker(rli->info_thd) && !is_mts_db_partitioned(rli)) {
    static_cast<Mts_submode_logical_clock *>(rli->current_mts_submode)
        ->jobs_done += cnt;
  }
  
  mysql_mutex_lock(&rli->data_lock);
  
  // 【关键】更新RLI位置到LWM
  rli->set_group_master_log_pos(rli->gaq->lwm.group_master_log_pos);
  rli->set_group_relay_log_pos(rli->gaq->lwm.group_relay_log_pos);
  
  if (rli->gaq->lwm.group_relay_log_name[0] != 0)
    rli->set_group_relay_log_name(rli->gaq->lwm.group_relay_log_name);
  
  // 【关键】持久化到信息表
  error = rli->flush_info(Relay_log_info::RLI_FLUSH_IGNORE_SYNC_OPT);
  
  mysql_cond_broadcast(&rli->data_cond);
  mysql_mutex_unlock(&rli->data_lock);
  
  // 【关键】通知Coordinator可能有新的可调度事务
  reset_notified_checkpoint(rli, cnt, rli->gaq->lwm.ts, true);
  
end:
  return error;
}
```

---

## 五、优化与修复

### 5.1 MTS性能优化

| 参数 | 说明 | 建议值 |
|:----|:----|:------|
| **replica_parallel_workers** | Worker数量 | CPU核数*2 |
| **replica_parallel_type** | 并行类型 | LOGICAL_CLOCK |
| **replica_preserve_commit_order** | 保持提交顺序 | ON (一致性要求高时) |
| **binlog_transaction_dependency_tracking** | 依赖跟踪方式 | WRITESET |
| **slave_checkpoint_period** | Checkpoint间隔(毫秒) | 300 |
| **slave_checkpoint_group** | Checkpoint事务数 | 512 |

### 5.2 常见问题与解决

**问题1：并行度不高**
```
症状：Worker利用率低，从库延迟
原因：主库事务依赖关系复杂，last_committed值相近的事务少
解决：
  1. 主库使用WRITESET依赖跟踪
  2. 优化业务减少跨表事务
  3. 增大binlog_group_commit_sync_delay
```

**问题2：崩溃恢复时间长**
```
症状：从库重启后恢复耗时
原因：GAP过大，需要重放大量relay log
解决：
  1. 减小slave_checkpoint_period
  2. 减小slave_checkpoint_group
  3. 开启GTID模式
```

---

## 六、总结与反思

### 6.1 核心要点回顾

1. **MTS架构**：Coordinator负责调度，多个Worker并行执行
2. **两种并行模式**：DATABASE(按库)和LOGICAL_CLOCK(按依赖)
3. **GAQ机制**：跟踪已分配但未完成的事务，支持LWM计算
4. **Checkpoint持久化**：定期保存进度到slave_relay_log_info和slave_worker_info
5. **崩溃恢复**：通过bitmap计算GAP，执行UNTIL_SQL_AFTER_MTS_GAPS填充

### 6.2 MTS完整交互时序图

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     MTS 完整交互时序图                                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  Coordinator          Worker0              Worker1              Storage     │
│       │                  │                    │                    │        │
│  读取Event               │                    │                    │        │
│       │                  │                    │                    │        │
│  解析依赖                │                    │                    │        │
│  (lc,sq)                │                    │                    │        │
│       │                  │                    │                    │        │
│  检查LWM                 │                    │                    │        │
│       │                  │                    │                    │        │
│  选择Worker              │                    │                    │        │
│       │                  │                    │                    │        │
│  分配事务──────────────▶│                    │                    │        │
│       │                  │ 执行事务           │                    │        │
│       │                  │────────────────────────────────────────▶│        │
│       │                  │                    │                    │        │
│  分配事务────────────────────────────────────▶│                    │        │
│       │                  │                    │ 执行事务           │        │
│       │                  │                    │───────────────────▶│        │
│       │                  │                    │                    │        │
│       │                  │ 提交              │                    │        │
│       │                  │◀───────────────────────────────────────│        │
│       │                  │                    │                    │        │
│       │                  │ 等待CommitOrder   │                    │        │
│       │                  │                    │                    │        │
│       │                  │ 标记done          │                    │        │
│       │◀─────────────────│                    │                    │        │
│       │                  │                    │ 提交               │        │
│       │                  │                    │◀──────────────────│        │
│       │                  │                    │                    │        │
│       │                  │                    │ 标记done           │        │
│       │◀────────────────────────────────────────│                    │        │
│       │                  │                    │                    │        │
│  移动LWM                 │                    │                    │        │
│       │                  │                    │                    │        │
│  触发Checkpoint          │                    │                    │        │
│       │──────────────────────────────────────────────────────────▶│        │
│       │                  │                    │     持久化info     │        │
│       │                  │                    │                    │        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

**【示意图描述】**
`![MTS架构流程图](mts_architecture_flow.png): 展示MTS的Coordinator-Worker架构，事务调度决策流程，GAQ队列和LWM的更新机制，Checkpoint的持久化过程，以及崩溃恢复时GAP计算和填充的完整流程。`

---

> 📝 **作者注**：本文基于Percona Server 8.4.3源码分析，不同版本实现细节可能略有差异。如有疑问，欢迎在评论区交流讨论。
