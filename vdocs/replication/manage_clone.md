# MySQL Clone克隆管理命令深度解析

## 概述

本文档详细解析MySQL Clone插件的管理命令，从**源码层面**深入分析命令执行流程、架构设计和可观测性信息的采集与存储机制。Clone插件允许在本地或远程复制InnoDB数据。

**基于源码**: `plugin/clone/src/clone_plugin.cc`, `plugin/clone/src/clone_client.cc`, `plugin/clone/src/clone_server.cc`

---

## 第一部分：Clone管理命令总览

### 1.1 核心管理命令列表

```mermaid
graph TB
    subgraph "<b>MySQL Clone管理命令体系</b>"
        subgraph "<b>克隆操作命令</b>"
            CLONE_LOCAL["<b>CLONE LOCAL</b><br/>本地克隆<br/>复制当前实例数据到本地目录"]
            CLONE_INSTANCE["<b>CLONE INSTANCE</b><br/>远程克隆<br/>从远程实例复制数据"]
        end
        
        subgraph "<b>状态查询命令</b>"
            STATUS["<b>性能模式表</b><br/>performance_schema.clone_status<br/>performance_schema.clone_progress"]
        end
        
        subgraph "<b>配置管理</b>"
            VARS["<b>系统变量</b><br/>clone_autotune_concurrency<br/>clone_buffer_size<br/>clone_max_concurrency"]
            DONOR_LIST["<b>白名单配置</b><br/>clone_valid_donor_list<br/>限制允许的donor"]
        end
        
        subgraph "<b>UDF函数</b>"
            SET_THRESHOLD["<b>clone_set_threshold</b><br/>设置DDL操作阈值"]
        end
    end
    
    CLONE_LOCAL --> STATUS
    CLONE_INSTANCE --> STATUS
    STATUS --> VARS
    VARS --> DONOR_LIST
    DONOR_LIST --> SET_THRESHOLD
    
    style CLONE_LOCAL fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CLONE_INSTANCE fill:#fff3e0,stroke:#333,stroke-width:2px
    style STATUS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**命令详细列表**:

| 命令类型 | 语法 | 权限 | 说明 |
|---------|------|------|------|
| `CLONE LOCAL` | `CLONE LOCAL DATA DIRECTORY = '/path'` | BACKUP_ADMIN | 克隆本地实例到指定目录 |
| `CLONE INSTANCE` | `CLONE INSTANCE FROM 'user'@'host':port IDENTIFIED BY 'password'` | BACKUP_ADMIN, CLONE_ADMIN | 从远程实例克隆数据 |
| `SELECT * FROM clone_status` | Performance Schema查询 | SELECT | 查看克隆状态 |
| `SELECT * FROM clone_progress` | Performance Schema查询 | SELECT | 查看克隆进度 |

---

## 第二部分：CLONE LOCAL 源码深度解析

### 2.1 CLONE LOCAL 架构

**源码位置**: `plugin/clone/src/clone_plugin.cc:464-479`

```mermaid
graph TB
    subgraph "<b>CLONE LOCAL命令执行架构</b>"
        PARSE["<b>SQL解析层</b><br/>sql_parse.cc<br/>解析CLONE语句"]
        
        PLUGIN_ENTRY["<b>Plugin入口</b><br/>plugin_clone_local()<br/>创建Local克隆实例"]
        
        subgraph "<b>克隆对象创建</b>"
            CLIENT_SHARE["<b>Client_Share</b><br/>共享配置<br/>data_dir目标路径"]
            SERVER_OBJ["<b>Server对象</b><br/>本地数据源<br/>INVALID_SOCKET"]
            LOCAL_INST["<b>Local克隆实例</b><br/>myclone::Local<br/>Master线程"]
        end
        
        subgraph "<b>执行流程</b>"
            CLONE_EXEC["<b>clone_exec()</b><br/>执行克隆逻辑"]
            STORAGE_INIT["<b>init_storage()</b><br/>初始化InnoDB存储引擎"]
            COPY_DATA["<b>hton_clone_copy()</b><br/>复制数据文件"]
        end
        
        PFS_MONITOR["<b>PFS监控</b><br/>clone_stmt_local_key<br/>更新progress表"]
    end
    
    PARSE --> PLUGIN_ENTRY
    PLUGIN_ENTRY --> CLIENT_SHARE
    PLUGIN_ENTRY --> SERVER_OBJ
    PLUGIN_ENTRY --> LOCAL_INST
    LOCAL_INST --> CLONE_EXEC
    CLONE_EXEC --> STORAGE_INIT
    STORAGE_INIT --> COPY_DATA
    COPY_DATA --> PFS_MONITOR
    
    style PARSE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CLONE_EXEC fill:#fff3e0,stroke:#333,stroke-width:2px
    style COPY_DATA fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 2.2 CLONE LOCAL 时序图

```mermaid
sequenceDiagram
    participant USER as **用户客户端**
    participant PARSER as **SQL Parser**
    participant PLUGIN as **Clone Plugin**
    participant LOCAL as **Local克隆实例**
    participant STORAGE as **InnoDB存储引擎**
    participant PFS as **Performance Schema**

    Note over USER,PFS: **CLONE LOCAL执行流程**

    USER->>PARSER: CLONE LOCAL DATA DIRECTORY = '/backup'
    PARSER->>PARSER: 解析SQL语句<br/>验证语法
    PARSER->>PLUGIN: plugin_clone_local(thd, '/backup')
    
    Note over PLUGIN: **初始化克隆对象**
    PLUGIN->>PLUGIN: 创建Client_Share<br/>data_dir = '/backup'
    PLUGIN->>PLUGIN: 创建Server对象<br/>socket = INVALID_SOCKET
    
    PLUGIN->>PFS: mysql_clone_start_statement()<br/>clone_stmt_local_key
    
    PLUGIN->>LOCAL: 创建Local实例<br/>myclone::Local(thd, server, share)
    
    PLUGIN->>LOCAL: clone()
    
    LOCAL->>PFS: pfs_begin_state()<br/>更新clone_status表
    LOCAL->>PFS: pfs_change_stage(0)<br/>更新clone_progress表
    
    LOCAL->>LOCAL: clone_exec()<br/>开始克隆
    
    Note over LOCAL,STORAGE: **数据复制流程**
    
    LOCAL->>STORAGE: init_storage(HA_CLONE_MODE_START)<br/>初始化存储引擎
    
    loop 阶段1-7
        LOCAL->>STORAGE: hton_clone_copy()<br/>调用存储引擎copy接口
        
        alt 阶段1: FILE_COPY
            STORAGE->>STORAGE: 扫描.ibd文件<br/>生成文件列表
            STORAGE->>STORAGE: 复制页面数据<br/>按chunk传输
            STORAGE->>PFS: 更新进度<br/>WORK_ESTIMATED, WORK_COMPLETED
        end
        
        alt 阶段2: PAGE_COPY
            STORAGE->>STORAGE: 复制脏页<br/>snapshot期间的变更
        end
        
        alt 阶段3: REDO_COPY
            STORAGE->>STORAGE: 复制Redo Log<br/>确保一致性
        end
        
        alt 阶段4-7: 其他阶段
            STORAGE->>STORAGE: FILE_SYNC, ACK, RESTART<br/>完成克隆
        end
        
        STORAGE-->>LOCAL: 阶段完成
        LOCAL->>PFS: pfs_change_stage(next)<br/>更新当前阶段
    end
    
    LOCAL->>PFS: pfs_end_state(0, nullptr)<br/>标记完成
    LOCAL-->>PLUGIN: 返回成功
    PLUGIN-->>USER: Query OK
```

### 2.3 CLONE LOCAL 关键源码

**源码位置**: `plugin/clone/src/clone_plugin.cc:464-479`

```cpp
static int plugin_clone_local(THD *thd, const char *data_dir) {
  // 1. 创建Client共享配置
  myclone::Client_Share client_share(
      nullptr, 0, nullptr, nullptr, data_dir, 0);

  // 2. 创建Server对象（本地克隆不需要网络socket）
  myclone::Server server(thd, MYSQL_INVALID_SOCKET);

  // 3. 更新Performance Schema监控
  mysql_service_clone_protocol->mysql_clone_start_statement(
      thd, PSI_NOT_INSTRUMENTED, clone_stmt_local_key);

  // 4. 创建Local克隆实例（Master线程）
  myclone::Local clone_inst(thd, &server, &client_share, 0, true);

  // 5. 执行克隆
  auto error = clone_inst.clone();

  return (error);
}
```

**Local::clone()实现** (`plugin/clone/src/clone_local.cc:63-85`):

```cpp
int Local::clone() {
  // 1. 开始PFS状态跟踪
  auto err = m_clone_client.pfs_begin_state();
  if (err != 0) {
    return (err);
  }

  // 2. 切换到第一个阶段
  m_clone_client.pfs_change_stage(0);

  // 3. 执行克隆
  err = clone_exec();

  // 4. 结束PFS状态
  const char *err_mesg = nullptr;
  uint32_t err_number = 0;
  auto thd = m_clone_client.get_thd();

  mysql_service_clone_protocol->mysql_clone_get_error(
      thd, &err_number, &err_mesg);
  m_clone_client.pfs_end_state(err_number, err_mesg);
  
  return (err);
}
```

---

## 第三部分：CLONE INSTANCE 源码深度解析

### 3.1 CLONE INSTANCE 架构

**源码位置**: `plugin/clone/src/clone_plugin.cc:490-514`

```mermaid
graph TB
    subgraph "<b>CLONE INSTANCE远程克隆架构</b>"
        PARSE["<b>SQL解析层</b><br/>解析远程主机信息<br/>用户、密码、端口"]
        
        DONOR_VALIDATE["<b>Donor验证</b><br/>match_valid_donor_address()<br/>白名单检查"]
        
        subgraph "<b>Client端对象</b>"
            CLIENT_SHARE_R["<b>Client_Share</b><br/>remote_host, port<br/>user, password, data_dir"]
            CLIENT_INST["<b>Client克隆实例</b><br/>myclone::Client<br/>Master线程"]
        end
        
        subgraph "<b>连接建立</b>"
            CONNECT_TASK["<b>任务连接</b><br/>connect_remote(false)<br/>主数据传输连接"]
            CONNECT_ACK["<b>ACK连接</b><br/>connect_remote(true)<br/>确认和状态连接"]
        end
        
        subgraph "<b>RPC通信</b>"
            COM_INIT["<b>COM_INIT</b><br/>初始化远程Server<br/>协商版本"]
            COM_EXECUTE["<b>COM_EXECUTE</b><br/>执行数据传输<br/>多线程并发"]
            COM_ACK["<b>COM_ACK</b><br/>确认接收<br/>更新进度"]
        end
        
        subgraph "<b>Server端处理</b>"
            SERVER_INST["<b>Server实例</b><br/>plugin_clone_remote_server()<br/>响应Client请求"]
            STORAGE_COPY["<b>存储引擎复制</b><br/>hton_clone_copy()<br/>读取数据发送"]
        end
    end
    
    PARSE --> DONOR_VALIDATE
    DONOR_VALIDATE --> CLIENT_SHARE_R
    CLIENT_SHARE_R --> CLIENT_INST
    CLIENT_INST --> CONNECT_TASK
    CLIENT_INST --> CONNECT_ACK
    CONNECT_TASK --> COM_INIT
    COM_INIT --> COM_EXECUTE
    COM_EXECUTE --> COM_ACK
    SERVER_INST --> STORAGE_COPY
    
    style PARSE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style COM_EXECUTE fill:#fff3e0,stroke:#333,stroke-width:2px
    style STORAGE_COPY fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 3.2 CLONE INSTANCE 完整时序图

```mermaid
sequenceDiagram
    participant RECIPIENT as **Recipient实例**
    participant DONOR as **Donor实例**
    participant DONOR_SE as **Donor存储引擎**
    participant RECIP_PFS as **Recipient PFS**

    Note over RECIPIENT,RECIP_PFS: **CLONE INSTANCE FROM远程克隆流程**

    RECIPIENT->>RECIPIENT: 解析命令<br/>获取donor地址
    RECIPIENT->>RECIPIENT: match_valid_donor_address()<br/>检查白名单
    
    RECIPIENT->>RECIP_PFS: mysql_clone_start_statement()<br/>clone_stmt_client_key
    
    RECIPIENT->>RECIPIENT: 创建Client实例<br/>myclone::Client(share)
    
    RECIPIENT->>RECIP_PFS: pfs_begin_state()<br/>BEGIN状态
    
    Note over RECIPIENT,DONOR: **建立连接（双连接模式）**
    
    RECIPIENT->>DONOR: connect_remote(false)<br/>主任务连接（TCP）
    DONOR->>DONOR: 接受连接<br/>创建Server实例
    DONOR-->>RECIPIENT: 连接建立
    
    RECIPIENT->>DONOR: connect_remote(true)<br/>ACK连接（TCP）
    DONOR-->>RECIPIENT: ACK连接建立
    
    Note over RECIPIENT,DONOR_SE: **协议协商和初始化**
    
    RECIPIENT->>DONOR: COM_INIT RPC<br/>发送协议版本、SE版本
    DONOR->>DONOR_SE: init_storage(HA_CLONE_MODE_START)<br/>初始化克隆
    DONOR_SE->>DONOR_SE: 获取SE locators<br/>数据库配置信息
    DONOR->>DONOR_SE: 验证配置兼容性
    DONOR-->>RECIPIENT: 返回locators和配置
    
    RECIPIENT->>RECIPIENT: 验证配置<br/>检查磁盘空间
    RECIPIENT->>RECIP_PFS: pfs_change_stage(1)<br/>FILE_COPY阶段
    
    Note over RECIPIENT,DONOR_SE: **阶段1: FILE_COPY**
    
    loop 并行Worker线程
        RECIPIENT->>DONOR: COM_EXECUTE<br/>请求数据chunk
        DONOR->>DONOR_SE: hton_clone_copy()<br/>读取数据页
        DONOR_SE->>DONOR_SE: 扫描.ibd文件<br/>按chunk分割
        DONOR_SE-->>DONOR: 返回数据chunk<br/>含元数据和数据
        DONOR-->>RECIPIENT: 发送chunk
        
        RECIPIENT->>RECIPIENT: 写入本地数据目录<br/>创建.ibd文件
        
        RECIPIENT->>DONOR: COM_ACK（通过ACK连接）<br/>确认接收
        DONOR->>RECIP_PFS: 更新进度<br/>WORK_COMPLETED++
    end
    
    Note over RECIPIENT,DONOR_SE: **阶段2: PAGE_COPY**
    
    RECIPIENT->>RECIP_PFS: pfs_change_stage(2)
    
    RECIPIENT->>DONOR: COM_EXECUTE<br/>请求脏页
    DONOR->>DONOR_SE: hton_clone_copy()<br/>读取snapshot期间的脏页
    DONOR_SE-->>DONOR: 返回脏页数据
    DONOR-->>RECIPIENT: 发送脏页
    RECIPIENT->>RECIPIENT: 应用脏页<br/>覆盖写入
    
    Note over RECIPIENT,DONOR_SE: **阶段3: REDO_COPY**
    
    RECIPIENT->>RECIP_PFS: pfs_change_stage(3)
    
    RECIPIENT->>DONOR: COM_EXECUTE<br/>请求Redo Log
    DONOR->>DONOR_SE: hton_clone_copy()<br/>读取Redo Log
    DONOR_SE->>DONOR_SE: 复制ib_logfile*<br/>确保一致性点
    DONOR_SE-->>DONOR: 返回Redo数据
    DONOR-->>RECIPIENT: 发送Redo
    RECIPIENT->>RECIPIENT: 写入Redo文件
    
    Note over RECIPIENT,DONOR_SE: **阶段4-7: 完成和重启**
    
    RECIPIENT->>RECIP_PFS: pfs_change_stage(4-7)
    RECIPIENT->>DONOR: COM_EXECUTE<br/>FILE_SYNC, COMPLETE
    DONOR->>DONOR_SE: 同步文件<br/>释放snapshot
    
    RECIPIENT->>RECIP_PFS: pfs_end_state(0, nullptr)<br/>COMPLETED状态
    RECIPIENT->>RECIPIENT: 准备重启<br/>加载克隆的数据
```

### 3.3 CLONE INSTANCE 关键源码

**源码位置**: `plugin/clone/src/clone_plugin.cc:490-514`

```cpp
static int plugin_clone_remote_client(
    THD *thd, const char *remote_host, uint remote_port,
    const char *remote_user, const char *remote_passwd,
    const char *data_dir, int ssl_mode) {
  
  // 1. 验证donor地址是否在白名单中
  auto error = match_valid_donor_address(thd, remote_host, remote_port);
  if (error != 0) {
    return (error);
  }

  // 2. 创建Client共享配置
  myclone::Client_Share client_share(
      remote_host, remote_port, remote_user, remote_passwd,
      data_dir, ssl_mode);

  // 3. 更新PFS监控
  mysql_service_clone_protocol->mysql_clone_start_statement(
      thd, PSI_NOT_INSTRUMENTED, clone_stmt_client_key);

  // 4. 创建Client克隆实例
  myclone::Client clone_inst(thd, &client_share, 0, true);

  // 5. 执行远程克隆
  error = clone_inst.clone();

  return (error);
}
```

**Client::clone()实现** (`plugin/clone/src/clone_client.cc:702-800`):

```cpp
int Client::clone() {
  bool restart = false;
  uint restart_count = 0;
  auto num_workers = get_max_concurrency() - 1;

  // 开始PFS状态
  auto err = pfs_begin_state();
  if (err != 0) {
    return (err);
  }

  do {
    ++restart_count;

    // 建立主任务连接
    err = connect_remote(restart, false);
    if (err != 0) {
      break;
    }

    // 建立ACK连接
    err = connect_remote(restart, true);
    if (err != 0) {
      break;
    }

    // 选择RPC命令
    auto rpc_com = is_master() ? COM_INIT : COM_ATTACH;
    if (restart) {
      rpc_com = COM_REINIT;
    }

    // 发送初始化命令
    err = remote_command(rpc_com, false);
    if (err != 0) {
      break;
    }

    // 执行克隆（多线程并发）
    err = clone_exec(num_workers);
    
    // 根据返回值决定是否重启
    if (err == ER_CLONE_DONOR_RESTART) {
      restart = true;
      continue;
    }
    break;
  } while (true);

  return (err);
}
```

---

## 第四部分：克隆阶段详解

### 4.1 克隆七阶段架构

```mermaid
graph LR
    subgraph "<b>Clone七阶段流程</b>"
        STAGE0["<b>阶段0: DROP_DATA</b><br/>删除旧数据<br/>清空数据目录"]
        STAGE1["<b>阶段1: FILE_COPY</b><br/>文件复制<br/>拷贝所有.ibd文件"]
        STAGE2["<b>阶段2: PAGE_COPY</b><br/>页面复制<br/>复制快照期间的脏页"]
        STAGE3["<b>阶段3: REDO_COPY</b><br/>重做日志复制<br/>复制Redo Log"]
        STAGE4["<b>阶段4: FILE_SYNC</b><br/>文件同步<br/>fsync所有文件"]
        STAGE5["<b>阶段5: ACK</b><br/>确认完成<br/>通知donor"]
        STAGE6["<b>阶段6: RESTART</b><br/>重启准备<br/>准备使用新数据"]
    end
    
    STAGE0 --> STAGE1
    STAGE1 --> STAGE2
    STAGE2 --> STAGE3
    STAGE3 --> STAGE4
    STAGE4 --> STAGE5
    STAGE5 --> STAGE6
    
    style STAGE1 fill:#e3f2fd,stroke:#333,stroke-width:2px
    style STAGE2 fill:#fff3e0,stroke:#333,stroke-width:2px
    style STAGE3 fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**阶段详细说明**:

| 阶段 | 枚举值 | 说明 | 工作内容 |
|-----|-------|------|---------|
| DROP_DATA | 0 | 删除数据 | 清空recipient的数据目录 |
| FILE_COPY | 1 | 文件复制 | 复制所有InnoDB数据文件(.ibd), 按chunk分块传输 |
| PAGE_COPY | 2 | 页面复制 | 复制FILE_COPY期间修改的脏页 |
| REDO_COPY | 3 | Redo复制 | 复制Redo Log, 确保一致性恢复点 |
| FILE_SYNC | 4 | 文件同步 | fsync所有文件到磁盘 |
| ACK | 5 | 确认 | 通知donor完成, 释放快照 |
| RESTART | 6 | 重启 | Recipient准备重启使用新数据 |

### 4.2 并发控制机制

```mermaid
graph TB
    subgraph "<b>Clone并发控制架构</b>"
        MASTER_THREAD["<b>Master线程</b><br/>主控线程<br/>负责协调和通信"]
        
        subgraph "<b>Worker线程池</b>"
            WORKER1["<b>Worker 1</b><br/>处理chunk 1"]
            WORKER2["<b>Worker 2</b><br/>处理chunk 2"]
            WORKERN["<b>Worker N</b><br/>处理chunk N"]
        end
        
        CONCURRENCY_CONTROL["<b>并发度控制</b><br/>clone_max_concurrency<br/>默认16个线程"]
        
        AUTO_TUNE["<b>自动调优</b><br/>clone_autotune_concurrency<br/>根据网络和磁盘IO动态调整"]
        
        BUFFER_SIZE["<b>缓冲区大小</b><br/>clone_buffer_size<br/>默认4MB"]
    end
    
    MASTER_THREAD --> WORKER1
    MASTER_THREAD --> WORKER2
    MASTER_THREAD --> WORKERN
    CONCURRENCY_CONTROL --> MASTER_THREAD
    AUTO_TUNE --> CONCURRENCY_CONTROL
    BUFFER_SIZE --> WORKER1
    BUFFER_SIZE --> WORKER2
    BUFFER_SIZE --> WORKERN
    
    style MASTER_THREAD fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CONCURRENCY_CONTROL fill:#fff3e0,stroke:#333,stroke-width:2px
```

---

## 第五部分：可观测性信息深度解析

### 5.1 可观测性架构

```mermaid
graph TB
    subgraph "<b>Clone可观测性信息流转架构</b>"
        subgraph "<b>运行时数据采集</b>"
            CLONE_STATE["<b>克隆状态</b><br/>• BEGIN, IN_PROGRESS<br/>• COMPLETED, FAILED"]
            
            STAGE_PROGRESS["<b>阶段进度</b><br/>• 当前阶段<br/>• 估计工作量<br/>• 已完成工作量"]
            
            PERF_METRICS["<b>性能指标</b><br/>• 网络带宽<br/>• 数据传输速率<br/>• 并发worker数"]
        end
        
        subgraph "<b>Performance Schema表</b>"
            CLONE_STATUS["<b>performance_schema.clone_status</b><br/>克隆任务状态<br/>开始时间、结束时间、错误信息"]
            
            CLONE_PROGRESS["<b>performance_schema.clone_progress</b><br/>各阶段进度<br/>WORK_ESTIMATED, WORK_COMPLETED"]
        end
        
        subgraph "<b>查询接口</b>"
            STATUS_QUERY["<b>SELECT查询</b><br/>实时查询clone_status<br/>和clone_progress表"]
            
            SHOW_STATUS["<b>SHOW STATUS</b><br/>Clone_*状态变量"]
        end
    end
    
    CLONE_STATE --> CLONE_STATUS
    STAGE_PROGRESS --> CLONE_PROGRESS
    PERF_METRICS --> CLONE_PROGRESS
    
    CLONE_STATUS --> STATUS_QUERY
    CLONE_PROGRESS --> STATUS_QUERY
    CLONE_STATUS --> SHOW_STATUS
    
    style CLONE_STATE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CLONE_STATUS fill:#fff3e0,stroke:#333,stroke-width:2px
    style STATUS_QUERY fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 5.2 Performance Schema表结构

**clone_status表结构**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `ID` | BIGINT | 克隆任务ID |
| `PID` | BIGINT | 进程ID |
| `STATE` | VARCHAR | 状态: Not Started, In Progress, Completed, Failed |
| `BEGIN_TIME` | TIMESTAMP | 开始时间 |
| `END_TIME` | TIMESTAMP | 结束时间 |
| `SOURCE` | VARCHAR | 数据源: LOCAL或远程主机地址 |
| `DESTINATION` | VARCHAR | 目标目录 |
| `ERROR_NO` | INT | 错误号 |
| `ERROR_MESSAGE` | VARCHAR | 错误消息 |
| `BINLOG_FILE` | VARCHAR | Binlog文件名 |
| `BINLOG_POSITION` | BIGINT | Binlog位置 |
| `GTID_EXECUTED` | LONGTEXT | 已执行的GTID集合 |

**clone_progress表结构**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `ID` | BIGINT | 克隆任务ID |
| `STAGE` | VARCHAR | 阶段名称: DROP DATA, FILE COPY, PAGE COPY等 |
| `STATE` | VARCHAR | 阶段状态: Not Started, In Progress, Completed |
| `BEGIN_TIME` | TIMESTAMP | 阶段开始时间 |
| `END_TIME` | TIMESTAMP | 阶段结束时间 |
| `THREADS` | INT | 当前worker线程数 |
| `ESTIMATE` | BIGINT | 估计工作量（字节） |
| `DATA` | BIGINT | 已完成工作量（字节） |
| `NETWORK` | BIGINT | 网络传输字节数 |
| `DATA_SPEED` | BIGINT | 数据传输速率（字节/秒） |
| `NETWORK_SPEED` | BIGINT | 网络传输速率（字节/秒） |

### 5.3 数据采集与更新流程

```mermaid
sequenceDiagram
    participant CLONE as **Clone线程**
    participant PFS_API as **PFS API**
    participant STATUS_TABLE as **clone_status表**
    participant PROGRESS_TABLE as **clone_progress表**

    Note over CLONE,PROGRESS_TABLE: **PFS数据采集流程**

    CLONE->>PFS_API: mysql_clone_start_statement()<br/>注册克隆任务
    PFS_API->>STATUS_TABLE: 插入新行<br/>STATE = 'Not Started'
    
    CLONE->>PFS_API: pfs_begin_state()<br/>开始克隆
    PFS_API->>STATUS_TABLE: 更新STATE = 'In Progress'<br/>设置BEGIN_TIME
    
    loop 对每个阶段 (0-6)
        CLONE->>PFS_API: pfs_change_stage(stage_num)<br/>切换阶段
        PFS_API->>PROGRESS_TABLE: 插入新阶段行<br/>STAGE = 'FILE COPY'<br/>STATE = 'In Progress'
        
        loop 处理chunk
            CLONE->>CLONE: 复制数据chunk
            CLONE->>PFS_API: pfs_add_data(bytes)<br/>增加已完成量
            PFS_API->>PROGRESS_TABLE: 更新DATA字段<br/>DATA += bytes
            
            CLONE->>PFS_API: pfs_add_network(bytes)<br/>增加网络传输量
            PFS_API->>PROGRESS_TABLE: 更新NETWORK字段<br/>NETWORK += bytes
            
            alt 动态调整并发度
                CLONE->>PFS_API: pfs_set_threads(num)<br/>更新线程数
                PFS_API->>PROGRESS_TABLE: 更新THREADS字段
            end
        end
        
        CLONE->>PFS_API: pfs_complete_stage()<br/>阶段完成
        PFS_API->>PROGRESS_TABLE: 更新STATE = 'Completed'<br/>设置END_TIME
    end
    
    alt 克隆成功
        CLONE->>PFS_API: pfs_end_state(0, nullptr)<br/>结束成功
        PFS_API->>STATUS_TABLE: STATE = 'Completed'<br/>END_TIME = NOW()<br/>ERROR_NO = 0
    else 克隆失败
        CLONE->>PFS_API: pfs_end_state(err_no, err_msg)<br/>结束失败
        PFS_API->>STATUS_TABLE: STATE = 'Failed'<br/>ERROR_NO = err_no<br/>ERROR_MESSAGE = err_msg
    end
```

### 5.4 监控SQL示例

**查看克隆状态**:

```sql
-- 查看当前克隆任务状态
SELECT 
    ID,
    STATE,
    BEGIN_TIME,
    END_TIME,
    TIMESTAMPDIFF(SECOND, BEGIN_TIME, IFNULL(END_TIME, NOW())) AS duration_seconds,
    SOURCE,
    DESTINATION,
    ERROR_NO,
    ERROR_MESSAGE
FROM performance_schema.clone_status
ORDER BY ID DESC
LIMIT 10;
```

**查看克隆进度**:

```sql
-- 查看各阶段进度
SELECT 
    p.STAGE,
    p.STATE,
    p.THREADS,
    ROUND(p.ESTIMATE / 1024 / 1024 / 1024, 2) AS estimate_gb,
    ROUND(p.DATA / 1024 / 1024 / 1024, 2) AS completed_gb,
    ROUND(p.DATA * 100.0 / NULLIF(p.ESTIMATE, 0), 2) AS progress_pct,
    ROUND(p.DATA_SPEED / 1024 / 1024, 2) AS data_speed_mbps,
    ROUND(p.NETWORK_SPEED / 1024 / 1024, 2) AS network_speed_mbps,
    TIMESTAMPDIFF(SECOND, p.BEGIN_TIME, IFNULL(p.END_TIME, NOW())) AS duration_seconds
FROM performance_schema.clone_progress p
JOIN performance_schema.clone_status s ON p.ID = s.ID
WHERE s.STATE = 'In Progress'
ORDER BY p.STAGE;
```

**计算剩余时间**:

```sql
-- 估算剩余时间
SELECT 
    STAGE,
    ROUND((ESTIMATE - DATA) / NULLIF(DATA_SPEED, 0)) AS estimated_remaining_seconds,
    SEC_TO_TIME(ROUND((ESTIMATE - DATA) / NULLIF(DATA_SPEED, 0))) AS remaining_time
FROM performance_schema.clone_progress
WHERE STATE = 'In Progress' AND DATA_SPEED > 0;
```

---

## 第六部分：配置与优化

### 6.1 系统变量详解

| 变量名 | 默认值 | 说明 | 优化建议 |
|-------|-------|------|---------|
| `clone_autotune_concurrency` | ON | 自动调整并发度 | 生产环境建议开启 |
| `clone_buffer_size` | 4MB | 每个线程的缓冲区大小 | 大文件可增大到16MB |
| `clone_ddl_timeout` | 300s | DDL操作超时时间 | 根据DDL复杂度调整 |
| `clone_donor_timeout_after_network_failure` | 5分钟 | 网络故障后donor超时 | 网络不稳定可增大 |
| `clone_enable_compression` | OFF | 启用压缩 | 网络带宽受限时开启 |
| `clone_max_concurrency` | 16 | 最大并发线程数 | 高性能服务器可增大到32 |
| `clone_max_data_bandwidth` | 0 | 最大数据带宽（MB/s） | 限流使用，0表示无限制 |
| `clone_max_network_bandwidth` | 0 | 最大网络带宽（MB/s） | 限流使用，0表示无限制 |
| `clone_valid_donor_list` | NULL | 允许的donor列表 | 安全考虑，配置白名单 |

### 6.2 性能优化策略

```mermaid
graph TB
    subgraph "<b>Clone性能优化策略</b>"
        subgraph "<b>网络优化</b>"
            BANDWIDTH["<b>带宽限制</b><br/>clone_max_network_bandwidth<br/>避免影响在线业务"]
            COMPRESSION["<b>压缩传输</b><br/>clone_enable_compression<br/>减少网络传输量"]
            SSL_OFF["<b>关闭SSL</b><br/>减少加密开销<br/>内网可考虑"]
        end
        
        subgraph "<b>并发优化</b>"
            CONCURRENCY["<b>并发度调整</b><br/>clone_max_concurrency<br/>根据CPU和磁盘IO"]
            AUTO_TUNE["<b>自动调优</b><br/>clone_autotune_concurrency<br/>动态调整线程数"]
        end
        
        subgraph "<b>存储优化</b>"
            BUFFER_SIZE["<b>缓冲区大小</b><br/>clone_buffer_size<br/>大文件传输增大"]
            FAST_DISK["<b>快速磁盘</b><br/>SSD或NVMe<br/>提高IO性能"]
        end
        
        subgraph "<b>Donor端优化</b>"
            READ_BUFFER["<b>InnoDB读缓冲</b><br/>innodb_read_io_threads<br/>增加读线程"]
            BUFFER_POOL["<b>Buffer Pool</b><br/>提高缓存命中率<br/>减少磁盘读取"]
        end
    end
    
    BANDWIDTH --> COMPRESSION
    COMPRESSION --> SSL_OFF
    CONCURRENCY --> AUTO_TUNE
    BUFFER_SIZE --> FAST_DISK
    READ_BUFFER --> BUFFER_POOL
    
    style CONCURRENCY fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BUFFER_SIZE fill:#fff3e0,stroke:#333,stroke-width:2px
    style BUFFER_POOL fill:#e8f5e8,stroke:#333,stroke-width:2px
```

---

## 第七部分：故障排查

### 7.1 常见错误及解决方案

| 错误码 | 错误信息 | 原因 | 解决方案 |
|-------|---------|------|---------|
| ER_CLONE_DONOR | Clone Donor Error | Donor端错误 | 检查donor日志，确保donor正常运行 |
| ER_CLONE_PROTOCOL | Clone Protocol Error | 协议不兼容 | 确保recipient和donor版本兼容 |
| ER_CLONE_DONOR_VERSION | Version mismatch | 版本不匹配 | 升级或降级到相同版本 |
| ER_CLONE_OS | OS mismatch | 操作系统不匹配 | 确保相同的操作系统 |
| ER_CLONE_CHARSET | Charset mismatch | 字符集不匹配 | 统一字符集配置 |
| ER_CLONE_CONFIG | Configuration error | 配置不兼容 | 检查innodb_page_size等配置 |
| ER_CLONE_SYS_CONFIG | System config error | 系统配置错误 | 检查datadir权限和磁盘空间 |
| ER_CLONE_DISK_SPACE | Insufficient disk space | 磁盘空间不足 | 清理磁盘或扩容 |
| ER_CLONE_NETWORK | Network error | 网络错误 | 检查网络连接和防火墙 |

### 7.2 诊断流程

```mermaid
graph TB
    subgraph "<b>Clone故障诊断流程</b>"
        ISSUE["<b>克隆失败</b>"]
        
        CHECK_STATUS["<b>检查状态</b><br/>SELECT * FROM clone_status<br/>查看ERROR_NO和ERROR_MESSAGE"]
        
        subgraph "<b>错误分类</b>"
            NET_ERROR["<b>网络错误</b><br/>ER_CLONE_NETWORK<br/>连接超时"]
            SPACE_ERROR["<b>空间错误</b><br/>ER_CLONE_DISK_SPACE<br/>磁盘不足"]
            VERSION_ERROR["<b>版本错误</b><br/>ER_CLONE_DONOR_VERSION<br/>版本不兼容"]
            CONFIG_ERROR["<b>配置错误</b><br/>ER_CLONE_CONFIG<br/>参数不匹配"]
        end
        
        subgraph "<b>解决方案</b>"
            FIX_NET["<b>修复网络</b><br/>检查防火墙<br/>增大timeout"]
            FIX_SPACE["<b>扩展空间</b><br/>清理磁盘<br/>增加存储"]
            FIX_VERSION["<b>统一版本</b><br/>升级/降级<br/>MySQL版本"]
            FIX_CONFIG["<b>调整配置</b><br/>修改innodb_page_size<br/>等参数"]
        end
    end
    
    ISSUE --> CHECK_STATUS
    CHECK_STATUS --> NET_ERROR
    CHECK_STATUS --> SPACE_ERROR
    CHECK_STATUS --> VERSION_ERROR
    CHECK_STATUS --> CONFIG_ERROR
    
    NET_ERROR --> FIX_NET
    SPACE_ERROR --> FIX_SPACE
    VERSION_ERROR --> FIX_VERSION
    CONFIG_ERROR --> FIX_CONFIG
    
    style ISSUE fill:#ffebee,stroke:#333,stroke-width:2px
    style CHECK_STATUS fill:#e3f2fd,stroke:#333,stroke-width:2px
```

---

## 总结

### 核心要点回顾

**命令管理**:

- **CLONE LOCAL**: 本地克隆，复制当前实例数据到指定目录
- **CLONE INSTANCE**: 远程克隆，从远程donor实例复制数据
- 七阶段流程：DROP_DATA, FILE_COPY, PAGE_COPY, REDO_COPY, FILE_SYNC, ACK, RESTART

**架构特点**:

- **双连接模式**: 主任务连接传输数据，ACK连接确认进度
- **多线程并发**: Master线程协调，Worker线程池并行传输
- **增量复制**: FILE_COPY后，PAGE_COPY处理脏页，REDO_COPY保证一致性
- **自动调优**: clone_autotune_concurrency动态调整并发度

**可观测性**:

- **Performance Schema表**: clone_status显示整体状态，clone_progress显示各阶段进度
- **实时监控**: 可查询估计工作量、已完成量、传输速率、剩余时间
- **错误追踪**: 详细的错误号和错误消息

**最佳实践**:

- 配置clone_valid_donor_list白名单，提高安全性
- 根据硬件资源调整clone_max_concurrency和clone_buffer_size
- 网络受限时开启clone_enable_compression
- 使用Performance Schema表实时监控进度和性能

MySQL Clone插件提供了高效、可靠的数据复制能力，是快速搭建副本、灾难恢复的重要工具。

---

## 第八部分：Clone内部RPC协议深度解析

### 8.1 Clone RPC命令体系

**源码位置**: `plugin/clone/src/clone_server.cc:204-249`

```mermaid
graph TB
    subgraph "<b>Clone RPC命令体系</b>"
        subgraph "<b>初始化命令</b>"
            COM_INIT["<b>COM_INIT</b><br/>初始化克隆<br/>协商版本和配置"]
            COM_REINIT["<b>COM_REINIT</b><br/>重新初始化<br/>用于重启后恢复"]
            COM_ATTACH["<b>COM_ATTACH</b><br/>附加任务<br/>Worker线程加入"]
        end
        
        subgraph "<b>执行命令</b>"
            COM_EXECUTE["<b>COM_EXECUTE</b><br/>执行数据传输<br/>传输chunk数据"]
            COM_ACK["<b>COM_ACK</b><br/>确认接收<br/>更新进度"]
        end
        
        subgraph "<b>控制命令</b>"
            COM_EXIT["<b>COM_EXIT</b><br/>退出克隆<br/>清理资源"]
        end
    end
    
    COM_INIT --> COM_ATTACH
    COM_ATTACH --> COM_EXECUTE
    COM_EXECUTE --> COM_ACK
    COM_ACK --> COM_EXECUTE
    COM_EXECUTE --> COM_EXIT
    COM_REINIT --> COM_EXECUTE
    
    style COM_INIT fill:#e3f2fd,stroke:#333,stroke-width:2px
    style COM_EXECUTE fill:#fff3e0,stroke:#333,stroke-width:2px
    style COM_ACK fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 8.2 Clone RPC协议格式

**COM_INIT协议**:

```text
COM_INIT Request (Recipient -> Donor):
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| command              | int<1>         | COM_INIT (1)                      |
| protocol_version     | int<1>         | Clone协议版本                     |
| donor_locator_len    | int<4>         | Donor locator长度                 |
| donor_locator        | string<VAR>    | Donor locator数据                 |
+----------------------+----------------+------------------------------------+

COM_INIT Response (Donor -> Recipient):
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| response_code        | int<1>         | 0=成功, 非0=错误                  |
| error_num            | int<4>         | 错误号                            |
| se_protocol_version  | int<1>         | 存储引擎协议版本                  |
| se_locator_len       | int<4>         | SE locator长度                    |
| se_locator           | blob           | SE locator数据（配置信息）        |
+----------------------+----------------+------------------------------------+
```

**COM_EXECUTE协议**:

```text
COM_EXECUTE Request (Recipient -> Donor):
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| command              | int<1>         | COM_EXECUTE (4)                   |
| task_id              | int<4>         | 任务ID（worker编号）              |
| chunk_num            | int<4>         | 请求的chunk编号                   |
| block_num            | int<4>         | 请求的block编号                   |
+----------------------+----------------+------------------------------------+

COM_EXECUTE Response (Donor -> Recipient):
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| response_code        | int<1>         | 0=成功, 非0=错误                  |
| state                | int<1>         | 传输状态                          |
| chunk_num            | int<4>         | 当前chunk编号                     |
| block_num            | int<4>         | 当前block编号                     |
| data_len             | int<4>         | 数据长度                          |
| file_metadata_len    | int<4>         | 文件元数据长度                    |
| file_metadata        | blob           | 文件元数据（路径、大小等）        |
| data                 | blob           | 实际数据块                        |
+----------------------+----------------+------------------------------------+
```

**COM_ACK协议**:

```text
COM_ACK Request (Recipient -> Donor via ACK connection):
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| command              | int<1>         | COM_ACK (5)                       |
| task_id              | int<4>         | 任务ID                            |
| chunk_num            | int<4>         | 已接收的chunk编号                 |
| block_num            | int<4>         | 已接收的block编号                 |
| data_size            | int<8>         | 已接收的数据大小                  |
+----------------------+----------------+------------------------------------+
```

### 8.3 Donor/Recipient完整交互时序

```mermaid
sequenceDiagram
    participant RECIP_MASTER as **Recipient Master线程**
    participant RECIP_WORKERS as **Recipient Workers**
    participant DONOR_MASTER as **Donor Master线程**
    participant DONOR_SE as **Donor存储引擎**
    participant ACK_CONN as **ACK连接**

    Note over RECIP_MASTER,ACK_CONN: **Clone完整RPC交互协议**

    Note over RECIP_MASTER: **阶段1: 初始化**
    
    RECIP_MASTER->>DONOR_MASTER: COM_INIT<br/>• protocol_version<br/>• donor_locator
    
    DONOR_MASTER->>DONOR_SE: init_storage(HA_CLONE_MODE_START)<br/>初始化存储引擎
    DONOR_SE->>DONOR_SE: 创建snapshot<br/>生成locators
    DONOR_SE-->>DONOR_MASTER: se_locator<br/>• innodb_page_size<br/>• innodb_data_file_path<br/>• redo_log_size等
    
    DONOR_MASTER-->>RECIP_MASTER: Response<br/>• se_protocol_version<br/>• se_locator
    
    RECIP_MASTER->>RECIP_MASTER: 验证配置兼容性<br/>• page_size匹配<br/>• 版本兼容
    
    Note over RECIP_MASTER: **阶段2: 启动Workers**
    
    RECIP_MASTER->>RECIP_MASTER: 创建Worker线程<br/>数量: clone_max_concurrency
    
    loop 对每个Worker
        RECIP_WORKERS->>DONOR_MASTER: COM_ATTACH<br/>• task_id = worker_id<br/>• locator
        
        DONOR_MASTER->>DONOR_SE: init_storage(HA_CLONE_MODE_ADD_TASK)<br/>添加任务
        DONOR_MASTER-->>RECIP_WORKERS: Response: OK
    end
    
    Note over RECIP_MASTER,DONOR_SE: **阶段3: FILE_COPY数据传输**
    
    loop 并行传输（多个workers）
        RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE<br/>• task_id<br/>• chunk_num = 0<br/>• block_num = 0
        
        DONOR_MASTER->>DONOR_SE: hton_clone_copy()<br/>读取数据chunk
        
        alt 第一个chunk（包含文件元数据）
            DONOR_SE->>DONOR_SE: 扫描.ibd文件列表<br/>计算总大小
            DONOR_SE-->>DONOR_MASTER: file_metadata<br/>• file_name<br/>• file_size<br/>• tablespace_id
        end
        
        DONOR_SE->>DONOR_SE: 读取数据页<br/>按clone_buffer_size分块
        DONOR_SE-->>DONOR_MASTER: data chunk<br/>• 数据内容<br/>• chunk_num<br/>• state
        
        DONOR_MASTER-->>RECIP_WORKERS: Response<br/>• file_metadata<br/>• data<br/>• data_len
        
        RECIP_WORKERS->>RECIP_WORKERS: 写入本地文件<br/>创建.ibd文件
        
        Note over ACK_CONN: **通过ACK连接确认**
        RECIP_WORKERS->>ACK_CONN: COM_ACK<br/>• chunk_num<br/>• data_size
        ACK_CONN->>DONOR_MASTER: 转发ACK
        DONOR_MASTER->>DONOR_MASTER: 更新PFS进度<br/>DATA += data_size
        
        alt 动态调整并发度
            DONOR_MASTER->>DONOR_MASTER: 根据网络和磁盘IO<br/>调整active_workers
            
            alt 网络慢或磁盘慢
                DONOR_MASTER->>DONOR_MASTER: 减少active_workers
            else 资源充足
                DONOR_MASTER->>DONOR_MASTER: 增加active_workers
            end
        end
        
        alt 达到chunk末尾
            RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE (下一个chunk)
        end
    end
    
    Note over RECIP_MASTER,DONOR_SE: **阶段4: PAGE_COPY脏页传输**
    
    RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE (PAGE_COPY阶段)
    DONOR_MASTER->>DONOR_SE: hton_clone_copy()<br/>读取脏页
    DONOR_SE->>DONOR_SE: 扫描Buffer Pool<br/>找到FILE_COPY期间修改的页
    DONOR_SE-->>DONOR_MASTER: 脏页数据
    DONOR_MASTER-->>RECIP_WORKERS: 脏页chunk
    RECIP_WORKERS->>RECIP_WORKERS: 覆盖写入对应页<br/>in-place update
    
    Note over RECIP_MASTER,DONOR_SE: **阶段5: REDO_COPY日志传输**
    
    RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE (REDO_COPY阶段)
    DONOR_MASTER->>DONOR_SE: hton_clone_copy()<br/>读取Redo Log
    DONOR_SE->>DONOR_SE: 复制ib_logfile*<br/>从snapshot点开始
    DONOR_SE-->>DONOR_MASTER: Redo log数据
    DONOR_MASTER-->>RECIP_WORKERS: Redo chunk
    RECIP_WORKERS->>RECIP_WORKERS: 写入ib_logfile0, ib_logfile1
    
    Note over RECIP_MASTER,DONOR_SE: **阶段6: 完成和清理**
    
    RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE (FILE_SYNC)
    DONOR_MASTER->>DONOR_SE: fsync所有文件
    
    RECIP_WORKERS->>DONOR_MASTER: COM_EXECUTE (COMPLETE)
    DONOR_MASTER->>DONOR_SE: 释放snapshot
    
    RECIP_WORKERS->>DONOR_MASTER: COM_EXIT
    DONOR_MASTER->>DONOR_MASTER: 清理任务<br/>释放资源
```

---

## 第九部分：Clone参数完整详解

### 9.1 并发控制参数详解

**源码位置**: `plugin/clone/src/clone_client.cc:699-707`

| 参数 | 默认值 | 范围 | 说明 | 功能时序 |
|------|-------|------|------|---------|
| `clone_max_concurrency` | 16 | 1-128 | 最大并发Worker线程数 | 见9.2节 |
| `clone_autotune_concurrency` | ON | ON/OFF | 自动调整并发度 | 见9.3节 |

### 9.2 clone_max_concurrency 功能时序

```mermaid
sequenceDiagram
    participant MASTER as **Master线程**
    participant WORKERS as **Worker线程池**
    participant SE as **存储引擎**
    participant NETWORK as **网络层**

    Note over MASTER,NETWORK: **并发度控制机制**

    MASTER->>MASTER: 启动时读取clone_max_concurrency<br/>max_workers = 16
    
    MASTER->>MASTER: 计算实际并发度<br/>num_workers = min(max_workers, available_cpu)
    
    loop 创建Workers
        MASTER->>WORKERS: 创建worker线程<br/>worker_id = 0..num_workers-1
        WORKERS->>WORKERS: 初始化worker<br/>分配buffer (clone_buffer_size)
    end
    
    Note over WORKERS: **并行数据传输**
    
    par Worker 0
        WORKERS->>SE: 请求chunk 0
        SE-->>WORKERS: 返回数据
        WORKERS->>NETWORK: 写入网络
    and Worker 1
        WORKERS->>SE: 请求chunk 1
        SE-->>WORKERS: 返回数据
        WORKERS->>NETWORK: 写入网络
    and Worker N
        WORKERS->>SE: 请求chunk N
        SE-->>WORKERS: 返回数据
        WORKERS->>NETWORK: 写入网络
    end
    
    Note over MASTER: **吞吐量提升**
    
    MASTER->>MASTER: 总吞吐 = Σ(worker_throughput)<br/>≈ num_workers × single_worker_throughput
```

### 9.3 clone_autotune_concurrency 自动调优

```mermaid
sequenceDiagram
    participant CLONE as **Clone控制器**
    participant MONITOR as **性能监控器**
    participant WORKERS as **Worker线程池**

    Note over CLONE,WORKERS: **自动调优并发度机制**

    loop 每5秒采样
        MONITOR->>MONITOR: 采集性能指标<br/>• 网络吞吐量<br/>• 磁盘IO<br/>• CPU使用率
        
        MONITOR->>MONITOR: 计算效率<br/>efficiency = current_throughput / num_active_workers
        
        alt 效率下降（资源瓶颈）
            MONITOR->>MONITOR: 检测瓶颈类型
            
            alt 网络带宽饱和
                MONITOR->>MONITOR: 网络吞吐接近clone_max_network_bandwidth
                MONITOR->>WORKERS: 减少active_workers<br/>num_active -= 2
            else 磁盘IO饱和
                MONITOR->>MONITOR: 磁盘IO接近100%
                MONITOR->>WORKERS: 减少active_workers<br/>num_active -= 1
            else CPU饱和
                MONITOR->>MONITOR: CPU使用率 > 90%
                MONITOR->>WORKERS: 减少active_workers<br/>num_active -= 1
            end
        else 效率提升且资源充足
            MONITOR->>MONITOR: 资源使用率 < 80%
            MONITOR->>WORKERS: 增加active_workers<br/>num_active += 1
            MONITOR->>MONITOR: 限制: num_active <= clone_max_concurrency
        end
        
        MONITOR->>CLONE: 更新PFS<br/>THREADS = num_active
    end
```

### 9.4 缓冲区和传输参数

| 参数 | 默认值 | 范围 | 说明 | 优化建议 |
|------|-------|------|------|---------|
| `clone_buffer_size` | 4MB | 1MB-128MB | 每个Worker的缓冲区大小 | 大文件场景增大到16MB |
| `clone_max_data_bandwidth` | 0 (unlimited) | 0-ULONG_MAX (MB/s) | 最大数据带宽限制 | 0表示不限制 |
| `clone_max_network_bandwidth` | 0 (unlimited) | 0-ULONG_MAX (MB/s) | 最大网络带宽限制 | 避免影响在线业务 |

### 9.5 带宽限制功能时序

```mermaid
sequenceDiagram
    participant WORKER as **Worker线程**
    participant THROTTLE as **带宽限流器**
    participant NETWORK as **网络层**

    Note over WORKER,NETWORK: **带宽限制机制**

    WORKER->>WORKER: 读取一个chunk<br/>size = 4MB
    
    WORKER->>THROTTLE: 请求发送许可<br/>request_permit(4MB)
    
    THROTTLE->>THROTTLE: 检查当前带宽使用<br/>current_bandwidth = Σ(worker_bandwidth)
    
    alt 超过clone_max_network_bandwidth
        THROTTLE->>THROTTLE: 计算需要等待的时间<br/>delay = (size / max_bandwidth) - elapsed
        THROTTLE->>THROTTLE: sleep(delay)<br/>限流等待
    end
    
    THROTTLE-->>WORKER: 允许发送
    
    WORKER->>NETWORK: 发送数据<br/>write(chunk_data)
    
    WORKER->>THROTTLE: 更新统计<br/>bytes_sent += 4MB<br/>timestamp = now()
    
    THROTTLE->>THROTTLE: 更新rolling window<br/>计算实时带宽
```

### 9.6 压缩参数

| 参数 | 默认值 | 范围 | 说明 | 源码位置 |
|------|-------|------|------|---------|
| `clone_enable_compression` | OFF | ON/OFF | 启用数据压缩传输 | `plugin/clone/src/clone_client.cc` |
| `clone_compression_algorithm` | zstd | zstd, lz4 | 压缩算法 | MySQL 8.0.30+ |
| `clone_compression_level` | 3 | 1-22 (zstd) | 压缩级别，越高压缩率越高但速度越慢 | 默认平衡设置 |

### 9.7 压缩传输时序

```mermaid
sequenceDiagram
    participant DONOR_SE as **Donor存储引擎**
    participant DONOR_CLONE as **Donor Clone层**
    participant COMPRESS as **压缩器**
    participant NETWORK as **网络**
    participant DECOMPRESS as **解压器**
    participant RECIP_CLONE as **Recipient Clone层**

    Note over DONOR_SE,RECIP_CLONE: **压缩传输流程**

    DONOR_SE->>DONOR_CLONE: 读取数据chunk<br/>原始大小: 4MB
    
    alt clone_enable_compression = ON
        DONOR_CLONE->>COMPRESS: compress_data(chunk, algorithm, level)
        
        alt algorithm = zstd
            COMPRESS->>COMPRESS: ZSTD_compress()<br/>压缩级别: clone_compression_level
        else algorithm = lz4
            COMPRESS->>COMPRESS: LZ4_compress()<br/>快速压缩
        end
        
        COMPRESS-->>DONOR_CLONE: 压缩后数据<br/>压缩大小: 1.2MB (70%压缩率)
        
        DONOR_CLONE->>DONOR_CLONE: 添加压缩元数据<br/>• 原始大小: 4MB<br/>• 压缩大小: 1.2MB<br/>• 算法: zstd
    end
    
    DONOR_CLONE->>NETWORK: 发送压缩数据<br/>1.2MB (节省2.8MB带宽)
    
    NETWORK-->>RECIP_CLONE: 接收压缩数据
    
    alt 数据已压缩
        RECIP_CLONE->>DECOMPRESS: decompress_data(compressed_chunk)
        DECOMPRESS->>DECOMPRESS: ZSTD_decompress()
        DECOMPRESS-->>RECIP_CLONE: 恢复原始数据<br/>4MB
    end
    
    RECIP_CLONE->>RECIP_CLONE: 写入本地文件<br/>4MB原始数据
```

### 9.8 超时和错误处理参数

| 参数 | 默认值 | 范围 | 说明 |
|------|-------|------|------|
| `clone_ddl_timeout` | 300 | 0-INT_MAX | DDL操作超时（秒） |
| `clone_donor_timeout_after_network_failure` | 5分钟 | 0-INT_MAX | Donor网络故障后的超时 |

### 9.9 错误处理和重试时序

```mermaid
sequenceDiagram
    participant RECIP as **Recipient**
    participant DONOR as **Donor**

    Note over RECIP,DONOR: **网络故障和重试机制**

    RECIP->>DONOR: COM_EXECUTE (请求chunk)
    
    alt 网络连接正常
        DONOR-->>RECIP: 返回数据chunk
        RECIP->>RECIP: 写入本地文件
    else 网络连接中断
        DONOR--xRECIP: 连接断开
        
        RECIP->>RECIP: 检测到网络错误<br/>errno = ECONNRESET
        
        RECIP->>RECIP: 启动重试机制<br/>retry_count = 0
        
        loop while (retry_count < 3)
            RECIP->>RECIP: 等待重连间隔<br/>sleep(5秒)
            
            RECIP->>DONOR: 尝试重新连接<br/>connect_remote(restart=true)
            
            alt 重连成功
                RECIP->>DONOR: COM_REINIT<br/>恢复克隆状态
                DONOR->>DONOR: 恢复snapshot<br/>从断点继续
                DONOR-->>RECIP: OK，继续传输
                RECIP->>DONOR: COM_EXECUTE<br/>从上次位置继续
            else 重连失败
                RECIP->>RECIP: retry_count++
                
                alt retry_count >= 3
                    RECIP->>RECIP: 放弃克隆<br/>报告错误
                    RECIP->>RECIP: 清理已下载的数据
                end
            end
        end
    end
```

### 9.10 Donor白名单参数

| 参数 | 默认值 | 说明 | 安全建议 |
|------|-------|------|---------|
| `clone_valid_donor_list` | NULL (允许所有) | 允许的Donor地址列表，格式: 'host1:port1,host2:port2' | 生产环境必须配置白名单 |

### 9.11 白名单验证时序

```mermaid
sequenceDiagram
    participant RECIP as **Recipient**
    participant VALIDATOR as **白名单验证器**
    participant DONOR as **Donor**

    Note over RECIP,DONOR: **Donor白名单验证**

    RECIP->>RECIP: 解析CLONE INSTANCE命令<br/>donor_host = '10.0.0.100'<br/>donor_port = 3306
    
    RECIP->>VALIDATOR: match_valid_donor_address(host, port)
    
    VALIDATOR->>VALIDATOR: 读取clone_valid_donor_list<br/>'10.0.0.100:3306,10.0.0.101:3306'
    
    VALIDATOR->>VALIDATOR: 解析白名单<br/>valid_donors[] = [<br/>  {host='10.0.0.100', port=3306},<br/>  {host='10.0.0.101', port=3306}<br/>]
    
    VALIDATOR->>VALIDATOR: 遍历白名单<br/>检查donor是否匹配
    
    alt donor在白名单中
        VALIDATOR-->>RECIP: 验证通过
        RECIP->>DONOR: 开始连接
    else donor不在白名单中
        VALIDATOR-->>RECIP: ERROR: ER_CLONE_DONOR_NOT_IN_VALID_LIST
        RECIP-->>RECIP: 克隆失败<br/>'Donor not in valid list'
    end
```

---

## 第十部分：Clone监控和诊断增强

### 10.1 实时监控SQL扩展

**监控传输速率趋势**:

```sql
-- 实时监控各阶段的传输速率（每秒更新）
SELECT 
    ID,
    STAGE,
    STATE,
    THREADS AS active_workers,
    ROUND(DATA / 1024 / 1024 / 1024, 2) AS completed_gb,
    ROUND(ESTIMATE / 1024 / 1024 / 1024, 2) AS total_gb,
    ROUND(DATA * 100.0 / NULLIF(ESTIMATE, 0), 2) AS progress_pct,
    ROUND(DATA_SPEED / 1024 / 1024, 2) AS data_mbps,
    ROUND(NETWORK_SPEED / 1024 / 1024, 2) AS network_mbps,
    -- 压缩率计算
    CASE 
        WHEN NETWORK > 0 THEN ROUND((1 - NETWORK * 1.0 / DATA) * 100, 2)
        ELSE 0
    END AS compression_ratio_pct,
    -- 预计剩余时间
    CASE 
        WHEN DATA_SPEED > 0 THEN 
            SEC_TO_TIME(ROUND((ESTIMATE - DATA) / DATA_SPEED))
        ELSE 'N/A'
    END AS eta
FROM performance_schema.clone_progress
WHERE STATE = 'In Progress'
ORDER BY BEGIN_TIME DESC;
```

**监控并发度自动调整**:

```sql
-- 观察自动调优如何调整并发度
SELECT 
    p.STAGE,
    p.THREADS AS current_workers,
    p.DATA_SPEED / 1024 / 1024 AS data_mbps,
    p.NETWORK_SPEED / 1024 / 1024 AS network_mbps,
    -- 每个Worker的平均速率
    ROUND(p.DATA_SPEED / NULLIF(p.THREADS, 0) / 1024 / 1024, 2) AS mbps_per_worker,
    TIMESTAMPDIFF(SECOND, p.BEGIN_TIME, NOW()) AS elapsed_seconds
FROM performance_schema.clone_progress p
WHERE p.STATE = 'In Progress'
ORDER BY p.BEGIN_TIME DESC;
```

### 10.2 故障诊断增强

**诊断慢速克隆**:

```sql
-- 诊断克隆慢的原因
SELECT 
    'Network Bottleneck' AS issue,
    CASE 
        WHEN NETWORK_SPEED < 100*1024*1024 THEN 'YES - Network < 100MB/s'
        ELSE 'NO'
    END AS detected,
    CONCAT(ROUND(NETWORK_SPEED/1024/1024, 2), ' MB/s') AS current_speed
FROM performance_schema.clone_progress
WHERE STATE = 'In Progress'

UNION ALL

SELECT 
    'Disk Bottleneck' AS issue,
    CASE 
        WHEN DATA_SPEED < NETWORK_SPEED * 0.7 THEN 'YES - Disk slower than network'
        ELSE 'NO'
    END AS detected,
    CONCAT(ROUND(DATA_SPEED/1024/1024, 2), ' MB/s') AS current_speed
FROM performance_schema.clone_progress
WHERE STATE = 'In Progress'

UNION ALL

SELECT 
    'Low Concurrency' AS issue,
    CASE 
        WHEN THREADS < 4 THEN 'YES - Only few workers active'
        ELSE 'NO'
    END AS detected,
    CAST(THREADS AS CHAR) AS current_speed
FROM performance_schema.clone_progress
WHERE STATE = 'In Progress';
```

MySQL Clone插件通过高效的并行传输、智能的带宽控制和完善的错误恢复机制，提供了可靠的数据克隆能力，是快速部署副本和灾难恢复的关键工具。
