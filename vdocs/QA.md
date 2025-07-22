# QA
1. mysql 的variables 参数值都可能是什么类型？这个类型对应golang是什么？给我映射列表。

## MySQL Variables 类型分析与 Golang 映射

通过分析MySQL源码（主要在 `include/mysql/components/services/bits/system_variables_bits.h`、`sql/sys_vars.h`、`include/mysql/plugin.h` 等文件），MySQL系统变量支持以下类型：

### 1. 基础变量类型定义

**源码位置**: `include/mysql/components/services/bits/system_variables_bits.h:14-49`

```cpp
#define PLUGIN_VAR_BOOL 0x0001        /** bool variable */
#define PLUGIN_VAR_INT 0x0002         /** int variable */
#define PLUGIN_VAR_LONG 0x0003        /** long variable */
#define PLUGIN_VAR_LONGLONG 0x0004    /** longlong variable */
#define PLUGIN_VAR_STR 0x0005         /** char * variable */
#define PLUGIN_VAR_ENUM 0x0006        /** Enum variable */
#define PLUGIN_VAR_SET 0x0007         /** A set variable */
#define PLUGIN_VAR_DOUBLE 0x0008      /** double variable */
```

### 2. 具体系统变量类型

**源码位置**: `sql/sys_vars.h:339-346`

```cpp
typedef Sys_var_integer<int32, GET_UINT, SHOW_INT, false> Sys_var_int32;
typedef Sys_var_integer<uint, GET_UINT, SHOW_INT, false> Sys_var_uint;
typedef Sys_var_integer<ulong, GET_ULONG, SHOW_LONG, false> Sys_var_ulong;
typedef Sys_var_integer<ha_rows, GET_HA_ROWS, SHOW_HA_ROWS, false> Sys_var_harows;
typedef Sys_var_integer<ulonglong, GET_ULL, SHOW_LONGLONG, false> Sys_var_ulonglong;
typedef Sys_var_integer<long, GET_LONG, SHOW_SIGNED_LONG, true> Sys_var_long;
```

### 3. MySQL 变量类型与 Golang 类型映射表

| MySQL变量类型 | MySQL C/C++ 类型 | 描述 | Golang 类型 | 全局变量样例 | 说明 |
|---------------|-----------------|------|-------------|-------------|------|
| **PLUGIN_VAR_BOOL** | `bool` | 布尔值 (OFF/ON) | `bool` | `autocommit`<br/>`foreign_key_checks` | 直接映射 `SET GLOBAL autocommit = ON` |
| **PLUGIN_VAR_INT** | `int` | 32位有符号整数 | `int32` | `thread_pool_stall_limit` | MySQL中实际为32位 |
| **PLUGIN_VAR_INT** (unsigned) | `uint` | 32位无符号整数 | `uint32` | `table_open_cache_instances`<br/>`eq_range_index_dive_limit` | 带 PLUGIN_VAR_UNSIGNED 标志 |
| **PLUGIN_VAR_LONG** | `long` | 平台相关的长整数 | `int64` | `max_digest_length` | 在64位系统上为64位 |
| **PLUGIN_VAR_LONG** (unsigned) | `ulong` | 无符号长整数 | `uint64` | `max_connections`<br/>`table_open_cache`<br/>`max_allowed_packet` | 带 PLUGIN_VAR_UNSIGNED 标志<br/>`SET GLOBAL max_connections = 1000` |
| **PLUGIN_VAR_LONGLONG** | `longlong` | 64位有符号整数 | `int64` | `long_query_time` (微秒存储) | MySQL的longlong就是long long |
| **PLUGIN_VAR_LONGLONG** (unsigned) | `ulonglong` | 64位无符号整数 | `uint64` | `innodb_buffer_pool_size`<br/>`max_heap_table_size`<br/>`global_connection_memory_limit` | 最常用的数值类型<br/>`SET GLOBAL innodb_buffer_pool_size = 8589934592` |
| **PLUGIN_VAR_STR** | `char*` | 字符串指针 | `string` | `datadir`<br/>`character_set_server`<br/>`lc_messages_dir` | 需要处理NULL指针情况<br/>`SET GLOBAL character_set_server = 'utf8mb4'` |
| **PLUGIN_VAR_ENUM** | `ulong` (存储) | 枚举值（存储为索引） | `string` 或 `int` | `binlog_format`<br/>`transaction_isolation`<br/>`concurrent_insert` | 可按索引或名称处理<br/>`SET GLOBAL binlog_format = 'ROW'` |
| **PLUGIN_VAR_SET** | `ulonglong` (存储) | 集合类型（位掩码） | `[]string` 或 `uint64` | `sql_mode`<br/>`optimizer_switch` | 位掩码或字符串数组<br/>`SET GLOBAL sql_mode = 'STRICT_TRANS_TABLES,NO_ZERO_DATE'` |
| **PLUGIN_VAR_DOUBLE** | `double` | 64位浮点数 | `float64` | `long_query_time`<br/>`innodb_segment_reserve_factor` | 直接映射<br/>`SET GLOBAL long_query_time = 10.5` |

### 4. 特殊类型说明

#### 4.1 ha_rows 类型

**源码位置**: `sql/sys_vars.h:160`
```cpp
#define GET_HA_ROWS GET_ULL  // ha_rows 实际上是 ulonglong
```

| 类型 | MySQL C/C++ 类型 | Golang 类型 | 全局变量样例 |
|------|-----------------|-------------|-------------|
| **ha_rows** | `ulonglong` | `uint64` | `select_limit`<br/>`max_join_size` |

#### 4.2 特殊结构体类型

**源码位置**: `sql/sys_vars.h:2334-2377`

| 类型 | 描述 | Golang 类型 | 全局变量样例 | 示例值 |
|------|------|-------------|-------------|--------|
| **字符集 (CHARSET_INFO)** | 字符集结构体 | `string` | `character_set_server`<br/>`character_set_database`<br/>`character_set_client` | `"utf8mb4"`<br/>`"latin1"` |
| **时区 (Time_zone)** | 时区结构体 | `string` | `time_zone`<br/>`system_time_zone` | `"+08:00"`<br/>`"Asia/Shanghai"`<br/>`"SYSTEM"` |
| **本地化 (MY_LOCALE)** | 区域设置结构体 | `string` | `lc_messages`<br/>`lc_time_names` | `"en_US"`<br/>`"zh_CN"`<br/>`"ja_JP"` |

### 5. 实际应用中的类型判断

**源码位置**: `sql/item_func.cc:7248-7473`

在MySQL源码中，系统变量的实际类型通过 `show_type()` 方法确定：

```cpp
switch (var->show_type()) {
    case SHOW_INT:          // uint
    case SHOW_LONG:         // ulong  
    case SHOW_LONGLONG:     // ulonglong
    case SHOW_SIGNED_INT:   // int
    case SHOW_SIGNED_LONG:  // long
    case SHOW_SIGNED_LONGLONG: // longlong
    case SHOW_HA_ROWS:      // ha_rows (ulonglong)
    case SHOW_BOOL:         // bool
    case SHOW_MY_BOOL:      // bool
    case SHOW_DOUBLE:       // double
    case SHOW_CHAR:         // char[]
    case SHOW_CHAR_PTR:     // char*
    case SHOW_LEX_STRING:   // LEX_STRING
}
```

### 6. Golang 处理建议

#### 6.1 推荐的 Golang 类型映射

```go
// MySQL系统变量值的统一接口
type MySQLVariableValue interface {
    String() string
    Int64() (int64, error)
    Uint64() (uint64, error) 
    Float64() (float64, error)
    Bool() (bool, error)
}

// 具体类型映射
type MySQLVariableType int

const (
    VarTypeBool MySQLVariableType = iota
    VarTypeInt32
    VarTypeUint32
    VarTypeInt64
    VarTypeUint64
    VarTypeFloat64
    VarTypeString
    VarTypeEnum
    VarTypeSet
)

// 类型映射函数
func MapMySQLType(mysqlType string) MySQLVariableType {
    switch mysqlType {
    case "SHOW_BOOL", "SHOW_MY_BOOL":
        return VarTypeBool
    case "SHOW_SIGNED_INT":
        return VarTypeInt32
    case "SHOW_INT":
        return VarTypeUint32
    case "SHOW_SIGNED_LONG", "SHOW_SIGNED_LONGLONG":
        return VarTypeInt64
    case "SHOW_LONG", "SHOW_LONGLONG", "SHOW_HA_ROWS":
        return VarTypeUint64
    case "SHOW_DOUBLE":
        return VarTypeFloat64
    case "SHOW_CHAR", "SHOW_CHAR_PTR", "SHOW_LEX_STRING":
        return VarTypeString
    default:
        return VarTypeString // 默认按字符串处理
    }
}
```

#### 6.2 处理注意事项

1. **NULL值处理**: MySQL变量可能为NULL，需要使用指针类型或 `sql.NullXxx` 类型
2. **枚举和集合**: 建议同时支持字符串表示和数值表示
3. **字符串编码**: 注意处理不同字符集的字符串
4. **数值范围**: 特别注意有符号和无符号整数的范围差异
5. **类型转换**: 提供灵活的类型转换机制，避免数据丢失

#### 6.3 变量类型测试 SQL

```sql
-- 测试各种变量类型的 SQL 示例

-- BOOL 类型测试
SELECT @@GLOBAL.autocommit;                    -- 返回: 1 或 0
SET GLOBAL autocommit = ON;

-- UINT 类型测试  
SELECT @@GLOBAL.table_open_cache_instances;     -- 返回: 数字 (如 16)
SET GLOBAL table_open_cache_instances = 32;

-- ULONG 类型测试
SELECT @@GLOBAL.max_connections;                -- 返回: 数字 (如 151)
SET GLOBAL max_connections = 1000;

-- ULONGLONG 类型测试
SELECT @@GLOBAL.innodb_buffer_pool_size;        -- 返回: 大数字 (如 134217728)
SET GLOBAL innodb_buffer_pool_size = 8589934592; -- 8GB

-- DOUBLE 类型测试
SELECT @@GLOBAL.long_query_time;                -- 返回: 浮点数 (如 10.000000)
SET GLOBAL long_query_time = 5.5;

-- STRING 类型测试
SELECT @@GLOBAL.character_set_server;           -- 返回: 字符串 (如 'utf8mb4')
SET GLOBAL character_set_server = 'latin1';

-- ENUM 类型测试
SELECT @@GLOBAL.binlog_format;                  -- 返回: 'ROW', 'STATEMENT', 或 'MIXED'
SET GLOBAL binlog_format = 'ROW';

-- SET 类型测试
SELECT @@GLOBAL.sql_mode;                       -- 返回: 逗号分隔的字符串
SET GLOBAL sql_mode = 'STRICT_TRANS_TABLES,NO_ZERO_DATE,NO_ZERO_IN_DATE';

-- 结构体类型测试
SELECT @@GLOBAL.time_zone;                      -- 返回: 'SYSTEM' 或 时区字符串
SET GLOBAL time_zone = '+08:00';

-- 查看变量的详细信息
SELECT 
    VARIABLE_NAME,
    VARIABLE_VALUE,
    'GLOBAL' as VARIABLE_SCOPE
FROM performance_schema.global_variables 
WHERE VARIABLE_NAME IN (
    'autocommit', 'max_connections', 'innodb_buffer_pool_size',
    'long_query_time', 'character_set_server', 'binlog_format', 'sql_mode'
)
ORDER BY VARIABLE_NAME;
```

2. innodb_adaptive_hash_index_parts 这个参数是动态修改，马上生效的不？

## innodb_adaptive_hash_index_parts 动态修改分析

通过分析源码，该参数**不能**动态修改，必须在MySQL服务器启动时配置。

### 源码分析结论

**参数定义位置**: `storage/innobase/handler/ha_innodb.cc:23347-23363`

```cpp
static MYSQL_SYSVAR_ULONG(
    adaptive_hash_index_parts, btr_ahi_parts,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,    // ← 注意 PLUGIN_VAR_READONLY 标志
    "Number of InnoDB Adaptive Hash Index Partitions. (default = 8). ", 
    nullptr, nullptr, 8, 1, 512, 0);
```

**关键标志含义**: `include/mysql/components/services/bits/system_variables_bits.h:46`
```cpp
#define PLUGIN_VAR_READONLY 0x0200  /**< Server variable is read only */
```

**运行时检查**: `sql/set_var.cc:1650-1660`
```cpp
if (var->is_readonly()) {
  if (type != OPT_PERSIST_ONLY) {
    my_error(ER_INCORRECT_GLOBAL_LOCAL_VAR, MYF(0), var->name.str,
             "read only");
    return -1;
  }
}
```

### 测试验证

**测试文件**: `mysql-test/suite/sys_vars/r/innodb_adaptive_hash_index_parts_basic.result`

```sql
-- 尝试动态修改会报错
SET @@GLOBAL.innodb_adaptive_hash_index_parts=1;
ERROR HY000: Variable 'innodb_adaptive_hash_index_parts' is a read only variable

-- 只能查看，不能修改
SELECT @@GLOBAL.innodb_adaptive_hash_index_parts;
-- 返回当前值（默认为8）
```

### 配置方法

由于该参数为只读变量，只能通过以下方式设置：

#### 1. 配置文件设置
```ini
# my.cnf 或 my.ini 文件中
[mysqld]
innodb_adaptive_hash_index_parts = 16
```

#### 2. 命令行启动参数
```bash
mysqld --innodb-adaptive-hash-index-parts=16
```

#### 3. 系统变量查看
```sql
-- 查看当前值
SELECT @@GLOBAL.innodb_adaptive_hash_index_parts;

-- 查看变量详情
SHOW GLOBAL VARIABLES LIKE 'innodb_adaptive_hash_index_parts';

-- 查看是否为只读
SELECT 
  VARIABLE_NAME,
  VARIABLE_VALUE,
  'READ_ONLY' as VARIABLE_SCOPE
FROM performance_schema.global_variables 
WHERE VARIABLE_NAME = 'innodb_adaptive_hash_index_parts';
```

### 参数作用说明

**功能**: 控制InnoDB自适应哈希索引(AHI)的分区数量
- **默认值**: 8个分区
- **取值范围**: 1-512
- **作用**: 将AHI分成多个分区，每个分区有独立的锁，减少并发访问时的锁争用

**性能影响**:
- **分区过少**: 高并发时锁竞争严重，AHI性能下降
- **分区过多**: 内存开销增加，管理复杂度提升  
- **推荐设置**: CPU核心数的1-2倍

### 总结

❌ **不能动态修改**: `innodb_adaptive_hash_index_parts` 是只读参数  
🔄 **需要重启**: 修改该参数需要重启MySQL服务器才能生效  
⚙️ **启动配置**: 只能通过配置文件或启动参数设置  
🎯 **用途明确**: 用于优化高并发场景下的AHI性能
