# MySQL dd_cache (数据字典缓存) 总结

## 1. 功能概述
dd_cache (dictionary cache) 是MySQL中用于缓存数据字典对象的共享内存缓存系统，旨在提高数据字典访问性能并减少磁盘I/O操作。它存储数据库元数据信息，如表结构、字符集、排序规则等，避免频繁从磁盘读取相同信息。

## 2. 核心结构
### 2.1 主要组件
- **Shared_dictionary_cache**：核心缓存管理类，定义于`<mcfile name="shared_dictionary_cache.h" path="/home/victor/work/mysql/mysqldoc/percona-server/sql/dd/impl/cache/shared_dictionary_cache.h"></mcfile>`
- **Shared_multi_map**：模板化的多映射容器，为每种数据字典对象类型提供独立缓存
- **Dictionary_client**：缓存客户端，处理缓存的获取、更新和失效
- **Storage_adapter**：连接缓存与底层存储系统的适配器

### 2.2 缓存对象类型
缓存系统为不同类型的数据字典对象维护独立缓存：
```cpp
// 部分缓存对象类型示例
Shared_multi_map<Abstract_table> m_abstract_table_map;      // 表定义缓存
Shared_multi_map<Collation> m_collation_map;                // 排序规则缓存
Shared_multi_map<Column_statistics> m_column_stat_map;      // 列统计信息缓存
Shared_multi_map<Tablespace> m_tablespace_map;              // 表空间缓存
Shared_multi_map<Resource_group> m_resource_group_map;      // 资源组缓存
```

## 3. 缓存配置
各类型对象的缓存容量在初始化时设定，基于对象类型的特性和使用频率：
```cpp
// 缓存容量定义（shared_dictionary_cache.h）
static const size_t collation_capacity = 256;               // 排序规则缓存容量
static const size_t column_statistics_capacity = 32;        // 列统计信息缓存容量
static const size_t charset_capacity = 64;                  // 字符集缓存容量
static const size_t resource_group_capacity = 32;           // 资源组缓存容量
```

初始化过程在`init()`方法中完成：`<mcfile name="shared_dictionary_cache.cc" path="/home/victor/work/mysql/mysqldoc/percona-server/sql/dd/impl/cache/shared_dictionary_cache.cc"></mcfile>`

## 4. 缓存策略
### 4.1 驱逐策略
采用LRU（最近最少使用）算法管理缓存项，当缓存达到容量上限时触发驱逐：
```cpp
// LRU驱逐实现（shared_multi_map.cc）
void Shared_multi_map<T>::rectify_free_list(Autolocker *lock) {
  while ((map_capacity_exceeded() || DBUG_EVALUATE_IF("simulate_dd_elements_cache_full", true, false)) &&
         m_free_list.length() > 0) {
    Cache_element<T> *e = m_free_list.get_lru();  // 获取LRU元素
    m_free_list.remove(e);
    e->use();
    remove(e, lock);  // 移除元素
  }
}
```

### 4.2 缓存失效与更新
- **显式失效**：通过`Dictionary_client::invalidate()`方法处理元数据变更时的缓存失效
- **自动更新**：对象修改时通过`Storage_adapter::core_store()`和`core_update()`更新缓存
- **事务一致性**：维护提交和未提交对象的分离存储，确保事务隔离

## 5. 关键操作流程
### 5.1 对象获取流程
1. 尝试从缓存获取对象
2. 缓存未命中时从磁盘加载
3. 将加载的对象放入缓存

### 5.2 对象更新流程
1. 获取对象的独占锁
2. 更新内存中的对象
3. 标记缓存项为脏
4. 提交时更新磁盘并刷新缓存

## 6. 性能优化点
- **类型分离**：不同类型对象独立缓存，避免相互干扰
- **预分配容量**：根据对象类型特性设置合理容量，减少频繁驱逐
- **细粒度锁定**：使用`MUTEX_LOCK`确保缓存操作线程安全的同时最小化锁竞争
- **延迟加载**：仅在需要时加载对象，减少内存占用

## 7. 代码参考
- 缓存管理核心实现：`<mcfile name="shared_dictionary_cache.cc" path="/home/victor/work/mysql/mysqldoc/percona-server/sql/dd/impl/cache/shared_dictionary_cache.cc"></mcfile>`
- 缓存客户端操作：`<mcfile name="dictionary_client.cc" path="/home/victor/work/mysql/mysqldoc/percona-server/sql/dd/impl/cache/dictionary_client.cc"></mcfile>`
- 存储适配器：`<mcfile name="storage_adapter.cc" path="/home/victor/work/mysql/mysqldoc/percona-server/sql/dd/impl/cache/storage_adapter.cc"></mcfile>`