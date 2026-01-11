# 🔥 MySQL 生产事故复盘：GTID 复制的"静默陷阱"，为何从库突然开始全量复制？

> **源码版本**：MySQL/Percona Server 8.4.3-LTS
>
> **关键词**：GTID、主从复制、binlog、Aurora、CDC、数据一致性

---

## 📌 开篇引子：一个真实的生产事故

**事故背景**：

某公司使用类似 Aurora 架构的存算分离数据库系统。系统中有两个集群（A 集群和 B 集群），每个集群内部通过 redo log 同步数据，binlog 仅用于 CDC（Change Data Capture）订阅。

**事故经过**：

1. 运维团队执行跨集群迁移，将 DNS 从 A 集群切换到 B 集群
2. CDC 订阅程序检测到连接断开，自动重连
3. CDC 程序携带原 A 集群的 GTID 集合，连接到 B 集群
4. **预期**：B 集群应该报错拒绝连接（因为 GTID 完全不相关）
5. **实际**：B 集群从 \`binlog.000001\` 开始发送所有数据！

**后果**：

- CDC 订阅程序收到大量重复数据
- 下游数据仓库出现数据重复
- 紧急回滚，业务受影响 2 小时

**问题核心**：为什么 MySQL 不报错，而是"静默"地从头开始发送 binlog？

---

## 🔬 问题复现与现象

### 复现场景

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                        问题复现环境                                  │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  主库 B 集群状态：                                                   │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  gtid_executed: b1b2b3b4-....:1-100                         │   │
│  │  gtid_purged:   (空)                                        │   │
│  │  binlog 文件:   binlog.000001 (唯一的文件)                   │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  从库（CDC程序）携带的 GTID：                                        │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │  a1a2a3a4-....:1-500  (A集群的UUID，与B集群完全不同)          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### 实际现象

CDC 程序重连后，主库从 \`binlog.000001\` 位置 4 开始发送所有 binlog 事件，没有任何报错！

---

## 📖 原理深入：MySQL GTID 复制的原始设计

### CDC 程序与 MySQL 复制协议

**重要澄清**：\`CHANGE MASTER TO\` 是在**从库本地执行**的 SQL 命令，**不会发送给主库**！它只是配置从库的复制参数。

CDC 程序模拟从库时，直接使用 **MySQL 复制协议** 与主库通信。

---

## 🔍 根因揭秘：空集是任何集合的子集

### 完整函数调用链（主库端）

当主库收到 \`COM_BINLOG_DUMP_GTID\` 请求后，处理流程如下：

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│           主库处理 COM_BINLOG_DUMP_GTID 完整调用链                   │
│                   (MySQL/Percona Server 8.4.3)                      │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  dispatch_command()                                                 │
│  └── sql/sql_parse.cc:2433                                          │
│      │                                                              │
│      └──► com_binlog_dump_gtid()                                    │
│           └── sql/rpl_source.cc:973                                 │
│               │                                                     │
│               └──► mysql_binlog_send()                              │
│                    └── sql/rpl_source.cc:1043                       │
│                        │                                            │
│                        └──► Binlog_sender::run()                    │
│                             └── sql/rpl_binlog_sender.cc:387        │
│                                 │                                   │
│                                 └──► check_start_file() ⭐          │
│                                      └── sql/rpl_binlog_sender.cc:883│
│                                          │                          │
│                                          ├──► is_subset() ⚠️        │
│                                          │    └── sql/rpl_gtid_set.cc:1177│
│                                          │                          │
│                                          └──► find_first_log_not_in_gtid_set()│
│                                               └── sql/binlog.cc:4730│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### 问题根源：is_subset() 函数

\`\`\`cpp
// 📁 sql/rpl_gtid_set.cc:1177
// ⚠️ 空集是任何集合的子集，直接返回 true！
bool Gtid_set::is_subset(const Gtid_set *super) const {
  for (int sidno = 1; sidno <= max_sidno; sidno++) {
    // 如果 max_sidno 为 0（空集），循环不执行
  }
  return true;  // 空集永远返回 true
}
\`\`\`

---

## 🐹 Golang CDC 程序示例 (go-mysql)

### go-mysql 库简介

\`go-mysql\` 是 Go 语言中最流行的 MySQL binlog 解析库，由 \`go-mysql-org\` 维护。

\`\`\`bash
go get github.com/go-mysql-org/go-mysql
\`\`\`

---

## 📦 go-mysql 协议包构建详解

### MySQL 复制协议通信流程

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                go-mysql 与 MySQL 主库通信完整流程                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────┐                        ┌──────────────┐           │
│  │  go-mysql    │                        │   MySQL      │           │
│  │  CDC Client  │                        │   Master     │           │
│  └──────┬───────┘                        └──────┬───────┘           │
│         │                                       │                   │
│         │  ① TCP 连接建立                        │                   │
│         │ ─────────────────────────────────────►│                   │
│         │                                       │                   │
│         │  ② MySQL 握手包 (Handshake Packet)     │                   │
│         │ ◄─────────────────────────────────────│                   │
│         │                                       │                   │
│         │  ③ 认证响应包 (Auth Response)          │                   │
│         │ ─────────────────────────────────────►│                   │
│         │                                       │                   │
│         │  ④ OK 包 / ERR 包                      │                   │
│         │ ◄─────────────────────────────────────│                   │
│         │                                       │                   │
│         │  ⑤ COM_REGISTER_SLAVE (可选)          │                   │
│         │ ─────────────────────────────────────►│                   │
│         │                                       │                   │
│         │  ⑥ COM_BINLOG_DUMP_GTID ⭐            │                   │
│         │ ─────────────────────────────────────►│                   │
│         │                                       │                   │
│         │  ⑦ Binlog Event 流                    │                   │
│         │ ◄─────────────────────────────────────│                   │
│         │ ◄─────────────────────────────────────│                   │
│         │ ◄─────────────────────────────────────│                   │
│         │              ...                      │                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### MySQL 协议包通用结构

每个 MySQL 协议包都由 **Header（4字节）+ Payload** 组成：

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                    MySQL 协议包通用结构                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                     Header (4 bytes)                        │   │
│  ├─────────────┬─────────────┬─────────────┬──────────────────┤   │
│  │ payload_len │ payload_len │ payload_len │   sequence_id    │   │
│  │   [0:7]     │   [8:15]    │   [16:23]   │     [24:31]      │   │
│  │  (低8位)    │  (中8位)    │  (高8位)    │   (包序号)        │   │
│  ├─────────────┴─────────────┴─────────────┴──────────────────┤   │
│  │                     Payload (N bytes)                       │   │
│  │                   具体命令或数据内容                          │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  示例: 一个 10 字节 payload 的包                                     │
│  ┌────────────────────────────────────────────────────────────┐    │
│  │ 0A 00 00 01 | xx xx xx xx xx xx xx xx xx xx                │    │
│  │ └────┬────┘   └──────────────┬──────────────┘              │    │
│  │   Header         Payload (10 bytes)                        │    │
│  │ len=10, seq=1                                              │    │
│  └────────────────────────────────────────────────────────────┘    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### COM_REGISTER_SLAVE 包结构 (命令码: 0x15)

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│              COM_REGISTER_SLAVE 包结构 (可选命令)                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 偏移量 │  长度  │  字段名        │  说明                        ││
│  ├────────┼────────┼────────────────┼──────────────────────────────┤│
│  │   0    │   1    │  command       │  0x15 (COM_REGISTER_SLAVE)   ││
│  │   1    │   4    │  server_id     │  从库的 server_id            ││
│  │   5    │   1    │  host_len      │  主机名长度                   ││
│  │   6    │   N    │  hostname      │  主机名 (可为空)             ││
│  │  6+N   │   1    │  user_len      │  用户名长度                   ││
│  │  7+N   │   M    │  username      │  用户名 (可为空)             ││
│  │ 7+N+M  │   1    │  pass_len      │  密码长度                     ││
│  │ 8+N+M  │   K    │  password      │  密码 (可为空)               ││
│  │8+N+M+K │   2    │  port          │  端口号                       ││
│  │10+N+M+K│   4    │  repl_rank     │  复制排名 (已废弃，通常为0)   ││
│  │14+N+M+K│   4    │  master_id     │  主库 ID (由主库填充)        ││
│  └────────┴────────┴────────────────┴──────────────────────────────┘│
│                                                                     │
│  go-mysql 源码位置: replication/binlogsyncer.go                      │
│                                                                     │
│  示例 (server_id=100001):                                           │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 15                           # COM_REGISTER_SLAVE              ││
│  │ A1 86 01 00                  # server_id = 100001 (小端序)     ││
│  │ 00                           # hostname 长度 = 0               ││
│  │ 00                           # username 长度 = 0               ││
│  │ 00                           # password 长度 = 0               ││
│  │ 00 00                        # port = 0                        ││
│  │ 00 00 00 00                  # replication_rank = 0            ││
│  │ 00 00 00 00                  # master_id = 0 (由主库填充)      ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### COM_BINLOG_DUMP_GTID 包结构 (命令码: 0x1E) ⭐

这是 **GTID 模式下最核心的命令包**：

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│             COM_BINLOG_DUMP_GTID 包结构 (核心命令)                   │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 偏移量 │  长度  │  字段名           │  说明                     ││
│  ├────────┼────────┼───────────────────┼───────────────────────────┤│
│  │   0    │   1    │  command          │  0x1E (COM_BINLOG_DUMP_GTID)││
│  │   1    │   2    │  flags            │  标志位                    ││
│  │   3    │   4    │  server_id        │  从库的 server_id          ││
│  │   7    │   4    │  binlog_name_len  │  binlog文件名长度          ││
│  │   11   │   N    │  binlog_filename  │  binlog文件名(可为空)      ││
│  │  11+N  │   8    │  binlog_pos       │  起始位置(GTID模式通常为4) ││
│  │  19+N  │   4    │  gtid_data_len    │  GTID数据长度              ││
│  │  23+N  │   M    │  gtid_data        │  GTID集合的二进制编码      ││
│  └────────┴────────┴───────────────────┴───────────────────────────┘│
│                                                                     │
│  go-mysql 源码位置: replication/binlogsyncer.go                      │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### GTID 集合二进制编码格式

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                    GTID 集合二进制编码格式                           │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  GTID 集合编码结构:                                                  │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 偏移量 │  长度  │  字段名         │  说明                       ││
│  ├────────┼────────┼─────────────────┼─────────────────────────────┤│
│  │   0    │   8    │  n_sids         │  UUID 数量 (小端序)         ││
│  │   8    │  ...   │  sid_blocks[]   │  每个UUID的数据块           ││
│  └────────┴────────┴─────────────────┴─────────────────────────────┘│
│                                                                     │
│  每个 SID Block 结构:                                                │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 偏移量 │  长度  │  字段名         │  说明                       ││
│  ├────────┼────────┼─────────────────┼─────────────────────────────┤│
│  │   0    │   16   │  uuid           │  UUID (16字节二进制)        ││
│  │   16   │   8    │  n_intervals    │  区间数量                   ││
│  │   24   │  16*K  │  intervals[]    │  K个区间 (start, end)       ││
│  └────────┴────────┴─────────────────┴─────────────────────────────┘│
│                                                                     │
│  每个 Interval 结构:                                                 │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │   8 bytes: start (起始GNO，包含)                               ││
│  │   8 bytes: end   (结束GNO+1，不包含)                           ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  示例: a1a2a3a4-b1b2-c1c2-d1d2-e1e2e3e4e5e6:1-500                   │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 01 00 00 00 00 00 00 00      # n_sids = 1                      ││
│  │ a1 a2 a3 a4 b1 b2 c1 c2      # UUID 前8字节                    ││
│  │ d1 d2 e1 e2 e3 e4 e5 e6      # UUID 后8字节                    ││
│  │ 01 00 00 00 00 00 00 00      # n_intervals = 1                 ││
│  │ 01 00 00 00 00 00 00 00      # interval start = 1              ││
│  │ F5 01 00 00 00 00 00 00      # interval end = 501 (即1-500)    ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### go-mysql 构建 COM_BINLOG_DUMP_GTID 包的源码

\`\`\`go
// go-mysql 库中构建 COM_BINLOG_DUMP_GTID 包的核心代码
// 📁 replication/binlogsyncer.go

func (b *BinlogSyncer) writeBinlogDumpGTIDCommand(gset GTIDSet) error {
    // GTID 集合编码
    gtidData := gset.Encode()
    
    // 计算包大小
    // 1(cmd) + 2(flags) + 4(server_id) + 4(name_len) + len(name) + 
    // 8(pos) + 4(gtid_len) + len(gtid_data)
    
    data := make([]byte, 4+1+2+4+4+len(b.cfg.Flavor)+8+4+len(gtidData))
    pos := 4  // 跳过 header
    
    // 1. 命令码
    data[pos] = COM_BINLOG_DUMP_GTID  // 0x1E
    pos++
    
    // 2. flags (2 bytes)
    binary.LittleEndian.PutUint16(data[pos:], BINLOG_DUMP_NON_BLOCK)
    pos += 2
    
    // 3. server_id (4 bytes)
    binary.LittleEndian.PutUint32(data[pos:], b.cfg.ServerID)
    pos += 4
    
    // 4. binlog filename length (4 bytes)
    binary.LittleEndian.PutUint32(data[pos:], 0)  // GTID模式不需要文件名
    pos += 4
    
    // 5. binlog filename (0 bytes, GTID模式为空)
    
    // 6. binlog position (8 bytes)
    binary.LittleEndian.PutUint64(data[pos:], 4)  // 固定从4开始
    pos += 8
    
    // 7. GTID data length (4 bytes)
    binary.LittleEndian.PutUint32(data[pos:], uint32(len(gtidData)))
    pos += 4
    
    // 8. GTID data
    copy(data[pos:], gtidData)
    
    // 写入包头 (3字节长度 + 1字节序号)
    data[0] = byte(len(data) - 4)
    data[1] = byte((len(data) - 4) >> 8)
    data[2] = byte((len(data) - 4) >> 16)
    data[3] = 0  // sequence_id
    
    return b.c.WritePacket(data)
}
\`\`\`

### go-mysql 编码 GTID 集合的源码

\`\`\`go
// go-mysql 库中 GTID 集合编码的核心代码
// 📁 mysql/gtid.go

// MysqlGTIDSet 实现了 MySQL 的 GTID 集合
type MysqlGTIDSet struct {
    Sets map[string]*UUIDSet  // key: UUID string, value: GNO intervals
}

// Encode 将 GTID 集合编码为二进制格式
func (s *MysqlGTIDSet) Encode() []byte {
    var buf bytes.Buffer
    
    // 1. 写入 UUID 数量 (8 bytes, 小端序)
    n := uint64(len(s.Sets))
    binary.Write(&buf, binary.LittleEndian, n)
    
    // 2. 遍历每个 UUID
    for uuidStr, uuidSet := range s.Sets {
        // 2.1 写入 UUID (16 bytes)
        uuid, _ := uuid.Parse(uuidStr)
        buf.Write(uuid[:])
        
        // 2.2 写入区间数量 (8 bytes)
        intervals := uuidSet.Intervals
        binary.Write(&buf, binary.LittleEndian, uint64(len(intervals)))
        
        // 2.3 写入每个区间 (每个 16 bytes)
        for _, interval := range intervals {
            binary.Write(&buf, binary.LittleEndian, interval.Start)
            binary.Write(&buf, binary.LittleEndian, interval.Stop)  // Stop = End + 1
        }
    }
    
    return buf.Bytes()
}
\`\`\`

---

## 🔄 重连时的包构建差异

### 首次连接 vs 重连的对比

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                  首次连接 vs 重连 的包内容对比                        │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      首次连接                                │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │                                                             │   │
│  │  1. TCP 连接建立                                             │   │
│  │  2. MySQL 认证握手                                           │   │
│  │  3. COM_REGISTER_SLAVE (可选)                                │   │
│  │  4. COM_BINLOG_DUMP_GTID                                    │   │
│  │     ┌─────────────────────────────────────────────────┐     │   │
│  │     │ GTID 集合: (空) 或 从当前 @@gtid_executed 获取   │     │   │
│  │     │                                                 │     │   │
│  │     │ 示例: 首次启动，从主库当前位置开始                │     │   │
│  │     │ gtid_data = encode("b1b2b3b4-...:1-100")        │     │   │
│  │     └─────────────────────────────────────────────────┘     │   │
│  │                                                             │   │
│  │  5. 开始接收 Binlog Events                                   │   │
│  │                                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                      重连 (网络中断后)                        │   │
│  ├─────────────────────────────────────────────────────────────┤   │
│  │                                                             │   │
│  │  1. TCP 连接建立 (新连接)                                    │   │
│  │  2. MySQL 认证握手 (重新认证)                                │   │
│  │  3. COM_REGISTER_SLAVE (重新注册)                           │   │
│  │  4. COM_BINLOG_DUMP_GTID ⭐ 【关键差异】                     │   │
│  │     ┌─────────────────────────────────────────────────┐     │   │
│  │     │ GTID 集合: 包含已处理的所有 GTID                 │     │   │
│  │     │                                                 │     │   │
│  │     │ 示例: 已处理到 GNO=150                           │     │   │
│  │     │ gtid_data = encode("b1b2b3b4-...:1-150")        │     │   │
│  │     │                                                 │     │   │
│  │     │ 主库会从 GNO=151 开始发送                        │     │   │
│  │     └─────────────────────────────────────────────────┘     │   │
│  │                                                             │   │
│  │  5. 从上次断点继续接收 Binlog Events                         │   │
│  │                                                             │   │
│  └─────────────────────────────────────────────────────────────┘   │
│                                                                     │
│  ⚠️ 关键点：                                                        │
│  重连时的 GTID 集合 = 首次连接的 GTID + 已成功处理的增量 GTID        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### 重连时 GTID 集合的更新示例

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                     GTID 集合更新时间线                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  时间线 ──────────────────────────────────────────────────────────► │
│                                                                     │
│  T0: 首次连接                                                        │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 发送 GTID: b1b2b3b4-...:1-100                                  ││
│  │ 含义: "我已有 1-100，请从 101 开始发送"                         ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  T1-T50: 正常接收事件                                                │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 收到并处理: GNO 101, 102, 103, ... 150                         ││
│  │ 本地记录更新: b1b2b3b4-...:1-150                                ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  T51: 网络中断! ❌                                                   │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 连接断开，保存当前位置                                         ││
│  │ 已处理的 GTID: b1b2b3b4-...:1-150                              ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  T52: 重连                                                           │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 发送 GTID: b1b2b3b4-...:1-150  【与首次不同!】                  ││
│  │ 含义: "我已有 1-150，请从 151 开始发送"                         ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  T53+: 继续接收                                                      │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ 收到: GNO 151, 152, 153, ...                                   ││
│  │ 没有重复! ✅                                                    ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

### 跨集群切换时的问题场景 ⚠️

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                    跨集群切换时的问题场景                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  正常情况 (同集群重连):                                              │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ CDC 记录的 GTID: b1b2b3b4-...:1-150 (B集群UUID)                ││
│  │ B集群 executed:  b1b2b3b4-...:1-200 (B集群UUID)                ││
│  │                                                                ││
│  │ 结果: 主库从 151 开始发送 ✅                                    ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
│  问题场景 (跨集群切换):                                              │
│  ┌────────────────────────────────────────────────────────────────┐│
│  │ CDC 记录的 GTID: a1a2a3a4-...:1-500 (A集群UUID)                ││
│  │ B集群 executed:  b1b2b3b4-...:1-200 (B集群UUID) ← 完全不同!    ││
│  │ B集群 purged:    (空)                                          ││
│  │ B集群 binlog:    binlog.000001 (previous_gtids 为空)           ││
│  │                                                                ││
│  │ 检查过程:                                                      ││
│  │ 1. purged(空) ⊆ CDC的GTID → true (空集是任何集合的子集)       ││
│  │ 2. 查找 binlog: previous_gtids(空) ⊆ CDC的GTID → true         ││
│  │                                                                ││
│  │ 结果: 主库从 binlog.000001:4 开始发送全部数据! ❌               ││
│  └────────────────────────────────────────────────────────────────┘│
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

---

## 🔧 go-mysql 完整通信流程代码示例

### BinlogSyncer 的连接和重连流程

\`\`\`go
// go-mysql 库中 BinlogSyncer 的核心流程
// 📁 replication/binlogsyncer.go

type BinlogSyncer struct {
    cfg          BinlogSyncerConfig
    c            *client.Conn        // MySQL 连接
    running      bool
    lastGTIDSet  GTIDSet             // 记录已处理的 GTID
}

// StartSyncGTID 使用 GTID 模式开始同步
func (b *BinlogSyncer) StartSyncGTID(gset GTIDSet) (*BinlogStreamer, error) {
    // 保存起始 GTID 集合
    b.lastGTIDSet = gset.Clone()
    
    // 建立连接
    if err := b.prepare(); err != nil {
        return nil, err
    }
    
    // 发送 COM_BINLOG_DUMP_GTID
    if err := b.writeBinlogDumpGTIDCommand(gset); err != nil {
        return nil, err
    }
    
    return b.startStreamer(), nil
}

// prepare 建立连接并注册从库
func (b *BinlogSyncer) prepare() error {
    // 1. 建立 MySQL 连接
    var err error
    b.c, err = client.Connect(
        fmt.Sprintf("%s:%d", b.cfg.Host, b.cfg.Port),
        b.cfg.User,
        b.cfg.Password,
        "",
    )
    if err != nil {
        return err
    }
    
    // 2. 设置 checksum
    if err := b.c.Exec("SET @master_binlog_checksum='NONE'"); err != nil {
        // 忽略错误，某些版本不支持
    }
    
    // 3. 注册从库 (发送 COM_REGISTER_SLAVE)
    if err := b.registerSlave(); err != nil {
        return err
    }
    
    return nil
}

// registerSlave 发送 COM_REGISTER_SLAVE 命令
func (b *BinlogSyncer) registerSlave() error {
    // 构建 COM_REGISTER_SLAVE 包
    // 格式: [1]cmd + [4]server_id + [1]host_len + host + ...
    
    data := make([]byte, 4+1+4+1+1+1+2+4+4)
    pos := 4
    
    data[pos] = COM_REGISTER_SLAVE  // 0x15
    pos++
    
    binary.LittleEndian.PutUint32(data[pos:], b.cfg.ServerID)
    pos += 4
    
    // hostname, username, password 都设为空
    data[pos] = 0  // host_len
    pos++
    data[pos] = 0  // user_len
    pos++
    data[pos] = 0  // pass_len
    pos++
    
    binary.LittleEndian.PutUint16(data[pos:], 0)  // port
    pos += 2
    
    binary.LittleEndian.PutUint32(data[pos:], 0)  // repl_rank
    pos += 4
    
    binary.LittleEndian.PutUint32(data[pos:], 0)  // master_id
    pos += 4
    
    return b.c.WritePacket(data[:pos])
}

// retrySync 错误后重试同步
func (b *BinlogSyncer) retrySync() error {
    // 关闭旧连接
    b.c.Close()
    
    // 重新建立连接
    if err := b.prepare(); err != nil {
        return err
    }
    
    // ⭐ 关键: 使用更新后的 GTID 集合重新发送请求
    // b.lastGTIDSet 已经被 onGTIDEvent 更新为最新值
    return b.writeBinlogDumpGTIDCommand(b.lastGTIDSet)
}

// onGTIDEvent 处理 GTID 事件，更新本地记录
func (b *BinlogSyncer) onGTIDEvent(e *GTIDEvent) {
    // 更新 GTID 集合
    gtid := fmt.Sprintf("%s:%d", e.SID, e.GNO)
    b.lastGTIDSet.Update(gtid)
}
\`\`\`

### 事件接收和错误处理

\`\`\`go
// go-mysql 事件接收循环
// 📁 replication/binlogstreamer.go

type BinlogStreamer struct {
    syncer    *BinlogSyncer
    eventChan chan *BinlogEvent
    errChan   chan error
}

// GetEvent 获取下一个 binlog 事件
func (s *BinlogStreamer) GetEvent(ctx context.Context) (*BinlogEvent, error) {
    for {
        select {
        case <-ctx.Done():
            return nil, ctx.Err()
        case err := <-s.errChan:
            return nil, err
        case ev := <-s.eventChan:
            return ev, nil
        }
    }
}

// 内部事件读取循环
func (s *BinlogStreamer) run() {
    defer close(s.eventChan)
    
    for s.syncer.running {
        // 读取一个 binlog 事件包
        data, err := s.syncer.c.ReadPacket()
        if err != nil {
            // 网络错误，尝试重连
            if s.syncer.cfg.DisableRetrySync {
                s.errChan <- err
                return
            }
            
            // 尝试重连
            for retries := 0; retries < s.syncer.cfg.MaxReconnectAttempts; retries++ {
                time.Sleep(time.Duration(retries+1) * time.Second)
                
                if err := s.syncer.retrySync(); err != nil {
                    continue
                }
                
                // 重连成功
                break
            }
            continue
        }
        
        // 解析事件
        switch data[0] {
        case OK_HEADER:
            ev, err := s.parseEvent(data[1:])
            if err != nil {
                s.errChan <- err
                return
            }
            
            // 如果是 GTID 事件，更新记录
            if gtidEv, ok := ev.Event.(*GTIDEvent); ok {
                s.syncer.onGTIDEvent(gtidEv)
            }
            
            s.eventChan <- ev
            
        case ERR_HEADER:
            s.errChan <- s.parseError(data[1:])
            return
            
        case EOF_HEADER:
            // 主库关闭了 dump
            return
        }
    }
}
\`\`\`

---

## 📊 首次连接 vs 重连 协议包对比图

\`\`\`
┌─────────────────────────────────────────────────────────────────────┐
│                 首次连接 vs 重连 协议包对比                          │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  【首次连接】                          【重连】                      │
│                                                                     │
│  ┌─────────────────────────┐          ┌─────────────────────────┐  │
│  │ ① Handshake            │          │ ① Handshake (相同)      │  │
│  │    (MySQL握手)          │          │                         │  │
│  └───────────┬─────────────┘          └───────────┬─────────────┘  │
│              ↓                                    ↓                 │
│  ┌─────────────────────────┐          ┌─────────────────────────┐  │
│  │ ② Auth Response        │          │ ② Auth Response (相同)  │  │
│  │    (认证)               │          │                         │  │
│  └───────────┬─────────────┘          └───────────┬─────────────┘  │
│              ↓                                    ↓                 │
│  ┌─────────────────────────┐          ┌─────────────────────────┐  │
│  │ ③ COM_REGISTER_SLAVE   │          │ ③ COM_REGISTER_SLAVE   │  │
│  │    server_id: 100001    │          │    server_id: 100001    │  │
│  │    (相同)               │          │    (相同)               │  │
│  └───────────┬─────────────┘          └───────────┬─────────────┘  │
│              ↓                                    ↓                 │
│  ┌─────────────────────────┐          ┌─────────────────────────┐  │
│  │ ④ COM_BINLOG_DUMP_GTID │          │ ④ COM_BINLOG_DUMP_GTID │  │
│  │ ┌───────────────────┐   │          │ ┌───────────────────┐   │  │
│  │ │ command: 0x1E     │   │          │ │ command: 0x1E     │   │  │
│  │ │ flags: 0x0004     │   │          │ │ flags: 0x0004     │   │  │
│  │ │ server_id: 100001 │   │          │ │ server_id: 100001 │   │  │
│  │ │ filename_len: 0   │   │          │ │ filename_len: 0   │   │  │
│  │ │ position: 4       │   │          │ │ position: 4       │   │  │
│  │ │                   │   │          │ │                   │   │  │
│  │ │ gtid_set: ⭐      │   │          │ │ gtid_set: ⭐      │   │  │
│  │ │ b1b2...:1-100    │   │          │ │ b1b2...:1-150    │   │  │
│  │ │ (初始位置)        │   │          │ │ (更新后的位置)    │   │  │
│  │ └───────────────────┘   │          │ └───────────────────┘   │  │
│  └───────────┬─────────────┘          └───────────┬─────────────┘  │
│              ↓                                    ↓                 │
│  ┌─────────────────────────┐          ┌─────────────────────────┐  │
│  │ ⑤ 收到 Event:          │          │ ⑤ 收到 Event:          │  │
│  │    GNO 101, 102...      │          │    GNO 151, 152...      │  │
│  │    (从101开始)          │          │    (从151继续) ✅       │  │
│  └─────────────────────────┘          └─────────────────────────┘  │
│                                                                     │
│  ════════════════════════════════════════════════════════════════  │
│  核心差异: COM_BINLOG_DUMP_GTID 中的 gtid_set 内容不同              │
│  ════════════════════════════════════════════════════════════════  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
\`\`\`

---

## 🛠️ 生产级 go-mysql CDC 程序完整示例

\`\`\`go
package main

import (
    "context"
    "database/sql"
    "encoding/json"
    "fmt"
    "log"
    "os"
    "os/signal"
    "sync"
    "syscall"
    "time"

    _ "github.com/go-sql-driver/mysql"
    "github.com/go-mysql-org/go-mysql/mysql"
    "github.com/go-mysql-org/go-mysql/replication"
)

// Position 位置信息
type Position struct {
    GTIDSet    string \`json:"gtid_set"\`
    MasterUUID string \`json:"master_uuid"\`
    UpdatedAt  string \`json:"updated_at"\`
}

// CDCClient CDC客户端
type CDCClient struct {
    cfg           CDCConfig
    syncer        *replication.BinlogSyncer
    position      Position
    positionMutex sync.RWMutex
    retryInterval time.Duration
}

type CDCConfig struct {
    Host            string
    Port            uint16
    User            string
    Password        string
    ServerID        uint32
    PositionFile    string
    StrictUUIDCheck bool
}

func NewCDCClient(cfg CDCConfig) *CDCClient {
    return &CDCClient{
        cfg:           cfg,
        retryInterval: time.Second,
    }
}

func (c *CDCClient) Start(ctx context.Context) error {
    c.loadPosition()

    for {
        select {
        case <-ctx.Done():
            return nil
        default:
        }

        if err := c.runOnce(ctx); err != nil {
            if err == context.Canceled {
                return nil
            }
            log.Printf("错误: %v, %v后重试", err, c.retryInterval)
            time.Sleep(c.retryInterval)
            c.retryInterval = min(c.retryInterval*2, 60*time.Second)
        }
    }
}

func (c *CDCClient) runOnce(ctx context.Context) error {
    // 1. 检查主库UUID
    masterUUID, err := c.getMasterUUID()
    if err != nil {
        return fmt.Errorf("获取主库UUID失败: %w", err)
    }

    // 2. 跨集群检测
    if c.cfg.StrictUUIDCheck && c.position.MasterUUID != "" &&
        c.position.MasterUUID != masterUUID {
        return fmt.Errorf("主库UUID变化: %s -> %s", c.position.MasterUUID, masterUUID)
    }
    c.position.MasterUUID = masterUUID

    // 3. 创建Syncer
    syncerCfg := replication.BinlogSyncerConfig{
        ServerID: c.cfg.ServerID,
        Flavor:   "mysql",
        Host:     c.cfg.Host,
        Port:     c.cfg.Port,
        User:     c.cfg.User,
        Password: c.cfg.Password,
    }
    c.syncer = replication.NewBinlogSyncer(syncerCfg)
    defer c.syncer.Close()

    // 4. 获取GTID
    var gtidSet mysql.GTIDSet
    if c.position.GTIDSet != "" {
        gtidSet, _ = mysql.ParseMysqlGTIDSet(c.position.GTIDSet)
    } else {
        gtidSet, _ = c.getCurrentGTIDSet()
    }

    // 5. 开始同步
    streamer, err := c.syncer.StartSyncGTID(gtidSet)
    if err != nil {
        return err
    }

    c.retryInterval = time.Second
    log.Printf("开始同步, GTID: %s", gtidSet.String())

    // 6. 事件循环
    for {
        select {
        case <-ctx.Done():
            c.savePosition()
            return ctx.Err()
        default:
        }

        ev, err := streamer.GetEvent(ctx)
        if err != nil {
            c.savePosition()
            return err
        }

        c.handleEvent(ev)
    }
}

func (c *CDCClient) handleEvent(ev *replication.BinlogEvent) {
    switch e := ev.Event.(type) {
    case *replication.GTIDEvent:
        c.updateGTID(e)
    case *replication.RowsEvent:
        log.Printf("[%s] %s.%s", ev.Header.EventType, e.Table.Schema, e.Table.Table)
    }
}

func (c *CDCClient) updateGTID(e *replication.GTIDEvent) {
    c.positionMutex.Lock()
    defer c.positionMutex.Unlock()

    newGTID := fmt.Sprintf("%s:%d", e.SID.String(), e.GNO)
    if c.position.GTIDSet == "" {
        c.position.GTIDSet = newGTID
    } else {
        gtidSet, _ := mysql.ParseMysqlGTIDSet(c.position.GTIDSet)
        newSet, _ := mysql.ParseMysqlGTIDSet(newGTID)
        gtidSet.Update(newSet)
        c.position.GTIDSet = gtidSet.String()
    }
    c.position.UpdatedAt = time.Now().Format(time.RFC3339)
}

func (c *CDCClient) getMasterUUID() (string, error) {
    dsn := fmt.Sprintf("%s:%s@tcp(%s:%d)/", c.cfg.User, c.cfg.Password, c.cfg.Host, c.cfg.Port)
    db, _ := sql.Open("mysql", dsn)
    defer db.Close()
    var uuid string
    db.QueryRow("SELECT @@server_uuid").Scan(&uuid)
    return uuid, nil
}

func (c *CDCClient) getCurrentGTIDSet() (mysql.GTIDSet, error) {
    dsn := fmt.Sprintf("%s:%s@tcp(%s:%d)/", c.cfg.User, c.cfg.Password, c.cfg.Host, c.cfg.Port)
    db, _ := sql.Open("mysql", dsn)
    defer db.Close()
    var gtid string
    db.QueryRow("SELECT @@gtid_executed").Scan(&gtid)
    return mysql.ParseMysqlGTIDSet(gtid)
}

func (c *CDCClient) loadPosition() {
    if c.cfg.PositionFile == "" {
        return
    }
    data, _ := os.ReadFile(c.cfg.PositionFile)
    json.Unmarshal(data, &c.position)
}

func (c *CDCClient) savePosition() {
    if c.cfg.PositionFile == "" {
        return
    }
    c.positionMutex.RLock()
    data, _ := json.MarshalIndent(c.position, "", "  ")
    c.positionMutex.RUnlock()
    os.WriteFile(c.cfg.PositionFile, data, 0644)
}

func main() {
    cfg := CDCConfig{
        Host:            "127.0.0.1",
        Port:            3306,
        User:            "repl_user",
        Password:        "repl_pass",
        ServerID:        100001,
        PositionFile:    "/tmp/cdc_position.json",
        StrictUUIDCheck: true,
    }

    client := NewCDCClient(cfg)
    ctx, cancel := context.WithCancel(context.Background())

    sigChan := make(chan os.Signal, 1)
    signal.Notify(sigChan, syscall.SIGINT, syscall.SIGTERM)
    go func() {
        <-sigChan
        cancel()
    }()

    client.Start(ctx)
}
\`\`\`

---

## 📝 总结与反思

### 核心发现

| **要点** | **说明** |
|:---:|:---|
| **CHANGE MASTER TO** | 从库本地命令，不发送给主库 |
| **COM_BINLOG_DUMP_GTID** | CDC/从库实际发送给主库的请求 |
| **数学原理** | 空集是任何集合的子集 |
| **重连关键** | GTID 集合需要包含已处理的所有事务 |

### 协议包对比

| **场景** | **GTID 集合内容** | **主库响应** |
|:---:|:---|:---|
| 首次连接 | 初始 GTID (如 1-100) | 从 101 开始发送 |
| 重连 | 更新后的 GTID (如 1-150) | 从 151 继续发送 |
| 跨集群切换 | A集群 GTID | ⚠️ 从头发送全部! |

### 相关源码文件速查

| **文件** | **功能** | **关键行号** |
|:---|:---|:---:|
| \`sql/sql_parse.cc\` | 命令分发入口 | 2433 |
| \`sql/rpl_source.cc\` | COM_BINLOG_DUMP_GTID 处理 | 973-1048 |
| \`sql/rpl_binlog_sender.cc\` | Binlog 发送逻辑 | 883 |
| \`sql/rpl_gtid_set.cc\` | GTID 子集判断 | 1177 |
| \`sql/binlog.cc\` | 查找起始文件 | 4730 |

---

> 📢 **如果这篇文章对你有帮助，欢迎点赞、转发、关注！**

---

**参考资料**：
- MySQL 8.4.3 源码
- [go-mysql-org/go-mysql](https://github.com/go-mysql-org/go-mysql)
- [MySQL 复制协议文档](https://dev.mysql.com/doc/dev/mysql-server/latest/page_protocol_replication.html)
