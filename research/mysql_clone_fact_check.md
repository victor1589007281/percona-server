# MySQL Clone Plugin 报告事实核验报告

> **核验对象**: `mysql_clone_analysis.md` / `mysql_clone_deep_analysis.md` / `mysql_clone_risk_audit.md`
> **核验方法**: 对 Percona Server 源码逐行对照，核验报告中关键论断
> **日期**: 2025

---

## 核验结果总览

| # | 论断 | 来源报告 | 核验结论 | 可信度 |
|---|------|---------|---------|--------|
| 1 | `clone_max_concurrency` 默认值为 **8** | deep_analysis | ❌ **错误**，实际为 **16** | 低 |
| 2 | `clone_max_concurrency` 默认值为 **16** | analysis (v1) | ✅ **正确** | 高 |
| 3 | `clone_block_ddl` 默认值为 **false** | deep_analysis | ✅ **正确** | 高 |
| 4 | 系统变量共 **13** 个 | deep_analysis | ❌ **错误**，实际为 **17** 个 | 低 |
| 5 | `clone_buffer_size` 默认 **4MB** | deep_analysis | ✅ **正确** | 高 |
| 6 | `clone_enable_compression` 默认 **OFF** | deep_analysis | ✅ **正确** | 高 |
| 7 | `clone_compression_algorithm` 默认 **ZSTD** | deep_analysis | ✅ **正确** | 高 |
| 8 | `clone_zstd_compression_level` 默认 **3** | deep_analysis | ✅ **正确** | 高 |
| 9 | `clone_ddl_timeout` 默认 **300s (5min)** | deep_analysis | ✅ **正确** | 高 |
| 10 | `clone_autotune_concurrency` 默认 **ON** | deep_analysis | ✅ **正确** | 高 |
| 11 | `clone_donor_timeout_after_network_failure` 默认 **5min** | deep_analysis | ✅ **正确** | 高 |
| 12 | HYBRID 模式三阶段: FILE_COPY → PAGE_COPY → REDO_COPY | 全部 | ✅ **正确** | 高 |
| 13 | 典型内存占用 ~82MB (8 线程) | deep_analysis | ❌ **基于错误前提** (默认非 8 线程) | 低 |
| 14 | 典型内存占用 ~120MB (16 线程) | analysis (v1) | ✅ **合理估算** | 中 |
| 15 | InnoDB 层 "7 个源文件 + **4** 个头文件" | analysis (v1) | ❌ **错误**，实际为 7 源文件 + **6** 个头文件 | 中 |
| 16 | SSL 默认关闭，明文传输 | risk_audit | ⚠️ **部分错误**，默认为 `SSL_MODE_PREFERRED`（非强制关闭） | 中 |
| 17 | 密码以 `const char*` 明文存储 | risk_audit | ✅ **正确** (`Client_Share::m_passwd`) | 高 |
| 18 | `clone0clone.cc` L193-L197 DDL abort 机制 | deep_analysis | ✅ **正确** (行号与代码吻合) | 高 |

---

## 逐条详细核验

### 1. `clone_max_concurrency` 默认值 — ❌ 重大错误

**deep_analysis 声称**: 默认值为 **8** (并标注 "源码 `clone_plugin.cc` L625: `CLONE_DEF_CON` = 8")

**源码实际**:

- `plugin/clone/include/clone_os.h` L42:
  ```c
  const uint CLONE_DEF_CON = 16;
  ```
- `plugin/clone/src/clone_plugin.cc` L625:
  ```c
  CLONE_DEF_CON, /* Default =   8 threads */
  ```

**判定**: 代码中的 **注释写错了**（注释说 8，但 `CLONE_DEF_CON` 的值是 16）。`deep_analysis` 报告采信了注释而非实际常量值，导致结论错误。**`analysis` (v1) 报告的 "默认 16" 反而是正确的。**

---

### 2. `clone_block_ddl` 默认值 — ✅ 正确

**源码**: `clone_plugin.cc` L601-603:
```c
static MYSQL_SYSVAR_BOOL(block_ddl, clone_block_ddl, PLUGIN_VAR_NOCMDARG,
                         "If clone should block concurrent DDL", nullptr,
                         nullptr, false); /* Allow concurrent ddl by default */
```
确认默认值为 `false`。

---

### 3. 系统变量数量 — ❌ 错误

**deep_analysis 声称**: 13 个系统变量

**源码实际** (`clone_plugin.cc` L740-758 `clone_system_variables[]` 数组):

| # | 变量名 | 类型 | 默认值 |
|---|--------|------|--------|
| 1 | `clone_buffer_size` | UINT | 4MB |
| 2 | `clone_block_ddl` | BOOL | false |
| 3 | `clone_ddl_timeout` | UINT | 300s |
| 4 | `clone_max_concurrency` | UINT | 16 |
| 5 | `clone_max_network_bandwidth` | UINT | 0 (无限) |
| 6 | `clone_max_data_bandwidth` | UINT | 0 (无限) |
| 7 | `clone_enable_compression` | BOOL | false |
| 8 | `clone_autotune_concurrency` | BOOL | true |
| 9 | `clone_valid_donor_list` | STR | nullptr |
| 10 | `clone_ssl_key` | STR | nullptr |
| 11 | `clone_ssl_cert` | STR | nullptr |
| 12 | `clone_ssl_ca` | STR | nullptr |
| 13 | `clone_donor_timeout_after_network_failure` | UINT | 5min |
| 14 | `clone_delay_after_data_drop` | UINT | 0 |
| 15 | `clone_exclude_plugins_list` | STR | nullptr |
| 16 | `clone_compression_algorithm` | ENUM | ZSTD |
| 17 | `clone_zstd_compression_level` | UINT | 3 |

**实际为 17 个变量**，非 13 个。漏掉了: `max_network_bandwidth`, `max_data_bandwidth`, `delay_after_data_drop`, `exclude_plugins_list`。

---

### 4. 内存占用估算 — ❌ 基于错误前提

**deep_analysis 声称**: ~82MB (8 线程)

**问题**: 该计算基于错误的默认并发数 (8)。实际默认并发数为 16。

**重新计算** (以默认配置):
- 每线程缓冲区: `clone_buffer_size` = 4MB
- 16 线程 × 4MB = **64MB** (纯缓冲区)
- 加上网络 socket 缓冲、Clone_Sys 全局结构体、描述符序列化缓冲等额外开销
- **~120MB** 是更合理的估算 (v1 报告的数值)

---

### 5. HYBRID 模式三阶段 — ✅ 正确

**源码**: `storage/innobase/clone/clone0snapshot.cc` L176-206 `get_next_state()`:
```c
if (m_snapshot_state == CLONE_SNAPSHOT_INIT) {
    next_state = CLONE_SNAPSHOT_FILE_COPY;
} else if (m_snapshot_state == CLONE_SNAPSHOT_FILE_COPY) {
    if (m_snapshot_type == HA_CLONE_HYBRID || m_snapshot_type == HA_CLONE_PAGE) {
        next_state = CLONE_SNAPSHOT_PAGE_COPY;
    } else if (m_snapshot_type == HA_CLONE_REDO) {
        next_state = CLONE_SNAPSHOT_REDO_COPY;
    } else {
        next_state = CLONE_SNAPSHOT_DONE;  // BLOCKING
    }
} else if (m_snapshot_state == CLONE_SNAPSHOT_PAGE_COPY) {
    next_state = CLONE_SNAPSHOT_REDO_COPY;
} else {
    next_state = CLONE_SNAPSHOT_DONE;  // REDO_COPY → DONE
}
```

HYBRID 模式: INIT → FILE_COPY → PAGE_COPY → REDO_COPY → DONE。与报告描述完全一致。

---

### 6. 压缩配置 — ✅ 全部正确

| 变量 | 报告声称 | 源码验证 |
|------|---------|---------|
| `clone_enable_compression` | OFF (false) | ✅ L652: `false` |
| `clone_compression_algorithm` | ZSTD | ✅ L666: `enum_clone_compression_algorithm::ZSTD` |
| `clone_zstd_compression_level` | 3 | ✅ L671: `3` |

---

### 7. SSL 默认行为 — ⚠️ 部分错误

**risk_audit 声称**: "SSL 默认关闭 (SSL_MODE_DISABLED)"，评为 CRITICAL (20/25)

**源码实际** (`sql/sql_admin.cc` L2231-2238):
```c
if (thd->lex->ssl_type == SSL_TYPE_NONE) {
    ssl_mode = SSL_MODE_DISABLED;
} else if (thd->lex->ssl_type == SSL_TYPE_SPECIFIED) {
    ssl_mode = SSL_MODE_REQUIRED;
} else {
    assert(thd->lex->ssl_type == SSL_TYPE_NOT_SPECIFIED);
    ssl_mode = SSL_MODE_PREFERRED;
}
```

`ssl_type` 在 `sql_admin.cc` L2409 初始化为 `SSL_TYPE_NOT_SPECIFIED`，因此默认 SSL 模式为 **`SSL_MODE_PREFERRED`**（尝试使用 SSL，若服务器不支持则回退到明文）。

**修正评估**:
- "SSL 默认完全关闭" 的说法 **不准确**
- 实际情况: 默认为 `PREFERRED` 模式，若 Donor 和 Recipient 都配置了证书则自动加密
- 安全风险仍存在（可降级为明文），但严重程度应降级为 **MEDIUM** (8-11/25)

---

### 8. 密码明文存储 — ✅ 正确

**源码**: `plugin/clone/include/clone_client.h` L322-323:
```c
/** Remote user password */
const char *m_passwd;
```

`Client_Share` 结构体以原始 C 字符串指针存储密码，无加密、无清零机制。core dump 中可提取。该安全风险论断正确。

---

### 9. 源文件数量 — ⚠️ 部分不准确

**analysis (v1) 声称**: InnoDB 层 "7 个源文件 + **4** 个头文件"

**实际统计**:
- Plugin src: 7 文件 ✅
- Plugin headers: 7 文件 ✅
- InnoDB src (`storage/innobase/clone/`): 7 文件 ✅
- InnoDB headers (`storage/innobase/include/`): **6** 文件 (非 4)
  - `clone0api.h`, `clone0clone.h`, `clone0desc.h`, `clone0monitor.h`, `clone0repl.h`, `clone0snapshot.h`
- SQL 层: `clone_handler.cc`, `clone_handler.h`, `clone_protocol_service.cc`

---

### 10. DDL abort 机制 — ✅ 正确

**deep_analysis 声称**: `clone0clone.cc` L193-L197 存在 DDL 导致的 abort 机制

**源码**: L193-197:
```c
} else if (Clone_Sys::s_clone_sys_state == CLONE_SYS_ABORT) {
    ib::info(ER_IB_CLONE_START_STOP)
        << "Clone Begin Master wait for abort interrupted by DDL";
    my_error(ER_CLONE_DDL_IN_PROGRESS, MYF(0));
    return (ER_CLONE_DDL_IN_PROGRESS);
```
行号与代码逻辑均吻合。

---

## 综合评分

| 报告 | 源码准确性 | 数据准确性 | 安全分析准确性 | 综合可信度 |
|------|:----------:|:----------:|:--------------:|:----------:|
| `mysql_clone_analysis.md` (v1) | 8/10 | 8/10 | N/A | **中高** |
| `mysql_clone_deep_analysis.md` (v2) | 6/10 | **5/10** | N/A | **中低** |
| `mysql_clone_risk_audit.md` | 7/10 | N/A | 6/10 | **中** |

---

## 关键修正清单

1. **`clone_max_concurrency` 默认值 = 16** (非 8) — deep_analysis 最严重的事实错误
2. **系统变量数量 = 17** (非 13) — 漏计 4 个变量
3. **内存占用 ~120MB (16 线程)** 比 ~82MB (8 线程) 更准确
4. **SSL 默认模式 = PREFERRED** (非 DISABLED) — 风险等级应从 CRITICAL 降级
5. **InnoDB 头文件 = 6** (非 4)
6. `clone0clone.cc` L625 注释 "Default = 8 threads" 是 **代码注释 bug**，与实际常量值不一致

---

## 建议

1. **修正 deep_analysis 中所有引用 "默认 8 线程" 的内容**，改为 16
2. **修正系统变量表格**，补充遗漏的 4 个变量
3. **修正内存估算**，基于 16 线程重新计算
4. **修正风险审计中的 SSL 评级**，从 CRITICAL 降至 MEDIUM，并说明 `SSL_MODE_PREFERRED` 的实际语义
5. **向上游报告注释 bug**: `clone_plugin.cc` L625 的注释与 `CLONE_DEF_CON` 实际值不一致，建议向 MySQL/Percona 提交 bug 报告
