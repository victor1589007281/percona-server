# Skill: Schema MVCC (多版本 Schema)

## 1. 模块职责

实现 Schema 级别的多版本控制，使得：
- DDL 期间 RO 查询无阻塞
- 旧查询继续访问旧 schema 版本
- 新查询访问新 schema 版本

**实现语言**: Golang

## 2. 为什么需要 Schema MVCC？

```
传统方案问题 (DDL Barrier):
┌─────────────────────────────────────────────────────────────────┐
│  RW: DROP TABLE t; CREATE TABLE t;                              │
│       │                                                         │
│       ▼                                                         │
│  RO: 正在执行的查询 ──► 阻塞等待 ──► 超时/报错                 │
│                                                                 │
│  问题: DDL 阻塞 RO 查询，影响读性能                             │
└─────────────────────────────────────────────────────────────────┘

Schema MVCC 方案:
┌─────────────────────────────────────────────────────────────────┐
│  RW: DROP TABLE t; CREATE TABLE t;                              │
│       │                                                         │
│       ▼                                                         │
│  旧查询 (snapshot=1050): 继续读旧版本 ✓                         │
│  新查询 (snapshot=1350): 读新版本 ✓                             │
│                                                                 │
│  优势: 无阻塞，读写互不干扰                                     │
└─────────────────────────────────────────────────────────────────┘
```

## 3. 核心概念

### 3.1 Schema Version

```cpp
struct SchemaVersion {
    uint64_t schema_version;    // 全局递增的版本号
    uint32_t space_id;          // 表空间 ID
    string   table_name;        // 表名
    string   db_name;           // 数据库名
    
    uint64_t create_lsn;        // 版本生效的 LSN
    uint64_t drop_lsn;          // 版本失效的 LSN (0=当前有效)
    
    // 表结构元数据
    vector<ColumnDef> columns;
    vector<IndexDef> indexes;
};
```

### 3.2 Schema Snapshot

```cpp
struct SchemaSnapshot {
    uint64_t snapshot_lsn;      // 快照时的 LSN
    
    // 查询可见的表版本 (懒加载缓存)
    HashMap<TableName, SchemaVersion*> visible_tables;
};

// 获取 Schema Snapshot
SchemaSnapshot* get_schema_snapshot(uint64_t lsn) {
    SchemaSnapshot* ss = new SchemaSnapshot();
    ss->snapshot_lsn = lsn;
    return ss;
}
```

### 3.3 可见性判断

```cpp
// 判断某个 schema 版本是否对 snapshot 可见
bool is_visible(SchemaVersion* v, uint64_t snapshot_lsn) {
    // 条件:
    // 1. create_lsn <= snapshot_lsn (已创建)
    // 2. drop_lsn == 0 OR drop_lsn > snapshot_lsn (未删除)
    
    return v->create_lsn <= snapshot_lsn &&
           (v->drop_lsn == 0 || v->drop_lsn > snapshot_lsn);
}

// 查找表的可见版本
SchemaVersion* find_visible_table(
    SchemaSnapshot* ss,
    string db_name,
    string table_name) 
{
    // 遍历该表的所有版本
    for (v in all_versions_of(db_name, table_name)) {
        if (is_visible(v, ss->snapshot_lsn)) {
            return v;
        }
    }
    return NULL;  // 表不存在
}
```

## 4. DDL 处理流程

### 4.1 DROP + CREATE

```
时间线:
────────────────────────────────────────────────────────────────►
  LSN 1000     LSN 1200       LSN 1300
     │            │              │
  CREATE t    DROP t         CREATE t
  (v=1001)   (v=1001失效)    (v=1002)

Schema Version 表:
┌─────────────────────────────────────────────────────────────┐
│ table="t", space=100, v=1001, create=1000, drop=1200       │
│ table="t", space=101, v=1002, create=1300, drop=0          │
└─────────────────────────────────────────────────────────────┘

查询:
• snapshot=1150 → 看到 v=1001 (space=100)
• snapshot=1250 → 表不存在 (1200 < 1250 < 1300)
• snapshot=1350 → 看到 v=1002 (space=101)
```

### 4.2 ALTER TABLE (Instant DDL)

```cpp
void handle_alter_instant(uint32_t space_id, uint64_t old_v,
                          AlterInfo* alter, uint64_t lsn) {
    // 创建新 schema 版本
    SchemaVersion new_v;
    new_v.schema_version = allocate_schema_version();
    new_v.space_id = space_id;  // 复用 space_id
    new_v.create_lsn = lsn;
    new_v.drop_lsn = 0;
    new_v.columns = apply_alter(old_v.columns, alter);
    
    // 标记旧版本失效
    old_v.drop_lsn = lsn;
    
    // 页数据共享 (COW)
    // 新版本的所有页指向旧版本的物理块
    for (page_no in all_pages(space_id, old_v)) {
        PageIndexEntry old_entry = get_page_entry(space_id, old_v, page_no);
        set_page_entry(space_id, new_v, page_no, {
            lba: old_entry.lba,
            is_shared: true,
            shared_with: old_v
        });
    }
}
```

## 5. 存储层支持

### 5.1 页索引扩展

```cpp
// 三元组索引
PageIndex: (space_id, schema_version, page_no) → PageIndexEntry

// 读取页时指定 schema 版本
ReadPageRequest {
    uint32 space_id;
    uint64 schema_version;  // 必须指定!
    uint32 page_no;
    uint64 min_lsn;
}
```

### 5.2 Redo 记录扩展

```cpp
struct RedoRecord {
    uint8_t  type;
    uint32_t space_id;
    uint64_t schema_version;  // 新增: 目标 schema 版本
    uint32_t page_no;
    bytes    body;
};

// 存储层应用 Redo 时检查
void apply_redo(RedoRecord* rec) {
    // 检查 schema version 是否仍活跃
    if (!is_schema_active(rec->space_id, rec->schema_version)) {
        return;  // 跳过已删除的 schema
    }
    // 正常应用
    apply_to_page(rec);
}
```

## 6. RO 节点处理

### 6.1 异步 DDL 处理

```cpp
// RO 节点架构
┌─────────────────────────────────────────────────────────────┐
│  Main Apply Thread          Schema Worker Thread            │
│  ─────────────────          ────────────────────            │
│  Apply Redo ──► DML: 直接应用                               │
│              └► DDL: 分发到 Schema Worker (不阻塞)          │
│                              │                              │
│                              ▼                              │
│                       更新 Schema Version 表                │
│                       (新查询看到新版本)                    │
│                                                             │
│  Query Threads                                              │
│  ─────────────                                              │
│  获取 Schema Snapshot → 根据 snapshot 找可见版本 → 查询     │
│  (DDL 不阻塞查询!)                                          │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 Schema Worker

```cpp
void schema_worker_process_ddl(DDLRedo* ddl) {
    switch (ddl->type) {
    case DDL_CREATE_TABLE:
        // 添加新版本
        schema_versions.add({
            schema_version: next_version(),
            space_id: ddl->space_id,
            table_name: ddl->table_name,
            create_lsn: ddl->lsn,
            drop_lsn: 0
        });
        break;
        
    case DDL_DROP_TABLE:
        // 标记旧版本失效
        old_v = find_active_version(ddl->space_id);
        old_v->drop_lsn = ddl->lsn;
        break;
        
    case DDL_ALTER_TABLE:
        // 创建新版本，标记旧版本失效
        // (具体逻辑取决于 ALTER 类型)
        break;
    }
}
```

## 7. 开发任务

- [ ] 定义 SchemaVersion protobuf
- [ ] 实现 Schema Version 存储
- [ ] 实现 Schema Snapshot
- [ ] 实现可见性判断
- [ ] RO 节点 Schema Worker
- [ ] 页索引三元组支持
- [ ] Redo 记录扩展
- [ ] DDL 处理流程
- [ ] 集成测试

## 8. 参考

- 主文档 8.7: RW/RO 节点一致性与 DDL 多版本设计
- 主文档 附录 D: Schema MVCC 三元组索引实现方案
- Skill 08: RO 一致性
- Skill 09: GC 机制
