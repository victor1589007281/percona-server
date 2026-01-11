# MySQL Error Log 问题分析

## "IO-layer timeout before wait_timeout was reached" 错误分析

### 📍 错误信息位置

该错误信息定义在 `sql-common/net_serv.cc` 文件中的 `net_read_raw_loop` 函数内：

```c
LogErr(ERROR_LEVEL, ER_CONDITIONAL_DEBUG,
       "IO-layer timeout before wait_timeout was reached.");
```

---

### 🔍 触发条件

该错误会在 **以下所有条件同时满足** 时打印到 errorlog：

| 条件 | 代码表达式 | 说明 |
|:-----|:----------|:-----|
| **1. 网络读取失败** | `count != 0` | 读取操作没有完成预期的字节数 |
| **2. 超时导致** | `!eof && (vio_was_timeout(net->vio) \|\| is_packet_timeout)` | 不是 EOF，而是超时导致的失败 |
| **3. 第一个数据包** | `net->pkt_nr == 0` | 服务端在等待客户端发送命令时超时 |
| **4. THD 存在** | `thd != nullptr` | 当前线程上下文存在 |
| **5. 关键条件** | `dur < wtout` | 实际等待时间 < `wait_timeout` |

---

### 📊 代码流程图

```
┌─────────────────────────────────────────────────────────────────────┐
│                     net_read_raw_loop() 函数                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────────────┐                                               │
│   │ vio_read() 读取 │                                               │
│   └────────┬────────┘                                               │
│            │                                                        │
│            ▼                                                        │
│   ┌─────────────────┐      ┌─────────────────┐                      │
│   │ 读取成功?       │──否──▶│ 可恢复错误?     │                      │
│   └────────┬────────┘      └────────┬────────┘                      │
│            │是                      │否                             │
│            ▼                        ▼                               │
│   ┌─────────────────┐      ┌─────────────────────────┐              │
│   │ 继续读取        │      │ 检查 count != 0 (失败)  │              │
│   └─────────────────┘      └────────────┬────────────┘              │
│                                         │                           │
│                                         ▼                           │
│                            ┌────────────────────────────┐           │
│                            │ 是超时导致? (非 EOF)       │           │
│                            │ vio_was_timeout() == true  │           │
│                            └────────────┬───────────────┘           │
│                                         │是                         │
│                                         ▼                           │
│                            ┌────────────────────────────┐           │
│                            │ 是第一个包? pkt_nr == 0    │           │
│                            └────────────┬───────────────┘           │
│                                         │是                         │
│                                         ▼                           │
│                            ┌────────────────────────────┐           │
│                            │ 记录 ER_LOG_CLIENT_...     │           │
│                            │ (INFORMATION_LEVEL)        │           │
│                            └────────────┬───────────────┘           │
│                                         │                           │
│                                         ▼                           │
│                            ┌────────────────────────────┐           │
│                            │ dur < wtout ?              │           │
│                            │ (实际时间 < wait_timeout)  │           │
│                            └────────────┬───────────────┘           │
│                                         │是                         │
│                                         ▼                           │
│                            ╔════════════════════════════╗           │
│                            ║  打印错误到 errorlog:      ║           │
│                            ║  "IO-layer timeout before  ║           │
│                            ║   wait_timeout was reached"║           │
│                            ╚════════════════════════════╝           │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

---

### 🎯 典型触发场景

#### 场景：使用 `kill_idle_transaction` 功能时

这是最常见的触发场景。当启用 Percona Server 的 `kill_idle_transaction` 功能时：

```sql
SET GLOBAL kill_idle_transaction = 2;    -- 2秒
SET GLOBAL wait_timeout = 28800;         -- 8小时 (默认值)
```

**触发机制：**

1. **实际使用的超时值** 由 `THD::get_wait_timeout()` 决定：

```cpp
// sql/sql_class.h
inline ulong get_wait_timeout(void) const noexcept {
  if (in_active_multi_stmt_transaction() &&
      kill_idle_transaction_timeout > 0 &&
      kill_idle_transaction_timeout < variables.net_wait_timeout)
    return kill_idle_transaction_timeout;  // 返回较小的值
  return variables.net_wait_timeout;
}
```

2. **日志中用于比较的值** 是 `thd_get_net_wait_timeout()`：

```cpp
// sql/sql_thd_api.cc
ulong thd_get_net_wait_timeout(THD *thd) {
  return thd->variables.net_wait_timeout;  // 总是返回 wait_timeout
}
```

3. **不匹配问题**：
   - IO层实际使用 `kill_idle_transaction_timeout` (例如 2秒)
   - 日志比较使用 `net_wait_timeout` (例如 28800秒)
   - 当 2秒后超时时，`dur (2) < wtout (28800)` 为真
   - 触发警告日志

---

### 📋 时序示例

```
时间线 (假设 kill_idle_transaction=2, wait_timeout=28800)
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

T=0s    客户端开启事务 (BEGIN)
        │
        ▼
T=0s    执行一些 SQL 语句
        │
        ▼
T=1s    客户端停止发送命令 (空闲)
        │
        ▼
T=3s    IO层超时 (kill_idle_transaction=2秒触发)
        │
        ├──▶ vio_was_timeout() == true
        │
        ├──▶ dur = 3秒, wtout = 28800秒
        │
        ├──▶ dur (3) < wtout (28800) ✓
        │
        ▼
        ╔═══════════════════════════════════════════════╗
        ║ 打印: "IO-layer timeout before wait_timeout   ║
        ║        was reached."                          ║
        ╚═══════════════════════════════════════════════╝
```

---

### 📌 相关配置变量

| 变量名 | 作用 | 默认值 |
|:-------|:-----|:-------|
| `wait_timeout` | 非交互式连接的空闲超时时间 | 28800 (8小时) |
| `interactive_timeout` | 交互式连接的空闲超时时间 | 28800 (8小时) |
| `kill_idle_transaction` | 空闲事务被杀死前的等待时间 (Percona 特有) | 0 (禁用) |

---

### ✅ 处理建议

1. **这是预期行为**：当 `kill_idle_transaction` 小于 `wait_timeout` 时，这个警告是正常的
2. **抑制警告**：在测试中可以使用 `call mtr.add_suppression("IO-layer timeout before wait_timeout was reached");`
3. **调整配置**：如果不需要此警告，可以将 `kill_idle_transaction` 设置为 0 或调整相关超时参数

---

### 📁 相关源文件

| 文件路径 | 作用 |
|:---------|:-----|
| `sql-common/net_serv.cc` | 错误打印位置 (`net_read_raw_loop` 函数) |
| `sql/sql_class.h` | `get_wait_timeout()` 函数定义 |
| `sql/sql_thd_api.cc` | `thd_get_net_wait_timeout()` 函数实现 |
| `sql/sql_parse.cc` | 设置网络读取超时 (`my_net_set_read_timeout`) |
| `vio/viosocket.cc` | `vio_was_timeout()` 函数实现 |

---

## "unknown variable 'validate_password.policy=STRONG'" 错误分析

### 📍 错误信息位置

该错误信息定义在 `mysys/errors.cc` 文件中：

```c
"unknown variable '%s'.",
```

对应错误代码为 `EE_UNKNOWN_VARIABLE` (67)，即 `MY-000067`。

错误触发代码位于 `mysys/my_getopt.cc` 的 `my_handle_options2` 函数中：

```cpp
// mysys/my_getopt.cc 第 459-464 行
if (must_be_var) {
  if (my_getopt_print_errors)
    my_getopt_error_reporter(
        option_is_loose ? WARNING_LEVEL : ERROR_LEVEL,
        EE_UNKNOWN_VARIABLE, cur_arg);
  if (!option_is_loose) return EXIT_UNKNOWN_VARIABLE;
}
```

---

### 🔍 触发条件

该错误会在以下条件满足时触发：

| 条件 | 说明 |
|:-----|:-----|
| **1. 找不到匹配的选项** | `findopt()` 函数返回 0，在已注册的选项列表中找不到该变量 |
| **2. 被识别为变量** | 选项后面有 `=` 赋值，如 `--validate_password.policy=STRONG` |
| **3. 没有 loose 前缀** | 选项前没有 `--loose-` 或 `--loose_` 前缀 |
| **4. 错误输出开启** | `my_getopt_print_errors` 为 true |

---

### 📊 错误触发流程图

```
┌─────────────────────────────────────────────────────────────────────┐
│                   MySQL 服务器启动流程                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────────────────────────┐                                   │
│   │ 1. 读取配置文件 (my.cnf)    │                                   │
│   │    或命令行参数             │                                   │
│   └─────────────┬───────────────┘                                   │
│                 │                                                   │
│                 ▼                                                   │
│   ┌─────────────────────────────┐                                   │
│   │ 2. handle_options() 解析    │                                   │
│   │    --validate_password.     │                                   │
│   │    policy=STRONG            │                                   │
│   └─────────────┬───────────────┘                                   │
│                 │                                                   │
│                 ▼                                                   │
│   ┌─────────────────────────────┐                                   │
│   │ 3. findopt() 在已注册的     │                                   │
│   │    选项列表中查找           │                                   │
│   └─────────────┬───────────────┘                                   │
│                 │                                                   │
│                 ▼                                                   │
│   ┌─────────────────────────────┐      ┌─────────────────────────┐  │
│   │ opt_found == 0 ?            │──是──▶│ 检查是否有 loose 前缀  │  │
│   │ (变量未注册)                │      └─────────────┬───────────┘  │
│   └─────────────────────────────┘                    │              │
│                                                      │              │
│                                         ┌────────────┴────────────┐ │
│                                         │                         │ │
│                                         ▼                         ▼ │
│                               ┌─────────────────┐      ┌───────────────┐
│                               │ 没有 loose 前缀 │      │ 有 loose 前缀 │
│                               └────────┬────────┘      └───────┬───────┘
│                                        │                       │       │
│                                        ▼                       ▼       │
│                               ╔════════════════════╗   ┌─────────────┐ │
│                               ║ ERROR_LEVEL        ║   │ WARNING     │ │
│                               ║ "unknown variable  ║   │ 继续启动    │ │
│                               ║ 'xxx'"             ║   └─────────────┘ │
│                               ║ 启动失败 ❌        ║                   │
│                               ╚════════════════════╝                   │
│                                                                        │
└────────────────────────────────────────────────────────────────────────┘
```

---

### 🎯 错误原因分析

#### 原因 1：组件/插件未安装或未加载

`validate_password.policy` 是 **组件版本** (component_validate_password) 的系统变量。

```
┌─────────────────────────────────────────────────────────────────┐
│                    变量注册时机                                 │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐     ┌─────────────┐     ┌─────────────────┐   │
│  │ 服务器启动  │────▶│ 解析配置文件 │────▶│ 加载组件/插件  │   │
│  └─────────────┘     └──────┬──────┘     └───────┬─────────┘   │
│                             │                    │              │
│                             ▼                    ▼              │
│                      ┌──────────────┐    ┌────────────────┐    │
│                      │ 此时变量还未 │    │ 组件加载后     │    │
│                      │ 注册！❌     │    │ 变量才注册 ✓   │    │
│                      └──────────────┘    └────────────────┘    │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

**变量注册代码** (components/validate_password/validate_password_imp.cc):

```cpp
// 组件初始化时注册变量
if (mysql_service_component_sys_variable_register->register_variable(
        "validate_password", "policy", PLUGIN_VAR_ENUM | PLUGIN_VAR_RQCMDARG,
        "password_validate_policy choosen policy to validate password "
        "possible values are LOW MEDIUM (default), STRONG",
        nullptr, nullptr, (void *)&enum_arg,
        (void *)&validate_password_policy)) {
  // 注册失败处理
}
```

#### 原因 2：插件和组件的变量名不同

| 版本 | 变量名 | 使用方式 |
|:-----|:-------|:---------|
| **插件版本** (validate_password plugin) | `validate_password_policy` | 下划线分隔 |
| **组件版本** (component_validate_password) | `validate_password.policy` | 点号分隔 |

**插件版本定义** (plugin/password_validation/validate_password.cc):

```cpp
static MYSQL_SYSVAR_ENUM(
    policy, validate_password_policy, PLUGIN_VAR_RQCMDARG,
    "password_validate_policy choosen policy to validate password"
    "possible values are LOW MEDIUM (default), STRONG",
    nullptr, nullptr, PASSWORD_POLICY_MEDIUM, &password_policy_typelib_t);
```

---

### ✅ 正确的使用方式

#### 方式 1：确保组件在配置文件处理前加载

在 my.cnf 中添加：

```ini
[mysqld]
# 1. 先确保组件被加载
early-plugin-load=component_validate_password

# 2. 然后设置变量
validate_password.policy=STRONG
```

#### 方式 2：使用 loose 前缀（推荐）

在 my.cnf 中使用 `loose_` 前缀避免启动失败：

```ini
[mysqld]
# 使用 loose 前缀，如果变量不存在只会产生警告而不是错误
loose_validate_password.policy=STRONG
# 或者
loose-validate_password.policy=STRONG
```

#### 方式 3：运行时安装并设置

```sql
-- 安装组件
INSTALL COMPONENT "file://component_validate_password";

-- 然后设置变量
SET GLOBAL validate_password.policy = 'STRONG';
```

#### 方式 4：使用插件版本（旧版兼容）

如果使用的是插件版本而非组件版本：

```ini
[mysqld]
# 插件版本使用下划线
plugin-load-add=validate_password.so
validate_password_policy=STRONG
```

---

### 📋 调试技巧

检查组件是否已安装：

```sql
-- 查看已安装的组件
SELECT * FROM mysql.component;

-- 查看变量是否存在
SHOW VARIABLES LIKE 'validate_password%';
```

---

### 📁 相关源文件

| 文件路径 | 作用 |
|:---------|:-----|
| `mysys/my_getopt.cc` | 选项解析和错误报告 (`my_handle_options2`, `findopt`) |
| `mysys/errors.cc` | 错误消息定义 (`EE_UNKNOWN_VARIABLE`) |
| `include/mysys_err.h` | 错误代码定义 |
| `components/validate_password/validate_password_imp.cc` | 组件版本变量注册 |
| `plugin/password_validation/validate_password.cc` | 插件版本变量定义 |
