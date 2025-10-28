# MySQL Group Replication(MGR)管理命令深度解析

## 概述

本文档详细解析MySQL Group Replication（MGR）的管理命令，从**源码层面**深入分析命令执行流程、架构设计和可观测性信息的采集与存储机制。MGR提供了多主和单主模式的分布式数据库复制方案。

**基于源码**: `plugin/group_replication/src/plugin.cc`, `plugin/group_replication/src/group_actions/`, `sql/sql_parse.cc`

---

## 第一部分：MGR管理命令总览

### 1.1 核心管理命令列表

```mermaid
graph TB
    subgraph "<b>MGR管理命令体系</b>"
        subgraph "<b>组启停命令</b>"
            START_GR["<b>START GROUP_REPLICATION</b><br/>启动组复制<br/>加入或创建组"]
            STOP_GR["<b>STOP GROUP_REPLICATION</b><br/>停止组复制<br/>离开组"]
        end
        
        subgraph "<b>成员配置命令</b>"
            CHANGE_MASTER_GR["<b>CHANGE REPLICATION SOURCE</b><br/>配置recovery channel<br/>用于分布式恢复"]
            SET_OPTION["<b>SET GLOBAL group_replication_*</b><br/>动态调整组复制参数"]
        end
        
        subgraph "<b>组操作命令</b>"
            PRIMARY_ELECTION["<b>group_replication_set_as_primary</b><br/>切换主节点<br/>单主模式"]
            SWITCH_MODE["<b>group_replication_switch_to_*_mode</b><br/>切换单主/多主模式"]
            GROUP_ACTION["<b>组协调操作UDF</b><br/>版本升级、配置变更"]
        end
        
        subgraph "<b>状态查询命令</b>"
            PS_TABLES["<b>Performance Schema表</b><br/>replication_group_members<br/>replication_group_member_stats"]
            SHOW_STATUS["<b>SHOW STATUS</b><br/>group_replication_*状态变量"]
        end
    end
    
    START_GR --> STOP_GR
    CHANGE_MASTER_GR --> SET_OPTION
    PRIMARY_ELECTION --> SWITCH_MODE
    SWITCH_MODE --> GROUP_ACTION
    START_GR --> PS_TABLES
    PS_TABLES --> SHOW_STATUS
    
    style START_GR fill:#e3f2fd,stroke:#333,stroke-width:2px
    style PRIMARY_ELECTION fill:#fff3e0,stroke:#333,stroke-width:2px
    style PS_TABLES fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**命令详细列表**:

| 命令类型 | 语法 | 权限 | 说明 |
|---------|------|------|------|
| `START GROUP_REPLICATION` | `START GROUP_REPLICATION [USER='user', PASSWORD='pwd']` | GROUP_REPLICATION_ADMIN | 启动组复制插件 |
| `STOP GROUP_REPLICATION` | `STOP GROUP_REPLICATION` | GROUP_REPLICATION_ADMIN | 停止组复制 |
| `group_replication_set_as_primary` | `SELECT group_replication_set_as_primary('uuid')` | GROUP_REPLICATION_ADMIN | 设置新主节点 |
| `group_replication_switch_to_single_primary_mode` | `SELECT group_replication_switch_to_single_primary_mode('uuid')` | GROUP_REPLICATION_ADMIN | 切换到单主模式 |
| `group_replication_switch_to_multi_primary_mode` | `SELECT group_replication_switch_to_multi_primary_mode()` | GROUP_REPLICATION_ADMIN | 切换到多主模式 |
| `SELECT * FROM replication_group_members` | Performance Schema查询 | SELECT | 查看组成员信息 |
| `SELECT * FROM replication_group_member_stats` | Performance Schema查询 | SELECT | 查看组成员统计 |

---

## 第二部分：START GROUP_REPLICATION 源码深度解析

### 2.1 START GROUP_REPLICATION 架构

**源码位置**: `plugin/group_replication/src/plugin.cc:564-745`

```mermaid
graph TB
    subgraph "<b>START GROUP_REPLICATION命令执行架构</b>"
        PARSE["<b>SQL解析层</b><br/>sql_parse.cc<br/>SQLCOM_START_GROUP_REPLICATION"]
        
        PLUGIN_ENTRY["<b>Plugin入口</b><br/>plugin_group_replication_start()<br/>权限检查和状态验证"]
        
        subgraph "<b>前置检查</b>"
            CHECK_CONFIG["<b>配置检查</b><br/>check_if_server_properly_configured()<br/>检查binlog、GTID等"]
            CHECK_NAME["<b>组名检查</b><br/>check_group_name_string()<br/>验证group_name有效性"]
            CHECK_SSL["<b>SSL检查</b><br/>check_recovery_ssl_string()<br/>验证SSL配置"]
        end
        
        subgraph "<b>核心启动流程</b>"
            INIT_COMPONENTS["<b>初始化组件</b><br/>plugin_group_replication_start_inner()<br/>GCS、事件处理器等"]
            INIT_GCS["<b>GCS初始化</b><br/>gcs_module->initialize()<br/>初始化通信层"]
            SET_READ_ONLY["<b>设置只读模式</b><br/>enable_super_read_only_mode()<br/>加入组前保护数据"]
            START_APPLIER["<b>启动Applier</b><br/>applier_module->setup_applier_module()<br/>启动SQL线程"]
            JOIN_GROUP["<b>加入组</b><br/>gcs_module->join()<br/>通过Paxos协议加入"]
        end
        
        RECOVERY["<b>分布式恢复</b><br/>recovery_module<br/>从组内成员恢复数据"]
    end
    
    PARSE --> PLUGIN_ENTRY
    PLUGIN_ENTRY --> CHECK_CONFIG
    PLUGIN_ENTRY --> CHECK_NAME
    PLUGIN_ENTRY --> CHECK_SSL
    CHECK_SSL --> INIT_COMPONENTS
    INIT_COMPONENTS --> INIT_GCS
    INIT_GCS --> SET_READ_ONLY
    SET_READ_ONLY --> START_APPLIER
    START_APPLIER --> JOIN_GROUP
    JOIN_GROUP --> RECOVERY
    
    style PARSE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style INIT_COMPONENTS fill:#fff3e0,stroke:#333,stroke-width:2px
    style JOIN_GROUP fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 2.2 START GROUP_REPLICATION 时序图

```mermaid
sequenceDiagram
    participant USER as **用户客户端**
    participant PLUGIN as **GR Plugin**
    participant GCS as **GCS通信层**
    participant APPLIER as **Applier模块**
    participant RECOVERY as **Recovery模块**
    participant GROUP as **现有组成员**

    Note over USER,GROUP: **START GROUP_REPLICATION执行流程**

    USER->>PLUGIN: START GROUP_REPLICATION
    
    Note over PLUGIN: **前置检查**
    PLUGIN->>PLUGIN: 检查是否已经在运行<br/>plugin_is_group_replication_running()
    PLUGIN->>PLUGIN: 检查服务器配置<br/>binlog_format=ROW, gtid_mode=ON
    PLUGIN->>PLUGIN: 检查组名有效性<br/>group_replication_group_name
    PLUGIN->>PLUGIN: 检查SSL配置<br/>recovery通道的SSL设置
    
    Note over PLUGIN,GCS: **初始化GCS通信层**
    PLUGIN->>GCS: gcs_module->initialize()
    GCS->>GCS: 初始化Paxos协议栈<br/>创建通信socket
    GCS-->>PLUGIN: 初始化完成
    
    Note over PLUGIN: **设置保护措施**
    PLUGIN->>PLUGIN: enable_super_read_only_mode()<br/>设置super_read_only=ON
    PLUGIN->>PLUGIN: 获取当前GTID状态<br/>gtid_executed
    
    Note over PLUGIN,APPLIER: **启动Applier线程**
    PLUGIN->>APPLIER: setup_applier_module()
    APPLIER->>APPLIER: 创建applier channel<br/>group_replication_applier
    APPLIER->>APPLIER: 启动SQL线程<br/>应用来自组的事务
    APPLIER-->>PLUGIN: Applier就绪
    
    Note over PLUGIN,GCS: **加入组（Paxos协议）**
    PLUGIN->>GCS: gcs_module->join(group_name)
    
    alt 组不存在（第一个成员）
        GCS->>GCS: 创建新组<br/>自己成为Primary
        GCS-->>PLUGIN: 组创建成功<br/>VIEW_CHANGED事件
        PLUGIN->>PLUGIN: 设置为ONLINE状态<br/>关闭super_read_only
    else 组已存在（加入现有组）
        GCS->>GROUP: 发送JOIN请求<br/>携带server_uuid和GTID
        GROUP->>GROUP: Paxos投票<br/>验证新成员合法性
        GROUP-->>GCS: 接受加入<br/>返回VIEW_CHANGED
        GCS-->>PLUGIN: 加入成功
        
        Note over PLUGIN,RECOVERY: **分布式恢复**
        PLUGIN->>RECOVERY: start_recovery()
        RECOVERY->>RECOVERY: 选择Donor成员<br/>根据GTID差异
        
        alt 使用Clone恢复（差异大）
            RECOVERY->>GROUP: CLONE INSTANCE FROM donor
            GROUP->>GROUP: 传输完整数据<br/>使用Clone插件
            GROUP-->>RECOVERY: 数据传输完成
            RECOVERY->>RECOVERY: 重启MySQL实例<br/>加载克隆的数据
        else 使用增量恢复（差异小）
            RECOVERY->>GROUP: 连接donor的binlog<br/>通过recovery channel
            GROUP->>GROUP: 传输缺失的事务<br/>标准复制协议
            GROUP-->>RECOVERY: 事务追赶完成
        end
        
        RECOVERY->>RECOVERY: 应用缓存的事务<br/>恢复期间组的新事务
        RECOVERY-->>PLUGIN: 恢复完成
        
        PLUGIN->>PLUGIN: 设置为ONLINE状态<br/>关闭super_read_only
        PLUGIN->>GCS: 通知组成员<br/>新成员已ONLINE
    end
    
    PLUGIN-->>USER: Query OK
```

### 2.3 START GROUP_REPLICATION 关键源码

**源码位置**: `plugin/group_replication/src/plugin.cc:564-640`

```cpp
int plugin_group_replication_start(char **error_message) {
  DBUG_TRACE;

  // 1. 检查插件是否正在卸载
  if (lv.plugin_is_being_uninstalled) {
    std::string err_msg("Group Replication plugin is being uninstalled.");
    *error_message = (char *)my_malloc(PSI_NOT_INSTRUMENTED, 
                                       err_msg.length() + 1, MYF(0));
    strcpy(*error_message, err_msg.c_str());
    return GROUP_REPLICATION_COMMAND_FAILURE;
  }

  // 2. 获取写锁
  Checkable_rwlock::Guard g(*lv.plugin_running_lock,
                            Checkable_rwlock::WRITE_LOCK);
  int error = 0;

  // 3. 检查是否已经在运行
  if (plugin_is_group_replication_running()) {
    error = GROUP_REPLICATION_ALREADY_RUNNING;
    goto err;
  }

  // 4. 检查服务器配置
  if (check_if_server_properly_configured()) {
    error = GROUP_REPLICATION_CONFIGURATION_ERROR;
    goto err;
  }

  // 5. 检查组名
  if (check_group_name_string(ov.group_name_var)) {
    error = GROUP_REPLICATION_CONFIGURATION_ERROR;
    goto err;
  }

  // 6. 检查SSL配置
  if (check_recovery_ssl_string(ov.recovery_ssl_ca_var, "ssl_ca") ||
      check_recovery_ssl_string(ov.recovery_ssl_cert_var, "ssl_cert") ||
      check_recovery_ssl_string(ov.recovery_ssl_key_var, "ssl_key")) {
    error = GROUP_REPLICATION_CONFIGURATION_ERROR;
    goto err;
  }

  // 7. 调用内部启动函数
  error = plugin_group_replication_start_inner(error_message, nullptr);

err:
  return error;
}
```

**内部启动函数** (`plugin.cc:745-1100`):

```cpp
static int plugin_group_replication_start_inner(
    char **error_message, 
    Delayed_initialization_thread *delayed_init_thd) {
  
  int error = 0;
  
  // 1. 建立SQL API连接
  Sql_service_command_interface sql_command_interface;
  if (sql_command_interface.establish_session_connection(
          sql_api_isolation, GROUPREPL_USER, lv.plugin_info_ptr)) {
    error = 1;
    goto err;
  }

  // 2. 初始化GCS模块
  if ((error = gcs_module->initialize())) {
    goto err;
  }

  // 3. 设置super_read_only模式
  bool read_only_mode = false, super_read_only_mode = false;
  get_read_mode_state(&read_only_mode, &super_read_only_mode);
  
  if (!super_read_only_mode) {
    if (enable_super_read_only_mode(&sql_command_interface)) {
      error = 1;
      goto err;
    }
  }

  // 4. 初始化各个模块
  if (init_group_sidno()) {
    error = 1;
    goto err;
  }

  // 5. 启动applier模块
  if (applier_module->setup_applier_module(
          CHANNEL_APPLIER_THREAD | CHANNEL_RECEIVER_THREAD,
          false, nullptr, nullptr, nullptr)) {
    error = 1;
    goto err;
  }

  // 6. 加入组
  view_change_notifier->start_view_modification();
  
  Gcs_operations::enum_gcs_error gcs_error = 
      gcs_module->join(*events_handler->get_notification_context());
      
  if (gcs_error != Gcs_operations::ERROR_OK) {
    error = 1;
    goto err;
  }

  // 7. 等待视图变更
  if (view_change_notifier->wait_for_view_modification()) {
    error = 1;
    goto err;
  }

  return 0;

err:
  // 清理和错误处理
  plugin_group_replication_leave_group();
  return error;
}
```

---

## 第三部分：STOP GROUP_REPLICATION 源码深度解析

### 3.1 STOP GROUP_REPLICATION 时序图

**源码位置**: `plugin/group_replication/src/plugin.cc:1274-1400`

```mermaid
sequenceDiagram
    participant USER as **用户客户端**
    participant PLUGIN as **GR Plugin**
    participant TRANS_MGR as **事务一致性管理器**
    participant APPLIER as **Applier模块**
    participant GCS as **GCS通信层**
    participant GROUP as **组成员**

    Note over USER,GROUP: **STOP GROUP_REPLICATION执行流程**

    USER->>PLUGIN: STOP GROUP_REPLICATION
    
    PLUGIN->>PLUGIN: 获取plugin_running_lock写锁
    
    Note over PLUGIN: **检查状态**
    PLUGIN->>PLUGIN: plugin_is_group_replication_running()?
    
    alt 未运行
        PLUGIN-->>USER: Query OK（直接返回）
    else 正在运行
        PLUGIN->>PLUGIN: plugin_is_stopping = true<br/>设置停止标志
        PLUGIN->>PLUGIN: 获取shared_plugin_stop_lock
        
        Note over PLUGIN,TRANS_MGR: **等待事务完成**
        PLUGIN->>TRANS_MGR: plugin_is_stopping()<br/>通知事务管理器
        TRANS_MGR->>TRANS_MGR: 阻止新事务开始认证
        
        PLUGIN->>PLUGIN: transactions_latch->block_until_empty()<br/>等待所有认证中的事务完成
        
        alt 等待超时
            PLUGIN->>PLUGIN: 强制kill认证中的事务<br/>TRANSACTION_KILL_TIMEOUT
        end
        
        Note over PLUGIN,GCS: **离开组**
        PLUGIN->>GCS: gcs_module->leave()<br/>发送LEAVE消息
        GCS->>GROUP: 广播LEAVE通知<br/>Paxos协议
        GROUP->>GROUP: 更新组视图<br/>移除该成员
        GROUP-->>GCS: 确认离开
        GCS-->>PLUGIN: 离开完成
        
        Note over PLUGIN,APPLIER: **停止Applier**
        PLUGIN->>APPLIER: applier_module->terminate_applier_pipeline()<br/>停止SQL线程
        APPLIER->>APPLIER: 完成队列中的事务<br/>graceful shutdown
        APPLIER->>APPLIER: 关闭applier channel
        APPLIER-->>PLUGIN: Applier已停止
        
        Note over PLUGIN: **清理资源**
        PLUGIN->>PLUGIN: terminate_recovery_module()<br/>停止recovery
        PLUGIN->>PLUGIN: gcs_module->finalize()<br/>清理GCS
        PLUGIN->>PLUGIN: 清理事件处理器<br/>释放内存
        
        PLUGIN->>PLUGIN: plugin_is_stopping = false<br/>plugin_is_group_replication_running = false
        
        PLUGIN-->>USER: Query OK
    end
```

### 3.2 优雅停止机制

```mermaid
graph TB
    subgraph "<b>STOP GROUP_REPLICATION优雅停止流程</b>"
        STOP_REQ["<b>停止请求</b><br/>plugin_group_replication_stop()"]
        
        BLOCK_NEW["<b>阻止新事务</b><br/>transaction_consistency_manager<br/>拒绝新的认证请求"]
        
        subgraph "<b>等待事务完成</b>"
            WAIT_CERT["<b>等待认证队列</b><br/>transactions_latch<br/>最多60秒"]
            WAIT_APPLY["<b>等待应用队列</b><br/>certification_latch<br/>等待已认证事务应用"]
        end
        
        LEAVE_GROUP["<b>离开组</b><br/>gcs_module->leave()<br/>通过Paxos通知其他成员"]
        
        STOP_APPLIER["<b>停止Applier</b><br/>terminate_applier_pipeline()<br/>优雅关闭SQL线程"]
        
        CLEANUP["<b>清理资源</b><br/>释放GCS、Recovery等模块"]
        
        FINAL["<b>完成</b><br/>plugin_is_group_replication_running = false"]
    end
    
    STOP_REQ --> BLOCK_NEW
    BLOCK_NEW --> WAIT_CERT
    WAIT_CERT --> WAIT_APPLY
    WAIT_APPLY --> LEAVE_GROUP
    LEAVE_GROUP --> STOP_APPLIER
    STOP_APPLIER --> CLEANUP
    CLEANUP --> FINAL
    
    style STOP_REQ fill:#e3f2fd,stroke:#333,stroke-width:2px
    style WAIT_CERT fill:#fff3e0,stroke:#333,stroke-width:2px
    style LEAVE_GROUP fill:#e8f5e8,stroke:#333,stroke-width:2px
```

---

## 第四部分：组协调操作（Group Actions）

### 4.1 Primary选举架构

**源码位置**: `plugin/group_replication/src/group_actions/`

```mermaid
graph TB
    subgraph "<b>Primary选举和模式切换架构</b>"
        UDF_ENTRY["<b>UDF入口</b><br/>group_replication_set_as_primary<br/>group_replication_switch_to_*_mode"]
        
        ACTION_COORDINATOR["<b>动作协调器</b><br/>Group_action_coordinator<br/>管理组协调操作"]
        
        subgraph "<b>Primary选举流程</b>"
            VALIDATE["<b>验证阶段</b><br/>检查目标成员是否在线<br/>版本兼容性检查"]
            BROADCAST["<b>广播阶段</b><br/>通过GCS广播到所有成员<br/>Paxos保证一致性"]
            EXECUTE["<b>执行阶段</b><br/>所有成员执行操作<br/>同步切换"]
            COMMIT["<b>提交阶段</b><br/>确认操作完成<br/>更新组元数据"]
        end
        
        PRIMARY_ELECTION_ACTION["<b>Primary选举动作</b><br/>Primary_election_action<br/>具体的选举逻辑"]
        
        MODE_SWITCH_ACTION["<b>模式切换动作</b><br/>Switch_primary_action<br/>单主/多主切换"]
    end
    
    UDF_ENTRY --> ACTION_COORDINATOR
    ACTION_COORDINATOR --> VALIDATE
    VALIDATE --> BROADCAST
    BROADCAST --> EXECUTE
    EXECUTE --> COMMIT
    ACTION_COORDINATOR --> PRIMARY_ELECTION_ACTION
    ACTION_COORDINATOR --> MODE_SWITCH_ACTION
    
    style UDF_ENTRY fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BROADCAST fill:#fff3e0,stroke:#333,stroke-width:2px
    style EXECUTE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 4.2 Primary选举时序图

```mermaid
sequenceDiagram
    participant USER as **用户（任意成员）**
    participant INITIATOR as **发起者成员**
    participant COORDINATOR as **动作协调器**
    participant GCS as **GCS通信层**
    participant ALL_MEMBERS as **所有组成员**
    participant NEW_PRIMARY as **新Primary成员**

    Note over USER,NEW_PRIMARY: **Primary选举流程**

    USER->>INITIATOR: SELECT group_replication_set_as_primary('new_uuid')
    
    INITIATOR->>COORDINATOR: handle_action_message()<br/>创建Primary_election_action
    
    Note over COORDINATOR: **验证阶段**
    COORDINATOR->>COORDINATOR: 检查new_uuid是否在组内<br/>检查是否ONLINE状态
    COORDINATOR->>COORDINATOR: 检查版本兼容性<br/>所有成员>=8.0.13
    
    alt 验证失败
        COORDINATOR-->>USER: ERROR: Member not found or incompatible
    else 验证成功
        Note over COORDINATOR,GCS: **广播阶段**
        COORDINATOR->>GCS: send_action_message()<br/>发送PRIMARY_ELECTION消息
        GCS->>ALL_MEMBERS: 广播消息<br/>通过Paxos保证顺序
        
        Note over ALL_MEMBERS: **所有成员接收消息**
        ALL_MEMBERS->>ALL_MEMBERS: 解析action消息<br/>创建本地action实例
        
        Note over ALL_MEMBERS: **执行阶段**
        loop 所有成员并行执行
            ALL_MEMBERS->>ALL_MEMBERS: 阻止新事务<br/>设置super_read_only
            ALL_MEMBERS->>ALL_MEMBERS: 等待队列清空<br/>certification_latch
            ALL_MEMBERS->>ALL_MEMBERS: 同步等待<br/>确保所有成员到达此点
        end
        
        Note over NEW_PRIMARY: **新Primary准备**
        NEW_PRIMARY->>NEW_PRIMARY: 禁用super_read_only<br/>允许写入
        NEW_PRIMARY->>NEW_PRIMARY: 设置group_replication_primary_member<br/>更新为自己的UUID
        
        Note over ALL_MEMBERS: **其他成员配置**
        ALL_MEMBERS->>ALL_MEMBERS: 保持super_read_only<br/>只读模式
        ALL_MEMBERS->>ALL_MEMBERS: 更新group_replication_primary_member<br/>指向新Primary的UUID
        
        Note over COORDINATOR: **提交阶段**
        ALL_MEMBERS->>COORDINATOR: 发送执行结果<br/>SUCCESS或FAILURE
        COORDINATOR->>COORDINATOR: 收集所有成员的结果<br/>判断是否全部成功
        
        alt 全部成功
            COORDINATOR->>GCS: 广播COMMIT消息
            GCS->>ALL_MEMBERS: Primary选举完成
            ALL_MEMBERS->>ALL_MEMBERS: 恢复正常事务处理
            COORDINATOR-->>USER: Query OK<br/>New primary: new_uuid
        else 部分失败
            COORDINATOR->>GCS: 广播ROLLBACK消息
            GCS->>ALL_MEMBERS: 回滚操作
            ALL_MEMBERS->>ALL_MEMBERS: 恢复原primary配置
            COORDINATOR-->>USER: ERROR: Primary election failed
        end
    end
```

---

## 第五部分：可观测性信息深度解析

### 5.1 可观测性架构

```mermaid
graph TB
    subgraph "<b>MGR可观测性信息流转架构</b>"
        subgraph "<b>运行时数据采集</b>"
            MEMBER_STATE["<b>成员状态</b><br/>• ONLINE, RECOVERING<br/>• OFFLINE, ERROR"]
            
            MEMBER_ROLE["<b>成员角色</b><br/>• PRIMARY<br/>• SECONDARY"]
            
            TRANS_METRICS["<b>事务指标</b><br/>• 认证队列长度<br/>• 冲突事务数<br/>• 应用延迟"]
            
            NETWORK_METRICS["<b>网络指标</b><br/>• 消息发送/接收<br/>• 带宽使用"]
        end
        
        subgraph "<b>Performance Schema表</b>"
            GROUP_MEMBERS["<b>replication_group_members</b><br/>成员列表、状态、角色"]
            
            MEMBER_STATS["<b>replication_group_member_stats</b><br/>事务统计、队列、冲突"]
            
            CONNECTION_STATUS["<b>replication_connection_status</b><br/>recovery连接状态"]
            
            APPLIER_STATUS["<b>replication_applier_status_by_worker</b><br/>applier worker状态"]
        end
        
        subgraph "<b>状态变量</b>"
            STATUS_VARS["<b>SHOW STATUS LIKE 'group_replication%'</b><br/>• primary_member<br/>• group_name<br/>• communication_protocol_version"]
        end
        
        subgraph "<b>系统变量</b>"
            SYSTEM_VARS["<b>SHOW VARIABLES LIKE 'group_replication%'</b><br/>• single_primary_mode<br/>• auto_increment_increment<br/>• member_weight"]
        end
    end
    
    MEMBER_STATE --> GROUP_MEMBERS
    MEMBER_ROLE --> GROUP_MEMBERS
    TRANS_METRICS --> MEMBER_STATS
    NETWORK_METRICS --> MEMBER_STATS
    
    GROUP_MEMBERS --> STATUS_VARS
    MEMBER_STATS --> STATUS_VARS
    CONNECTION_STATUS --> STATUS_VARS
    APPLIER_STATUS --> SYSTEM_VARS
    
    style MEMBER_STATE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style GROUP_MEMBERS fill:#fff3e0,stroke:#333,stroke-width:2px
    style STATUS_VARS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 5.2 Performance Schema表详解

**replication_group_members表结构**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `CHANNEL_NAME` | VARCHAR | 通道名称（group_replication_applier） |
| `MEMBER_ID` | VARCHAR | 成员UUID（server_uuid） |
| `MEMBER_HOST` | VARCHAR | 成员主机名 |
| `MEMBER_PORT` | INT | 成员端口号 |
| `MEMBER_STATE` | VARCHAR | 成员状态: ONLINE, RECOVERING, OFFLINE, ERROR, UNREACHABLE |
| `MEMBER_ROLE` | VARCHAR | 成员角色: PRIMARY, SECONDARY |
| `MEMBER_VERSION` | VARCHAR | MySQL版本 |
| `MEMBER_COMMUNICATION_STACK` | VARCHAR | 通信协议栈: XCom或MySQL |

**replication_group_member_stats表结构**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `CHANNEL_NAME` | VARCHAR | 通道名称 |
| `VIEW_ID` | VARCHAR | 当前视图ID |
| `MEMBER_ID` | VARCHAR | 成员UUID |
| `COUNT_TRANSACTIONS_IN_QUEUE` | BIGINT | 认证队列中的事务数 |
| `COUNT_TRANSACTIONS_CHECKED` | BIGINT | 已检查的事务数 |
| `COUNT_CONFLICTS_DETECTED` | BIGINT | 检测到的冲突事务数 |
| `COUNT_TRANSACTIONS_ROWS_VALIDATING` | BIGINT | 正在验证行的事务数 |
| `TRANSACTIONS_COMMITTED_ALL_MEMBERS` | LONGTEXT | 所有成员都已提交的GTID集合 |
| `LAST_CONFLICT_FREE_TRANSACTION` | VARCHAR | 最后一个无冲突事务的GTID |
| `COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE` | BIGINT | applier队列中的远程事务数 |
| `COUNT_TRANSACTIONS_REMOTE_APPLIED` | BIGINT | 已应用的远程事务数 |
| `COUNT_TRANSACTIONS_LOCAL_PROPOSED` | BIGINT | 本地提出的事务数 |
| `COUNT_TRANSACTIONS_LOCAL_ROLLBACK` | BIGINT | 本地回滚的事务数 |

### 5.3 数据采集与更新流程

```mermaid
sequenceDiagram
    participant GR_PLUGIN as **GR Plugin**
    participant CERTIFIER as **Certifier模块**
    participant APPLIER as **Applier模块**
    participant PFS as **Performance Schema**
    participant USER as **用户查询**

    Note over GR_PLUGIN,USER: **MGR可观测性数据流转**

    loop 事务处理循环
        Note over CERTIFIER: **认证统计**
        CERTIFIER->>CERTIFIER: 接收事务进行认证<br/>检测写集合冲突
        
        alt 无冲突
            CERTIFIER->>CERTIFIER: COUNT_TRANSACTIONS_CHECKED++<br/>COUNT_TRANSACTIONS_REMOTE_APPLIED++
        else 有冲突
            CERTIFIER->>CERTIFIER: COUNT_CONFLICTS_DETECTED++<br/>COUNT_TRANSACTIONS_LOCAL_ROLLBACK++
        end
        
        CERTIFIER->>CERTIFIER: 更新TRANSACTIONS_COMMITTED_ALL_MEMBERS<br/>GTID集合
        
        Note over APPLIER: **应用统计**
        APPLIER->>APPLIER: 从队列获取已认证事务
        APPLIER->>APPLIER: COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE--
        APPLIER->>APPLIER: 应用事务到本地<br/>更新innodb表
        APPLIER->>APPLIER: COUNT_TRANSACTIONS_REMOTE_APPLIED++
    end
    
    Note over GR_PLUGIN,PFS: **状态更新（定期或事件驱动）**
    
    alt 视图变更（成员加入/离开）
        GR_PLUGIN->>PFS: 更新replication_group_members<br/>MEMBER_STATE, MEMBER_ROLE
    end
    
    alt Primary选举完成
        GR_PLUGIN->>PFS: 更新MEMBER_ROLE<br/>PRIMARY或SECONDARY
    end
    
    loop 定期更新（每秒）
        CERTIFIER->>PFS: 更新replication_group_member_stats<br/>事务计数器
        APPLIER->>PFS: 更新队列长度<br/>应用统计
    end
    
    Note over USER: **用户查询**
    USER->>PFS: SELECT * FROM replication_group_members
    PFS-->>USER: 返回当前成员状态
    
    USER->>PFS: SELECT * FROM replication_group_member_stats
    PFS-->>USER: 返回事务统计信息
```

### 5.4 监控SQL示例

**查看组成员状态**:

```sql
-- 查看所有组成员及其状态
SELECT 
    MEMBER_ID,
    MEMBER_HOST,
    MEMBER_PORT,
    MEMBER_STATE,
    MEMBER_ROLE,
    MEMBER_VERSION,
    IF(MEMBER_STATE = 'ONLINE', 
       'HEALTHY', 
       CONCAT('ISSUE: ', MEMBER_STATE)) AS health_status
FROM performance_schema.replication_group_members
ORDER BY MEMBER_ROLE DESC, MEMBER_HOST;
```

**查看Primary成员**:

```sql
-- 查看当前Primary成员
SELECT 
    MEMBER_ID,
    MEMBER_HOST,
    MEMBER_PORT,
    MEMBER_VERSION
FROM performance_schema.replication_group_members
WHERE MEMBER_ROLE = 'PRIMARY';

-- 或使用状态变量
SHOW STATUS LIKE 'group_replication_primary_member';
```

**查看事务冲突统计**:

```sql
-- 查看每个成员的事务冲突情况
SELECT 
    m.MEMBER_HOST,
    m.MEMBER_PORT,
    m.MEMBER_ROLE,
    s.COUNT_TRANSACTIONS_CHECKED AS total_checked,
    s.COUNT_CONFLICTS_DETECTED AS conflicts,
    ROUND(s.COUNT_CONFLICTS_DETECTED * 100.0 / 
          NULLIF(s.COUNT_TRANSACTIONS_CHECKED, 0), 2) AS conflict_rate_pct,
    s.COUNT_TRANSACTIONS_LOCAL_ROLLBACK AS local_rollbacks
FROM performance_schema.replication_group_members m
JOIN performance_schema.replication_group_member_stats s
  ON m.MEMBER_ID = s.MEMBER_ID
WHERE m.MEMBER_STATE = 'ONLINE';
```

**查看认证队列和应用延迟**:

```sql
-- 查看认证队列和applier队列长度
SELECT 
    m.MEMBER_HOST,
    m.MEMBER_PORT,
    m.MEMBER_ROLE,
    s.COUNT_TRANSACTIONS_IN_QUEUE AS cert_queue_size,
    s.COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE AS applier_queue_size,
    s.COUNT_TRANSACTIONS_ROWS_VALIDATING AS validating_transactions
FROM performance_schema.replication_group_members m
JOIN performance_schema.replication_group_member_stats s
  ON m.MEMBER_ID = s.MEMBER_ID
WHERE m.MEMBER_STATE = 'ONLINE'
ORDER BY cert_queue_size DESC;
```

**查看已提交GTID一致性**:

```sql
-- 查看所有成员都已提交的GTID集合
SELECT 
    MEMBER_HOST,
    MEMBER_PORT,
    TRANSACTIONS_COMMITTED_ALL_MEMBERS,
    LAST_CONFLICT_FREE_TRANSACTION
FROM performance_schema.replication_group_member_stats s
JOIN performance_schema.replication_group_members m
  ON s.MEMBER_ID = m.MEMBER_ID
WHERE m.MEMBER_STATE = 'ONLINE';
```

---

## 第六部分：配置与优化

### 6.1 核心系统变量详解

| 变量名 | 默认值 | 说明 | 优化建议 |
|-------|-------|------|---------|
| `group_replication_group_name` | NULL | 组UUID | 必须配置，全组统一 |
| `group_replication_single_primary_mode` | ON | 单主模式 | 简化写入逻辑，推荐 |
| `group_replication_auto_increment_increment` | 7 | 自增步长 | 多主模式避免ID冲突 |
| `group_replication_bootstrap_group` | OFF | 引导组 | 仅第一个成员启动时设置 |
| `group_replication_flow_control_mode` | QUOTA | 流控模式 | QUOTA性能更好 |
| `group_replication_member_weight` | 50 | 成员权重 | Primary选举时的优先级 |
| `group_replication_consistency` | EVENTUAL | 一致性级别 | BEFORE/AFTER提高一致性 |
| `group_replication_exit_state_action` | READ_ONLY | 退出动作 | ABORT_SERVER用于严格场景 |
| `group_replication_unreachable_majority_timeout` | 0 | 少数派超时 | 网络分区处理策略 |
| `group_replication_communication_max_message_size` | 10MB | 最大消息大小 | 大事务需增大 |

### 6.2 性能优化策略

```mermaid
graph TB
    subgraph "<b>MGR性能优化策略</b>"
        subgraph "<b>写入优化</b>"
            SINGLE_PRIMARY["<b>单主模式</b><br/>避免多主冲突<br/>减少回滚"]
            BATCH_WRITE["<b>批量写入</b><br/>减少认证次数<br/>提高吞吐"]
            LARGE_TRANS["<b>避免大事务</b><br/>拆分为小事务<br/>减少写集合"]
        end
        
        subgraph "<b>网络优化</b>"
            MSG_SIZE["<b>消息大小</b><br/>communication_max_message_size<br/>大事务场景增大"]
            COMPRESSION["<b>压缩传输</b><br/>group_replication_compression_threshold<br/>启用压缩"]
            NETWORK_BANDWIDTH["<b>网络带宽</b><br/>确保低延迟高带宽<br/>避免成为瓶颈"]
        end
        
        subgraph "<b>一致性调优</b>"
            CONSISTENCY["<b>一致性级别</b><br/>EVENTUAL: 性能最好<br/>BEFORE_ON_PRIMARY_FAILOVER: 平衡"]
            FLOW_CONTROL["<b>流控策略</b><br/>QUOTA模式<br/>自动限流"]
        end
        
        subgraph "<b>硬件优化</b>"
            FAST_DISK["<b>快速磁盘</b><br/>SSD或NVMe<br/>降低认证延迟"]
            MULTI_CORE["<b>多核CPU</b><br/>并行处理事务<br/>提高认证效率"]
        end
    end
    
    SINGLE_PRIMARY --> BATCH_WRITE
    BATCH_WRITE --> LARGE_TRANS
    MSG_SIZE --> COMPRESSION
    COMPRESSION --> NETWORK_BANDWIDTH
    CONSISTENCY --> FLOW_CONTROL
    FAST_DISK --> MULTI_CORE
    
    style SINGLE_PRIMARY fill:#e3f2fd,stroke:#333,stroke-width:2px
    style MSG_SIZE fill:#fff3e0,stroke:#333,stroke-width:2px
    style FAST_DISK fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 6.3 高可用配置

**推荐配置（3节点单主模式）**:

```sql
-- 节点1（Primary候选）
SET GLOBAL group_replication_group_name = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee';
SET GLOBAL group_replication_local_address = '192.168.1.1:33061';
SET GLOBAL group_replication_group_seeds = '192.168.1.1:33061,192.168.1.2:33061,192.168.1.3:33061';
SET GLOBAL group_replication_single_primary_mode = ON;
SET GLOBAL group_replication_enforce_update_everywhere_checks = OFF;
SET GLOBAL group_replication_member_weight = 80;  -- 高权重，优先成为Primary
SET GLOBAL group_replication_consistency = 'BEFORE_ON_PRIMARY_FAILOVER';
SET GLOBAL group_replication_exit_state_action = 'READ_ONLY';
SET GLOBAL group_replication_bootstrap_group = ON;  -- 仅第一次启动
START GROUP_REPLICATION;
SET GLOBAL group_replication_bootstrap_group = OFF;

-- 节点2和节点3（Secondary）
SET GLOBAL group_replication_group_name = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee';
SET GLOBAL group_replication_local_address = '192.168.1.2:33061';  -- 节点2地址
SET GLOBAL group_replication_group_seeds = '192.168.1.1:33061,192.168.1.2:33061,192.168.1.3:33061';
SET GLOBAL group_replication_single_primary_mode = ON;
SET GLOBAL group_replication_enforce_update_everywhere_checks = OFF;
SET GLOBAL group_replication_member_weight = 50;  -- 正常权重
SET GLOBAL group_replication_consistency = 'BEFORE_ON_PRIMARY_FAILOVER';
SET GLOBAL group_replication_exit_state_action = 'READ_ONLY';
START GROUP_REPLICATION;
```

---

## 第七部分：故障排查

### 7.1 常见问题诊断流程

```mermaid
graph TB
    subgraph "<b>MGR故障诊断流程</b>"
        ISSUE["<b>组复制问题</b><br/>成员无法加入、Primary选举失败等"]
        
        CHECK_STATUS["<b>检查成员状态</b><br/>SELECT * FROM replication_group_members<br/>查看MEMBER_STATE"]
        
        subgraph "<b>问题分类</b>"
            RECOVERING["<b>RECOVERING状态</b><br/>分布式恢复未完成<br/>GTID差异大"]
            
            OFFLINE["<b>OFFLINE或ERROR</b><br/>启动失败<br/>配置错误"]
            
            UNREACHABLE["<b>UNREACHABLE状态</b><br/>网络分区<br/>心跳超时"]
            
            CONFLICT["<b>事务冲突率高</b><br/>多主模式冲突<br/>写集合重叠"]
        end
        
        subgraph "<b>解决方案</b>"
            FIX_RECOVERY["<b>修复恢复</b><br/>• 检查donor成员<br/>• 使用Clone加速<br/>• 检查recovery channel"]
            
            FIX_CONFIG["<b>修复配置</b><br/>• 检查group_name<br/>• 检查binlog_format<br/>• 检查gtid_mode"]
            
            FIX_NETWORK["<b>修复网络</b><br/>• 检查防火墙<br/>• 增大timeout<br/>• 修复网络分区"]
            
            FIX_CONFLICT["<b>减少冲突</b><br/>• 切换到单主模式<br/>• 分散写入<br/>• 使用分区表"]
        end
    end
    
    ISSUE --> CHECK_STATUS
    CHECK_STATUS --> RECOVERING
    CHECK_STATUS --> OFFLINE
    CHECK_STATUS --> UNREACHABLE
    CHECK_STATUS --> CONFLICT
    
    RECOVERING --> FIX_RECOVERY
    OFFLINE --> FIX_CONFIG
    UNREACHABLE --> FIX_NETWORK
    CONFLICT --> FIX_CONFLICT
    
    style ISSUE fill:#ffebee,stroke:#333,stroke-width:2px
    style CHECK_STATUS fill:#e3f2fd,stroke:#333,stroke-width:2px
    style FIX_RECOVERY fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 7.2 常见错误及解决方案

| 错误信息 | 原因 | 解决方案 |
|---------|------|---------|
| `Member configuration is not compatible with the group` | 配置不兼容 | 检查gtid_mode、binlog_format等配置 |
| `The member is already leaving or joining a group` | 操作冲突 | 等待当前操作完成 |
| `Timeout on wait for view after joining group` | 加入组超时 | 检查网络，增大group_replication_components_stop_timeout |
| `There is already a member with server_uuid` | UUID冲突 | 修改server_uuid，确保唯一性 |
| `This member has more executed transactions than those present in the group` | GTID超前 | 使用group_replication_allow_local_lower_version_join |
| `Certification database is not installed` | 认证数据库未初始化 | 检查mysql.gtid_executed表 |
| `Error on group communication engine start` | GCS初始化失败 | 检查group_replication_local_address和防火墙 |

---

## 总结

### 核心要点回顾

**命令管理**:

- **START GROUP_REPLICATION**: 启动组复制，加入或创建组，自动分布式恢复
- **STOP GROUP_REPLICATION**: 优雅停止，等待事务完成，通知组成员离开
- **Primary选举UDF**: group_replication_set_as_primary()，所有成员协同切换
- **模式切换UDF**: 单主/多主模式动态切换

**架构特点**:

- **Paxos协议**: GCS通信层基于Paxos，保证组成员视图一致性
- **分布式恢复**: 自动选择Clone或增量复制，快速追赶数据
- **认证机制**: 基于写集合的冲突检测，乐观并发控制
- **流控机制**: QUOTA模式自动限流，避免慢节点拖累整组

**可观测性**:

- **Performance Schema表**: replication_group_members、replication_group_member_stats
- **事务统计**: 认证队列、冲突检测、应用延迟
- **成员状态**: ONLINE、RECOVERING、OFFLINE、ERROR、UNREACHABLE

**最佳实践**:

- 使用单主模式（simple_primary_mode=ON）避免冲突
- 配置合理的member_weight，控制Primary选举
- 设置group_replication_consistency提高数据一致性
- 监控COUNT_CONFLICTS_DETECTED，及时发现冲突问题
- 使用group_replication_exit_state_action控制故障行为

MySQL Group Replication提供了高可用、自动故障转移的分布式复制方案，是构建弹性数据库架构的核心组件。

---

## 第八部分：MGR GCS通信协议深度解析

### 8.1 GCS (Group Communication System) 协议栈

**源码位置**: `plugin/group_replication/libmysqlgcs/`

```mermaid
graph TB
    subgraph "<b>MGR GCS协议栈架构</b>"
        subgraph "<b>应用层</b>"
            GR_PLUGIN["<b>GR Plugin</b><br/>group_replication插件<br/>业务逻辑"]
        end
        
        subgraph "<b>GCS抽象层</b>"
            GCS_INTERFACE["<b>GCS Interface</b><br/>统一的通信接口<br/>gcs_interface.h"]
        end
        
        subgraph "<b>GCS实现层</b>"
            XCOM_ENGINE["<b>XCom Engine</b><br/>Paxos实现<br/>共识算法"]
            MYSQL_GCS["<b>MySQL GCS</b><br/>MySQL 8.0.27+<br/>新通信栈"]
        end
        
        subgraph "<b>传输层</b>"
            TCP_LAYER["<b>TCP/IP</b><br/>可靠传输<br/>group_replication_local_address"]
            SSL_LAYER["<b>SSL/TLS</b><br/>加密通信<br/>group_replication_ssl_mode"]
        end
    end
    
    GR_PLUGIN --> GCS_INTERFACE
    GCS_INTERFACE --> XCOM_ENGINE
    GCS_INTERFACE --> MYSQL_GCS
    XCOM_ENGINE --> TCP_LAYER
    MYSQL_GCS --> TCP_LAYER
    TCP_LAYER --> SSL_LAYER
    
    style GR_PLUGIN fill:#e3f2fd,stroke:#333,stroke-width:2px
    style XCOM_ENGINE fill:#fff3e0,stroke:#333,stroke-width:2px
    style TCP_LAYER fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 8.2 GCS消息类型

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/gcs_xcom_communication_interface.cc`

```mermaid
graph LR
    subgraph "<b>GCS消息分类</b>"
        subgraph "<b>控制消息</b>"
            VIEW_CHANGE["<b>VIEW_CHANGE</b><br/>成员变更通知<br/>加入/离开/失败"]
            SUSPECT["<b>SUSPECT</b><br/>怀疑成员失败<br/>触发故障检测"]
        end
        
        subgraph "<b>数据消息</b>"
            DATA_MSG["<b>DATA</b><br/>事务数据<br/>certification消息"]
            STATS_MSG["<b>STATS</b><br/>统计信息<br/>性能指标"]
        end
        
        subgraph "<b>配置消息</b>"
            CONFIG_CHANGE["<b>CONFIG_CHANGE</b><br/>配置变更<br/>参数更新"]
            GROUP_ACTION["<b>GROUP_ACTION</b><br/>组操作<br/>Primary选举等"]
        end
    end
    
    VIEW_CHANGE --> DATA_MSG
    SUSPECT --> VIEW_CHANGE
    CONFIG_CHANGE --> GROUP_ACTION
    
    style VIEW_CHANGE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style DATA_MSG fill:#fff3e0,stroke:#333,stroke-width:2px
    style GROUP_ACTION fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 8.3 成员间Paxos协议交互

```mermaid
sequenceDiagram
    participant M1 as **成员1 (提议者)**
    participant M2 as **成员2**
    participant M3 as **成员3**
    participant XCOM1 as **XCom1 (Paxos)**
    participant XCOM2 as **XCom2**
    participant XCOM3 as **XCom3**

    Note over M1,XCOM3: **Paxos三阶段提交协议**

    Note over M1: **阶段1: Prepare**
    
    M1->>XCOM1: 提交事务<br/>GTID: uuid:100
    XCOM1->>XCOM1: 生成Proposal ID<br/>ballot = (term, node_id)
    
    XCOM1->>XCOM2: PREPARE<br/>• ballot = (5, 1)<br/>• proposal_id = 1001
    XCOM1->>XCOM3: PREPARE<br/>• ballot = (5, 1)<br/>• proposal_id = 1001
    
    XCOM2->>XCOM2: 检查ballot<br/>ballot > last_accepted_ballot?
    XCOM2-->>XCOM1: PROMISE<br/>• 承诺不接受更小的ballot<br/>• 返回已接受的值（如果有）
    
    XCOM3->>XCOM3: 检查ballot
    XCOM3-->>XCOM1: PROMISE
    
    Note over XCOM1: **达到多数派（2/3）**
    
    Note over M1: **阶段2: Accept**
    
    XCOM1->>XCOM1: 选择要提交的值<br/>• 如果有已接受的值，使用它<br/>• 否则使用自己的提议
    
    XCOM1->>XCOM2: ACCEPT<br/>• ballot = (5, 1)<br/>• value = transaction_data
    XCOM1->>XCOM3: ACCEPT<br/>• ballot = (5, 1)<br/>• value = transaction_data
    
    XCOM2->>XCOM2: 检查ballot<br/>ballot >= promised_ballot?
    XCOM2->>XCOM2: 接受并持久化<br/>accepted_ballot = (5, 1)<br/>accepted_value = transaction_data
    XCOM2-->>XCOM1: ACCEPTED
    
    XCOM3->>XCOM3: 接受并持久化
    XCOM3-->>XCOM1: ACCEPTED
    
    Note over XCOM1: **达到多数派（2/3）**
    
    Note over M1: **阶段3: Learn (Commit)**
    
    XCOM1->>XCOM1: 值已被chosen<br/>可以提交
    
    XCOM1->>M1: 广播LEARN<br/>• chosen_value<br/>• 所有成员apply
    XCOM1->>XCOM2: LEARN
    XCOM1->>XCOM3: LEARN
    
    M1->>M1: apply_transaction()<br/>执行事务
    M2->>M2: apply_transaction()
    M3->>M3: apply_transaction()
    
    Note over M1,M3: **所有成员达成一致**
```

### 8.4 成员加入协议

```mermaid
sequenceDiagram
    participant NEW as **新成员**
    participant SEED as **Seed成员**
    participant GROUP as **现有组成员**
    participant XCOM_NEW as **XCom (新成员)**
    participant XCOM_GROUP as **XCom (组)**

    Note over NEW,XCOM_GROUP: **成员加入完整流程**

    NEW->>NEW: START GROUP_REPLICATION
    
    NEW->>NEW: 初始化GCS<br/>gcs_module->initialize()
    NEW->>NEW: 配置group_replication_group_seeds<br/>'seed1:33061,seed2:33061'
    
    Note over NEW: **步骤1: 连接Seed节点**
    
    NEW->>SEED: TCP连接<br/>group_replication_local_address
    SEED-->>NEW: 连接建立
    
    NEW->>SEED: SSL握手 (如果启用)<br/>group_replication_ssl_mode
    SEED-->>NEW: SSL建立
    
    Note over NEW: **步骤2: 发送JOIN请求**
    
    NEW->>XCOM_NEW: gcs_module->join(group_name)
    XCOM_NEW->>XCOM_NEW: 创建JOIN消息<br/>• server_uuid<br/>• gtid_executed<br/>• member_version
    
    XCOM_NEW->>XCOM_GROUP: JOIN_REQUEST<br/>• group_name = 'aaaaa-bbbb-...'<br/>• joining_member_uuid<br/>• joining_member_host:port
    
    Note over XCOM_GROUP: **步骤3: 组验证**
    
    XCOM_GROUP->>XCOM_GROUP: 验证加入条件<br/>• group_name匹配<br/>• server_uuid唯一<br/>• 版本兼容
    
    alt 验证失败
        XCOM_GROUP-->>XCOM_NEW: JOIN_REJECT<br/>• 原因: 版本不兼容/UUID重复
        NEW->>NEW: 加入失败<br/>报告错误
    else 验证通过
        Note over XCOM_GROUP: **步骤4: Paxos投票**
        
        XCOM_GROUP->>XCOM_GROUP: 提议VIEW_CHANGE<br/>添加新成员
        
        loop Paxos协议
            XCOM_GROUP->>XCOM_GROUP: PREPARE阶段
            XCOM_GROUP->>XCOM_GROUP: ACCEPT阶段
            XCOM_GROUP->>XCOM_GROUP: LEARN阶段
        end
        
        XCOM_GROUP->>XCOM_GROUP: 达成共识<br/>新成员加入
        
        Note over XCOM_GROUP: **步骤5: VIEW_CHANGE广播**
        
        XCOM_GROUP->>NEW: VIEW_CHANGE消息<br/>• view_id = 'view_15'<br/>• members = [M1, M2, M3, NEW]<br/>• joined = [NEW]
        XCOM_GROUP->>GROUP: VIEW_CHANGE消息<br/>通知所有成员
        
        NEW->>NEW: 处理VIEW_CHANGE<br/>on_view_changed()
        NEW->>NEW: 设置为RECOVERING状态
        
        GROUP->>GROUP: 更新成员列表<br/>replication_group_members
    end
    
    Note over NEW: **步骤6: 分布式恢复**
    
    NEW->>NEW: start_recovery()<br/>选择Donor
    NEW->>GROUP: 建立recovery channel<br/>标准MySQL复制协议
    GROUP->>NEW: 传输缺失的事务<br/>binlog events
    
    NEW->>NEW: 追赶完成<br/>gtid_executed = 组的gtid
    NEW->>NEW: 设置为ONLINE状态
    
    NEW->>XCOM_GROUP: 广播状态更新<br/>STATE_EXCHANGE
    XCOM_GROUP->>GROUP: 通知所有成员<br/>NEW is ONLINE
```

---

## 第九部分：MGR参数完整详解

### 9.1 组配置参数

| 参数 | 默认值 | 范围 | 说明 | 源码位置 |
|------|-------|------|------|---------|
| `group_replication_group_name` | NULL | UUID格式 | 组的唯一标识符 | 必须在所有成员上一致 |
| `group_replication_local_address` | NULL | host:port | 本成员的GCS监听地址 | `plugin/group_replication/src/plugin_variables.cc` |
| `group_replication_group_seeds` | NULL | host1:port1,host2:port2 | Seed成员列表，用于加入组 | 至少配置一个可用成员 |
| `group_replication_bootstrap_group` | OFF | ON/OFF | 是否引导创建新组 | 仅第一个成员启动时设置为ON |

### 9.2 bootstrap_group参数时序

```mermaid
sequenceDiagram
    participant ADMIN as **管理员**
    participant M1 as **第一个成员**
    participant XCOM as **XCom (M1)**

    Note over ADMIN,XCOM: **引导组创建流程**

    ADMIN->>M1: SET GLOBAL group_replication_bootstrap_group = ON
    M1->>M1: bootstrap_group = true
    
    ADMIN->>M1: START GROUP_REPLICATION
    
    M1->>M1: 检查bootstrap_group<br/>if (bootstrap_group == ON)
    
    M1->>XCOM: gcs_module->join(group_name)
    
    alt bootstrap模式
        XCOM->>XCOM: 不尝试连接seeds<br/>直接创建新组
        XCOM->>XCOM: 初始化Paxos状态<br/>view_id = 1<br/>members = [M1]
        XCOM->>XCOM: 设置自己为ONLINE
        XCOM-->>M1: 组创建成功
        
        M1->>M1: 广播初始VIEW_CHANGE<br/>view_id = 'view_1'<br/>members = [M1]
    else 非bootstrap模式
        XCOM->>XCOM: 尝试连接seeds<br/>加入现有组
    end
    
    M1->>M1: 设置PRIMARY (如果单主模式)<br/>group_replication_primary_member = M1.uuid
    
    ADMIN->>M1: SET GLOBAL group_replication_bootstrap_group = OFF
    M1->>M1: 关闭bootstrap模式<br/>防止意外创建多个组
```

### 9.3 通信和超时参数

| 参数 | 默认值 | 范围 | 说明 | 用途 |
|------|-------|------|------|------|
| `group_replication_member_expel_timeout` | 5 | 0-3600 | 怀疑成员失败的超时时间（秒） | 故障检测 |
| `group_replication_unreachable_majority_timeout` | 0 | 0-31536000 | 少数派成员的超时（秒），0表示永久等待 | 网络分区处理 |
| `group_replication_communication_max_message_size` | 10MB | 0-1GB | 最大消息大小 | 大事务处理 |
| `group_replication_compression_threshold` | 1MB | 0-1GB | 启用压缩的阈值 | 节省带宽 |

### 9.4 故障检测机制

```mermaid
sequenceDiagram
    participant M1 as **成员1**
    participant M2 as **成员2**
    participant M3 as **成员3 (疑似故障)**
    participant FD as **故障检测器**

    Note over M1,FD: **故障检测和驱逐机制**

    loop 定期心跳（每秒）
        M3->>M1: Heartbeat消息
        M3->>M2: Heartbeat消息
        M1->>M1: 更新M3的last_seen_time
        M2->>M2: 更新M3的last_seen_time
    end
    
    Note over M3: **M3网络故障，停止发送心跳**
    
    M1->>FD: 监控心跳<br/>now() - M3.last_seen_time
    
    alt 超过member_expel_timeout
        FD->>FD: 怀疑M3失败<br/>suspect_member(M3)
        
        M1->>M2: 发送SUSPECT消息<br/>suspected_member = M3
        M2->>M2: 收到SUSPECT<br/>检查自己的M3心跳状态
        
        alt M2也未收到M3心跳
            M2->>M1: 确认SUSPECT
            
            Note over M1,M2: **达成共识: M3失败**
            
            M1->>M1: 提议VIEW_CHANGE<br/>移除M3
            
            loop Paxos协议
                M1->>M2: PREPARE (移除M3)
                M2-->>M1: PROMISE
                M1->>M2: ACCEPT (移除M3)
                M2-->>M1: ACCEPTED
            end
            
            M1->>M1: 广播VIEW_CHANGE<br/>view_id = 'view_16'<br/>members = [M1, M2]<br/>left = [M3]<br/>reason = MEMBER_EXPELLED
            
            M1->>M1: 更新replication_group_members<br/>M3 state = UNREACHABLE
        else M2仍能收到M3心跳
            M2->>M1: 拒绝SUSPECT<br/>M3仍然活跃
        end
    end
```

### 9.5 少数派处理参数

```mermaid
sequenceDiagram
    participant M1 as **成员1 (少数派)**
    participant M2 as **成员2 (多数派)**
    participant M3 as **成员3 (多数派)**
    participant SPLIT as **网络分区**

    Note over M1,SPLIT: **网络分区场景**

    Note over SPLIT: **网络分裂: M1 <==> | 分区 | <==> M2,M3**
    
    M1->>M2: Heartbeat (发送失败)
    M1->>M3: Heartbeat (发送失败)
    
    M1->>M1: 检测到网络分区<br/>无法联系大多数成员
    M1->>M1: 计算当前可见成员<br/>visible = [M1] (1/3)
    M1->>M1: 判断: visible < (total/2 + 1)<br/>1 < 2，少数派
    
    alt unreachable_majority_timeout = 0
        M1->>M1: 永久等待网络恢复<br/>保持ONLINE状态<br/>但拒绝新事务
        
        Note over M1: **阻塞状态**<br/>等待网络恢复或人工干预
    else unreachable_majority_timeout > 0
        M1->>M1: 启动超时计时器<br/>timeout = unreachable_majority_timeout秒
        
        loop 等待期间
            M1->>M1: 尝试重连M2, M3
            
            alt 网络恢复
                M1->>M2: 重新建立连接
                M1->>M1: 重新加入组<br/>恢复ONLINE
            end
        end
        
        alt 超时未恢复
            M1->>M1: 执行exit_state_action<br/>默认: READ_ONLY
            
            alt exit_state_action = READ_ONLY
                M1->>M1: 设置super_read_only = ON<br/>拒绝写入
                M1->>M1: 状态变为ERROR
            else exit_state_action = ABORT_SERVER
                M1->>M1: MySQL服务器关闭<br/>exit(1)
            end
        end
    end
    
    Note over M2,M3: **多数派继续运行**
    
    M2->>M3: Paxos协议正常<br/>达成共识
    M3->>M2: 事务正常提交
    
    M2->>M2: 驱逐M1<br/>VIEW_CHANGE (members = [M2, M3])
```

### 9.6 一致性参数

| 参数 | 默认值 | 选项 | 说明 | 性能影响 |
|------|-------|------|------|---------|
| `group_replication_consistency` | EVENTUAL | EVENTUAL, BEFORE, AFTER, BEFORE_AND_AFTER, BEFORE_ON_PRIMARY_FAILOVER | 事务一致性级别 | 越高一致性，性能越低 |
| `group_replication_transaction_size_limit` | 150MB | 0-2GB | 单个事务的最大大小 | 避免大事务阻塞组 |

### 9.7 一致性级别时序对比

```mermaid
sequenceDiagram
    participant CLIENT as **客户端**
    participant PRIMARY as **Primary成员**
    participant SECONDARY1 as **Secondary1**
    participant SECONDARY2 as **Secondary2**

    Note over CLIENT,SECONDARY2: **EVENTUAL vs BEFORE一致性对比**

    Note over PRIMARY: **场景A: EVENTUAL (默认)**
    
    CLIENT->>PRIMARY: BEGIN; INSERT INTO t1 VALUES(1);
    PRIMARY->>PRIMARY: 本地执行<br/>不等待secondaries
    PRIMARY->>PRIMARY: 广播给certification
    par 并行执行
        PRIMARY->>PRIMARY: 本地提交
    and
        PRIMARY->>SECONDARY1: 传播事务
        PRIMARY->>SECONDARY2: 传播事务
    end
    PRIMARY-->>CLIENT: Query OK (立即返回)
    
    Note over SECONDARY1: **可能存在延迟**<br/>Secondary还未应用事务
    
    CLIENT->>SECONDARY1: SELECT * FROM t1 WHERE id=1
    SECONDARY1-->>CLIENT: Empty set (事务还未到达)
    
    Note over PRIMARY: **场景B: BEFORE**
    
    CLIENT->>SECONDARY1: SET @@SESSION.group_replication_consistency = 'BEFORE'
    CLIENT->>SECONDARY1: SELECT * FROM t1
    
    SECONDARY1->>SECONDARY1: 检查队列中的事务<br/>确保所有先前的事务已应用
    
    alt 队列中有未应用事务
        SECONDARY1->>SECONDARY1: 阻塞查询<br/>等待事务应用完成
        loop 应用队列中的事务
            SECONDARY1->>SECONDARY1: apply_transaction()
        end
    end
    
    SECONDARY1->>SECONDARY1: 队列为空<br/>读取最新数据
    SECONDARY1-->>CLIENT: 返回结果 (保证是最新的)
    
    Note over PRIMARY: **场景C: BEFORE_AND_AFTER**
    
    CLIENT->>PRIMARY: SET @@SESSION.group_replication_consistency = 'BEFORE_AND_AFTER'
    CLIENT->>PRIMARY: BEGIN; UPDATE t1 SET val=2 WHERE id=1;
    
    PRIMARY->>PRIMARY: BEFORE: 等待之前所有事务完成
    PRIMARY->>PRIMARY: 执行UPDATE
    PRIMARY->>PRIMARY: 广播certification
    PRIMARY->>SECONDARY1: 传播事务
    PRIMARY->>SECONDARY2: 传播事务
    
    PRIMARY->>PRIMARY: AFTER: 等待所有成员ACK
    SECONDARY1->>SECONDARY1: apply_transaction()
    SECONDARY1-->>PRIMARY: ACK
    SECONDARY2->>SECONDARY2: apply_transaction()
    SECONDARY2-->>PRIMARY: ACK
    
    PRIMARY->>PRIMARY: 所有成员已应用<br/>强一致性保证
    PRIMARY-->>CLIENT: Query OK (耗时更长但一致性最强)
```

### 9.8 流控参数

| 参数 | 默认值 | 范围 | 说明 |
|------|-------|------|------|
| `group_replication_flow_control_mode` | QUOTA | DISABLED, QUOTA | 流控模式 |
| `group_replication_flow_control_certifier_threshold` | 25000 | 0-ULONG_MAX | Certifier队列阈值 |
| `group_replication_flow_control_applier_threshold` | 25000 | 0-ULONG_MAX | Applier队列阈值 |

### 9.9 流控机制时序

```mermaid
sequenceDiagram
    participant PRIMARY as **Primary (快)**
    participant SLOW_SEC as **Slow Secondary**
    participant CERTIFIER as **Certifier**
    participant FLOW_CTRL as **流控器**

    Note over PRIMARY,FLOW_CTRL: **QUOTA流控模式**

    PRIMARY->>PRIMARY: 高速提交事务<br/>100 TPS
    PRIMARY->>CERTIFIER: 发送事务到certification
    
    CERTIFIER->>CERTIFIER: certification_queue.size()<br/>增长中...
    
    SLOW_SEC->>SLOW_SEC: 应用速度慢<br/>50 TPS (CPU负载高)
    SLOW_SEC->>SLOW_SEC: applier_queue.size()<br/>持续增长
    
    loop 监控队列长度
        FLOW_CTRL->>CERTIFIER: 检查certification_queue
        FLOW_CTRL->>SLOW_SEC: 检查applier_queue
        
        alt applier_queue > applier_threshold
            FLOW_CTRL->>FLOW_CTRL: 检测到Slow Secondary<br/>applier_queue = 30000 > 25000
            
            FLOW_CTRL->>FLOW_CTRL: 计算限流quota<br/>quota = min_member_capacity<br/>= 50 TPS
            
            FLOW_CTRL->>PRIMARY: 应用流控<br/>throttle(quota = 50 TPS)
            
            PRIMARY->>PRIMARY: 限制提交速度<br/>从100 TPS降到50 TPS
            
            loop 每次事务提交
                PRIMARY->>FLOW_CTRL: 请求提交许可
                
                alt 超过quota
                    FLOW_CTRL->>PRIMARY: sleep(delay)<br/>延迟提交
                else 在quota内
                    FLOW_CTRL->>PRIMARY: 允许提交
                end
            end
        else 队列正常
            FLOW_CTRL->>PRIMARY: 无限流<br/>全速运行
        end
    end
    
    Note over SLOW_SEC: **队列逐渐消化**
    
    SLOW_SEC->>SLOW_SEC: applier_queue减少<br/>30000 -> 20000 -> 10000
    
    FLOW_CTRL->>FLOW_CTRL: 队列低于阈值<br/>解除限流
    FLOW_CTRL->>PRIMARY: 取消流控<br/>恢复全速
```

---

## 第十部分：MGR监控和诊断增强

### 10.1 实时监控SQL扩展

**监控组成员健康状态**:

```sql
-- 全面监控组成员状态
SELECT 
    m.MEMBER_ID,
    m.MEMBER_HOST,
    m.MEMBER_PORT,
    m.MEMBER_STATE,
    m.MEMBER_ROLE,
    CASE m.MEMBER_STATE
        WHEN 'ONLINE' THEN '健康'
        WHEN 'RECOVERING' THEN '恢复中'
        WHEN 'OFFLINE' THEN '离线'
        WHEN 'ERROR' THEN '错误'
        WHEN 'UNREACHABLE' THEN '不可达'
    END AS state_cn,
    s.COUNT_TRANSACTIONS_IN_QUEUE AS cert_queue,
    s.COUNT_CONFLICTS_DETECTED AS conflicts,
    s.COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE AS applier_queue,
    ROUND(s.COUNT_CONFLICTS_DETECTED * 100.0 / 
          NULLIF(s.COUNT_TRANSACTIONS_CHECKED, 0), 2) AS conflict_rate_pct
FROM performance_schema.replication_group_members m
LEFT JOIN performance_schema.replication_group_member_stats s
  ON m.MEMBER_ID = s.MEMBER_ID
ORDER BY m.MEMBER_ROLE DESC, m.MEMBER_HOST;
```

**诊断流控状态**:

```sql
-- 检测是否触发流控
SELECT 
    @@group_replication_flow_control_mode AS flow_control_mode,
    @@group_replication_flow_control_certifier_threshold AS cert_threshold,
    @@group_replication_flow_control_applier_threshold AS applier_threshold,
    MAX(s.COUNT_TRANSACTIONS_IN_QUEUE) AS max_cert_queue,
    MAX(s.COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE) AS max_applier_queue,
    CASE 
        WHEN MAX(s.COUNT_TRANSACTIONS_IN_QUEUE) > @@group_replication_flow_control_certifier_threshold
        THEN 'YES - Certifier队列超过阈值'
        WHEN MAX(s.COUNT_TRANSACTIONS_REMOTE_IN_APPLIER_QUEUE) > @@group_replication_flow_control_applier_threshold
        THEN 'YES - Applier队列超过阈值'
        ELSE 'NO - 队列正常'
    END AS flow_control_triggered
FROM performance_schema.replication_group_member_stats s;
```

MySQL Group Replication通过Paxos共识协议和完善的故障检测机制，提供了高可用、自动故障转移的分布式复制方案。以上补充了详细的GCS通信协议、成员间交互时序、以及完整的参数说明和流控机制。
