# MySQL 8.4 CBO (Cost-Based Optimizer) 深度架构分析

## 概述

MySQL 8.4 采用了先进的CBO（基于成本的优化器）架构，它能够基于统计信息和成本模型来选择最优的执行计划。本文档深入分析CBO的完整架构、数据采集机制、决策过程以及算子下推等核心功能。

## CBO总体架构

```mermaid
graph TB
    subgraph "SQL解析层"
        A["SQL查询"]
        B["语法解析器"]
        C["语义分析器"]
        D["查询块生成"]
    end
    
    subgraph "CBO优化器架构"
        E["传统优化器<br/>Classic Optimizer"]
        F["超图优化器<br/>Hypergraph Optimizer"]
        G["优化器选择器"]
    end
    
    subgraph "统计信息系统"
        H["表统计信息<br/>Table Statistics"]
        I["列直方图<br/>Histograms"]
        J["索引统计<br/>Index Statistics"]
        K["成本常数<br/>Cost Constants"]
    end
    
    subgraph "成本模型"
        L["服务器成本模型<br/>Server Cost Model"]
        M["存储引擎成本<br/>Engine Cost Model"]
        N["操作成本计算<br/>Operation Costing"]
    end
    
    subgraph "执行计划生成"
        O["访问路径枚举<br/>Access Path Enumeration"]
        P["连接顺序优化<br/>Join Order Optimization"]
        Q["算子下推<br/>Predicate Pushdown"]
        R["最优计划选择<br/>Plan Selection"]
    end
    
    A --> B
    B --> C
    C --> D
    D --> G
    
    G --> E
    G --> F
    
    E --> O
    F --> O
    
    H --> L
    I --> L
    J --> L
    K --> M
    
    L --> N
    M --> N
    
    N --> O
    O --> P
    P --> Q
    Q --> R
```

## 双优化器架构详解

### 1. 传统优化器（Classic Optimizer）

**位置：** `sql/sql_optimizer.cc`

```cpp
/** 传统优化器的主要入口 */
bool JOIN::optimize() {
  DBUG_TRACE;
  
  // 1. 逻辑变换阶段
  if (optimize_cond()) return true;
  
  // 2. 常量表提取
  if (extract_const_tables()) return true;
  if (extract_func_dependent_tables()) return true;
  
  // 3. 行数估算
  if (estimate_rowcount()) return true;
  
  // 4. 连接顺序优化
  if (Optimize_table_order(thd, this, nullptr).choose_table_order())
    return true;
    
  // 5. 生成执行计划
  if (get_best_combination()) return true;
  
  // 6. 后优化处理
  if (!plan_is_const()) {
    test_skip_sort();  // 测试是否可以跳过排序
    if (finalize_table_conditions(thd)) return true;
  }
  
  return false;
}
```

### 2. 超图优化器（Hypergraph Optimizer）

**位置：** `sql/join_optimizer/join_optimizer.cc`

```cpp
/** 超图优化器的主要流程 */
AccessPath *FindBestQueryPlan(THD *thd, Query_block *query_block) {
  // 1. 构建连接超图
  JoinHypergraph graph(thd->mem_root, query_block);
  if (MakeJoinHypergraph(thd, &graph)) return nullptr;
  
  // 2. 收集有趣的排序
  LogicalOrderings orderings(thd);
  BuildInterestingOrders(thd, &graph, query_block, &orderings, /*...*/);
  
  // 3. 枚举子计划并选择最优路径
  CostingReceiver receiver(thd, graph.nodes.size(), &orderings, &graph);
  if (EnumerateSubgraphPairs(&graph, &receiver)) return nullptr;
  
  // 4. 构建最终访问路径
  Mem_root_array<AccessPath *> root_candidates = 
    receiver.root_candidates();
  
  return root_candidates.empty() ? nullptr : root_candidates[0];
}
```

#### 超图优化器特点对比

| 特性 | 传统优化器 | 超图优化器 |
|------|-----------|-----------|
| **算法复杂度** | O(n!) 指数级 | O(3^n) 可控指数 |
| **连接枚举** | 基于左深树 | 支持任意连接形状 |
| **谓词处理** | 分阶段处理 | 统一下推框架 |
| **并行度** | 有限 | 更好的并行支持 |
| **扩展性** | 受限于表数量 | 更好的可扩展性 |

## 统计信息采集系统

### 1. 表统计信息采集

**位置：** `sql/dd/info_schema/table_stats.cc`

```cpp
/** 表统计信息更新 */
bool update_table_stats(THD *thd, Table_ref *table) {
  TABLE *analyze_table = table->table;
  handler *file = analyze_table->file;
  
  // 获取存储引擎统计信息
  if (analyze_table->file->info(HA_STATUS_VARIABLE | 
                               HA_STATUS_TIME |
                               HA_STATUS_VARIABLE_EXTRA | 
                               HA_STATUS_AUTO) != 0)
    return true;
  
  // 更新统计信息到数据字典
  std::unique_ptr<Table_stat> ts_obj(create_object<Table_stat>());
  setup_table_stats_record(thd, ts_obj.get(), 
                           dd::String_type(table->db, strlen(table->db)),
                           dd::String_type(table->alias, strlen(table->alias)), 
                           file->stats, file->checksum(),
                           file->ha_table_flags() & (ulong)HA_HAS_CHECKSUM,
                           analyze_table->found_next_number_field);
  
  return thd->dd_client()->store(ts_obj.get());
}
```

### 2. 列直方图采集

**位置：** `sql/histograms/histogram.cc`

```cpp
/** 直方图采集的核心函数 */
static bool fill_value_maps(
    const Mem_root_array<HistogramSetting> &settings,
    double sample_percentage, TABLE *table, 
    value_map_collection &value_maps) {
  
  // 使用抽样方法采集数据
  void *scan_ctx = nullptr;
  int sampling_seed = static_cast<int>(global_histogram_sampling_seed++);
  
  // 初始化抽样
  if (table->file->ha_sample_init(scan_ctx, sample_percentage, 
                                 sampling_seed, SYSTEM, false)) {
    return true;
  }
  
  // 读取抽样数据到Value_maps
  int res = table->file->ha_sample_next(scan_ctx, table->record[0]);
  while (res == 0) {
    // 处理每一行数据
    for (const auto &setting : settings) {
      Field *field = setting.field;
      auto it = value_maps.find(field->field_index());
      if (it != value_maps.end()) {
        // 将字段值添加到直方图统计中
        it->second->add_value(field);
      }
    }
    res = table->file->ha_sample_next(scan_ctx, table->record[0]);
  }
  
  table->file->ha_sample_end(scan_ctx);
  return false;
}
```

#### 统计信息采集流程图

```mermaid
graph TD
    A[ANALYZE TABLE 命令] --> B{是否包含直方图选项?}
    
    B -->|是| C[列直方图采集]
    B -->|否| D[基础统计信息采集]
    
    C --> E[确定采样率]
    E --> F[初始化存储引擎抽样]
    F --> G[读取抽样数据]
    G --> H[构建直方图]
    H --> I[存储到数据字典]
    
    D --> J[调用存储引擎统计API]
    J --> K[更新表行数统计]
    K --> L[更新索引选择性]
    L --> M[更新数据长度信息]
    M --> N[存储到information_schema]
    
    I --> O[更新TABLE_SHARE直方图缓存]
    N --> P[优化器可用统计信息]
    O --> P
```

### 3. 索引统计信息

```cpp
/** 索引统计信息计算 */
class Key_info_statistics {
  public:
  // 计算索引的选择性
  double calculate_selectivity(uint key_part) {
    if (has_records_per_key(key_part)) {
      double records = static_cast<double>(table->file->stats.records);
      double unique_values = records / records_per_key(key_part);
      return 1.0 / unique_values;  // 选择性 = 1 / 基数
    }
    return DEFAULT_SELECTIVITY;
  }
  
  // 获取索引覆盖率
  bool is_covering_index(const table_map &used_columns) {
    // 检查索引是否覆盖所有需要的列
    return (key_info.user_defined_key_parts >= popcount(used_columns));
  }
};
```

## 成本模型系统

### 1. 成本模型架构

**位置：** `sql/opt_costmodel.h` 和 `sql/opt_costconstants.cc`

```mermaid
graph TB
    subgraph "成本模型层次"
        A["THD成本模型<br/>Thread Level"]
        B["服务器成本模型<br/>Server Cost Model"]
        C["存储引擎成本模型<br/>Storage Engine Cost Model"]
    end
    
    subgraph "成本常数来源"
        D["默认成本常数<br/>Default Constants"]
        E["mysql.server_cost表"]
        F["mysql.engine_cost表"]
        G["存储引擎提供的常数"]
    end
    
    subgraph "成本计算组件"
        H["IO成本<br/>IO Cost"]
        I["CPU成本<br/>CPU Cost"]
        J["内存成本<br/>Memory Cost"]
        K["网络成本<br/>Network Cost"]
    end
    
    A --> B
    B --> C
    
    D --> B
    E --> B
    F --> C
    G --> C
    
    B --> H
    B --> I
    C --> J
    C --> K
```

```cpp
/** 成本模型的核心实现 */
class Cost_model_server {
public:
  // 行评估成本 - CPU密集型操作
  double row_evaluate_cost(double rows) const {
    return rows * m_server_cost_constants->row_evaluate_cost();
  }
  
  // 索引块读取成本 - IO密集型操作  
  double page_read_cost(double pages) const {
    return pages * m_server_cost_constants->disk_temptable_row_cost();
  }
  
  // 临时表成本计算
  enum_tmptable_type { MEMORY_TMPTABLE, DISK_TMPTABLE };
  double tmptable_readwrite_cost(enum_tmptable_type type, 
                                double rows_read, double rows_written) {
    if (type == MEMORY_TMPTABLE) {
      return (rows_read + rows_written) * 
             m_server_cost_constants->memory_temptable_row_cost();
    } else {
      return (rows_read + rows_written) * 
             m_server_cost_constants->disk_temptable_row_cost();
    }
  }
};
```

### 2. 访问路径成本计算

**位置：** `sql/join_optimizer/cost_model.cc`

```cpp
/** 不同访问路径的成本计算 */

// 表扫描成本
double EstimateTableScanCost(const TABLE *table) {
  return table->file->table_scan_cost().total_cost();
}

// 索引扫描成本  
double EstimateIndexScanCost(const TABLE *table, int key_idx, double rows) {
  if (table->covering_keys.is_set(key_idx)) {
    // 覆盖索引扫描
    return table->file->index_scan_cost(key_idx, 1.0, rows).total_cost();
  } else {
    // 非覆盖索引需要回表
    return table->file->read_cost(key_idx, 1.0, rows).total_cost();
  }
}

// REF访问成本
double EstimateCostForRefAccess(THD *thd, TABLE *table, 
                               unsigned key_idx, double num_output_rows) {
  const double num_seeks = std::max(num_output_rows / 
    table->key_info[key_idx].records_per_key(0), 1.0);
  
  // 计算索引查找 + 数据页读取成本
  return table->file->index_read_cost(key_idx, num_seeks, num_output_rows);
}

// 连接成本计算
double EstimateHashJoinCost(double build_input_rows, double probe_input_rows,
                           double output_rows) {
  // 哈希表构建成本
  const double build_cost = build_input_rows * kHashBuildOneRowCost;
  
  // 探测成本
  const double probe_cost = probe_input_rows * kHashProbeOneRowCost;
  
  // 输出成本
  const double output_cost = output_rows * kHashReturnOneRowCost;
  
  return build_cost + probe_cost + output_cost;
}
```

## 算子下推（Predicate Pushdown）系统

### 1. 条件下推架构

**位置：** `sql/join_optimizer/make_join_hypergraph.cc`

```mermaid
graph TD
    subgraph "下推决策层"
        A["条件分析器<br/>Condition Analyzer"]
        B["下推可行性检查<br/>Pushdown Feasibility"]
        C["成本效益评估<br/>Cost-Benefit Analysis"]
    end
    
    subgraph "下推目标"
        D["表级过滤<br/>Table Filters"]
        E["索引条件下推<br/>Index Condition Pushdown"]
        F["存储引擎下推<br/>Engine Pushdown"]
        G["派生表下推<br/>Derived Table Pushdown"]
    end
    
    subgraph "下推类型"
        H["WHERE条件下推"]
        I["JOIN条件下推"]
        J["HAVING条件下推"]
        K["聚合下推"]
    end
    
    A --> B
    B --> C
    
    C --> D
    C --> E
    C --> F
    C --> G
    
    H --> A
    I --> A
    J --> A
    K --> A
```

```cpp
/** 条件下推的核心实现 */
void PushDownCondition(THD *thd, Item *cond, RelationalExpression *expr,
                      bool is_join_condition_for_expr,
                      const CompanionSetCollection &companion_collection,
                      Mem_root_array<Item *> *table_filters,
                      Mem_root_array<Item *> *cycle_inducing_edges,
                      Mem_root_array<Item *> *remaining_parts) {
  
  // 如果是表级条件，直接添加到表过滤器
  if (expr->type == RelationalExpression::TABLE) {
    assert(!IsMultipleEquals(cond));
    table_filters->push_back(cond);
    return;
  }
  
  // 检查条件引用的表是否都在子树中
  const table_map used_tables = 
    cond->used_tables() & (expr->tables_in_subtree | RAND_TABLE_BIT);
  
  // 检查是否可以下推到左子树
  bool can_push_into_left = false;
  if (expr->left != nullptr) {
    table_map left_tables = used_tables & expr->left->tables_in_subtree;
    can_push_into_left = (left_tables != 0) && 
      CanPushDownToLeftSide(expr->type, is_join_condition_for_expr);
  }
  
  // 检查是否可以下推到右子树  
  bool can_push_into_right = false;
  if (expr->right != nullptr) {
    table_map right_tables = used_tables & expr->right->tables_in_subtree;
    can_push_into_right = (right_tables != 0) && 
      CanPushDownToRightSide(expr->type, is_join_condition_for_expr);
  }
  
  // 递归下推到子树
  if (can_push_into_left) {
    PushDownCondition(thd, cond, expr->left, false, companion_collection,
                     table_filters, cycle_inducing_edges, remaining_parts);
  }
  if (can_push_into_right) {
    PushDownCondition(thd, cond, expr->right, false, companion_collection,
                     table_filters, cycle_inducing_edges, remaining_parts);
  }
}
```

### 2. 索引条件下推（ICP）

**位置：** `sql/sql_select.cc`

```cpp
/** 索引条件下推的实现 */
void QEP_TAB::push_index_condition() {
  /* 索引条件下推的必要条件：
     1. 表有选择条件
     2. 存储引擎支持ICP
     3. index_condition_pushdown开关打开
     4. 不是多表更新/删除语句
     5. 没有保护条件
     6. 不是CONST或SYSTEM连接类型
     7. 不是聚簇主键索引
  */
  
  if (condition() &&
      tbl->file->index_flags(keyno, 0, true) & HA_DO_INDEX_COND_PUSHDOWN &&
      hint_key_state(join_->thd, table_ref, keyno, ICP_HINT_ENUM,
                     OPTIMIZER_SWITCH_INDEX_CONDITION_PUSHDOWN) &&
      join_->thd->lex->sql_command != SQLCOM_UPDATE_MULTI &&
      join_->thd->lex->sql_command != SQLCOM_DELETE_MULTI &&
      !has_guarded_conds() && type() != JT_CONST && type() != JT_SYSTEM &&
      !(keyno == tbl->s->primary_key &&
        tbl->file->primary_key_is_clustered())) {
    
    // 生成适合当前索引的条件
    Item *idx_cond = make_cond_for_index(condition(), tbl, keyno, true);
    
    if (idx_cond) {
      // 检查条件是否真的引用了索引字段
      idx_cond->update_used_tables();
      if ((idx_cond->used_tables() & table_ref->map()) != 0) {
        // 将条件下推到存储引擎
        Item *idx_remainder_cond = tbl->file->idx_cond_push(keyno, idx_cond);
        
        if (idx_remainder_cond != idx_cond) {
          // 下推成功，更新剩余条件
          and_conditions(&pushed_idx_cond, idx_remainder_cond);
        }
      }
    }
  }
}
```

### 3. 派生表条件下推

**位置：** `sql/sql_derived.cc`

```cpp
/** 派生表条件下推实现 */
class Condition_pushdown {
public:
  bool make_cond_for_derived() {
    // 检查条件是否可以下推到派生表
    m_cond_to_push = extract_cond_for_table(m_cond_to_check);
    
    if (m_cond_to_push == nullptr) {
      m_remainder_cond = m_cond_to_check;
      return false;
    }
    
    // 为每个查询块处理条件下推
    for (Query_block *qb = derived_query_expression()->first_query_block();
         qb != nullptr; qb = qb->next_query_block()) {
      
      m_query_block = qb;
      
      // 分析条件，区分HAVING和WHERE
      if (push_past_window_functions()) return true;
      if (m_having_cond == nullptr) continue;
      if (push_past_group_by()) return true;
      
      // 替换列引用为派生表表达式
      if (m_having_cond != nullptr) {
        if (replace_columns_in_cond(&m_having_cond, true)) return true;
      }
      if (m_where_cond != nullptr) {
        if (replace_columns_in_cond(&m_where_cond, false)) return true;
      }
      
      // 将条件附加到派生表的查询块
      if (m_having_cond &&
          attach_cond_to_derived(qb->having_cond(), m_having_cond, true))
        return true;
      if (m_where_cond &&
          attach_cond_to_derived(qb->where_cond(), m_where_cond, false))
        return true;
    }
    
    return false;
  }
};
```

## CBO决策过程

### 1. 连接顺序优化

**位置：** `sql/sql_planner.cc`

```cpp
/** 连接顺序优化的核心算法 */
bool Optimize_table_order::choose_table_order() {
  // 使用动态规划或启发式算法选择最优连接顺序
  
  if (search_depth == 1) {
    // 贪心算法 - 每次选择成本最低的表
    return choose_table_order_greedy();
  } else {
    // 动态规划算法 - 穷举搜索最优解
    return choose_table_order_exhaustive();
  }
}

bool Optimize_table_order::choose_table_order_exhaustive() {
  const table_map remaining_tables = 
    join->all_table_map & ~join->const_table_map;
  
  // 递归搜索所有可能的连接顺序
  bool res = find_best(remaining_tables, join->const_tables, 0.0, 0.0);
  
  if (!res && join->best_read < DBL_MAX) {
    // 找到最优解，复制到positions数组
    memcpy(join->best_positions, join->positions,
           sizeof(POSITION) * join->primary_tables);
  }
  
  return res;
}
```

### 2. 访问路径选择

**位置：** `sql/join_optimizer/join_optimizer.cc`

```cpp
/** 单表访问路径选择 */
bool CostingReceiver::FoundSingleNode(int node_idx) {
  TABLE *table = m_graph->nodes[node_idx].table();
  
  // 1. 首先考虑唯一索引常量查找
  bool found_eq_ref = false;
  if (ProposeAllUniqueIndexLookupsWithConstantKey(node_idx, &found_eq_ref)) {
    return true;
  }
  
  // 如果找到EQ_REF，跳过其他访问方法（点查询优化）
  if (found_eq_ref) {
    return false;
  }
  
  // 2. 运行范围优化器
  double range_optimizer_row_estimate = -1.0;
  if (ProposeTableScan(table, node_idx, &range_optimizer_row_estimate)) {
    return true;
  }
  
  // 3. 考虑索引扫描（获取有趣的排序）
  for (const ActiveIndexInfo &order_info : *m_active_indexes) {
    if (order_info.table != table) continue;
    
    for (bool reverse : {false, true}) {
      const int key_idx = order_info.key_idx;
      // 只有在能避免排序或是覆盖索引时才考虑索引扫描
      if (order != 0 || (table->covering_keys.is_set(key_idx) &&
                         !IsClusteredPrimaryKey(key_idx, *table))) {
        if (ProposeIndexScan(table, node_idx, range_optimizer_row_estimate,
                            key_idx, reverse, order)) {
          return true;
        }
      }
      
      // 4. 考虑REF访问（使用可推导谓词）
      if (ProposeRefAccess(table, node_idx, key_idx,
                          range_optimizer_row_estimate, reverse,
                          0, order)) {
        return true;
      }
    }
  }
  
  return false;
}
```

### 3. 成本比较与计划选择

```cpp
/** 访问路径成本比较 */
void CostingReceiver::ProposeAccessPath(AccessPath *path,
                                       Mem_root_array<AccessPath *> *paths,
                                       const char *description) {
  
  // 应用过滤条件的效果
  if (path->num_output_rows_before_filter > 0) {
    const double filter_effect = CalculateFilterEffect(
        m_thd, path, m_predicates, m_graph->num_where_predicates);
    path->set_num_output_rows(path->num_output_rows_before_filter * filter_effect);
    
    // 更新过滤后的成本
    const double filter_cost = 
      path->num_output_rows_before_filter * kApplyOneFilterCost;
    path->set_cost(path->cost_before_filter() + filter_cost);
  }
  
  // 与现有路径比较，保留帕累托最优解
  bool dominated = false;
  for (auto it = paths->begin(); it != paths->end();) {
    AccessPath *existing_path = *it;
    
    if (PathDominates(path, existing_path)) {
      // 新路径更优，移除被支配的路径
      it = paths->erase(it);
    } else if (PathDominates(existing_path, path)) {
      // 现有路径更优，不添加新路径
      dominated = true;
      break;
    } else {
      ++it;
    }
  }
  
  if (!dominated) {
    paths->push_back(path);
  }
}

bool PathDominates(AccessPath *a, AccessPath *b) {
  // 路径a支配路径b需要满足：
  // 1. 成本更低或相等
  // 2. 排序属性至少一样好
  // 3. 输出行数相等（对于相同的逻辑操作）
  
  return (a->cost() <= b->cost()) &&
         (a->ordering_state >= b->ordering_state) &&
         (abs(a->num_output_rows() - b->num_output_rows()) < 0.01);
}
```

## 性能监控与调优

### 1. CBO性能指标监控

```sql
-- 查看优化器统计信息
SELECT 
    VARIABLE_NAME,
    VARIABLE_VALUE,
    VARIABLE_COMMENT
FROM performance_schema.global_status 
WHERE VARIABLE_NAME LIKE '%optimizer%'
   OR VARIABLE_NAME LIKE '%cost%'
   OR VARIABLE_NAME LIKE '%histogram%'
ORDER BY VARIABLE_NAME;

-- 监控不同访问路径的使用情况
SELECT 
    EVENT_NAME,
    COUNT_STAR as total_queries,
    SUM_TIMER_WAIT/1000000000000 as total_time_sec,
    AVG_TIMER_WAIT/1000000000000 as avg_time_sec
FROM performance_schema.events_stages_summary_global_by_event_name
WHERE EVENT_NAME LIKE '%optimizer%'
   OR EVENT_NAME LIKE '%join%'
   OR EVENT_NAME LIKE '%sort%'
ORDER BY total_time_sec DESC;
```

### 2. CBO调优脚本

```bash
#!/bin/bash
# mysql_cbo_tuning.sh - MySQL CBO优化脚本

echo "=== MySQL CBO 性能调优分析 ==="

MYSQL_CMD="mysql -u root -p"

echo "1. 检查优化器模式..."
$MYSQL_CMD -e "
SELECT 
    @@optimizer_switch as current_switches,
    @@use_hypergraph_optimizer as hypergraph_enabled,
    @@optimizer_search_depth as search_depth,
    @@optimizer_prune_level as prune_level;
"

echo "2. 检查成本模型配置..."
$MYSQL_CMD -e "
SELECT 
    cost_name,
    cost_value,
    last_update,
    comment
FROM mysql.server_cost
ORDER BY cost_name;
"

echo "3. 检查直方图统计..."
$MYSQL_CMD -e "
SELECT 
    schema_name,
    table_name,
    column_name,
    JSON_EXTRACT(histogram, '$.\"number-of-buckets-specified\"') as buckets
FROM information_schema.column_statistics
ORDER BY schema_name, table_name, column_name
LIMIT 10;
"

echo "4. 分析查询计划缓存..."
$MYSQL_CMD -e "
SELECT 
    COUNT(*) as cached_plans,
    AVG(LENGTH(query_sample_text)) as avg_query_length,
    SUM(sum_timer_wait)/1000000000000 as total_time_sec
FROM performance_schema.events_statements_summary_by_digest
WHERE digest_text IS NOT NULL;
"

echo "CBO调优分析完成！"
```

### 3. 执行计划分析工具

```sql
-- 分析查询的执行计划和成本
EXPLAIN FORMAT=TREE 
SELECT * FROM orders o 
JOIN customers c ON o.customer_id = c.id 
WHERE o.order_date > '2023-01-01'
  AND c.country = 'USA';

-- 查看成本详细信息
EXPLAIN FORMAT=JSON
SELECT * FROM products p
WHERE p.category_id IN (
    SELECT c.id FROM categories c 
    WHERE c.name LIKE 'Electronics%'
)
ORDER BY p.price DESC
LIMIT 10;

-- 分析直方图对查询计划的影响
SELECT 
    table_schema,
    table_name,
    column_name,
    JSON_EXTRACT(histogram, '$.\"histogram-type\"') as histogram_type,
    JSON_EXTRACT(histogram, '$.\"data-type\"') as data_type
FROM information_schema.column_statistics
WHERE table_schema = 'your_database'
  AND table_name = 'your_table';
```

## 高级优化特性

### 1. 窗口函数优化

```cpp
/** 窗口函数的CBO处理 */
bool Window::setup_windows(THD *thd, Query_block *select) {
  // 分析窗口函数的分区和排序需求
  for (Window &w : select->windows) {
    // 评估不同的窗口实现策略
    if (w.optimizable_row_aggregates() || w.optimizable_range_aggregates()) {
      // 可以优化的聚合函数，考虑索引优化
      w.m_opt_first_row = true;
      w.m_opt_last_row = true;
    }
    
    // 考虑窗口帧的成本
    if (w.frame()->m_from->m_border_type != WBT_UNBOUNDED_PRECEDING ||
        w.frame()->m_to->m_border_type != WBT_CURRENT_ROW) {
      // 复杂窗口帧需要更多计算成本
      w.set_need_card_check(true);
    }
  }
  return false;
}
```

### 2. 子查询优化

```cpp
/** 子查询转换策略 */
class Subquery_strategy {
public:
  enum Strategy {
    SUBQ_EXISTS,           // EXISTS转换
    SUBQ_IN_TO_EXISTS,     // IN转EXISTS
    SUBQ_MATERIALIZATION,  // 物化
    SUBQ_SEMIJOIN         // 半连接
  };
  
  Strategy choose_strategy(Item_subselect *subquery, 
                          const Cost_estimate &outer_cost) {
    // 基于成本选择最优子查询策略
    
    Cost_estimate exists_cost = estimate_exists_cost(subquery);
    Cost_estimate materialize_cost = estimate_materialize_cost(subquery);
    Cost_estimate semijoin_cost = estimate_semijoin_cost(subquery);
    
    if (exists_cost.total_cost() <= materialize_cost.total_cost() &&
        exists_cost.total_cost() <= semijoin_cost.total_cost()) {
      return SUBQ_IN_TO_EXISTS;
    } else if (materialize_cost.total_cost() <= semijoin_cost.total_cost()) {
      return SUBQ_MATERIALIZATION;
    } else {
      return SUBQ_SEMIJOIN;
    }
  }
};
```

### 3. 分区表优化

```cpp
/** 分区表的CBO优化 */
class Partition_pruning {
public:
  bool prune_partitions(THD *thd, TABLE *table, Item *condition) {
    partition_info *part_info = table->part_info;
    
    // 分析分区键条件
    if (part_info->get_part_func_type() == partition_info::RANGE_PART) {
      // 范围分区裁剪
      return prune_range_partitions(condition, part_info);
    } else if (part_info->get_part_func_type() == partition_info::HASH_PART) {
      // 哈希分区裁剪
      return prune_hash_partitions(condition, part_info);
    }
    
    return false;
  }
  
private:
  bool prune_range_partitions(Item *condition, partition_info *part_info) {
    // 基于范围条件裁剪分区
    for (uint i = 0; i < part_info->num_parts; i++) {
      if (!partition_matches_condition(i, condition)) {
        bitmap_clear_bit(&part_info->read_partitions, i);
      }
    }
    return true;
  }
};
```

## 总结与最佳实践

### 🎯 **CBO核心优势**

1. **📊 数据驱动决策**：基于真实统计信息而非启发式规则
2. **🔄 自适应优化**：随着数据分布变化自动调整策略
3. **⚡ 多维度成本**：综合考虑IO、CPU、内存、网络成本
4. **🚀 现代化架构**：支持复杂查询模式和新型存储引擎

### 🛠️ **调优建议**

#### 对于DBA：
1. **统计信息维护**：定期运行`ANALYZE TABLE`更新统计
2. **直方图管理**：为高选择性列创建直方图统计
3. **成本参数调整**：根据硬件特性调整成本常数
4. **监控查询计划**：使用`EXPLAIN`分析执行计划变化

#### 对于开发者：
1. **查询设计**：编写CBO友好的查询语句
2. **索引策略**：基于CBO反馈优化索引设计
3. **分区策略**：合理使用分区裁剪功能
4. **子查询优化**：利用CBO的子查询转换能力

#### 对于系统架构师：
1. **存储引擎选择**：选择CBO支持良好的存储引擎
2. **硬件配置**：基于成本模型调整硬件配比
3. **容量规划**：考虑CBO对系统资源的需求
4. **性能基线**：建立CBO性能监控体系

MySQL 8.4的CBO代表了现代数据库优化器的技术巅峰，其精妙的设计和强大的功能为复杂查询的高效执行提供了坚实保障。深入理解CBO的工作原理，对于数据库系统的性能调优和架构设计具有重要意义。
