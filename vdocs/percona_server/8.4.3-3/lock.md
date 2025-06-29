# MySQL 锁系统
## 问题
**1. MDL_INTENTION_EXCLUSIVE 跟 MDL_EXCLUSIVE 有什么区别？为什么 alter database * read only=1 lock_schema_name 后需要添加这两个锁？另外，为什么 MDL_INTENTION_EXCLUSIVE 是MDL_STATEMENT，而 MDL_EXCLUSIVE 是MDL_TRANSACTION？**

解答：
### 1.1 MDL_INTENTION_EXCLUSIVE与MDL_EXCLUSIVE的核心区别
| 特性 | MDL_INTENTION_EXCLUSIVE (IX) | MDL_EXCLUSIVE (X) |
|------|------------------------------|-------------------|
| **兼容性** | 与共享锁兼容，与排他锁冲突 | 与所有锁冲突 |
| **粒度** | **数据库级别** | **数据库级别** |
| **用途** | 标记数据库元数据变更意图 | 执行数据库级排他操作 |
| **锁定粒度** | 表级意向锁定 | 表级排他锁定 |
| **主要作用** | 表示后续可能需要排他锁，阻止其他事务获取X锁 | 完全阻止其他事务访问元数据 |
| **典型场景** | DDL操作的预备阶段 | DDL操作的执行阶段 |

### 1.2 ALTER DATABASE设置只读需要双锁的原因
`ALTER DATABASE ... READ ONLY=1 LOCK_SCHEMA_NAME`操作需要**同时持有**两种锁而非升级关系：
1. **持有IX锁**：
   - 阻止其他事务获取冲突的X锁（避免并发DDL冲突）
   - 允许现有读事务继续执行（保证读一致性）
   - 标记数据库处于元数据变更预备状态

2. **同时持有X锁**：
   - 确保修改`read_only`参数期间完全阻塞其他元数据操作
   - 实现数据库只读状态变更的原子性
   - 防止并发事务导致的元数据不一致

**为何两者能共存**：在MDL锁机制中，意向锁(IX)与显式排他锁(X)属于不同锁模式层级。IX锁表示「可能需要后续排他操作」的意向声明，而X锁是实际执行排他操作的锁。Percona Server在执行数据库级只读变更时采用两阶段锁定设计，两者在操作期间同时存在于`m_tickets_store`中，直到操作完成后按生命周期规则释放。

#### 源码佐证
```cpp
// sql/lock.cc 中 lock_schema_name 函数实现片段
MDL_REQUEST_INIT(&mdl_request, MDL_key::SCHEMA, db, 
```

2. MDL锁票证(ticket)存储于`MDL_context`的`m_ticket_store`容器中，不同生命周期的锁（STATEMENT/TRANSACTION）分属不同存储链表，因此IX和X锁可同时存在：
   ```c++
   // 源码片段引自`sql/mdl.cc`中MDL_context释放锁的逻辑
   m_ticket_store.remove(duration, ticket);  // duration区分STATEMENT/TRANSACTION
   ```
### 1.3 锁生命周期差异的原因
1. **MDL_INTENTION_EXCLUSIVE使用MDL_STATEMENT生命周期**：
   - IX锁仅在语句执行期间持有，**语句执行完毕后由服务器自动释放**
   - 释放逻辑通过`MDL_context::release_all_locks()`在语句结束时触发
   - 对应源码：`m_ticket_store.remove(MDL_STATEMENT, ticket);`

2. **MDL_EXCLUSIVE使用MDL_TRANSACTION生命周期**：
   - X锁需要在**整个事务期间持有**，事务提交/回滚后由`ha_commit_trans`释放
   - 释放逻辑通过`MDL_context::release_transactional_locks()`触发
   - 对应源码：`m_ticket_store.remove(MDL_TRANSACTION, ticket);`

**为何commit后锁仍存在**：
`trans_commit_stmt(thd) || trans_commit(thd)`调用仅触发事务提交流程，而非立即释放锁。MDL锁释放发生在事务提交的**后续阶段**：
1. **事务提交异步性**：提交操作涉及日志写入、存储引擎确认等步骤，锁释放是提交成功后的收尾动作
2. **调用栈顺序**：`mysql_alter_db`函数中commit调用返回仅表示提交流程启动，实际释放发生在更高层级的调用栈中（如`sql_parse.cc`的`mysql_execute_command`函数中显式调用`release_transactional_locks`）
3. **源码佐证**：
   ```c++
   // 事务提交后才调用锁释放逻辑（引自sql/mdl.cc）
   void MDL_context::release_transactional_locks() {
     m_ticket_store.remove(MDL_TRANSACTION, ticket);
   }
   ```

**MDL锁与行锁释放机制的差异**：
| 锁类型 | 释放机制 | 适用原则 | 典型释放时机 |
|--------|----------|----------|--------------|
| **MDL锁** | 按生命周期（STATEMENT/TRANSACTION）释放 | 语句/事务结束时自动释放 | `mysql_execute_command`执行完毕后 |
| **行锁（InnoDB）** | 事务提交/回滚时统一释放 | "prepare阶段申请，commit阶段释放" | `ha_commit_trans`调用时 |

原因在于：MDL锁属于**元数据锁**，需保证语句执行期间元数据一致性；行锁属于**数据锁**，遵循ACID事务隔离原则。两者分属不同锁系统，由服务器层和存储引擎层分别管理。

**为何return时锁仍存在**：
在`mysql_alter_db`函数返回时，虽然ALTER DATABASE操作已执行，但：
1. IX锁（MDL_STATEMENT）需等待当前语句执行周期完全结束（函数返回≠语句结束）
2. X锁（MDL_TRANSACTION）需等待事务提交（DDL操作默认自动提交，但提交逻辑在函数返回后执行）
3. 锁票证仍存在于`m_ticket_store`的对应生命周期链表中，直至各自释放条件满足。
   - 避免长时间阻塞其他事务的读操作
   - 符合"最小权限原则"，仅在必要阶段持有

2. **MDL_EXCLUSIVE使用MDL_TRANSACTION生命周期**：
   - X锁需要在整个事务期间持有，确保操作的原子性
   - 防止在事务提交前，其他事务修改元数据
   - 与InnoDB事务隔离级别保持一致

**实现依据**：Percona Server 8.4.3-3继承自MySQL 8.0的MDL机制，在`sql/mdl.cc`中定义了这两种锁的类型和生命周期管理。对于数据库级别的`read_only`修改，需要严格的元数据保护，因此采用IX→X的两阶段锁定策略。
