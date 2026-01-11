# Redo Log 归档程序深度解析

## 概述

Redo Log 归档是 MySQL 8.0 引入的功能，主要用于支持 Clone 和增量备份场景。归档程序将实时的 Redo Log 复制到归档文件中，使得备份工具可以获取连续的事务日志。

## 整体架构

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                         Redo Log 归档架构                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌──────────────┐          ┌──────────────────┐                            │
│   │  Log Writer  │  ──────► │  Redo Log Files  │                            │
│   │    线程      │   写入    │   (ib_redo*)     │                            │
│   └──────────────┘          └────────┬─────────┘                            │
│          │                           │                                       │
│          │ 唤醒                        │ 读取                                 │
│          ▼                           ▼                                       │
│   ┌──────────────┐          ┌──────────────────┐                            │
│   │ Log Archiver │  ◄────── │  Arch_Log_Sys    │                            │
│   │    线程      │          │   (归档系统)     │                            │
│   └──────────────┘          └────────┬─────────┘                            │
│          │                           │                                       │
│          │ 写入                        │ 管理                                 │
│          ▼                           ▼                                       │
│   ┌──────────────┐          ┌──────────────────┐                            │
│   │ Archive Files│          │  Arch_Group      │                            │
│   │ (归档文件)   │          │  (归档组)        │                            │
│   └──────────────┘          └──────────────────┘                            │
│                                                                             │
│   触发条件:                                                                  │
│   1. Clone 操作启动时                                                        │
│   2. innodb_redo_log_archive_dirs 配置后手动启动                             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 核心数据结构

```text
Arch_Log_Sys - storage/innobase/include/arch0arch.h
│  【全局归档系统对象: arch_log_sys】
│
├── m_state: Arch_State
│   │  【当前状态】
│   │
│   ├── ARCH_STATE_INIT      - 初始化
│   ├── ARCH_STATE_IDLE      - 空闲（无活跃客户端）
│   ├── ARCH_STATE_ACTIVE    - 活跃（正在归档）
│   ├── ARCH_STATE_PREPARE_IDLE - 准备转为空闲
│   └── ARCH_STATE_ABORT     - 中止
│
├── m_current_group: Arch_Group*
│   │  【当前归档组】
│   │
│   ├── m_begin_lsn         - 组的起始LSN
│   ├── m_end_lsn           - 组的结束LSN
│   ├── m_file_size         - 归档文件大小
│   └── m_file_ctx          - 文件上下文
│
├── m_archived_lsn: lsn_t
│   │  【已归档到的LSN位置】
│
├── m_chunk_size: uint
│   │  【每次复制的块大小，默认1MB】
│   └── ARCH_LOG_CHUNK_SIZE = 1024 * 1024
│
├── m_group_list: Arch_Grp_List
│   │  【归档组列表】
│
└── m_log_consumer: Arch_log_consumer
    │  【注册到Log系统的消费者】
    │  【用于通知归档进度，防止Redo被覆盖】
```

## 完整调用链

### 归档线程主循环

```text
log_archiver_thread() - storage/innobase/arch/arch0arch.cc:615
│  【归档后台线程入口】
│
├── 初始化文件上下文
│   └── Arch_File_Ctx log_file_ctx
│
└── while (true)  【主循环】
        │
        ├── 执行归档
        │   │
        │   └── arch_log_sys->archive(init, &log_file_ctx, &log_arch_lsn, &log_wait)
        │       │  【核心归档函数】
        │       │
        │       └── 详见下方展开
        │
        ├── 检查是否需要退出
        │   └── if (log_abort) break
        │
        └── 如果没有数据需要归档，等待唤醒
            │
            └── if (log_wait)
                ├── os_event_wait(log_archiver_thread_event)
                └── os_event_reset(log_archiver_thread_event)
                    【等待 Log Writer 唤醒】

Arch_Log_Sys::archive() - storage/innobase/arch/arch0log.cc:845
│  【归档核心逻辑】
│
├── 参数
│   ├── init: 是否首次调用
│   ├── curr_ctx: 系统Redo文件上下文
│   ├── arch_lsn: [out] 当前已归档LSN
│   └── wait: [out] 是否需要等待
│
├── 1. 首次初始化
│   │
│   └── if (init)
│       │
│       └── curr_ctx->init(log_directory_path, ...)
│           │  【初始化源文件上下文】
│           │  【指向系统Redo Log目录】
│
├── 2. 检查状态并获取归档量
│   │
│   └── check_set_state(is_abort, arch_lsn, &arch_len)
│       │  【检查归档系统状态】
│       │
│       ├── 获取可归档的数据量
│       │   ├── lsn_diff = log_sys->write_lsn - m_archived_lsn
│       │   │   【★关键：归档的是已写入OS的日志】
│       │   │   【不是 flushed_to_disk，而是 write_lsn】
│       │   │
│       │   └── arch_len = min(lsn_diff, m_chunk_size)
│       │       【每次最多归档 1MB】
│       │
│       └── 返回当前状态
│
├── 3. 如果状态为 ACTIVE 且有数据
│   │
│   └── if (curr_state == ARCH_STATE_ACTIVE && arch_len > 0)
│       │
│       └── copy_log(curr_ctx, *arch_lsn, arch_len)
│           │  【执行实际的数据复制】
│           │
│           └── 详见下方展开
│
├── 4. 归档成功后更新LSN
│   ├── *arch_lsn += arch_len
│   └── *wait = false
│
└── 5. 处理其他状态
    ├── ARCH_STATE_ABORT → 关闭文件，返回 true
    ├── ARCH_STATE_IDLE → 关闭文件，等待
    └── ARCH_STATE_PREPARE_IDLE → 继续处理
```

### 数据复制流程

```text
Arch_Log_Sys::copy_log() - storage/innobase/arch/arch0log.cc:627
│  【从系统Redo复制数据到归档文件】
│
├── 参数
│   ├── file_ctx: 源文件上下文（系统Redo）
│   ├── start_lsn: 开始复制的LSN
│   └── length: 复制长度
│
├── 1. 打开源文件（如果未打开）
│   │
│   └── if (file_ctx->is_closed())
│       └── file_ctx->open(true, LSN_MAX, m_start_log_index, m_start_log_offset, 0)
│           │  【打开系统Redo Log文件】
│
├── 2. 获取当前归档组
│   └── curr_group = arch_log_sys->get_arch_group()
│
└── 3. 循环复制数据
    │
    └── while (length > 0)
            │
            ├── 检查当前归档文件空间
            │   │
            │   └── len_left = file_ctx->bytes_left()
            │
            ├── 如果文件写满，切换到下一个
            │   │
            │   └── if (len_left == 0)
            │       └── file_ctx->open_next(...)
            │           │  【创建新的归档文件】
            │
            ├── 计算本次写入量
            │   └── write_size = min(len_left, length)
            │
            ├── 执行写入
            │   │
            │   └── curr_group->write_to_file(file_ctx, nullptr, write_size, ...)
            │       │
            │       └── Arch_Group::write_to_file() - storage/innobase/arch/arch0log.cc
            │           │
            │           ├── 从系统Redo读取数据
            │           │   └── read_from_file(...)
            │           │
            │           ├── 更新归档文件头（如果是新文件）
            │           │   └── update_header(...)
            │           │
            │           └── 写入归档文件
            │               └── write(...)
            │
            └── 更新进度
                ├── length -= write_size
                └── start_lsn += write_size
```

### 归档触发机制

```text
归档触发点:
│
├── 1. Log Writer 写入完成后
│   │  【每次写入都可能触发】
│   │
│   └── log_writer_write_buffer() 内部:
│       └── notify_about_advanced_write_lsn() - log0write.cc:1595
│           │
│           └── if (arch_log_sys && arch_log_sys->is_active())
│               └── os_event_set(log_archiver_thread_event)
│                   【★唤醒归档线程】
│
├── 2. Log Writer 切换文件时
│   │
│   └── log0write.cc:2050
│       └── os_event_set(log_archiver_thread_event)
│
└── 3. 客户端请求时
    │
    └── Arch_Log_Sys::start() 等函数内部
        └── os_event_set(log_archiver_thread_event)
```

### 归档消费者注册

```text
Arch_log_consumer - storage/innobase/arch/arch0log.cc:943
│  【归档作为Log消费者注册】
│  【用于防止Redo被覆盖】
│
├── get_name() → "log_archiver"
│
├── get_consumer_type() → SERVER
│
└── get_consumed_lsn()
    │  【返回已归档的LSN】
    │  【Log系统不会覆盖超过此位置的日志】
    │
    └── return arch_log_sys->get_archived_lsn()

注册/注销:
│
├── 归档激活时注册
│   └── update_state_low(ARCH_STATE_ACTIVE)
│       └── log_consumer_register(log, &m_log_consumer)
│
└── 归档停用时注销
    └── update_state_low(ARCH_STATE_IDLE)
        └── log_consumer_unregister(log, &m_log_consumer)
```

## 归档文件格式

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                         归档文件格式                                         │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  文件名: archive_log_N (N从0开始)                                            │
│  默认大小: 100MB (可配置)                                                    │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  File Header (512 bytes) - LOG_FILE_HDR_SIZE                          │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  Offset 0-3:    LOG_HEADER_FORMAT      (格式版本)                     │  │
│  │  Offset 4-11:   LOG_HEADER_START_LSN   (文件起始LSN)                  │  │
│  │  Offset 12-63:  LOG_HEADER_CREATOR     (创建者信息)                   │  │
│  │  Offset 64-67:  LOG_HEADER_FLAGS       (标志位)                       │  │
│  │  ...                                                                  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  Log Block 1 (512 bytes)                                              │  │
│  ├───────────────────────────────────────────────────────────────────────┤  │
│  │  ┌─────────────────────────────────────────────────────────────────┐  │  │
│  │  │  Block Header (12 bytes)                                        │  │  │
│  │  │    hdr_no, data_len, first_rec_group, epoch_no                 │  │  │
│  │  ├─────────────────────────────────────────────────────────────────┤  │  │
│  │  │  Log Records (496 bytes max)                                    │  │  │
│  │  │    MLOG_*, 日志记录数据                                         │  │  │
│  │  ├─────────────────────────────────────────────────────────────────┤  │  │
│  │  │  Block Trailer (4 bytes)                                        │  │  │
│  │  │    checksum (CRC32)                                             │  │  │
│  │  └─────────────────────────────────────────────────────────────────┘  │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  Log Block 2...N                                                      │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  Trailer Block (512 bytes) - 最后一个块                               │  │
│  │    用于标记归档结束位置                                                │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 与 Clone 的协作

```text
Clone 使用归档的流程:
│
├── 1. Clone 启动
│   │
│   └── Log_Arch_Client_Ctx::start() - arch0log.cc:56
│       │
│       ├── arch_log_sys->start(m_group, m_begin_lsn, header, false)
│       │   │  【启动归档，记录起始LSN】
│       │
│       └── m_state = ARCH_CLIENT_STATE_STARTED
│
├── 2. Clone 进行中
│   │  【归档线程持续归档新产生的Redo】
│   │
│   └── 数据页复制 + Redo 归档并行进行
│
├── 3. Clone 结束
│   │
│   └── Log_Arch_Client_Ctx::stop() - arch0log.cc:79
│       │
│       ├── arch_log_sys->stop(m_group, m_end_lsn, trailer, len)
│       │   │  【停止归档，记录结束LSN】
│       │
│       └── m_state = ARCH_CLIENT_STATE_STOPPED
│
└── 4. 获取归档文件列表
    │
    └── Log_Arch_Client_Ctx::get_files() - arch0log.cc:112
        │  【返回归档文件路径和范围】
```

## 相关参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `innodb_redo_log_archive_dirs` | 空 | 归档目录配置 |
| `ARCH_LOG_CHUNK_SIZE` | 1MB | 每次归档的块大小 |
| 归档文件大小 | 约100MB | 由Clone或备份工具决定 |

## 性能考虑

1. **归档延迟**: 归档基于 `write_lsn`，不是 `flushed_to_disk_lsn`，因此归档数据可能尚未持久化
2. **I/O开销**: 归档会增加额外的读写I/O
3. **空间占用**: 归档文件会占用额外磁盘空间
4. **消费者限制**: 归档作为Log消费者，会影响Redo文件的回收

## 代码位置汇总

| 模块 | 文件 | 行号 | 说明 |
|------|------|------|------|
| 归档线程 | `arch0arch.cc` | 615 | `log_archiver_thread()` |
| 归档核心 | `arch0log.cc` | 845 | `Arch_Log_Sys::archive()` |
| 数据复制 | `arch0log.cc` | 627 | `Arch_Log_Sys::copy_log()` |
| 触发唤醒 | `log0write.cc` | 1617 | `notify_about_advanced_write_lsn()` |
| 消费者 | `arch0log.cc` | 943 | `Arch_log_consumer` |
| 全局对象 | `arch0arch.cc` | 37 | `arch_log_sys` |
