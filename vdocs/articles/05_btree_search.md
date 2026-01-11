# B+树搜索定位逻辑深度分析

## 概述

InnoDB 使用 B+树作为核心索引结构，INSERT/UPDATE/DELETE 操作都需要通过 B+树搜索定位到目标记录位置。本文深入分析 B+树搜索的完整调用链和算法实现。

## B+树搜索核心架构

```mermaid
graph TB
    subgraph "**B+树搜索架构**"
        A[**SQL层<br/>ha_index_read_map()**]
        B[**Handler层<br/>ha_innobase::index_read()**]
        C[**Row层<br/>row_search_mvcc()**]
        D[**Btr层<br/>btr_cur_search_to_nth_level()**]
        E[**Page层<br/>page_cur_search_with_match()**]
        F[**目标记录**]
    end
    
    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    
    style A fill:#ffe1e1,stroke:#333,stroke-width:2px,color:#000
    style B fill:#e1ffe1,stroke:#333,stroke-width:2px,color:#000
    style C fill:#e1f5ff,stroke:#333,stroke-width:2px,color:#000
    style D fill:#fff3e1,stroke:#333,stroke-width:2px,color:#000
    style E fill:#f5e1ff,stroke:#333,stroke-width:2px,color:#000
    style F fill:#fffacd,stroke:#333,stroke-width:2px,color:#000
```

## INSERT/UPDATE/DELETE 中的 B+树搜索调用链

### INSERT 语句的搜索定位

\`\`\`text
ha_innobase::write_row() - storage/innobase/handler/ha_innodb.cc:10024
├── row_insert_for_mysql() - storage/innobase/row/row0mysql.cc:1853
│   └── row_ins_step() - storage/innobase/row/row0ins.cc:3632
│       └── row_ins() - storage/innobase/row/row0ins.cc:3546
│           └── row_ins_index_entry() - storage/innobase/row/row0ins.cc:3353
│               └── row_ins_clust_index_entry() - storage/innobase/row/row0ins.cc:3119
│                   │
│                   └── row_ins_clust_index_entry_low() - storage/innobase/row/row0ins.cc:2673
│                       │  【聚簇索引插入的核心函数】
│                       │
│                       ├── ★ btr_cur_search_to_nth_level() - storage/innobase/btr/btr0cur.cc:638
│                       │   │  【B+树搜索定位到叶子节点】
│                       │   │  【mode=PAGE_CUR_LE，搜索<=目标key的位置】
│                       │   │
│                       │   └── 【详细搜索流程见下文】
│                       │
│                       └── btr_cur_optimistic_insert() - storage/innobase/btr/btr0cur.cc:2959
│                           │  【乐观插入，在定位到的位置插入记录】
│                           │
│                           └── page_cur_tuple_insert() - storage/innobase/page/page0cur.cc:1420
│                               └── 【在页内插入记录】
\`\`\`

### UPDATE 语句的搜索定位

\`\`\`text
ha_innobase::update_row() - storage/innobase/handler/ha_innodb.cc:10494
├── row_update_for_mysql() - storage/innobase/row/row0mysql.cc:2945
│   └── row_update_for_mysql_using_upd_graph() - storage/innobase/row/row0mysql.cc:2756
│       └── row_upd_step() - storage/innobase/row/row0upd.cc:3243
│           └── row_upd() - storage/innobase/row/row0upd.cc:3151
│               └── row_upd_clust_step() - storage/innobase/row/row0upd.cc:2987
│                   │
│                   ├── ★ btr_pcur_restore_position() - storage/innobase/include/btr0pcur.ic:540
│                   │   │  【恢复游标位置（从上次读取位置）】
│                   │   │  【内部调用 btr_cur_search_to_nth_level()】
│                   │   │
│                   │   └── btr_cur_search_to_nth_level()
│                   │
│                   └── btr_cur_optimistic_update() / btr_cur_pessimistic_update()
│                       │  【执行更新操作】
\`\`\`

### DELETE 语句的搜索定位

\`\`\`text
ha_innobase::delete_row() - storage/innobase/handler/ha_innodb.cc:10660
├── row_update_for_mysql() - storage/innobase/row/row0mysql.cc:2945
│   │  【DELETE在InnoDB内部是UPDATE操作，设置delete mark】
│   │
│   └── row_update_for_mysql_using_upd_graph() - storage/innobase/row/row0mysql.cc:2756
│       └── row_upd_step() - storage/innobase/row/row0upd.cc:3243
│           └── row_upd() - storage/innobase/row/row0upd.cc:3151
│               └── row_upd_clust_step() - storage/innobase/row/row0upd.cc:2987
│                   │
│                   ├── ★ btr_pcur_restore_position()
│                   │   └── btr_cur_search_to_nth_level()
│                   │
│                   └── row_upd_del_mark_clust_rec() - storage/innobase/row/row0upd.cc:2938
│                       │  【设置delete mark】
│                       │
│                       └── btr_cur_del_mark_set_clust_rec() - storage/innobase/btr/btr0cur.cc:4344
\`\`\`

## B+树搜索核心函数详解

### btr_cur_search_to_nth_level() 完整调用链

\`\`\`text
btr_cur_search_to_nth_level() - storage/innobase/btr/btr0cur.cc:638
│  【B+树多层搜索的核心函数】
│  【参数说明】
│  │  index      : 要搜索的索引
│  │  level      : 搜索停止的层级 (0=叶子层)
│  │  tuple      : 搜索键值
│  │  mode       : PAGE_CUR_L/LE/G/GE 搜索模式
│  │  latch_mode : BTR_SEARCH_LEAF/BTR_MODIFY_LEAF 等
│  │  cursor     : 输出，定位到的游标位置
│
├── 初始化与参数检查
│   ├── ut_ad(level == 0 || mode == PAGE_CUR_LE)  【非叶子层只支持LE模式】
│   ├── cursor->flag = BTR_CUR_BINARY             【默认使用二分查找】
│   └── cursor->index = index                      【设置游标的索引】
│
├── 获取索引根节点
│   ├── space = index->space                       【索引所在表空间】
│   ├── page_no = index->page                      【根页号】
│   │
│   └── buf_page_get_gen() - storage/innobase/buf/buf0buf.cc:4356
│       │  【从Buffer Pool获取根页面】
│       │
│       └── 如果页面不在内存
│           └── buf_read_page() → 从磁盘读取
│
├── 从根节点开始逐层向下搜索
│   │
│   │  ┌────────────────────────────────────────────────────────────────────────────────────────────────┐
│   │  │                              B+树层级结构                                                       │
│   │  ├────────────────────────────────────────────────────────────────────────────────────────────────┤
│   │  │                                                                                                │
│   │  │     Level 3 (根)     ┌────────────────────────────────────────┐                               │
│   │  │                      │  [key1, ptr1] [key2, ptr2] [key3, ptr3] │                               │
│   │  │                      └──────────┬─────────┬─────────┬─────────┘                               │
│   │  │                                 │         │         │                                          │
│   │  │     Level 2 (内节点) ┌──────────┴─────────┴─────────┴──────────┐                               │
│   │  │                      │  非叶子节点，存储key和子节点指针         │                               │
│   │  │                      └──────────┬─────────┬─────────┬─────────┘                               │
│   │  │                                 │         │         │                                          │
│   │  │     Level 1 (内节点) ┌──────────┴─────────┴─────────┴──────────┐                               │
│   │  │                      │  非叶子节点                              │                               │
│   │  │                      └──────────┬─────────┬─────────┬─────────┘                               │
│   │  │                                 │         │         │                                          │
│   │  │     Level 0 (叶子)   ┌──────────┴─────────┴─────────┴──────────┐                               │
│   │  │                      │  叶子节点，存储完整记录（聚簇索引）       │                               │
│   │  │                      │  或 key + 主键（二级索引）               │                               │
│   │  │                      └────────────────────────────────────────┘                               │
│   │  │                                                                                                │
│   │  └────────────────────────────────────────────────────────────────────────────────────────────────┘
│   │
│   └── while (height > level) {  【从根到目标层级】
│           │
│           ├── 1. 在当前页内二分查找
│           │   │
│           │   └── ★ page_cur_search_with_match() - storage/innobase/page/page0cur.cc:328
│           │       │  【页内二分查找，详见下文】
│           │       │
│           │       └── 返回：cursor指向目标记录或其相邻位置
│           │
│           ├── 2. 如果是非叶子节点，获取子节点指针
│           │   │
│           │   └── node_ptr = btr_node_ptr_get_child_page_no()
│           │       │  【从非叶子节点记录中提取子节点页号】
│           │
│           ├── 3. 释放父节点锁（根据latch_mode）
│           │   │
│           │   └── btr_cur_latch_leaves() 或释放非叶子节点的latch
│           │
│           ├── 4. 获取子节点页面
│           │   │
│           │   └── buf_page_get_gen(space, node_ptr, ...)
│           │       │  【从Buffer Pool获取子节点页面】
│           │
│           └── 5. height-- 继续下一层
│       }
│
├── 到达目标层级，执行最终页内定位
│   │
│   └── page_cur_search_with_match()
│       │  【在叶子节点(或目标层)执行精确定位】
│       │
│       └── cursor->page_cur 指向目标记录
│
└── 设置输出参数
    ├── cursor->up_match = 向上匹配的字段数
    ├── cursor->low_match = 向下匹配的字段数
    └── cursor->page_cur = 页内游标位置
\`\`\`

### page_cur_search_with_match() 页内二分查找详解

\`\`\`text
page_cur_search_with_match() - storage/innobase/page/page0cur.cc:328
│  【页内二分查找的核心函数】
│  【在单个B+树页内定位记录】
│
├── 参数说明
│   │  block    : 页面缓冲块
│   │  index    : 索引定义
│   │  tuple    : 搜索键值
│   │  mode     : PAGE_CUR_L/LE/G/GE
│   │            ┌──────────────────────────────────────────────────────────────────────────┐
│   │            │ PAGE_CUR_L  : 定位到第一个 < tuple 的记录                                 │
│   │            │ PAGE_CUR_LE : 定位到第一个 <= tuple 的记录 (INSERT使用)                   │
│   │            │ PAGE_CUR_G  : 定位到第一个 > tuple 的记录                                 │
│   │            │ PAGE_CUR_GE : 定位到第一个 >= tuple 的记录 (SELECT使用)                   │
│   │            └──────────────────────────────────────────────────────────────────────────┘
│   │  iup_matched_fields  : 已匹配的上界字段数(输入输出)
│   │  ilow_matched_fields : 已匹配的下界字段数(输入输出)
│   │  cursor   : 输出，页内游标
│
├── 页目录结构说明
│   │
│   │  ┌────────────────────────────────────────────────────────────────────────────────────────────────┐
│   │  │                              InnoDB 页内结构                                                    │
│   │  ├────────────────────────────────────────────────────────────────────────────────────────────────┤
│   │  │                                                                                                │
│   │  │  Page Header (38 bytes)                                                                        │
│   │  │       │                                                                                        │
│   │  │       ▼                                                                                        │
│   │  │  ┌─────────────────────────────────────────────────────────────────────────────────────────┐  │
│   │  │  │  Infimum Record  │ Rec1 │ Rec2 │ Rec3 │ ... │ RecN │ Supremum Record                   │  │
│   │  │  └─────────────────────────────────────────────────────────────────────────────────────────┘  │
│   │  │       ▲                ▲                           ▲          ▲                               │
│   │  │       │                │                           │          │                               │
│   │  │  ┌────┴────┐     ┌────┴────┐              ┌───────┴─┐   ┌────┴────┐                          │
│   │  │  │ Slot 0  │     │ Slot 1  │   ...        │ Slot N-1│   │ Slot N  │  ← Page Directory        │
│   │  │  │(Infimum)│     │ (own:4-8│              │         │   │(Supremum│                          │
│   │  │  └─────────┘     │  recs)  │              └─────────┘   └─────────┘                          │
│   │  │                  └─────────┘                                                                  │
│   │  │                                                                                                │
│   │  │  Page Directory: 存储部分记录的偏移量，用于加速二分查找                                          │
│   │  │  每个Slot指向一个"拥有者"记录，该记录"拥有"其后的4-8条记录                                        │
│   │  │                                                                                                │
│   │  └────────────────────────────────────────────────────────────────────────────────────────────────┘
│
├── 第一阶段：页目录二分查找
│   │
│   │  low = 0                           【Infimum slot】
│   │  up = page_dir_get_n_slots() - 1   【Supremum slot】
│   │
│   └── while (up - low > 1) {
│           │
│           ├── mid = (low + up) / 2
│           │
│           ├── slot = page_dir_get_nth_slot(page, mid)
│           │   │  【获取第mid个slot】
│           │
│           ├── mid_rec = page_dir_slot_get_rec(slot)
│           │   │  【获取slot指向的记录】
│           │
│           ├── cmp = cmp_dtuple_rec_with_match()
│           │   │  【比较搜索键与mid_rec】
│           │   │
│           │   │  ┌─────────────────────────────────────────────────────────────────────────────────┐
│           │   │  │ cmp_dtuple_rec_with_match() 字段比较逻辑                                        │
│           │   │  ├─────────────────────────────────────────────────────────────────────────────────┤
│           │   │  │ 1. 从cur_matched_fields开始比较（利用之前的比较结果）                           │
│           │   │  │ 2. 逐字段比较，更新cur_matched_fields                                          │
│           │   │  │ 3. 返回值：                                                                     │
│           │   │  │    < 0 : tuple < rec                                                            │
│           │   │  │    = 0 : tuple = rec                                                            │
│           │   │  │    > 0 : tuple > rec                                                            │
│           │   │  └─────────────────────────────────────────────────────────────────────────────────┘
│           │
│           └── if (cmp > 0) {
│                   low = mid
│                   low_matched_fields = cur_matched_fields
│               } else {
│                   up = mid
│                   up_matched_fields = cur_matched_fields
│               }
│       }
│
├── 第二阶段：线性搜索Slot内的记录
│   │
│   │  【此时 up - low = 1，目标在low_slot和up_slot之间】
│   │
│   │  low_rec = page_dir_slot_get_rec(low_slot)   【low slot的记录】
│   │  up_rec = page_dir_slot_get_rec(up_slot)     【up slot的记录】
│   │
│   └── while (low_rec != up_rec) {
│           │
│           ├── mid_rec = page_rec_get_next(low_rec)
│           │   │  【获取下一条记录】
│           │
│           ├── cmp = cmp_dtuple_rec_with_match()
│           │   │  【比较搜索键与mid_rec】
│           │
│           └── if (cmp > 0) {
│                   low_rec = mid_rec
│               } else {
│                   up_rec = mid_rec
│                   break  【找到目标范围】
│               }
│       }
│
└── 根据mode设置游标位置
    │
    ├── if (mode <= PAGE_CUR_GE) {
    │       page_cur_position(up_rec, block, cursor)
    │   }
    │
    └── else {  【PAGE_CUR_L 或 PAGE_CUR_LE】
            page_cur_position(low_rec, block, cursor)
        }
\`\`\`

## 搜索模式与使用场景

\`\`\`text
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    搜索模式 (PAGE_CUR_MODE) 使用场景                                        │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                            │
│  ┌─────────────────┬──────────────────────────────────────────────────────────────────────────────────────┐│
│  │ 模式             │ 使用场景                                                                             ││
│  ├─────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤│
│  │ PAGE_CUR_LE     │ 【INSERT操作】                                                                        ││
│  │ (<=)            │ 定位到 <= 目标key 的最后一条记录                                                      ││
│  │                 │ 新记录将插入到该位置之后                                                               ││
│  │                 │ 代码: row_ins_clust_index_entry_low(..., PAGE_CUR_LE, ...)                           ││
│  ├─────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤│
│  │ PAGE_CUR_GE     │ 【SELECT/UPDATE/DELETE操作】                                                          ││
│  │ (>=)            │ 定位到 >= 目标key 的第一条记录                                                        ││
│  │                 │ 用于精确查找或范围扫描起点                                                             ││
│  │                 │ 代码: row_search_mvcc(..., PAGE_CUR_GE, ...)                                         ││
│  ├─────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤│
│  │ PAGE_CUR_G      │ 【范围查询 key > value】                                                              ││
│  │ (>)             │ 定位到 > 目标key 的第一条记录                                                         ││
│  │                 │ 用于开区间范围扫描                                                                     ││
│  ├─────────────────┼──────────────────────────────────────────────────────────────────────────────────────┤│
│  │ PAGE_CUR_L      │ 【范围查询 key < value】                                                              ││
│  │ (<)             │ 定位到 < 目标key 的最后一条记录                                                       ││
│  │                 │ 用于反向扫描或定位前驱                                                                 ││
│  └─────────────────┴──────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
\`\`\`

## Latch 模式与并发控制

\`\`\`text
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    B+树搜索的Latch模式                                                      │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                            │
│  ┌─────────────────────────┬──────────────────────────────────────────────────────────────────────────────┐│
│  │ Latch模式                │ 说明                                                                         ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_SEARCH_LEAF          │ 【S-latch on leaf】                                                          ││
│  │                         │ 只读查询，对叶子节点加S锁                                                      ││
│  │                         │ 用于: 普通SELECT查询                                                          ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_MODIFY_LEAF          │ 【X-latch on leaf】                                                          ││
│  │                         │ 修改叶子节点，乐观操作（不会导致分裂/合并）                                    ││
│  │                         │ 用于: 乐观INSERT/UPDATE/DELETE                                                ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_MODIFY_TREE          │ 【X-latch on index】                                                         ││
│  │                         │ 可能修改树结构（分裂/合并）                                                    ││
│  │                         │ 用于: 悲观INSERT/UPDATE/DELETE                                                ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_CONT_MODIFY_TREE     │ 【继续持有X-latch on index】                                                 ││
│  │                         │ 在已持有索引锁的情况下继续修改                                                 ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_SEARCH_PREV          │ 【S-latch，需要访问前驱节点】                                                 ││
│  │                         │ 用于反向扫描                                                                   ││
│  ├─────────────────────────┼──────────────────────────────────────────────────────────────────────────────┤│
│  │ BTR_MODIFY_PREV          │ 【X-latch，需要访问前驱节点】                                                 ││
│  │                         │ 用于可能影响前驱的修改                                                         ││
│  └─────────────────────────┴──────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
│  【Latch coupling（锁耦合）策略】                                                                           │
│  在树遍历过程中，先获取子节点latch，再释放父节点latch                                                         │
│  确保在任意时刻至少持有一个节点的latch，防止并发修改导致的问题                                                 │
│                                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
\`\`\`

## 关键数据结构

\`\`\`text
btr_cur_t 结构 - storage/innobase/include/btr0cur.h:668
│
├── dict_index_t *index          【当前索引】
│
├── page_cur_t page_cur          【页内游标】
│   │
│   ├── buf_block_t *block       【当前页缓冲块】
│   │
│   ├── rec_t *rec               【当前记录指针】
│   │
│   └── ulint *offsets           【记录字段偏移量数组】
│
├── ulint up_match               【向上(大于)方向匹配的字段数】
│   │  用于优化后续比较，避免重复比较已匹配的字段
│
├── ulint low_match              【向下(小于)方向匹配的字段数】
│
├── btr_cur_method flag          【搜索方法标志】
│   │
│   │  ┌─────────────────────────┬──────────────────────────────────────────────────────────────────────────┐
│   │  │ BTR_CUR_HASH             │ 通过AHI(Adaptive Hash Index)命中                                        │
│   │  │ BTR_CUR_HASH_FAIL        │ AHI查找失败，回退到B+树搜索                                               │
│   │  │ BTR_CUR_BINARY           │ 标准B+树二分查找                                                          │
│   │  │ BTR_CUR_INSERT_TO_IBUF   │ 插入到Change Buffer                                                      │
│   │  └─────────────────────────┴──────────────────────────────────────────────────────────────────────────┘
│
├── buf_block_t *left_block      【左邻居页（BTR_SEARCH_PREV使用）】
│
└── que_thr_t *thr               【查询线程（用于Change Buffer）】

dtuple_t 结构 (搜索键) - storage/innobase/include/data0type.h
│
├── ulint n_fields               【字段数】
│
├── ulint n_fields_cmp           【用于比较的字段数】
│
├── dfield_t *fields             【字段数组】
│   │
│   └── dfield_t 结构
│       ├── void *data           【字段数据指针】
│       ├── ulint len            【数据长度】
│       └── dtype_t type         【字段类型】
│
└── ulint info_bits              【记录信息位（如delete mark）】
\`\`\`

## 搜索性能优化

\`\`\`text
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│                                    B+树搜索性能优化机制                                                     │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                                            │
│  【1. Adaptive Hash Index (AHI)】                                                                          │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │ 原理：对热点页面建立哈希索引，直接定位到叶子节点记录                                                     ││
│  │                                                                                                        ││
│  │ btr_cur_search_to_nth_level() {                                                                        ││
│  │     // 尝试AHI查找                                                                                     ││
│  │     if (btr_search_guess_on_hash(index, info, tuple, mode, latch_mode, cursor, ...)) {                 ││
│  │         cursor->flag = BTR_CUR_HASH;  // AHI命中                                                       ││
│  │         return;  // 跳过B+树遍历                                                                       ││
│  │     }                                                                                                  ││
│  │     cursor->flag = BTR_CUR_BINARY;  // 回退到B+树搜索                                                  ││
│  │     ...                                                                                                ││
│  │ }                                                                                                      ││
│  │                                                                                                        ││
│  │ 位置: storage/innobase/btr/btr0sea.cc                                                                  ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
│  【2. 匹配字段复用 (Matched Fields Optimization)】                                                         │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │ 原理：记录已匹配的字段数，下次比较从该位置开始                                                           ││
│  │                                                                                                        ││
│  │ // 假设 tuple = (10, 'abc', 100), rec = (10, 'abc', 200)                                               ││
│  │ // 第一次比较后: cur_matched_fields = 2 (前两个字段匹配)                                                ││
│  │ // 下次比较时从第3个字段开始，跳过已匹配的字段                                                          ││
│  │                                                                                                        ││
│  │ cmp_dtuple_rec_with_match(tuple, rec, &cur_matched_fields, &cur_matched_bytes)                         ││
│  │                                                                                                        ││
│  │ 位置: storage/innobase/rem/rem0cmp.cc                                                                  ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
│  【3. 缓存偏移量 (Cached Offsets)】                                                                        │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │ 原理：对于固定格式的索引，预计算记录字段偏移量                                                           ││
│  │                                                                                                        ││
│  │ if (cached_offsets && !page_cur_has_null(mid_rec, index)) {                                            ││
│  │     // 使用预计算的偏移量，避免每次重新计算                                                             ││
│  │     offsets = cached_offsets;                                                                          ││
│  │ } else {                                                                                               ││
│  │     offsets = rec_get_offsets(mid_rec, index, ...);                                                    ││
│  │ }                                                                                                      ││
│  │                                                                                                        ││
│  │ 位置: storage/innobase/page/page0cur.cc:455                                                            ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
│  【4. Page Directory（页目录）】                                                                            │
│  ┌────────────────────────────────────────────────────────────────────────────────────────────────────────┐│
│  │ 原理：将页内记录分组，每组4-8条记录，通过目录项实现O(log n)查找                                          ││
│  │                                                                                                        ││
│  │ 搜索复杂度：                                                                                           ││
│  │   - 无目录：O(n) 线性扫描                                                                               ││
│  │   - 有目录：O(log(n/k) + k) ≈ O(log n)，其中k为每组记录数(4-8)                                          ││
│  │                                                                                                        ││
│  │ 位置: page_cur_search_with_match() 第一阶段                                                             ││
│  └────────────────────────────────────────────────────────────────────────────────────────────────────────┘│
│                                                                                                            │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
\`\`\`

## 关键函数速查表

| 层级 | 函数名 | 文件路径 | 行号 | 功能说明 |
|:-----|:-------|:---------|:-----|:---------|
| **Handler** | `ha_innobase::index_read()` | storage/innobase/handler/ha_innodb.cc | 11287 | 索引读取入口 |
| **Row** | `row_search_mvcc()` | storage/innobase/row/row0sel.cc | 4825 | MVCC查询入口 |
| | `row_ins_clust_index_entry_low()` | storage/innobase/row/row0ins.cc | 2673 | 聚簇索引插入 |
| **Btr** | `btr_cur_search_to_nth_level()` | storage/innobase/btr/btr0cur.cc | 638 | B+树层级搜索核心 |
| | `btr_pcur_open()` | storage/innobase/include/btr0pcur.ic | 455 | 打开持久化游标 |
| | `btr_pcur_restore_position()` | storage/innobase/include/btr0pcur.ic | 540 | 恢复游标位置 |
| **Page** | `page_cur_search_with_match()` | storage/innobase/page/page0cur.cc | 328 | 页内二分查找 |
| | `page_cur_search_with_match_bytes()` | storage/innobase/page/page0cur.cc | 614 | 带字节比较的页内搜索 |
| **Cmp** | `cmp_dtuple_rec_with_match()` | storage/innobase/rem/rem0cmp.cc | 795 | 元组与记录比较 |
| **Buffer** | `buf_page_get_gen()` | storage/innobase/buf/buf0buf.cc | 4356 | 获取页面缓冲 |
| **AHI** | `btr_search_guess_on_hash()` | storage/innobase/btr/btr0sea.cc | 1156 | AHI查找 |

