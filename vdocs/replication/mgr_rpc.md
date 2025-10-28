# MySQL Group Replication RPC协议深度解析

## 概述

本文档专门深入解析MySQL Group Replication使用的RPC（Remote Procedure Call）协议，包括GCS (Group Communication System)架构、XCom Paxos协议、消息格式和成员间通信机制。

**基于源码**: `plugin/group_replication/libmysqlgcs/`, `plugin/group_replication/src/`

---

## 第一部分：GCS RPC协议栈架构

### 1.1 整体架构

**源码位置**: `plugin/group_replication/libmysqlgcs/include/mysql/gcs/gcs_interface.h`

```mermaid
graph TB
    subgraph "<b>MGR RPC协议栈完整架构</b>"
        subgraph "<b>应用层 (Application Layer)</b>"
            APP_PLUGIN["<b>Group Replication Plugin</b><br/>plugin.cc<br/>• 事务处理<br/>• 状态管理<br/>• Recovery协调"]
            APP_CERTIFIER["<b>Certifier</b><br/>certifier.cc<br/>• 冲突检测<br/>• GTID分配"]
            APP_APPLIER["<b>Applier</b><br/>applier.cc<br/>• 事务应用<br/>• 队列管理"]
        end
        
        subgraph "<b>GCS抽象层 (GCS Interface)</b>"
            GCS_INTERFACE["<b>GCS_interface</b><br/>gcs_interface.h<br/>• 统一API<br/>• 插件化设计"]
            GCS_CTRL["<b>GCS_control_interface</b><br/>join/leave/get_view"]
            GCS_COMM["<b>GCS_communication_interface</b><br/>send/receive消息"]
        end
        
        subgraph "<b>GCS实现层 (Implementation)</b>"
            XCOM_IMPL["<b>XCom Implementation</b><br/>gcs_xcom_*<br/>• XCom绑定<br/>• 协议转换"]
            MYSQL_GCS_IMPL["<b>MySQL GCS (8.0.27+)</b><br/>新通信栈"]
        end
        
        subgraph "<b>XCom核心 (Paxos Engine)</b>"
            XCOM_CORE["<b>XCom Core</b><br/>xcom_base.c<br/>• Paxos算法<br/>• 共识引擎"]
            XCOM_CACHE["<b>XCom Cache</b><br/>xcom_cache.c<br/>• 消息缓存<br/>• 重传机制"]
            XCOM_TRANSPORT["<b>XCom Transport</b><br/>xcom_transport.c<br/>• TCP管理<br/>• SSL支持"]
        end
        
        subgraph "<b>网络传输层 (Transport Layer)</b>"
            TCP["<b>TCP/IP Socket</b><br/>• 可靠传输<br/>• 连接管理"]
            SSL["<b>SSL/TLS</b><br/>• 加密<br/>• 认证"]
        end
    end
    
    APP_PLUGIN --> GCS_INTERFACE
    APP_CERTIFIER --> GCS_INTERFACE
    APP_APPLIER --> GCS_INTERFACE
    
    GCS_INTERFACE --> GCS_CTRL
    GCS_INTERFACE --> GCS_COMM
    
    GCS_CTRL --> XCOM_IMPL
    GCS_COMM --> XCOM_IMPL
    GCS_CTRL --> MYSQL_GCS_IMPL
    
    XCOM_IMPL --> XCOM_CORE
    XCOM_IMPL --> XCOM_CACHE
    XCOM_IMPL --> XCOM_TRANSPORT
    
    XCOM_TRANSPORT --> TCP
    TCP --> SSL
    
    style APP_PLUGIN fill:#e3f2fd,stroke:#333,stroke-width:2px
    style GCS_INTERFACE fill:#fff3e0,stroke:#333,stroke-width:2px
    style XCOM_CORE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 1.2 GCS接口层模块

| 模块 | 源码文件 | 功能 | 关键API |
|------|---------|------|---------|
| **Control Interface** | `gcs_control_interface.h` | 组成员管理 | `join()`, `leave()`, `belongs_to_group()` |
| **Communication Interface** | `gcs_communication_interface.h` | 消息收发 | `send_message()`, `add_event_listener()` |
| **Group Management Interface** | `gcs_group_management_interface.h` | 组配置管理 | `get_peers()`, `get_view()` |
| **Statistics Interface** | `gcs_statistics_interface.h` | 统计信息 | `get_stats()` |

---

## 第二部分：XCom Paxos协议详解

### 2.1 XCom消息类型

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/pax_msg.h`

```mermaid
graph TB
    subgraph "<b>XCom消息分类体系</b>"
        subgraph "<b>Paxos协议消息</b>"
            PREPARE["<b>prepare</b><br/>阶段1: 准备<br/>提议ballot"]
            PROMISE["<b>promise</b><br/>阶段1响应<br/>承诺不接受更小ballot"]
            ACCEPT["<b>accept</b><br/>阶段2: 接受<br/>提议值"]
            ACCEPTED["<b>accepted</b><br/>阶段2响应<br/>接受值"]
            LEARN["<b>learn</b><br/>阶段3: 学习<br/>值已被chosen"]
        end
        
        subgraph "<b>控制消息</b>"
            ALIVE["<b>alive</b><br/>心跳消息<br/>故障检测"]
            I_AM_ALIVE["<b>i_am_alive</b><br/>心跳响应"]
            NEED_BOOT_OP["<b>need_boot_op</b><br/>请求引导<br/>加入组"]
            BOOT_OP["<b>boot_op</b><br/>引导操作<br/>初始化配置"]
        end
        
        subgraph "<b>配置变更消息</b>"
            ADD_NODE["<b>add_node</b><br/>添加成员<br/>VIEW_CHANGE"]
            REMOVE_NODE["<b>remove_node</b><br/>移除成员<br/>成员离开/驱逐"]
            VIEW_MSG["<b>view_msg</b><br/>视图消息<br/>成员列表变更通知"]
        end
        
        subgraph "<b>数据传输消息</b>"
            APP_DATA["<b>app_data</b><br/>应用数据<br/>事务payload"]
            FRAG["<b>frag_msg</b><br/>分片消息<br/>大消息分片"]
        end
        
        subgraph "<b>错误和重传消息</b>"
            READ["<b>read</b><br/>读取请求<br/>请求missing消息"]
            RECOVER["<b>recover</b><br/>恢复消息<br/>重传lost消息"]
        end
    end
    
    PREPARE --> PROMISE
    PROMISE --> ACCEPT
    ACCEPT --> ACCEPTED
    ACCEPTED --> LEARN
    
    ALIVE --> I_AM_ALIVE
    NEED_BOOT_OP --> BOOT_OP
    
    ADD_NODE --> VIEW_MSG
    REMOVE_NODE --> VIEW_MSG
    
    APP_DATA --> FRAG
    
    READ --> RECOVER
    
    style PREPARE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style APP_DATA fill:#fff3e0,stroke:#333,stroke-width:2px
    style VIEW_MSG fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 2.2 Paxos消息格式

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/pax_msg.h:pax_msg`

```text
pax_msg结构（Paxos消息）:
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| op                   | pax_op         | 消息类型(prepare/accept/learn等)  |
| synode               | synode_no      | Synode编号(epoch:msgno:node)      |
| proposal             | ballot         | Ballot编号(cnt:node)              |
| a                    | pax_msg*       | 接受阶段的值指针                  |
| from                 | node_no        | 发送方节点编号                    |
| to                   | node_no        | 接收方节点编号                    |
| force_delivery       | int            | 强制投递标志                      |
| refcnt               | int            | 引用计数                          |
| msg_type             | pax_msg_type   | 消息业务类型                      |
| max_synode           | synode_no      | 最大已知synode                    |
| delivered_msg        | synode_no      | 已投递消息编号                    |
| app_data             | app_data_ptr   | 应用数据指针                      |
| cli_err              | int            | 客户端错误码                      |
+----------------------+----------------+------------------------------------+

synode_no结构（Synode编号）:
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| group_id             | uint32_t       | 组ID（epoch编号）                 |
| msgno                | uint64_t       | 消息序号（单调递增）              |
| node                 | node_no        | 节点编号（提议者）                |
+----------------------+----------------+------------------------------------+

ballot结构（Ballot编号）:
+----------------------+----------------+------------------------------------+
| 字段                 | 类型           | 说明                              |
+----------------------+----------------+------------------------------------+
| cnt                  | int32_t        | 轮次计数（提议编号）              |
| node                 | node_no        | 节点编号（提议者）                |
+----------------------+----------------+------------------------------------+
```

### 2.3 完整Paxos协议时序

```mermaid
sequenceDiagram
    participant A as **节点A (Proposer)**
    participant B as **节点B (Acceptor)**
    participant C as **节点C (Acceptor)**
    participant L as **所有节点 (Learners)**

    Note over A,L: **XCom Paxos完整协议流程**

    Note over A: **提议者A接收到应用数据**
    A->>A: 接收app_data<br/>事务payload
    A->>A: 分配synode<br/>synode = {epoch=5, msgno=1001, node=A}
    A->>A: 生成ballot<br/>ballot = {cnt=1, node=A}
    
    Note over A,C: **阶段1: Prepare**
    
    A->>A: 构造prepare消息<br/>pax_msg{<br/>  op=prepare,<br/>  synode={5,1001,A},<br/>  proposal={1,A}<br/>}
    
    par 并行发送
        A->>B: prepare消息
        A->>C: prepare消息
    end
    
    B->>B: 检查proposal<br/>proposal > last_promised?
    
    alt proposal大于last_promised
        B->>B: 记录promised_ballot = {1,A}
        B->>B: 返回已接受的值(如果有)<br/>accepted_ballot, accepted_value
        B->>A: promise消息<br/>pax_msg{<br/>  op=promise,<br/>  synode={5,1001,A},<br/>  a=previous_accepted<br/>}
    else proposal不大于last_promised
        B->>A: nack消息<br/>拒绝prepare
    end
    
    C->>C: 检查并promise
    C->>A: promise消息
    
    Note over A: **达到多数派（2/3）**
    
    A->>A: 收到2个promise<br/>检查返回的accepted_value
    
    alt 有accepted_value
        A->>A: 选择最大ballot的value<br/>value = highest_ballot_value
    else 无accepted_value
        A->>A: 使用自己的value<br/>value = app_data
    end
    
    Note over A,C: **阶段2: Accept**
    
    A->>A: 构造accept消息<br/>pax_msg{<br/>  op=accept,<br/>  synode={5,1001,A},<br/>  proposal={1,A},<br/>  a->app_data=value<br/>}
    
    par 并行发送
        A->>B: accept消息
        A->>C: accept消息
    end
    
    B->>B: 检查proposal<br/>proposal >= promised_ballot?
    
    alt proposal有效
        B->>B: 持久化<br/>accepted_ballot = {1,A}<br/>accepted_value = value
        B->>B: 写入XCom缓存<br/>xcom_cache[synode] = value
        B->>A: accepted消息<br/>pax_msg{<br/>  op=accepted,<br/>  synode={5,1001,A}<br/>}
    else proposal无效
        B->>A: nack消息<br/>拒绝accept
    end
    
    C->>C: 持久化accepted
    C->>A: accepted消息
    
    Note over A: **达到多数派（2/3）**
    
    A->>A: 值已被chosen<br/>synode={5,1001,A}<br/>的值达成共识
    
    Note over A,L: **阶段3: Learn**
    
    A->>A: 构造learn消息<br/>pax_msg{<br/>  op=learn,<br/>  synode={5,1001,A},<br/>  a->app_data=value<br/>}
    
    par 广播learn
        A->>A: 投递给本地应用
        A->>B: learn消息
        A->>C: learn消息
    end
    
    L->>L: 按synode顺序投递<br/>确保FIFO顺序
    L->>L: 调用GCS回调<br/>on_message_received(app_data)
    
    Note over L: **应用层处理**
    L->>L: Certifier检查冲突<br/>Applier应用事务
```

---

## 第三部分：成员加入和离开RPC流程

### 3.1 成员加入RPC详解

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/gcs_xcom_control_interface.cc:join()`

```mermaid
sequenceDiagram
    participant NEW as **新成员N**
    participant SEED as **Seed成员S**
    participant XCOM_N as **XCom(N)**
    participant XCOM_S as **XCom(S)**
    participant GROUP as **现有组成员**

    Note over NEW,GROUP: **成员加入RPC完整协议**

    NEW->>XCOM_N: gcs_xcom_control_interface::join()
    XCOM_N->>XCOM_N: 解析group_seeds<br/>seeds = [S:33061, ...]
    
    Note over XCOM_N: **步骤1: TCP连接建立**
    
    XCOM_N->>SEED: TCP SYN<br/>目标: S:33061
    SEED-->>XCOM_N: TCP SYN-ACK
    XCOM_N->>SEED: TCP ACK<br/>连接建立
    
    alt group_replication_ssl_mode != DISABLED
        XCOM_N->>SEED: SSL Client Hello
        SEED->>XCOM_N: SSL Server Hello<br/>证书交换
        XCOM_N->>SEED: SSL Finished<br/>加密通道建立
    end
    
    Note over XCOM_N: **步骤2: 发送need_boot_op**
    
    XCOM_N->>XCOM_N: 构造need_boot_op消息<br/>pax_msg{<br/>  op=need_boot_op,<br/>  group_id=hash(group_name),<br/>  from=N<br/>}
    
    XCOM_N->>XCOM_S: need_boot_op消息<br/>请求加入组
    
    XCOM_S->>XCOM_S: 验证请求<br/>• group_id匹配<br/>• 版本兼容
    
    Note over XCOM_S: **步骤3: 返回boot_op配置**
    
    XCOM_S->>XCOM_S: 构造boot_op消息<br/>pax_msg{<br/>  op=boot_op,<br/>  current_view=[M1,M2,M3],<br/>  max_synode={5,1000,M1},<br/>  config_data<br/>}
    
    XCOM_S->>XCOM_N: boot_op消息<br/>返回组配置
    
    XCOM_N->>XCOM_N: 接收配置<br/>• current_view<br/>• synode范围<br/>• 组成员列表
    
    Note over XCOM_N: **步骤4: 连接所有现有成员**
    
    loop 对每个成员M in current_view
        XCOM_N->>GROUP: TCP连接到M
        XCOM_N->>GROUP: SSL握手(如果需要)
        XCOM_N->>GROUP: 建立持久连接
    end
    
    Note over XCOM_N: **步骤5: 提议add_node**
    
    XCOM_N->>XCOM_N: 构造add_node消息<br/>pax_msg{<br/>  op=add_node,<br/>  new_node={<br/>    uuid=N.uuid,<br/>    host=N.host,<br/>    port=N.port<br/>  }<br/>}
    
    XCOM_N->>XCOM_S: add_node消息<br/>请求添加到视图
    
    Note over XCOM_S,GROUP: **步骤6: Paxos协议达成共识**
    
    XCOM_S->>XCOM_S: 作为提议者<br/>启动Paxos协议
    
    rect rgb(230, 242, 253)
        Note over XCOM_S,GROUP: **Paxos阶段1: Prepare**
        XCOM_S->>GROUP: prepare(add_node)
        GROUP-->>XCOM_S: promise
        
        Note over XCOM_S,GROUP: **Paxos阶段2: Accept**
        XCOM_S->>GROUP: accept(add_node)
        GROUP-->>XCOM_S: accepted
        
        Note over XCOM_S,GROUP: **Paxos阶段3: Learn**
        XCOM_S->>GROUP: learn(add_node)
        XCOM_S->>XCOM_N: learn(add_node)
    end
    
    Note over XCOM_N,GROUP: **步骤7: 处理VIEW_CHANGE**
    
    XCOM_S->>XCOM_S: 生成新视图<br/>new_view = old_view + N<br/>view_id++
    
    XCOM_S->>XCOM_S: 构造view_msg<br/>pax_msg{<br/>  op=view_msg,<br/>  view_id=16,<br/>  members=[M1,M2,M3,N],<br/>  joined=[N],<br/>  left=[]<br/>}
    
    par 广播view_msg
        XCOM_S->>XCOM_N: view_msg
        XCOM_S->>GROUP: view_msg
    end
    
    XCOM_N->>NEW: GCS回调<br/>on_view_changed(new_view)
    GROUP->>GROUP: 更新本地视图<br/>members += N
    
    NEW->>NEW: 设置状态为RECOVERING<br/>开始分布式恢复
```

### 3.2 成员离开RPC详解

```mermaid
sequenceDiagram
    participant M3 as **离开成员M3**
    participant M1 as **成员M1**
    participant M2 as **成员M2**
    participant XCOM3 as **XCom(M3)**
    participant XCOM1 as **XCom(M1)**

    Note over M3,XCOM1: **成员主动离开RPC流程**

    M3->>XCOM3: gcs_xcom_control_interface::leave()
    
    Note over XCOM3: **步骤1: 构造remove_node消息**
    
    XCOM3->>XCOM3: 构造remove_node<br/>pax_msg{<br/>  op=remove_node,<br/>  node=M3,<br/>  reason=MEMBER_LEFT<br/>}
    
    XCOM3->>XCOM1: remove_node消息<br/>请求从组中移除
    XCOM3->>M2: remove_node消息
    
    Note over XCOM1,M2: **步骤2: Paxos达成共识**
    
    XCOM1->>XCOM1: 启动Paxos<br/>提议remove_node
    
    rect rgb(230, 242, 253)
        XCOM1->>M2: prepare(remove_node)
        M2-->>XCOM1: promise
        XCOM1->>M2: accept(remove_node)
        M2-->>XCOM1: accepted
    end
    
    Note over XCOM1: **步骤3: 生成新视图**
    
    XCOM1->>XCOM1: 生成新视图<br/>new_view = old_view - M3<br/>view_id++
    
    XCOM1->>XCOM1: 构造view_msg<br/>pax_msg{<br/>  op=view_msg,<br/>  view_id=17,<br/>  members=[M1,M2],<br/>  joined=[],<br/>  left=[M3]<br/>}
    
    par 广播view_msg
        XCOM1->>M1: view_msg
        XCOM1->>M2: view_msg
        XCOM1->>XCOM3: view_msg (通知M3已被移除)
    end
    
    M1->>M1: 更新视图<br/>members -= M3
    M2->>M2: 更新视图
    
    Note over XCOM3: **步骤4: 清理资源**
    
    XCOM3->>XCOM3: 关闭XCom引擎<br/>xcom_exit()
    XCOM3->>XCOM3: 关闭所有TCP连接
    XCOM3->>XCOM3: 清理消息缓存<br/>xcom_cache清空
    
    XCOM3->>M3: GCS回调<br/>on_view_changed(empty_view)
    M3->>M3: Group Replication停止
```

---

## 第四部分：心跳和故障检测RPC

### 4.1 心跳机制

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_base.c`

```mermaid
sequenceDiagram
    participant M1 as **成员M1**
    participant M2 as **成员M2**
    participant M3 as **成员M3**
    participant FD as **故障检测器**

    Note over M1,FD: **XCom心跳和故障检测机制**

    loop 每1秒
        Note over M1: **发送alive消息**
        
        M1->>M1: 构造alive消息<br/>pax_msg{<br/>  op=alive,<br/>  from=M1,<br/>  max_synode=当前最大synode,<br/>  delivered_msg=已投递synode<br/>}
        
        par 广播alive
            M1->>M2: alive消息
            M1->>M3: alive消息
        end
        
        M2->>M2: 更新M1心跳时间<br/>last_seen[M1] = now()
        M2->>M2: 检查synode<br/>如果M1.max_synode > my.delivered<br/>请求missing消息
        
        M2->>M2: 构造i_am_alive响应<br/>pax_msg{<br/>  op=i_am_alive,<br/>  from=M2,<br/>  max_synode=my.max_synode<br/>}
        M2->>M1: i_am_alive消息
        
        M3->>M3: 更新M1心跳
        M3->>M1: i_am_alive消息
        
        M1->>M1: 收到响应<br/>更新M2, M3状态
    end
    
    Note over FD: **故障检测**
    
    loop 每2秒检查
        FD->>FD: 遍历所有成员<br/>检查心跳超时
        
        FD->>FD: 检查M3<br/>now() - last_seen[M3]
        
        alt 超过member_expel_timeout (5秒)
            FD->>FD: 怀疑M3失败<br/>suspect_member(M3)
            
            FD->>FD: 构造suspect消息<br/>pax_msg{<br/>  op=suspect,<br/>  suspected_node=M3,<br/>  from=M1<br/>}
            
            FD->>M2: suspect消息
            
            M2->>M2: 检查自己的M3心跳<br/>last_seen[M3]
            
            alt M2也未收到M3心跳
                M2->>FD: 确认suspect<br/>agree_on_suspect(M3)
                
                Note over FD,M2: **达成共识: M3失败**
                
                FD->>FD: 提议remove_node<br/>启动Paxos移除M3
                
                rect rgb(230, 242, 253)
                    FD->>M2: prepare(remove_node, M3)
                    M2-->>FD: promise
                    FD->>M2: accept(remove_node, M3)
                    M2-->>FD: accepted
                end
                
                FD->>FD: 生成VIEW_CHANGE<br/>移除M3
                FD->>M1: view_msg (members=[M1,M2])
                FD->>M2: view_msg
            else M2仍能收到M3心跳（网络分区）
                M2->>FD: 拒绝suspect<br/>M3 is alive
                FD->>FD: 取消suspect<br/>可能是网络抖动
            end
        end
    end
```

### 4.2 消息重传机制

```mermaid
sequenceDiagram
    participant M1 as **成员M1**
    participant M2 as **成员M2 (消息丢失)**
    participant M3 as **成员M3**
    participant CACHE as **XCom Cache**

    Note over M1,CACHE: **消息丢失和重传机制**

    M1->>M1: 提议新消息<br/>synode={5,1005,M1}
    
    rect rgb(230, 242, 253)
        M1->>M3: Paxos协议<br/>prepare/accept/learn
        M3->>M3: 接收并投递<br/>synode=1005
        
        M1->>M2: learn消息 (网络丢包)
        Note over M2: **消息丢失**
    end
    
    M1->>CACHE: 存储消息到cache<br/>cache[1005] = message
    
    M1->>M1: 提议下一个消息<br/>synode={5,1006,M1}
    
    par 广播
        M1->>M2: learn(synode=1006)
        M1->>M3: learn(synode=1006)
    end
    
    M2->>M2: 接收synode=1006<br/>但expected=1005
    M2->>M2: 检测到gap<br/>missing: 1005
    
    Note over M2: **步骤1: 请求missing消息**
    
    M2->>M2: 构造read消息<br/>pax_msg{<br/>  op=read,<br/>  from=M2,<br/>  missing_synode={5,1005,M1}<br/>}
    
    M2->>M1: read消息<br/>请求重传1005
    
    M1->>CACHE: 查询cache<br/>cache[1005]
    CACHE-->>M1: 返回cached message
    
    Note over M1: **步骤2: 重传消息**
    
    M1->>M1: 构造recover消息<br/>pax_msg{<br/>  op=recover,<br/>  synode={5,1005,M1},<br/>  a->app_data=original_data<br/>}
    
    M1->>M2: recover消息<br/>重传1005
    
    M2->>M2: 接收recover<br/>填充gap
    M2->>M2: 按顺序投递<br/>1005 -> 1006
    
    M2->>M2: 调用应用层回调<br/>on_message_received(1005)<br/>on_message_received(1006)
```

---

## 第五部分：XCom缓存和性能优化

### 5.1 XCom Cache架构

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_cache.c`

```mermaid
graph TB
    subgraph "<b>XCom Cache架构</b>"
        subgraph "<b>Cache结构</b>"
            HASH_TABLE["<b>Hash Table</b><br/>synode -> pax_machine<br/>快速查找"]
            LRU_LIST["<b>LRU List</b><br/>双向链表<br/>淘汰策略"]
        end
        
        subgraph "<b>pax_machine (Paxos状态机)</b>"
            PROPOSER["<b>Proposer状态</b><br/>• promised_ballot<br/>• proposed_value"]
            ACCEPTOR["<b>Acceptor状态</b><br/>• accepted_ballot<br/>• accepted_value"]
            LEARNER["<b>Learner状态</b><br/>• learned_value<br/>• delivery_status"]
        end
        
        subgraph "<b>Cache操作</b>"
            INSERT["<b>cache_insert</b><br/>插入新synode"]
            LOOKUP["<b>cache_lookup</b><br/>查找synode"]
            EVICT["<b>cache_evict</b><br/>淘汰旧synode"]
        end
    end
    
    HASH_TABLE --> PROPOSER
    HASH_TABLE --> ACCEPTOR
    HASH_TABLE --> LEARNER
    LRU_LIST --> EVICT
    
    INSERT --> HASH_TABLE
    LOOKUP --> HASH_TABLE
    EVICT --> LRU_LIST
    
    style HASH_TABLE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style ACCEPTOR fill:#fff3e0,stroke:#333,stroke-width:2px
    style EVICT fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 5.2 Cache生命周期

```mermaid
sequenceDiagram
    participant APP as **应用层**
    participant XCOM as **XCom Core**
    participant CACHE as **XCom Cache**
    participant PAX_M as **pax_machine**

    Note over APP,PAX_M: **Cache条目生命周期**

    APP->>XCOM: 发送消息<br/>app_data
    
    XCOM->>XCOM: 分配synode<br/>synode={5,2000,M1}
    
    Note over CACHE: **阶段1: 创建**
    
    XCOM->>CACHE: cache_insert(synode)
    CACHE->>PAX_M: 创建pax_machine<br/>• synode={5,2000,M1}<br/>• state=PROPOSING
    CACHE->>CACHE: 加入hash_table<br/>key=synode
    CACHE->>CACHE: 加入LRU链表<br/>尾部（最新）
    
    Note over PAX_M: **阶段2: Paxos协议**
    
    loop Paxos阶段
        XCOM->>PAX_M: 更新状态<br/>PROPOSING->ACCEPTING->LEARNED
        PAX_M->>PAX_M: 记录ballot和value
    end
    
    PAX_M->>PAX_M: 状态=LEARNED<br/>value已确定
    
    Note over CACHE: **阶段3: 投递**
    
    XCOM->>CACHE: cache_lookup(synode)
    CACHE-->>XCOM: 返回pax_machine
    XCOM->>XCOM: 检查是否可投递<br/>synode是连续的?
    XCOM->>APP: 投递消息<br/>on_message_received()
    XCOM->>PAX_M: 标记已投递<br/>delivered=true
    
    Note over CACHE: **阶段4: 老化**
    
    loop 定期清理（每分钟）
        CACHE->>CACHE: 扫描LRU链表<br/>从头部开始（最旧）
        
        alt synode < min_active_synode && delivered
            CACHE->>CACHE: cache_evict(synode)<br/>从hash_table删除
            CACHE->>CACHE: 从LRU链表删除
            CACHE->>PAX_M: 释放pax_machine<br/>free memory
        end
    end
    
    Note over CACHE: **阶段5: 重传（如果需要）**
    
    alt 其他成员请求重传
        XCOM->>CACHE: cache_lookup(old_synode)
        
        alt 仍在cache中
            CACHE-->>XCOM: 返回pax_machine
            XCOM->>XCOM: 构造recover消息<br/>重传给请求者
        else 已被淘汰
            CACHE-->>XCOM: NULL (未找到)
            XCOM->>XCOM: 无法重传<br/>请求者需要full recovery
        end
    end
```

---

## 总结

### MGR RPC协议核心特点

**协议栈分层**:

- **应用层**: Group Replication插件，业务逻辑
- **GCS层**: 统一抽象接口，隔离实现细节
- **XCom层**: Paxos共识算法，保证一致性
- **传输层**: TCP/SSL，可靠加密通信

**Paxos协议**:

- **三阶段**: Prepare → Accept → Learn
- **多数派**: 至少(N/2+1)个节点同意
- **持久化**: accepted值持久化到XCom cache
- **FIFO保证**: 按synode顺序投递消息

**故障检测**:

- **心跳机制**: alive/i_am_alive消息，每秒一次
- **超时驱逐**: member_expel_timeout（默认5秒）
- **分区处理**: unreachable_majority_timeout，少数派自动降级

**性能优化**:

- **XCom Cache**: Hash table + LRU，快速查找和淘汰
- **消息重传**: 支持gap填充，自动恢复丢失消息
- **并行处理**: 多线程处理Paxos消息

**可靠性保证**:

- **消息持久化**: accepted值持久化
- **自动恢复**: 通过read/recover机制
- **视图一致性**: VIEW_CHANGE通过Paxos达成共识

MySQL Group Replication的RPC协议通过XCom Paxos实现了分布式共识，提供了高可用、自动故障转移和强一致性保证。

---

## 第六部分：RPC协议定义和序列化机制

### 6.1 XDR (External Data Representation) 协议定义

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_vp.x`

MGR使用**XDR**（External Data Representation）作为RPC的IDL（Interface Definition Language）和序列化协议。

```mermaid
graph TB
    subgraph "<b>XDR定义到运行时的转换</b>"
        subgraph "<b>编译时 (IDL定义)</b>"
            XDR_FILE["<b>xcom_vp.x</b><br/>XDR IDL文件<br/>• struct pax_msg<br/>• enum pax_op<br/>• union app_data"]
            RPCGEN["<b>rpcgen</b><br/>XDR编译器<br/>生成序列化代码"]
        end
        
        subgraph "<b>生成代码</b>"
            XDR_H["<b>xcom_vp.h</b><br/>C结构体定义<br/>• typedef pax_msg<br/>• typedef synode_no"]
            XDR_C["<b>xcom_vp_xdr.c</b><br/>序列化函数<br/>• xdr_pax_msg()<br/>• xdr_synode_no()"]
        end
        
        subgraph "<b>运行时</b>"
            SERIALIZE["<b>serialize_msg()</b><br/>编码消息<br/>XDR_ENCODE"]
            DESERIALIZE["<b>deserialize_msg()</b><br/>解码消息<br/>XDR_DECODE"]
            WIRE["<b>Wire Format</b><br/>网络字节流<br/>跨平台兼容"]
        end
    end
    
    XDR_FILE --> RPCGEN
    RPCGEN --> XDR_H
    RPCGEN --> XDR_C
    
    XDR_H --> SERIALIZE
    XDR_C --> SERIALIZE
    XDR_H --> DESERIALIZE
    XDR_C --> DESERIALIZE
    
    SERIALIZE --> WIRE
    WIRE --> DESERIALIZE
    
    style XDR_FILE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style RPCGEN fill:#fff3e0,stroke:#333,stroke-width:2px
    style WIRE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 6.2 pax_msg结构XDR定义

**源码**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_vp.x:506-569`

```c
// XDR IDL定义
struct pax_msg {
  node_no to;             /* 目标节点编号 */
  node_no from;           /* 源节点编号 */
  uint32_t group_id;      /* 组ID（epoch） */
  synode_no max_synode;   /* Gossip：最大已知synode */
  start_t start_type;     /* 启动类型（已废弃） */
  ballot reply_to;        /* 回复给哪个ballot */
  ballot proposal;        /* 提议编号（Paxos ballot） */
  pax_op op;              /* 操作码：prepare/accept/learn等 */
  synode_no synode;       /* 消息编号（Paxos实例） */
  pax_msg_type msg_type;  /* 消息类型：normal/noop */
  bit_set *receivers;     /* 接收者位图 */
  app_data *a;            /* 应用数据payload */
  snapshot *snap;         /* 快照（未使用） */
  gcs_snapshot *gcs_snap; /* GCS快照（gcs_snapshot_op时） */
  client_reply_code cli_err; /* 客户端错误码 */
  bool force_delivery;    /* 强制投递标志 */
  int32_t refcnt;         /* 引用计数 */
  synode_no delivered_msg;/* Gossip：最后投递的消息 */
  xcom_event_horizon event_horizon; /* 事件视界 */
  synode_app_data_array requested_synode_app_data; /* 请求的synode数据 */
  reply_data *rd;         /* 回复数据 */
};
```

### 6.3 消息序列化格式

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.h:35-52`

```mermaid
graph LR
    subgraph "<b>完整消息线上格式 (Wire Format)</b>"
        subgraph "<b>消息头 (MSG_HDR_SIZE = 12字节)</b>"
            VERSION["<b>Version</b><br/>4字节<br/>协议版本"]
            LENGTH["<b>Length</b><br/>4字节<br/>消息体长度"]
            TYPE_TAG["<b>Type+Tag</b><br/>4字节<br/>Type(1B)+Tag(2B)+Unused(1B)"]
        end
        
        subgraph "<b>消息体 (Variable Length)</b>"
            PAX_MSG_BODY["<b>XDR序列化的pax_msg</b><br/>• to (4B)<br/>• from (4B)<br/>• group_id (4B)<br/>• synode (12B)<br/>• proposal (8B)<br/>• op (4B)<br/>• app_data (VAR)<br/>..."]
        end
    end
    
    VERSION --> LENGTH
    LENGTH --> TYPE_TAG
    TYPE_TAG --> PAX_MSG_BODY
    
    style VERSION fill:#e3f2fd,stroke:#333,stroke-width:2px
    style PAX_MSG_BODY fill:#fff3e0,stroke:#333,stroke-width:2px
```

**消息头字段详解**:

```text
消息头布局 (12字节):
+-------------------+--------+--------+------------------------------------+
| 字段              | 偏移   | 大小   | 说明                              |
+-------------------+--------+--------+------------------------------------+
| version           | 0      | 4B     | XCom协议版本(x_1_0..x_1_9)        |
| length            | 4      | 4B     | 消息体长度（不包括头部）          |
| type              | 8      | 1B     | 消息类型:                         |
|                   |        |        | • 0=x_normal (正常消息)           |
|                   |        |        | • 1=x_version_req (版本协商请求)  |
|                   |        |        | • 2=x_version_reply (版本协商响应)|
| tag               | 9      | 2B     | 标签（协商时用于匹配请求/响应）   |
| unused            | 11     | 1B     | 保留字段                          |
+-------------------+--------+--------+------------------------------------+

示例（假设pax_msg序列化后200字节）:
总长度 = 12 (header) + 200 (body) = 212字节

字节流:
[0x00 0x00 0x01 0x09] <- version = x_1_9 (9)
[0x00 0x00 0x00 0xC8] <- length = 200 (0xC8)
[0x00 0x02 0x9A 0x00] <- type=0, tag=666(0x029A), unused=0
[... 200字节XDR序列化的pax_msg ...]
```

### 6.4 序列化和反序列化流程

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.cc:549-570`

```mermaid
sequenceDiagram
    participant APP as **应用层**
    participant XCOM as **XCom Core**
    participant SERIAL as **序列化器**
    participant XDR as **XDR库**
    participant NETWORK as **网络层**

    Note over APP,NETWORK: **消息发送序列化流程**

    APP->>XCOM: 发送pax_msg *p<br/>• p->op = prepare_op<br/>• p->synode = {5,1000,1}<br/>• p->proposal = {1,1}
    
    XCOM->>SERIAL: serialize_msg(p, x_proto, &buflen, &buf)
    
    Note over SERIAL: **步骤1: 计算序列化长度**
    
    SERIAL->>XDR: xdr_sizeof(xdr_pax_msg, p)
    XDR->>XDR: 遍历pax_msg所有字段<br/>累加各字段XDR长度
    XDR-->>SERIAL: msg_buflen = 256字节
    
    SERIAL->>SERIAL: tot_buflen = msg_buflen + MSG_HDR_SIZE<br/>= 256 + 12 = 268字节
    
    Note over SERIAL: **步骤2: 分配缓冲区**
    
    SERIAL->>SERIAL: buf = xcom_calloc(1, 268)
    
    Note over SERIAL: **步骤3: 写入协议版本**
    
    SERIAL->>SERIAL: write_protoversion(buf, x_1_9)<br/>buf[0..3] = 0x00000109
    
    Note over SERIAL: **步骤4: XDR编码消息体**
    
    SERIAL->>XDR: apply_xdr(MSG_PTR(buf), 256,<br/>          xdr_pax_msg, p, XDR_ENCODE)
    
    XDR->>XDR: XDR编码p->to<br/>buf[12..15] = htonl(p->to)
    XDR->>XDR: XDR编码p->from<br/>buf[16..19] = htonl(p->from)
    XDR->>XDR: XDR编码p->group_id<br/>buf[20..23] = htonl(p->group_id)
    XDR->>XDR: XDR编码p->synode<br/>• synode.group_id (4B)<br/>• synode.msgno (8B)<br/>• synode.node (4B)
    XDR->>XDR: XDR编码p->proposal<br/>• ballot.cnt (4B)<br/>• ballot.node (4B)
    XDR->>XDR: XDR编码p->op<br/>buf[...] = htonl(prepare_op)
    XDR->>XDR: XDR编码p->a (app_data)<br/>• 递归编码app_data结构
    XDR-->>SERIAL: 返回成功 (retval=1)
    
    Note over SERIAL: **步骤5: 写入消息头**
    
    SERIAL->>SERIAL: put_header_1_0(buf, 256, x_normal, 666)<br/>• LENGTH_PTR(buf) = 256<br/>• TYPE = x_normal (0)<br/>• TAG = 666
    
    SERIAL-->>XCOM: buflen=268, buf=序列化数据
    
    XCOM->>NETWORK: 发送268字节到网络
    
    Note over APP,NETWORK: **消息接收反序列化流程**
    
    NETWORK->>XCOM: 接收到268字节数据
    
    XCOM->>SERIAL: deserialize_msg(p, x_proto, buf, 256)
    
    Note over SERIAL: **步骤1: XDR解码**
    
    SERIAL->>XDR: apply_xdr(buf, 256,<br/>          xdr_pax_msg, p, XDR_DECODE)
    
    XDR->>XDR: XDR解码p->to<br/>p->to = ntohl(buf[0..3])
    XDR->>XDR: XDR解码p->from<br/>p->from = ntohl(buf[4..7])
    XDR->>XDR: XDR解码p->group_id<br/>p->group_id = ntohl(buf[8..11])
    XDR->>XDR: XDR解码p->synode<br/>重建synode_no结构
    XDR->>XDR: XDR解码p->proposal<br/>重建ballot结构
    XDR->>XDR: XDR解码p->op<br/>p->op = (pax_op)ntohl(...)
    XDR->>XDR: XDR解码p->a<br/>递归重建app_data链表
    
    XDR-->>SERIAL: 返回成功 (apply_ok=1)
    
    SERIAL-->>XCOM: pax_msg *p 已填充
    
    XCOM->>APP: dispatch_op(site, p, reply_queue)<br/>分发到对应的handler
```

### 6.5 XDR多版本支持

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.cc:536-556`

MGR支持多个XCom协议版本，实现向后兼容：

```mermaid
graph TB
    subgraph "<b>XCom协议版本演进</b>"
        subgraph "<b>版本定义</b>"
            X_1_0["<b>x_1_0</b><br/>基础版本<br/>MySQL 5.7.17"]
            X_1_1["<b>x_1_1</b><br/>+delivered_msg<br/>MySQL 5.7.19"]
            X_1_3["<b>x_1_3</b><br/>+event_horizon<br/>MySQL 8.0.14"]
            X_1_5["<b>x_1_5</b><br/>+requested_synode_app_data<br/>MySQL 8.0.16"]
            X_1_8["<b>x_1_8</b><br/>+reply_data<br/>MySQL 8.0.21"]
            X_1_9["<b>x_1_9 (当前)</b><br/>最新版本<br/>MySQL 8.0.27+"]
        end
        
        subgraph "<b>版本协商</b>"
            NEG_START["<b>连接建立</b><br/>发送x_version_req"]
            NEG_RECV["<b>接收响应</b><br/>x_version_reply"]
            NEG_DECIDE["<b>选择版本</b><br/>min(my_version, peer_version)"]
        end
        
        subgraph "<b>序列化函数表</b>"
            FUNC_TABLE["<b>pax_msg_func[]</b><br/>• xdr_pax_msg_1_0<br/>• xdr_pax_msg_1_1<br/>• ...<br/>• xdr_pax_msg_1_9"]
        end
    end
    
    X_1_0 --> X_1_1
    X_1_1 --> X_1_3
    X_1_3 --> X_1_5
    X_1_5 --> X_1_8
    X_1_8 --> X_1_9
    
    NEG_START --> NEG_RECV
    NEG_RECV --> NEG_DECIDE
    NEG_DECIDE --> FUNC_TABLE
    
    style X_1_9 fill:#e3f2fd,stroke:#333,stroke-width:2px
    style NEG_DECIDE fill:#fff3e0,stroke:#333,stroke-width:2px
    style FUNC_TABLE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**版本协商时序**:

```mermaid
sequenceDiagram
    participant M1 as **成员M1 (v1.9)**
    participant M2 as **成员M2 (v1.8)**

    Note over M1,M2: **XCom协议版本协商**

    M1->>M1: 初始化<br/>my_xcom_version = x_1_9<br/>my_min_xcom_version = x_1_0
    
    M2->>M2: 初始化<br/>my_xcom_version = x_1_8<br/>my_min_xcom_version = x_1_0
    
    Note over M1: **M1连接M2**
    
    M1->>M2: TCP连接建立
    
    M1->>M1: 构造x_version_req消息<br/>type = x_version_req<br/>payload = x_1_9
    M1->>M2: x_version_req (x_1_9)
    
    M2->>M2: 检查协议兼容性<br/>min = min(x_1_9, x_1_8) = x_1_8<br/>max = max(my_min, M1.min) = x_1_0<br/>min >= max? YES
    
    M2->>M2: 构造x_version_reply<br/>type = x_version_reply<br/>payload = x_1_8
    M2->>M1: x_version_reply (x_1_8)
    
    M1->>M1: 协商结果<br/>negotiated_version = x_1_8
    M1->>M1: 设置连接协议版本<br/>s->con->x_proto = x_1_8
    
    Note over M1,M2: **后续所有消息使用x_1_8协议**
    
    M1->>M1: serialize_msg(p, x_1_8, ...)<br/>使用xdr_pax_msg_1_8()
    M1->>M2: pax_msg (x_1_8 format)
    
    M2->>M2: deserialize_msg(p, x_1_8, ...)<br/>使用xdr_pax_msg_1_8()
```

---

## 第七部分：RPC调用机制

### 7.1 消息发送调用链

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_base.cc:1596-1622`

```mermaid
sequenceDiagram
    participant APP as **应用层<br/>(GR Plugin)**
    participant PROP as **Proposer Task**
    participant PREPARE as **prepare_msg()**
    participant SEND_ACC as **send_to_acceptors()**
    participant SEND_SRV as **send_server_msg()**
    participant SEND_MSG as **_send_msg()**
    participant SERIAL as **serialize_msg()**
    participant NETWORK as **TCP Write**

    Note over APP,NETWORK: **RPC调用完整链路**

    APP->>APP: xcom_send(app_data, msg)<br/>应用想发送数据
    
    APP->>PROP: channel_put(&prop_input_queue, msg)<br/>投递到proposer队列
    
    Note over PROP: **Proposer Task被唤醒**
    
    PROP->>PROP: proposer_task()<br/>从队列取出消息
    
    PROP->>PROP: 分配synode<br/>msgno = {epoch, msg_no, node}
    
    PROP->>PROP: 创建pax_machine<br/>p = get_cache(msgno)
    
    PROP->>PROP: 初始化提议<br/>p->proposer.msg = msg<br/>p->proposer.bal = {cnt, node}
    
    Note over PROP: **启动3阶段Paxos**
    
    PROP->>PREPARE: prepare_msg(pax_msg *p)
    
    PREPARE->>PREPARE: init_prepare_msg(p)<br/>p->op = prepare_op<br/>p->reply_to = p->proposal
    
    PREPARE->>SEND_ACC: send_to_acceptors(p, "prepare_msg")
    
    Note over SEND_ACC: **发送给所有acceptors**
    
    Note over SEND_ACC: 遍历所有节点<br/>for each node in site
    
    loop 对每个节点
        SEND_ACC->>SEND_SRV: send_server_msg(site, i, p)
        
        SEND_SRV->>SEND_SRV: 获取server结构<br/>s = get_server(site, node)
        
        alt 本地节点 (node == p->from)
            SEND_SRV->>SEND_SRV: dispatch_op(site, p, NULL)<br/>直接本地调用，不走网络
        else 远程节点
            SEND_SRV->>SEND_MSG: _send_msg(s, p, node, &ret)
            
            SEND_MSG->>SEND_MSG: p->to = node<br/>p->max_synode = get_max_synode()
            
            SEND_MSG->>SERIAL: serialize_msg(p, s->con->x_proto,<br/>             &buflen, &buf)
            
            SERIAL-->>SEND_MSG: buflen, buf
            
            alt 缓冲区空间足够
                SEND_MSG->>SEND_MSG: put_srv_buf(&s->out_buf, buf, buflen)<br/>写入发送缓冲区
            else 缓冲区满
                SEND_MSG->>SEND_MSG: flush_srv_buf(s, &ret)<br/>先刷新缓冲区
                
                alt 消息超大
                    SEND_MSG->>NETWORK: task_write(s->con, buf, buflen)<br/>直接发送，不缓冲
                else 正常大小
                    SEND_MSG->>SEND_MSG: put_srv_buf(&s->out_buf, buf, buflen)
                end
            end
            
            SEND_MSG->>SEND_MSG: send_count[p->op]++<br/>send_bytes[p->op] += buflen<br/>统计
            
            SEND_MSG->>SEND_MSG: alive(s)<br/>标记服务器活跃
        end
    end
    
    SEND_ACC-->>PREPARE: 发送完成
    PREPARE-->>PROP: 返回
    
    Note over PROP: **等待promise响应**
```

### 7.2 消息接收和分发

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_base.cc:6425-6457`

```mermaid
sequenceDiagram
    participant NETWORK as **TCP Read**
    participant READ_MSG as **read_msg()**
    participant DESERIAL as **deserialize_msg()**
    participant DISPATCH as **dispatch_op()**
    participant HANDLER as **Handler函数**
    participant PAX_M as **pax_machine**

    Note over NETWORK,PAX_M: **RPC接收和分发流程**

    Note over NETWORK: **acceptor_learner_task()监听连接**
    
    loop 持续接收
        NETWORK->>READ_MSG: read_msg(rfd, &p, s, &ret)
        
        Note over READ_MSG: **步骤1: 读取消息头**
        
        READ_MSG->>NETWORK: read_bytes(rfd, header_buf, MSG_HDR_SIZE)
        NETWORK-->>READ_MSG: 12字节消息头
        
        READ_MSG->>READ_MSG: 解析消息头<br/>• x_version = read_protoversion(buf)<br/>• get_header_1_0(buf, &msgsize, &x_type, &tag)
        
        alt x_type == x_version_req (版本协商)
            READ_MSG->>READ_MSG: handle_version_negotiation()<br/>发送x_version_reply
        else x_type == x_normal (正常消息)
            Note over READ_MSG: **步骤2: 读取消息体**
            
            READ_MSG->>READ_MSG: bytes = xcom_calloc(1, msgsize)
            READ_MSG->>NETWORK: read_bytes(rfd, bytes, msgsize)
            NETWORK-->>READ_MSG: msgsize字节消息体
            
            Note over READ_MSG: **步骤3: 反序列化**
            
            READ_MSG->>DESERIAL: deserialize_msg(&p, x_version,<br/>                  bytes, msgsize)
            
            DESERIAL->>DESERIAL: apply_xdr(bytes, msgsize,<br/>         pax_msg_func[x_version],<br/>         &p, XDR_DECODE)
            
            DESERIAL-->>READ_MSG: pax_msg *p 已填充
            
            Note over READ_MSG: **步骤4: 分发消息**
            
            READ_MSG->>DISPATCH: dispatch_op(site, &p, reply_queue)
            
            DISPATCH->>DISPATCH: 检查消息来源<br/>if (is_server_connected(site, p->from))
            DISPATCH->>DISPATCH: note_detected(site, p->from)<br/>标记节点活跃
            DISPATCH->>DISPATCH: update_delivered(site, p->from,<br/>                  p->delivered_msg)<br/>更新gossip信息
            
            DISPATCH->>DISPATCH: 根据op查找handler<br/>handler = site->dispatch_table[p->op]
            
            alt handler存在
                DISPATCH->>HANDLER: handler(site, &p, reply_queue)
                
                Note over HANDLER: **根据op类型调用对应handler**
                
                alt op == prepare_op
                    HANDLER->>HANDLER: handle_prepare(site, &p, reply_queue)
                    HANDLER->>PAX_M: 获取pax_machine<br/>pm = get_cache(p->synode)
                    HANDLER->>PAX_M: 检查ballot<br/>if (p->proposal > pm->acceptor.promise)
                    HANDLER->>PAX_M: 更新promised_ballot<br/>pm->acceptor.promise = p->proposal
                    HANDLER->>HANDLER: 构造promise消息<br/>reply->op = promise_op<br/>reply->a = pm->acceptor.value
                    HANDLER->>HANDLER: send_to_server(site, p->from, reply)<br/>发送promise响应
                else op == accept_op
                    HANDLER->>HANDLER: handle_accept(site, &p, reply_queue)
                    HANDLER->>PAX_M: 获取pax_machine
                    HANDLER->>PAX_M: 检查ballot<br/>if (p->proposal >= pm->acceptor.promise)
                    HANDLER->>PAX_M: 持久化accepted值<br/>pm->acceptor.value = p->a<br/>pm->acceptor.ballot = p->proposal
                    HANDLER->>HANDLER: 构造accepted消息<br/>reply->op = accepted_op
                    HANDLER->>HANDLER: send_to_server(site, p->from, reply)
                else op == learn_op
                    HANDLER->>HANDLER: handle_learn(site, &p, reply_queue)
                    HANDLER->>PAX_M: 标记已chosen<br/>pm->learner.msg = p->a
                    HANDLER->>HANDLER: deliver_to_app(p->a)<br/>投递给应用层
                else op == promise_op
                    HANDLER->>HANDLER: handle_promise(site, &p, reply_queue)
                    HANDLER->>PAX_M: 记录promise<br/>BIT_SET(pm->proposer.prep_nodeset, p->from)
                    HANDLER->>HANDLER: if (majority(prep_nodeset))<br/>进入accept阶段
                else op == accepted_op
                    HANDLER->>HANDLER: handle_accepted(site, &p, reply_queue)
                    HANDLER->>PAX_M: 记录accepted<br/>BIT_SET(pm->proposer.prop_nodeset, p->from)
                    HANDLER->>HANDLER: if (majority(prop_nodeset))<br/>进入learn阶段
                end
            else handler不存在
                DISPATCH->>DISPATCH: G_WARNING("No handler for op %d", p->op)
            end
        end
    end
```

### 7.3 Dispatch Table（分发表）

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_base.cc:6374-6400`

```mermaid
graph TB
    subgraph "<b>Dispatch Table架构</b>"
        subgraph "<b>操作码枚举 (pax_op)</b>"
            OP_INIT["<b>initial_op (0)</b>"]
            OP_PREPARE["<b>prepare_op (1)</b>"]
            OP_ACCEPT["<b>accept_op (2)</b>"]
            OP_LEARN["<b>learn_op (3)</b>"]
            OP_RECOVER["<b>recover_op (4)</b>"]
            OP_PROMISE["<b>promise_op (5)</b>"]
            OP_ACCEPTED["<b>accepted_op (6)</b>"]
            OP_READ["<b>read_op (7)</b>"]
            OP_ALIVE["<b>alive_op (8)</b>"]
            OP_CLIENT["<b>client_msg (12)</b>"]
        end
        
        subgraph "<b>Handler函数指针表 (msg_handler[])</b>"
            HANDLER_TABLE["<b>dispatch_table[]</b><br/>msg_handler数组<br/>op -> handler映射"]
        end
        
        subgraph "<b>Handler函数</b>"
            H_PREPARE["<b>handle_prepare()</b><br/>处理Prepare<br/>返回Promise"]
            H_ACCEPT["<b>handle_accept()</b><br/>处理Accept<br/>返回Accepted"]
            H_LEARN["<b>handle_learn()</b><br/>处理Learn<br/>投递消息"]
            H_PROMISE["<b>handle_promise()</b><br/>处理Promise<br/>收集多数派"]
            H_ACCEPTED["<b>handle_accepted()</b><br/>处理Accepted<br/>进入Learn"]
            H_RECOVER["<b>handle_recover()</b><br/>处理重传请求<br/>发送缓存消息"]
            H_ALIVE["<b>handle_alive()</b><br/>处理心跳<br/>更新last_seen"]
        end
    end
    
    OP_PREPARE --> HANDLER_TABLE
    OP_ACCEPT --> HANDLER_TABLE
    OP_LEARN --> HANDLER_TABLE
    OP_PROMISE --> HANDLER_TABLE
    OP_ACCEPTED --> HANDLER_TABLE
    OP_RECOVER --> HANDLER_TABLE
    OP_ALIVE --> HANDLER_TABLE
    
    HANDLER_TABLE --> H_PREPARE
    HANDLER_TABLE --> H_ACCEPT
    HANDLER_TABLE --> H_LEARN
    HANDLER_TABLE --> H_PROMISE
    HANDLER_TABLE --> H_ACCEPTED
    HANDLER_TABLE --> H_RECOVER
    HANDLER_TABLE --> H_ALIVE
    
    style HANDLER_TABLE fill:#e3f2fd,stroke:#333,stroke-width:2px
    style H_LEARN fill:#fff3e0,stroke:#333,stroke-width:2px
    style H_PREPARE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

**Dispatch Table初始化**:

```c
// 源码: plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_base.cc
static msg_handler dispatch_table[LAST_OP] = {
    [initial_op] = handle_initial,
    [prepare_op] = handle_prepare,
    [ack_prepare_op] = handle_ack_prepare,
    [accept_op] = handle_accept,
    [ack_accept_op] = handle_ack_accept,
    [learn_op] = handle_learn,
    [recover_learn_op] = handle_recover_learn,
    [multi_noop_learn_op] = handle_multi_noop_learn,
    [promise_op] = handle_promise,
    [accepted_op] = handle_accepted,
    [read_op] = handle_read,
    [recover_op] = handle_recover,
    [alive_op] = handle_alive,
    [i_am_alive_op] = handle_i_am_alive,
    [need_boot_op] = handle_need_boot,
    [boot_op] = handle_boot,
    [client_msg] = handle_client_msg,
    [add_node_op] = handle_add_node,
    [remove_node_op] = handle_remove_node,
    [view_msg] = handle_view_msg,
    [synode_request] = handle_synode_request,
    [synode_allocated] = handle_synode_allocated,
    ...
};
```

---

## 第八部分：RPC完整交互原理

### 8.1 同步RPC vs 异步RPC

MGR XCom使用**异步RPC**模型，基于协程（Task）实现非阻塞通信。

```mermaid
graph TB
    subgraph "<b>异步RPC模型</b>"
        subgraph "<b>发送侧</b>"
            SEND_TASK["<b>Sender Task</b><br/>proposer_task<br/>非阻塞发送"]
            SEND_QUEUE["<b>发送队列</b><br/>prop_input_queue<br/>异步投递"]
            SEND_BUF["<b>发送缓冲区</b><br/>srv_buf<br/>批量发送"]
        end
        
        subgraph "<b>接收侧</b>"
            RECV_TASK["<b>Receiver Task</b><br/>acceptor_learner_task<br/>非阻塞接收"]
            RECV_QUEUE["<b>接收队列</b><br/>reply_queue<br/>异步处理"]
            DISPATCH["<b>Dispatcher</b><br/>dispatch_op()<br/>分发处理"]
        end
        
        subgraph "<b>协程调度器</b>"
            SCHEDULER["<b>Task Scheduler</b><br/>task_loop()<br/>协作式多任务"]
        end
    end
    
    SEND_TASK --> SEND_QUEUE
    SEND_QUEUE --> SEND_BUF
    SEND_BUF --> RECV_TASK
    
    RECV_TASK --> RECV_QUEUE
    RECV_QUEUE --> DISPATCH
    
    SCHEDULER --> SEND_TASK
    SCHEDULER --> RECV_TASK
    
    style SEND_TASK fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SCHEDULER fill:#fff3e0,stroke:#333,stroke-width:2px
    style DISPATCH fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 8.2 完整Paxos RPC交互示例

以一个完整的3阶段Paxos为例，展示RPC交互的全貌：

```mermaid
sequenceDiagram
    participant APP as **应用层**
    participant TASK_A as **Task A<br/>(proposer_task)**
    participant SERIAL_A as **序列化器A**
    participant NET_A as **网络A->B**
    participant NET_B as **网络B**
    participant RECV_B as **Task B<br/>(acceptor_task)**
    participant DESERIAL_B as **反序列化器B**
    participant HANDLER_B as **Handler B**
    participant SERIAL_B as **序列化器B**
    participant NET_B2A as **网络B->A**
    participant RECV_A as **接收器A**

    Note over APP,RECV_A: **完整Paxos RPC交互（3阶段）**

    rect rgb(230, 242, 253)
        Note over APP,RECV_A: **阶段1: Prepare RPC**
        
        APP->>TASK_A: xcom_send(app_data)<br/>客户端请求
        
        TASK_A->>TASK_A: 分配synode<br/>synode = {5, 1000, A}
        TASK_A->>TASK_A: 构造prepare消息<br/>pax_msg{<br/>  op=prepare_op,<br/>  synode={5,1000,A},<br/>  proposal={1,A}<br/>}
        
        TASK_A->>SERIAL_A: serialize_msg(prepare_msg)
        SERIAL_A->>SERIAL_A: XDR编码<br/>• header: version+length+type<br/>• body: XDR(pax_msg)
        SERIAL_A-->>TASK_A: buf[268字节]
        
        TASK_A->>NET_A: task_write(buf, 268)
        NET_A->>NET_B: TCP传输
        
        RECV_B->>NET_B: task_read(buf, 268)
        RECV_B->>DESERIAL_B: deserialize_msg(buf)
        DESERIAL_B->>DESERIAL_B: XDR解码<br/>重建pax_msg结构
        DESERIAL_B-->>RECV_B: pax_msg *p
        
        RECV_B->>HANDLER_B: dispatch_op(p)<br/>调用handle_prepare()
        
        HANDLER_B->>HANDLER_B: 获取pax_machine<br/>pm = get_cache({5,1000,A})
        HANDLER_B->>HANDLER_B: 检查ballot<br/>p->proposal > pm->acceptor.promise?
        
        alt ballot有效
            HANDLER_B->>HANDLER_B: 更新promise<br/>pm->acceptor.promise = {1,A}
            HANDLER_B->>HANDLER_B: 构造promise消息<br/>pax_msg{<br/>  op=promise_op,<br/>  synode={5,1000,A},<br/>  reply_to={1,A},<br/>  a=pm->acceptor.value<br/>}
        else ballot无效
            HANDLER_B->>HANDLER_B: 构造nack消息<br/>拒绝prepare
        end
        
        HANDLER_B->>SERIAL_B: serialize_msg(promise_msg)
        SERIAL_B-->>HANDLER_B: buf[200字节]
        
        HANDLER_B->>NET_B2A: task_write(buf, 200)
        NET_B2A->>NET_A: TCP传输
        
        RECV_A->>NET_A: task_read(buf, 200)
        RECV_A->>RECV_A: deserialize_msg(buf)
        RECV_A->>TASK_A: dispatch_op(promise_msg)
        
        TASK_A->>TASK_A: handle_promise()<br/>BIT_SET(prep_nodeset, B)
        TASK_A->>TASK_A: 检查多数派<br/>count(prep_nodeset) >= majority?
    end
    
    rect rgb(255, 243, 224)
        Note over APP,RECV_A: **阶段2: Accept RPC**
        
        TASK_A->>TASK_A: 多数派达成<br/>进入Accept阶段
        TASK_A->>TASK_A: 构造accept消息<br/>pax_msg{<br/>  op=accept_op,<br/>  synode={5,1000,A},<br/>  proposal={1,A},<br/>  a=app_data<br/>}
        
        TASK_A->>SERIAL_A: serialize_msg(accept_msg)
        SERIAL_A-->>TASK_A: buf[512字节]
        
        TASK_A->>NET_A: task_write(buf, 512)
        NET_A->>NET_B: TCP传输
        
        RECV_B->>NET_B: task_read(buf, 512)
        RECV_B->>DESERIAL_B: deserialize_msg(buf)
        DESERIAL_B-->>RECV_B: pax_msg *p
        
        RECV_B->>HANDLER_B: dispatch_op(p)<br/>调用handle_accept()
        
        HANDLER_B->>HANDLER_B: 获取pax_machine
        HANDLER_B->>HANDLER_B: 检查ballot<br/>p->proposal >= pm->acceptor.promise?
        
        alt ballot有效
            HANDLER_B->>HANDLER_B: 持久化accepted值<br/>pm->acceptor.value = p->a<br/>pm->acceptor.ballot = {1,A}
            HANDLER_B->>HANDLER_B: 写入XCom Cache<br/>cache[{5,1000,A}] = p->a
            HANDLER_B->>HANDLER_B: 构造accepted消息<br/>pax_msg{<br/>  op=accepted_op,<br/>  synode={5,1000,A},<br/>  reply_to={1,A}<br/>}
        end
        
        HANDLER_B->>SERIAL_B: serialize_msg(accepted_msg)
        SERIAL_B-->>HANDLER_B: buf[180字节]
        
        HANDLER_B->>NET_B2A: task_write(buf, 180)
        NET_B2A->>NET_A: TCP传输
        
        RECV_A->>NET_A: task_read(buf, 180)
        RECV_A->>RECV_A: deserialize_msg(buf)
        RECV_A->>TASK_A: dispatch_op(accepted_msg)
        
        TASK_A->>TASK_A: handle_accepted()<br/>BIT_SET(prop_nodeset, B)
        TASK_A->>TASK_A: 检查多数派<br/>count(prop_nodeset) >= majority?
    end
    
    rect rgb(232, 245, 233)
        Note over APP,RECV_A: **阶段3: Learn RPC（广播）**
        
        TASK_A->>TASK_A: 多数派达成<br/>值已被chosen<br/>进入Learn阶段
        TASK_A->>TASK_A: 构造learn消息<br/>pax_msg{<br/>  op=learn_op,<br/>  synode={5,1000,A},<br/>  a=chosen_value<br/>}
        
        TASK_A->>SERIAL_A: serialize_msg(learn_msg)
        SERIAL_A-->>TASK_A: buf[500字节]
        
        par 广播learn到所有节点
            TASK_A->>NET_A: broadcast(buf, 500)
            NET_A->>NET_B: TCP传输
        end
        
        RECV_B->>NET_B: task_read(buf, 500)
        RECV_B->>DESERIAL_B: deserialize_msg(buf)
        DESERIAL_B-->>RECV_B: pax_msg *p
        
        RECV_B->>HANDLER_B: dispatch_op(p)<br/>调用handle_learn()
        
        HANDLER_B->>HANDLER_B: 标记已chosen<br/>pm->learner.msg = p->a
        HANDLER_B->>HANDLER_B: 检查投递条件<br/>synode是连续的?
        
        alt 可以投递
            HANDLER_B->>HANDLER_B: deliver_to_app(p->a)<br/>GCS回调on_message_received()
        else 存在gap
            HANDLER_B->>HANDLER_B: 缓存消息<br/>等待前序消息
        end
    end
```

### 8.3 RPC错误处理和重试

```mermaid
graph TB
    subgraph "<b>RPC错误处理机制</b>"
        subgraph "<b>网络层错误</b>"
            CONN_ERR["<b>连接错误</b><br/>ECONNREFUSED<br/>ETIMEDOUT"]
            SEND_ERR["<b>发送错误</b><br/>EPIPE<br/>ECONNRESET"]
            RECV_ERR["<b>接收错误</b><br/>EOF<br/>EAGAIN"]
        end
        
        subgraph "<b>协议层错误</b>"
            DESER_ERR["<b>反序列化错误</b><br/>XDR解码失败"]
            VER_ERR["<b>版本不兼容</b><br/>协议版本不匹配"]
            BALLOT_ERR["<b>Ballot拒绝</b><br/>nack消息"]
        end
        
        subgraph "<b>错误恢复策略</b>"
            RECONNECT["<b>重连机制</b><br/>connect_with_retry()<br/>指数退避"]
            RETRANS["<b>消息重传</b><br/>read_op请求<br/>recover_op响应"]
            SUSPECT["<b>故障检测</b><br/>suspect机制<br/>驱逐节点"]
        end
    end
    
    CONN_ERR --> RECONNECT
    SEND_ERR --> RECONNECT
    RECV_ERR --> RECONNECT
    
    DESER_ERR --> SUSPECT
    VER_ERR --> SUSPECT
    
    BALLOT_ERR --> RETRANS
    
    RECONNECT --> RETRANS
    
    style CONN_ERR fill:#ffebee,stroke:#333,stroke-width:2px
    style RECONNECT fill:#e8f5e8,stroke:#333,stroke-width:2px
    style SUSPECT fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 8.4 错误处理时序示例

```mermaid
sequenceDiagram
    participant A as **节点A**
    participant B as **节点B**
    participant NET as **网络**

    Note over A,NET: **场景1: 网络连接中断**

    A->>A: send_msg(prepare)
    A->>NET: TCP write
    
    Note over NET: **网络中断**
    
    NET--xB: 连接中断 (ECONNRESET)
    
    A->>A: 检测到错误<br/>errno = ECONNRESET
    A->>A: 标记连接失效<br/>s->con->fd = -1
    
    loop 重连尝试
        A->>A: sleep(1秒)<br/>等待重连间隔
        A->>NET: connect(B.host, B.port)
        
        alt 重连成功
            NET-->>A: 连接建立
            A->>A: 重建连接<br/>s->con->fd = new_fd
            A->>B: 重新发送prepare
        else 重连失败
            A->>A: 重试次数++
            
            alt 超过最大重试次数
                A->>A: 标记节点为SUSPECT<br/>suspect_member(B)
                A->>A: 广播SUSPECT消息
            end
        end
    end
    
    Note over A,NET: **场景2: 消息丢失**
    
    A->>B: learn(synode=1000)
    
    Note over NET: **消息在网络中丢失**
    
    A->>B: learn(synode=1001)
    B->>B: 接收synode=1001<br/>但expected=1000<br/>检测到gap
    
    B->>B: 构造read消息<br/>pax_msg{<br/>  op=read_op,<br/>  synode=1000<br/>}
    
    B->>A: read(synode=1000)<br/>请求重传
    
    A->>A: 查询XCom Cache<br/>cache[1000]
    
    alt 消息在cache中
        A->>A: 构造recover消息<br/>pax_msg{<br/>  op=recover_op,<br/>  synode=1000,<br/>  a=cached_data<br/>}
        A->>B: recover(synode=1000)
        B->>B: 填充gap<br/>投递1000和1001
    else 消息已被淘汰
        A->>B: 无法重传<br/>B需要full recovery
        B->>B: 启动recovery流程<br/>从其他节点同步
    end
```

---

## 第九部分：RPC底层网络和线程模型

### 9.1 网络传输层架构

**源码位置**: `plugin/group_replication/libmysqlgcs/src/bindings/xcom/xcom/xcom_transport.cc`

```mermaid
graph TB
    subgraph "<b>XCom网络传输架构</b>"
        subgraph "<b>连接管理</b>"
            SERVER_POOL["<b>Server Pool</b><br/>all_servers[]<br/>维护所有连接"]
            CONN_DESC["<b>connection_descriptor</b><br/>• fd<br/>• x_proto<br/>• ssl_fd"]
        end
        
        subgraph "<b>缓冲区管理</b>"
            SEND_BUF["<b>发送缓冲区</b><br/>srv_buf out_buf<br/>• 批量发送<br/>• 减少系统调用"]
            RECV_BUF["<b>接收缓冲区</b><br/>• 流式接收<br/>• 粘包处理"]
        end
        
        subgraph "<b>SSL/TLS支持</b>"
            SSL_CTX["<b>SSL Context</b><br/>• 证书验证<br/>• 加密套件"]
            SSL_CONN["<b>SSL Connection</b><br/>• SSL_read()<br/>• SSL_write()"]
        end
        
        subgraph "<b>协程IO</b>"
            TASK_READ["<b>task_read()</b><br/>非阻塞读<br/>协程yield"]
            TASK_WRITE["<b>task_write()</b><br/>非阻塞写<br/>协程yield"]
        end
    end
    
    SERVER_POOL --> CONN_DESC
    CONN_DESC --> SEND_BUF
    CONN_DESC --> RECV_BUF
    CONN_DESC --> SSL_CONN
    
    SSL_CTX --> SSL_CONN
    
    SEND_BUF --> TASK_WRITE
    RECV_BUF --> TASK_READ
    
    SSL_CONN --> TASK_READ
    SSL_CONN --> TASK_WRITE
    
    style SERVER_POOL fill:#e3f2fd,stroke:#333,stroke-width:2px
    style SEND_BUF fill:#fff3e0,stroke:#333,stroke-width:2px
    style TASK_READ fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 9.2 协程（Task）模型

XCom使用**协作式协程**（Cooperative Coroutine）实现异步IO，避免线程上下文切换开销。

```mermaid
graph TB
    subgraph "<b>XCom协程调度模型</b>"
        subgraph "<b>核心Task</b>"
            MAIN_TASK["<b>main_task</b><br/>XCom主循环"]
            PROP_TASK["<b>proposer_task</b><br/>处理客户端请求"]
            ACC_TASK["<b>acceptor_learner_task</b><br/>接收网络消息"]
            DET_TASK["<b>detector_task</b><br/>故障检测"]
            EXEC_TASK["<b>executor_task</b><br/>执行Paxos"]
        end
        
        subgraph "<b>任务调度器</b>"
            SCHEDULER["<b>task_loop()</b><br/>• 轮询所有task<br/>• 协作式调度<br/>• 无抢占"]
            TASK_QUEUE["<b>任务队列</b><br/>runnable tasks<br/>按优先级排序"]
        end
        
        subgraph "<b>同步原语</b>"
            CHANNEL["<b>Channel</b><br/>• 消息传递<br/>• 阻塞/唤醒"]
            WAIT["<b>task_wait()</b><br/>• yield控制权<br/>• 等待事件"]
            WAKEUP["<b>task_wakeup()</b><br/>• 唤醒task<br/>• 加入runnable队列"]
        end
    end
    
    SCHEDULER --> TASK_QUEUE
    TASK_QUEUE --> MAIN_TASK
    TASK_QUEUE --> PROP_TASK
    TASK_QUEUE --> ACC_TASK
    TASK_QUEUE --> DET_TASK
    TASK_QUEUE --> EXEC_TASK
    
    CHANNEL --> WAIT
    CHANNEL --> WAKEUP
    WAIT --> SCHEDULER
    WAKEUP --> SCHEDULER
    
    style SCHEDULER fill:#e3f2fd,stroke:#333,stroke-width:2px
    style CHANNEL fill:#fff3e0,stroke:#333,stroke-width:2px
    style TASK_QUEUE fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 9.3 协程调度时序

```mermaid
sequenceDiagram
    participant SCHED as **调度器<br/>(task_loop)**
    participant TASK_A as **Task A<br/>(proposer)**
    participant CHAN as **Channel**
    participant TASK_B as **Task B<br/>(acceptor)**

    Note over SCHED,TASK_B: **协程调度示例**

    loop task_loop()
        SCHED->>SCHED: 从runnable队列取出Task A
        
        SCHED->>TASK_A: 恢复执行<br/>从上次yield点继续
        
        TASK_A->>TASK_A: 执行代码<br/>构造pax_msg
        TASK_A->>CHAN: channel_put(&prop_queue, msg)<br/>投递消息
        
        CHAN->>CHAN: 将msg加入队列
        CHAN->>SCHED: task_wakeup(Task B)<br/>唤醒等待者
        
        TASK_A->>TASK_A: TASK_CALL(task_write(...))<br/>发起IO操作
        
        alt IO未完成
            TASK_A->>SCHED: yield控制权<br/>Task A进入waiting状态
            
            SCHED->>SCHED: Task A移出runnable队列<br/>加入waiting队列
        end
        
        SCHED->>SCHED: 从runnable队列取出Task B
        
        SCHED->>TASK_B: 恢复执行
        
        TASK_B->>CHAN: channel_get(&prop_queue, &msg)<br/>取出消息
        
        alt 队列为空
            TASK_B->>SCHED: yield控制权<br/>Task B进入waiting状态
        else 队列有消息
            CHAN-->>TASK_B: 返回msg
            TASK_B->>TASK_B: 处理消息<br/>dispatch_op(msg)
        end
        
        SCHED->>SCHED: 检查IO事件<br/>epoll/select
        
        alt Task A的IO完成
            SCHED->>SCHED: task_wakeup(Task A)<br/>移回runnable队列
        end
        
        SCHED->>SCHED: 下一轮调度
    end
```

### 9.4 发送缓冲区优化

```mermaid
graph TB
    subgraph "<b>发送缓冲区批量发送机制</b>"
        subgraph "<b>缓冲区结构 (srv_buf)</b>"
            BUF_DATA["<b>buf[]</b><br/>32KB固定缓冲区"]
            BUF_PTR["<b>n</b><br/>当前写入位置"]
            BUF_FREE["<b>free_space</b><br/>剩余空间"]
        end
        
        subgraph "<b>写入策略</b>"
            SMALL_MSG["<b>小消息</b><br/>< 剩余空间<br/>→ 写入缓冲区"]
            LARGE_MSG["<b>大消息</b><br/>> 缓冲区大小<br/>→ 直接发送"]
            BUF_FULL["<b>缓冲区满</b><br/>→ flush_srv_buf()"]
        end
        
        subgraph "<b>发送优化</b>"
            BATCH["<b>批量发送</b><br/>减少系统调用<br/>提高吞吐"]
            NAGLE["<b>Nagle算法</b><br/>TCP_NODELAY=0<br/>自动合并小包"]
        end
    end
    
    SMALL_MSG --> BUF_DATA
    BUF_DATA --> BUF_PTR
    BUF_PTR --> BUF_FREE
    
    BUF_FULL --> BATCH
    LARGE_MSG --> BATCH
    
    BATCH --> NAGLE
    
    style BUF_DATA fill:#e3f2fd,stroke:#333,stroke-width:2px
    style BATCH fill:#fff3e0,stroke:#333,stroke-width:2px
```

### 9.5 性能优化总结

**MGR RPC性能优化策略**:

1. **XDR序列化优化**:
   - 二进制编码，紧凑高效
   - 网络字节序，跨平台兼容
   - 零拷贝序列化（直接写入发送缓冲区）

2. **网络传输优化**:
   - 发送缓冲区批量发送，减少系统调用
   - SSL/TLS硬件加速（AES-NI）
   - TCP_NODELAY可配置

3. **协程异步IO**:
   - 单线程处理大量连接，避免线程切换
   - 协作式调度，无锁设计
   - epoll/kqueue高性能IO多路复用

4. **消息缓存**:
   - XCom Cache支持消息重传
   - Hash Table + LRU快速查找
   - 避免重复序列化

5. **Paxos优化**:
   - 2阶段Fast Paxos（无冲突场景）
   - Multi-Paxos（共享Prepare阶段）
   - Pipeline并发提议

**性能指标**（典型场景）:

| 指标 | 值 | 说明 |
|------|---|------|
| **消息延迟** | < 1ms | 本地网络，prepare->learn往返 |
| **吞吐量** | 10K+ TPS | 3节点组，小事务 |
| **序列化开销** | < 100μs | 1KB消息XDR编码 |
| **内存开销** | ~100MB | XCom Cache + 连接缓冲区 |

MySQL Group Replication的RPC协议通过XDR标准化序列化、协程异步IO和Paxos共识算法，实现了高性能、高可用的分布式通信。

---

## 第十部分：XCom RPC vs gRPC 对比分析

### 10.1 核心架构对比

```mermaid
graph TB
    subgraph "<b>XCom RPC架构</b>"
        subgraph "<b>协议栈</b>"
            XCOM_IDL["<b>XDR IDL</b><br/>xcom_vp.x<br/>rpcgen生成C代码"]
            XCOM_SERIAL["<b>XDR序列化</b><br/>二进制编码<br/>网络字节序"]
            XCOM_TRANS["<b>TCP + SSL</b><br/>自定义传输<br/>协程异步IO"]
        end
        
        subgraph "<b>特性</b>"
            XCOM_PAXOS["<b>Paxos集成</b><br/>共识算法内置"]
            XCOM_TASK["<b>协程模型</b><br/>单线程异步"]
            XCOM_CACHE["<b>XCom Cache</b><br/>消息重传"]
        end
    end
    
    subgraph "<b>gRPC架构</b>"
        subgraph "<b>协议栈</b>"
            GRPC_IDL["<b>Protocol Buffers</b><br/>.proto文件<br/>protoc生成多语言"]
            GRPC_SERIAL["<b>Protobuf序列化</b><br/>变长编码<br/>向后兼容"]
            GRPC_TRANS["<b>HTTP/2 + TLS</b><br/>标准协议<br/>流多路复用"]
        end
        
        subgraph "<b>特性</b>"
            GRPC_STREAM["<b>多种RPC模式</b><br/>Unary/Stream/Bidi"]
            GRPC_ASYNC["<b>线程池模型</b><br/>多线程异步"]
            GRPC_LOAD["<b>负载均衡</b><br/>客户端LB"]
        end
    end
    
    XCOM_IDL --> XCOM_SERIAL
    XCOM_SERIAL --> XCOM_TRANS
    XCOM_TRANS --> XCOM_PAXOS
    XCOM_PAXOS --> XCOM_TASK
    XCOM_TASK --> XCOM_CACHE
    
    GRPC_IDL --> GRPC_SERIAL
    GRPC_SERIAL --> GRPC_TRANS
    GRPC_TRANS --> GRPC_STREAM
    GRPC_STREAM --> GRPC_ASYNC
    GRPC_ASYNC --> GRPC_LOAD
    
    style XCOM_IDL fill:#e3f2fd,stroke:#333,stroke-width:2px
    style GRPC_IDL fill:#fff3e0,stroke:#333,stroke-width:2px
    style XCOM_PAXOS fill:#e8f5e8,stroke:#333,stroke-width:2px
```

### 10.2 详细对比表

| 对比维度 | **XCom RPC** | **gRPC** |
|---------|-------------|---------|
| **IDL语言** | XDR (External Data Representation) | Protocol Buffers |
| **IDL编译器** | rpcgen (C语言) | protoc (多语言：C++/Java/Python/Go等) |
| **序列化格式** | XDR二进制（固定长度+网络字节序） | Protobuf（变长编码+Tag-Length-Value） |
| **传输协议** | 自定义TCP + 12字节Header | HTTP/2 + 标准Header |
| **多路复用** | 单连接多消息（顺序） | HTTP/2 Stream（并发） |
| **RPC模式** | 请求-响应（异步回调） | Unary/ServerStream/ClientStream/BidiStream |
| **IO模型** | 协程（Cooperative Coroutine） | 线程池（Thread Pool） |
| **并发模型** | 单线程+异步IO | 多线程+异步IO |
| **SSL/TLS** | 手动SSL_read/SSL_write | 自动TLS握手（ALPN） |
| **流控** | 自定义缓冲区 | HTTP/2流控（Window Update） |
| **负载均衡** | 无（应用层选择节点） | 客户端LB（gRPC-LB协议） |
| **服务发现** | 静态配置（group_seeds） | 支持DNS/Consul等 |
| **超时控制** | 手动超时检测 | Deadline传播 |
| **元数据** | 无标准化元数据 | HTTP/2 Headers（自定义metadata） |
| **跨语言** | 仅C/C++ | 多语言（官方支持10+语言） |
| **协议版本** | 手动版本协商（x_1_0到x_1_9） | Protobuf向后兼容 |
| **错误处理** | 自定义errno | 标准gRPC Status Code |
| **重试机制** | 手动重连+重传 | 自动重试策略（Retry Policy） |
| **压缩** | 无内置压缩 | gzip/deflate/snappy |
| **监控指标** | 自定义统计（send_count/send_bytes） | OpenTelemetry集成 |
| **生态系统** | MySQL专用 | 通用RPC框架（广泛应用） |

### 10.3 序列化格式对比

#### XDR序列化示例

```text
pax_msg{to=2, from=1, synode={5,1000,1}, op=prepare_op}

XDR编码（16进制）:
+--------+--------+--------+--------+
| 00 00 00 02 | to = 2            | 4字节固定
| 00 00 00 01 | from = 1          | 4字节固定
| 00 00 00 05 | group_id = 5      | 4字节固定
| 00 00 00 05 | synode.group_id   | 4字节固定
| 00 00 00 00 00 00 03 E8 | msgno=1000 | 8字节固定
| 00 00 00 01 | synode.node = 1   | 4字节固定
| 00 00 00 01 | op = prepare_op   | 4字节固定
...
+--------+--------+--------+--------+

特点：固定长度，网络字节序（大端），解析快
```

#### Protobuf序列化示例

```text
message PaxMsg {
  uint32 to = 1;
  uint32 from = 2;
  Synode synode = 3;
  PaxOp op = 4;
}

Protobuf编码（16进制）:
+--------+--------+--------+
| 08 02  | tag=1(to), varint=2        | 2字节变长
| 10 01  | tag=2(from), varint=1      | 2字节变长
| 1A 0C  | tag=3(synode), length=12   | 2字节+子消息
| ...    | synode内容                 | 12字节
| 20 01  | tag=4(op), varint=1        | 2字节变长
+--------+--------+--------+

特点：变长编码，Tag-Length-Value，更紧凑，向后兼容
```

### 10.4 消息发送对比

#### XCom RPC发送流程

```mermaid
sequenceDiagram
    participant APP as **应用**
    participant XCOM as **XCom**
    participant XDR as **XDR**
    participant TCP as **TCP**

    APP->>XCOM: xcom_send(app_data)
    XCOM->>XCOM: 分配synode<br/>创建pax_msg
    XCOM->>XDR: xdr_pax_msg(ENCODE)
    XDR->>XDR: 固定长度编码<br/>• to: 4B<br/>• from: 4B<br/>• synode: 16B
    XDR-->>XCOM: 二进制buf
    XCOM->>XCOM: 添加12字节Header<br/>• version<br/>• length<br/>• type+tag
    XCOM->>TCP: write(buf, buflen)
    TCP-->>TCP: 单连接顺序发送
```

#### gRPC发送流程

```mermaid
sequenceDiagram
    participant APP as **应用**
    participant GRPC as **gRPC Stub**
    participant PROTO as **Protobuf**
    participant HTTP2 as **HTTP/2**

    APP->>GRPC: stub.SendMessage(msg)
    GRPC->>PROTO: msg.SerializeToString()
    PROTO->>PROTO: 变长编码<br/>• varint<br/>• Tag-Length-Value
    PROTO-->>GRPC: 二进制payload
    GRPC->>GRPC: 构造HTTP/2帧<br/>• HEADERS帧<br/>• DATA帧
    GRPC->>HTTP2: stream_id=3 (新stream)
    HTTP2-->>HTTP2: 多路复用<br/>多个stream并发
```

### 10.5 性能对比

| 性能指标 | **XCom RPC** | **gRPC** | 说明 |
|---------|-------------|---------|------|
| **序列化速度** | 极快（固定长度） | 快（变长编码） | XDR更快，但消息更大 |
| **消息大小** | 较大（固定长度浪费） | 较小（变长紧凑） | Protobuf平均小20-50% |
| **解析开销** | 低（无需解析tag） | 中（需要解析tag） | XDR直接memcpy |
| **并发性** | 低（单连接顺序） | 高（HTTP/2多路复用） | gRPC同时多个RPC |
| **延迟** | 极低（< 1ms） | 低（1-5ms） | XCom协程切换快 |
| **吞吐量** | 高（10K+ TPS） | 极高（100K+ TPS） | gRPC线程池并发高 |
| **内存占用** | 低（单线程） | 高（线程池+连接池） | XCom ~100MB，gRPC ~500MB |
| **CPU占用** | 低（协程无切换） | 中（线程切换） | XCom单核，gRPC多核 |

### 10.6 使用场景对比

#### XCom RPC适用场景

**优势**:

- ✅ **低延迟要求**：协程模型，微秒级延迟
- ✅ **嵌入式系统**：单线程，内存占用低
- ✅ **强一致性**：Paxos内置，共识协议集成
- ✅ **定制化需求**：完全控制协议细节
- ✅ **简单部署**：无外部依赖

**劣势**:

- ❌ 跨语言支持差（仅C/C++）
- ❌ 生态系统小（MySQL专用）
- ❌ 并发性受限（单线程瓶颈）
- ❌ 无标准工具链（监控、追踪）

**典型应用**:

- MySQL Group Replication（共识协议）
- 数据库内核通信（低延迟）
- 嵌入式分布式系统

#### gRPC适用场景

**优势**:

- ✅ **跨语言通信**：支持10+语言
- ✅ **微服务架构**：标准RPC框架
- ✅ **高并发**：HTTP/2多路复用
- ✅ **丰富功能**：流式RPC、负载均衡、服务发现
- ✅ **成熟生态**：监控（Prometheus）、追踪（Jaeger）

**劣势**:

- ❌ 延迟较高（HTTP/2开销）
- ❌ 内存占用大（线程池）
- ❌ 依赖复杂（HTTP/2库、Protobuf）

**典型应用**:

- 微服务间通信（Kubernetes）
- API Gateway
- 云原生应用（gRPC-Web）

### 10.7 为什么MySQL选择XCom RPC而非gRPC？

**历史原因**:

- MGR开发于2014年，当时gRPC还未发布（2015年）
- XCom基于Paxos，是专门为共识协议设计的

**技术原因**:

1. **低延迟优先**:
   - MGR需要微秒级延迟（共识协议对延迟敏感）
   - gRPC的HTTP/2开销不可接受

2. **紧密集成**:
   - XCom与Paxos深度集成（pax_msg就是Paxos消息）
   - gRPC需要额外的适配层

3. **资源受限**:
   - 数据库进程已占用大量资源
   - XCom单线程协程模型资源占用极低

4. **简单可控**:
   - 数据库内核需要完全控制通信细节
   - gRPC过于复杂，依赖多

5. **无需跨语言**:
   - MySQL核心是C/C++
   - 不需要多语言支持

**如果今天重新设计，可能会选择gRPC吗？**

可能不会，因为：

- **延迟要求**: Paxos共识需要极低延迟
- **紧密耦合**: XCom与Paxos是一体化设计
- **成熟稳定**: XCom已在生产环境验证多年

但gRPC可能用于：

- MySQL Router与MGR的管理接口
- 监控数据导出（非关键路径）
- 客户端SDK（多语言支持）

---

## 总结对比

| 维度 | **XCom RPC** | **gRPC** | **选择建议** |
|------|------------|---------|-------------|
| **定位** | 数据库内核专用RPC | 通用微服务RPC | 根据场景选择 |
| **性能** | 极致低延迟 | 高吞吐 | 延迟敏感→XCom |
| **生态** | MySQL专用 | 云原生标准 | 通用服务→gRPC |
| **复杂度** | 简单（单线程） | 复杂（多线程+HTTP/2） | 资源受限→XCom |
| **适用性** | 共识协议 | 微服务通信 | 看业务需求 |

**核心结论**:

- **XCom RPC**: 为极致性能和共识协议优化，适合数据库内核
- **gRPC**: 为通用性和生态优化，适合微服务架构

两者都是优秀的RPC框架，选择取决于具体需求。MySQL Group Replication选择XCom是基于其特定的性能和集成需求做出的正确决策。
