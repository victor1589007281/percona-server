# Aurora MySQL 源码集成补丁

本文档描述了需要在 MySQL/InnoDB 源码中进行的最小化改动。

## 1. log/log0write.cc - Redo 写入拦截

```cpp
// 在 log_writer_write_buffer() 函数中

#include "aurora/aurora_integration.h"  // 添加在文件顶部

void log_writer_write_buffer(log_t &log, ...) {
    // ... 准备数据 ...
    
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_REDO_WRITE(log.write_ahead_buf, write_size, start_lsn, end_lsn) {
        // Aurora 模式：Redo 已发送到远程存储，跳过本地写入
        return;
    }
    // ================================
    
    // 原始逻辑：写入本地文件
    fil_io(IORequestLogWrite, true, ...);
}

// 在 log_writer_flush() 或类似函数中
void log_writer_flush(...) {
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_REDO_FLUSH(flush_to_lsn) {
        // Aurora 模式：Quorum 已确认，跳过本地 fsync
        return;
    }
    // ================================
    
    // 原始逻辑：fsync
    fil_flush(SRV_LOG_SPACE_FIRST_ID);
}
```

## 2. buf/buf0buf.cc - Page 读取拦截

```cpp
// 在 buf_page_get_gen() 或 buf_read_page_low() 中

#include "aurora/aurora_integration.h"

buf_page_t* buf_page_get_gen(
    const page_id_t &page_id,
    const page_size_t &page_size,
    ulint rw_latch,
    buf_block_t *guess,
    Page_fetch mode,
    const char *file,
    ulint line,
    mtr_t *mtr) {
    
    // ... 查找 buffer pool ...
    
    if (block == nullptr) {
        // Page 不在 buffer pool 中
        
        // ====== Aurora Hook 插入点 ======
        if (AURORA_IS_ENABLED()) {
            byte* frame = allocate_page_frame();
            uint64_t target_lsn = get_current_lsn();  // 或从 mtr 获取
            
            if (AURORA_HOOK_PAGE_READ(page_id.space(), page_id.page_no(), 
                                      frame, target_lsn)) {
                // 从远程存储读取成功
                block = add_page_to_buffer_pool(frame, page_id);
                return block;
            }
        }
        // ================================
        
        // 原始逻辑：从本地文件读取
        buf_read_page(page_id, page_size);
    }
    
    return block;
}
```

## 3. buf/buf0flu.cc - Page 写入禁用

```cpp
// 在 buf_flush_page() 中

#include "aurora/aurora_integration.h"

bool buf_flush_page(buf_page_t *bpage, buf_flush_t flush_type, ...) {
    
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_PAGE_WRITE(bpage->id.space(), bpage->id.page_no(), 
                           ((buf_block_t*)bpage)->frame) {
        // Aurora 模式：不写本地，直接标记为干净
        buf_flush_remove(bpage);
        return true;
    }
    // ================================
    
    // 原始逻辑：写入本地文件
    fil_io(IORequestWrite, ...);
}
```

## 4. log/log0chkp.cc - Checkpoint 禁用

```cpp
// 在 log_checkpoint() 中

#include "aurora/aurora_integration.h"

void log_checkpoint(log_t &log, bool sync) {
    
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_CHECKPOINT(log.last_checkpoint_lsn) {
        // Aurora 模式：不需要本地 checkpoint
        // VDL 作为持久化点
        return;
    }
    // ================================
    
    // 原始逻辑
    log_checkpoint_impl(log, sync);
}
```

## 5. log/log0recv.cc - 恢复跳过

```cpp
// 在 recv_recovery_from_checkpoint_start() 中

#include "aurora/aurora_integration.h"

dberr_t recv_recovery_from_checkpoint_start(...) {
    
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_RECOVERY() {
        // Aurora 模式：跳过本地恢复
        // 从元数据服务获取 VDL，Page 按需物化
        return DB_SUCCESS;
    }
    // ================================
    
    // 原始逻辑：从本地 redo log 恢复
    recv_recovery_impl(...);
}
```

## 6. srv/srv0start.cc - 启动初始化

```cpp
// 在 srv_start() 中

#include "aurora/aurora_integration.h"
#include "aurora/aurora_startup.h"
#include "aurora/aurora_config.h"

dberr_t srv_start(bool create_new_db, ...) {
    // ... 其他初始化 ...
    
    // ====== Aurora 初始化 ======
#ifdef HAVE_AURORA
    if (srv_aurora_mode) {
        aurora::AuroraConfig config;
        config.load_from_sysvars();
        
        if (!aurora::aurora_startup(config)) {
            ib::error() << "Aurora startup failed: " 
                        << aurora::g_startup_ctx.error_message;
            return DB_ERROR;
        }
        
        ib::info() << "Aurora mode enabled, role: " 
                   << (aurora::aurora_is_writer() ? "WRITER" : "READER");
    }
#endif
    // ============================
    
    // ... 后续初始化 ...
}

// 在 srv_shutdown() 或 innodb_shutdown() 中
void srv_shutdown() {
    // ====== Aurora 关闭 ======
#ifdef HAVE_AURORA
    if (srv_aurora_mode) {
        aurora::aurora_shutdown();
    }
#endif
    // ========================
    
    // ... 其他关闭逻辑 ...
}
```

## 7. trx/trx0trx.cc - 事务提交

```cpp
// 在 trx_commit_low() 或类似函数中

#include "aurora/aurora_integration.h"

void trx_commit_low(trx_t *trx, mtr_t *mtr) {
    // ... 提交逻辑 ...
    
    // 获取提交 LSN
    lsn_t commit_lsn = mtr_commit(mtr);
    
    // ====== Aurora Hook 插入点 ======
    AURORA_HOOK_TRX_COMMIT(trx->id, commit_lsn);
    // ================================
    
    // ... 后续处理 ...
}
```

## 8. sql/sys_vars.cc - 系统变量注册

```cpp
// 在文件末尾添加

#ifdef HAVE_AURORA
#include "aurora/aurora_sysvars.h"

static Sys_var_bool Sys_aurora_mode(
    "aurora_mode",
    "Enable Aurora distributed storage mode",
    GLOBAL_VAR(srv_aurora_mode),
    CMD_LINE(OPT_ARG),
    DEFAULT(false),
    NO_MUTEX_GUARD,
    NOT_IN_BINLOG
);

static Sys_var_charptr Sys_aurora_volume_id(
    "aurora_volume_id",
    "Aurora volume identifier",
    READ_ONLY GLOBAL_VAR(srv_aurora_volume_id),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("")
);

static Sys_var_charptr Sys_aurora_storage_nodes(
    "aurora_storage_nodes",
    "Comma-separated list of storage nodes (host:port)",
    READ_ONLY GLOBAL_VAR(srv_aurora_storage_nodes),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("")
);

static Sys_var_charptr Sys_aurora_metadata_nodes(
    "aurora_metadata_nodes",
    "Comma-separated list of metadata nodes (host:port)",
    READ_ONLY GLOBAL_VAR(srv_aurora_metadata_nodes),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("")
);

static Sys_var_charptr Sys_aurora_instance_mode(
    "aurora_instance_mode",
    "Instance mode: writer or reader",
    READ_ONLY GLOBAL_VAR(srv_aurora_instance_mode),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("writer")
);

static Sys_var_uint Sys_aurora_quorum_write(
    "aurora_quorum_write",
    "Number of nodes required for write quorum",
    GLOBAL_VAR(srv_aurora_quorum_write),
    CMD_LINE(REQUIRED_ARG),
    VALID_RANGE(1, 6),
    DEFAULT(4),
    BLOCK_SIZE(1)
);

static Sys_var_ulong Sys_aurora_redo_buffer_size(
    "aurora_redo_buffer_size",
    "Size of redo buffer in bytes",
    GLOBAL_VAR(srv_aurora_redo_buffer_size),
    CMD_LINE(REQUIRED_ARG),
    VALID_RANGE(1024*1024, 256*1024*1024),
    DEFAULT(16*1024*1024),
    BLOCK_SIZE(1024*1024)
);

static Sys_var_bool Sys_aurora_binlog_compat(
    "aurora_binlog_compat",
    "Enable binlog compatibility layer for replication tools",
    GLOBAL_VAR(srv_aurora_binlog_compat),
    CMD_LINE(OPT_ARG),
    DEFAULT(true),
    NO_MUTEX_GUARD,
    NOT_IN_BINLOG
);

static Sys_var_charptr Sys_aurora_replication_protocol(
    "aurora_replication_protocol",
    "Replication protocol: quorum or raft",
    READ_ONLY GLOBAL_VAR(srv_aurora_replication_protocol),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("quorum")
);

static Sys_var_charptr Sys_aurora_transport_type(
    "aurora_transport_type",
    "Network transport type: tcp or rdma",
    READ_ONLY GLOBAL_VAR(srv_aurora_transport_type),
    CMD_LINE(REQUIRED_ARG),
    IN_FS_CHARSET,
    DEFAULT("tcp")
);

#endif  // HAVE_AURORA
```

## 9. storage/innobase/CMakeLists.txt - 编译配置

```cmake
# 添加到 storage/innobase/CMakeLists.txt

OPTION(WITH_AURORA "Build with Aurora distributed storage support" OFF)

IF(WITH_AURORA)
    ADD_DEFINITIONS(-DHAVE_AURORA)
    
    # Aurora 源文件
    SET(AURORA_SOURCES
        aurora/aurora.cc
        aurora/aurora_hook.cc
        aurora/aurora_config.cc
        aurora/aurora_client.cc
        aurora/aurora_redo_sender.cc
        aurora/aurora_page_reader.cc
        aurora/aurora_reader_sync.cc
        aurora/aurora_gtid.cc
        aurora/aurora_binlog.cc
        aurora/aurora_commands.cc
        aurora/aurora_sysvars.cc
        aurora/aurora_replication.cc
        aurora/aurora_startup.cc
    )
    
    # 添加到 InnoDB 源文件列表
    SET(INNOBASE_SOURCES
        ${INNOBASE_SOURCES}
        ${AURORA_SOURCES}
    )
    
    # 添加 gRPC 和 Protobuf 依赖
    # FIND_PACKAGE(gRPC REQUIRED)
    # FIND_PACKAGE(Protobuf REQUIRED)
    # TARGET_LINK_LIBRARIES(innobase gRPC::grpc++ protobuf::libprotobuf)
    
ENDIF()
```

## 10. 编译命令

```bash
# 配置编译（启用 Aurora）
cmake .. \
    -DWITH_AURORA=ON \
    -DWITH_BOOST=/path/to/boost \
    -DCMAKE_BUILD_TYPE=Release

# 编译
make -j$(nproc)

# 或仅编译 InnoDB
make innobase
```

## 11. 运行配置

```ini
# my.cnf

[mysqld]
# Aurora 核心配置
aurora_mode = ON
aurora_volume_id = vol-12345678
aurora_storage_nodes = storage1:9002,storage2:9002,storage3:9002,storage4:9002,storage5:9002,storage6:9002
aurora_metadata_nodes = metadata1:9003,metadata2:9003,metadata3:9003
aurora_instance_mode = writer

# Quorum 配置
aurora_quorum_write = 4
aurora_redo_buffer_size = 16M

# Binlog 兼容
aurora_binlog_compat = ON

# 禁用本地存储
innodb_doublewrite = OFF
innodb_flush_log_at_trx_commit = 0
```

## 改动统计

| 文件 | 改动行数 | 说明 |
|------|----------|------|
| log/log0write.cc | ~10 | Redo 写入拦截 |
| buf/buf0buf.cc | ~15 | Page 读取拦截 |
| buf/buf0flu.cc | ~5 | Page 写入禁用 |
| log/log0chkp.cc | ~5 | Checkpoint 禁用 |
| log/log0recv.cc | ~5 | 恢复跳过 |
| srv/srv0start.cc | ~15 | 启动初始化 |
| trx/trx0trx.cc | ~3 | 事务提交 Hook |
| sql/sys_vars.cc | ~80 | 系统变量注册 |
| CMakeLists.txt | ~20 | 编译配置 |
| **总计** | **~158** | |

所有 Aurora 核心逻辑在独立模块中（约 8000+ 行），MySQL 源码改动保持最小化。
