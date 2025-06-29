# Database Read-Only 的实现原理

## ALTER DATABASE READ ONLY 源码分析

MySQL 8.4 中 `ALTER DATABASE db_name READ ONLY=1` 语句的实现主要涉及以下几个关键部分：

1. **SQL解析和执行入口**
   - 位于 `sql_db.cc` 文件中的 `mysql_alter_db` 函数
   - 处理 ALTER DATABASE 语句的核心逻辑

2. **只读状态检查**
   - 首先检查数据库当前是否已处于只读状态
   - 如果已经是只读状态，则只允许以下操作：
     - 关闭只读模式（READ ONLY=0）
     - 保持只读状态不变（不允许同时修改其他选项）

3. **只读状态设置**
   - 通过 `Schema_impl::set_read_only()` 方法设置只读状态
   - 实际将 `read_only` 选项存储在数据字典的 `options` 属性中

4. **数据字典交互**
   - `Schema_impl` 类（dd/impl/types/schema_impl.h）提供了读写只读状态的方法：
     - `read_only()`: 获取当前只读状态
     - `set_read_only()`: 设置只读状态
   - 底层通过数据字典的 `options` 属性存储 `read_only` 标志

5. **并发控制**
   - 在执行 ALTER DATABASE 时会锁定相关表
   - 防止在修改只读状态时有并发写操作

## 实现流程

1. 用户执行 `ALTER DATABASE db_name READ ONLY=1`
2. MySQL 解析语句并调用 `mysql_alter_db` 函数
3. 检查当前数据库是否已处于只读状态及操作合法性
4. 锁定相关表以防止并发修改
```c++
  if (lock_schema_name(thd, db)) return true;

  if (find_db_tables(thd, *schema, db, &tables) ||
      lock_table_names(thd, tables, nullptr, thd->variables.lock_wait_timeout,
                       0))
    return true;
```
5. 通过 `Schema_impl::set_read_only(true)` 设置只读状态
```c++
    schema->set_read_only(create_info->schema_read_only);
```  
6. 将修改持久化到数据字典中
```c++
  // Update schema.
  if (thd->dd_client()->update(schema)) return true;
```   
7. 【可选】记录日志。迁移场景通常在执行这个命令的时候关闭日志记录
```c++
  ha_binlog_log_query(thd, nullptr, LOGCOM_ALTER_DB, thd->query().str,
                      thd->query().length, db, "");

  if (write_db_cmd_to_binlog(thd, db, true)) return true;
···
8. Commit： 这里会释放上面持有的MDL锁
```c++
  if (trans_commit_stmt(thd) || trans_commit(thd)) return true;

```  
9. 关闭涉及的handler表
```c++
    mysql_ha_flush_tables(thd, tables);
```
```c++
# 调用存储引擎的index_end/rnd_end方法,清理游标以及m_record_buffer，以达到重置状态以及避免资源不释放，内存泄露的问题。
    tables->table->file->ha_index_or_rnd_end();
    tables->table->open_by_handler = false;
# 把这个表从线程打开表链表中移除，table obj 释放占用状态，放回table cache manager管理    
    close_thread_table(thd, &tables->table);
    ```c++
        # TABLE结构体内部维护的MDL锁引用,与表缓存机制关联，生命周期由系统管理
        table->mdl_ticket = nullptr;
        table->pos_in_table_list = nullptr;
        *table_ptr = table->next;
        release_or_close_table(thd, table);
        ```c++
              # 把table obj 从used链表中拿掉，放到unused链表中，等待后续复用
              Table_cache *tc = table_cache_manager.get_cache(thd);
              tc->release_table(thd, table);
             ```c++
                    table->in_use = nullptr;
                    el->used_tables.remove(table);
                    link_unused_table(table);
             ```     
        ```    
    ```
    # Table_ref结构体中的MDL请求票据,显式(EXPLICIT)释放线程占有的MDL锁
/*
MDL_ticket 是MySQL元数据锁系统的核心数据结构，包含以下关键信息：

1. m_lock ：指向对应的 MDL_lock 对象，表示具体的锁资源
2. m_duration ：锁的生命周期类型（STATEMENT/TRANSACTION/EXPLICIT）
3. m_type ：锁的类型（如SHARED_READ、EXCLUSIVE等）
4. m_ctx ：所属的MDL上下文
*/    
    thd->mdl_context.release_lock(tables->mdl_request.ticket);
    ```c++
          # 从对应duration存储中移除
          m_ticket_store.remove(duration, ticket);  
          # 更新锁状态
          lock->remove_ticket(this, m_pins, &MDL_lock::m_granted, ticket);
          # 唤醒锁等待者
         if (ticket->m_hton_notified) {
           mysql_mdl_set_status(ticket->m_psi, MDL_ticket::POST_RELEASE_NOTIFY);
           m_owner->notify_hton_post_release_exclusive(&key_for_hton);
         }
         # 删除ticket对象
         MDL_ticket::destroy(ticket);
    ```      
```
10. 删除table cache manager中的表缓存
```c++
      tdc_remove_table(thd, TDC_RT_REMOVE_ALL, table->db, table->table_name,
                       false);
```  

## 只读状态的生效

当数据库被标记为只读后，后续对该数据库的写操作（INSERT/UPDATE/DELETE等）会被拒绝，并返回 `ER_SCHEMA_READ_ONLY` 错误。
1. check_schema_readonly 进行数据库只读状态判断
* 可以忽略数据库只读状态的场景
```c++
  if (thread_can_ignore_schema_read_only(thd)) return false;
  # 特殊的线程或者标注
  ```c++
  # 启动线程
  # 升级线程
  # 从库线程
  # 跳过只读模式的SQL语句
  return (thd->is_bootstrap_system_thread() ||
          thd->is_server_upgrade_thread() || thd->slave_thread ||
          thd->is_cmd_skip_readonly());
  
  # 指定的SQL类型
  if (thd->lex->sql_command == SQLCOM_ALTER_DB ||
      thd->lex->sql_command == SQLCOM_CREATE_DB)
    return false;  
  # 数据库不存在  
    if (sch_obj == nullptr) return false;

```
2. check_schema_readonly 函数被调用的时机分析
* 打开对象(view/trigger/存储过程)前
* 需要锁定表/数据库的时候(申请MDL的时候)
* IMPORT操作的时候