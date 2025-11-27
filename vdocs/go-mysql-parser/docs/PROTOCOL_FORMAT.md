# MySQL 二进制通信协议格式

本文档详细描述了MySQL客户端-服务器通信协议的二进制格式，基于Percona Server 8.4.3-3源码分析。

## 协议概述

MySQL协议是一个基于TCP的应用层协议，使用二进制格式传输数据。协议分为两个主要阶段：

1. **连接阶段（Connection Phase）**：握手和认证
2. **命令阶段（Command Phase）**：执行命令和返回结果

## 包格式（Packet Format）

所有MySQL数据都以包（packet）的形式传输。每个包的格式如下：

### 包头（Packet Header）- 4字节

```
+-------------------+-------------------+-------------------+-------------------+
| payload_length[0] | payload_length[1] | payload_length[2] |   sequence_id     |
+-------------------+-------------------+-------------------+-------------------+
      1 byte              1 byte              1 byte              1 byte
```

字段说明：
- **payload_length** (3字节)：payload的长度，小端序
  - 最大值：16777215 (2^24 - 1) = 0xFFFFFF
  - 值为包体的字节数，不包括包头的4字节
- **sequence_id** (1字节)：包序列号
  - 从0开始，每发送一个包递增1
  - 新命令开始时重置为0
  - 可以回绕（255后变为0）

### 包体（Packet Payload）

包体包含实际的数据，格式取决于包的类型。

### 大包处理（> 16MB）

如果数据超过16MB（0xFFFFFF字节），需要分成多个包：

1. 第一个包：长度=0xFFFFFF，包含前16MB数据
2. 第二个包：长度=剩余数据长度或0xFFFFFF
3. 如此重复，直到最后一个包长度<0xFFFFFF

示例：发送16777215字节的数据
```
FF FF FF 00 ... (16777215字节数据)
00 00 00 01     (空包，表示结束)
```

## 数据类型编码

### 1. 固定长度整数（Fixed-Length Integer）

- **int<1>**：1字节无符号整数
- **int<2>**：2字节无符号整数，小端序
- **int<3>**：3字节无符号整数，小端序
- **int<4>**：4字节无符号整数，小端序
- **int<8>**：8字节无符号整数，小端序

示例：
```
int<1>: 0x01
int<2>: 0x01 0x00  (值为1)
int<3>: 0x01 0x00 0x00  (值为1)
int<4>: 0x01 0x00 0x00 0x00  (值为1)
```

### 2. 变长整数（Length-Encoded Integer）

根据第一个字节的值决定整数的长度：

- 如果 < 0xFB (251)：该字节就是值
- 如果 = 0xFC：后面2字节是值（小端序）
- 如果 = 0xFD：后面3字节是值（小端序）
- 如果 = 0xFE：后面8字节是值（小端序）
- 如果 = 0xFF：保留（用于标识ERR包）

示例：
```
250:     FA
251:     FC FB 00
65536:   FD 00 00 01
16777216: FE 00 00 00 01 00 00 00 00
```

### 3. 字符串类型

#### 固定长度字符串（Fixed-Length String）

```
string[n]：固定n字节的字符串
```

#### Null结尾字符串（Null-Terminated String）

```
string<NUL>：以0x00结尾的字符串
```

示例：
```
"mysql" -> 6D 79 73 71 6C 00
```

#### 变长字符串（Length-Encoded String）

```
length-encoded string：
  1. length-encoded integer（字符串长度）
  2. string[length]（字符串内容）
```

示例：
```
"mysql" -> 05 6D 79 73 71 6C
```

#### EOF字符串（Rest-of-Packet String）

```
string<EOF>：从当前位置到包末尾的所有字节
```

## 包类型标识

第一个字节通常用于标识包类型：

- **0x00**：OK包（当包长度>7时）
- **0xFE**：EOF包（当包长度<9时）或OK包（当包长度>7时）
- **0xFF**：ERR包
- **0x01-0x1F**：命令包（COM_*）
- 其他：结果集数据或列定义

## 能力标志（Capability Flags）

客户端和服务器通过能力标志协商协议特性（4字节位掩码）：

```c
CLIENT_LONG_PASSWORD     = 0x00000001  // 使用改进的旧密码
CLIENT_FOUND_ROWS        = 0x00000002  // 返回找到的行数而不是影响的行数
CLIENT_LONG_FLAG         = 0x00000004  // 获取所有列标志
CLIENT_CONNECT_WITH_DB   = 0x00000008  // 可以在连接时指定数据库
CLIENT_NO_SCHEMA         = 0x00000010  // 不允许database.table.column
CLIENT_COMPRESS          = 0x00000020  // 可以使用压缩协议
CLIENT_ODBC              = 0x00000040  // ODBC客户端
CLIENT_LOCAL_FILES       = 0x00000080  // 可以使用LOAD DATA LOCAL
CLIENT_IGNORE_SPACE      = 0x00000100  // 忽略函数名后的空格
CLIENT_PROTOCOL_41       = 0x00000200  // 使用4.1协议
CLIENT_INTERACTIVE       = 0x00000400  // 这是一个交互式客户端
CLIENT_SSL               = 0x00000800  // 切换到SSL
CLIENT_IGNORE_SIGPIPE    = 0x00001000  // 忽略SIGPIPE
CLIENT_TRANSACTIONS      = 0x00002000  // 客户端知道事务
CLIENT_RESERVED          = 0x00004000  // 旧标志：4.1协议
CLIENT_RESERVED2         = 0x00008000  // 旧标志：4.1认证
CLIENT_MULTI_STATEMENTS  = 0x00010000  // 支持多语句
CLIENT_MULTI_RESULTS     = 0x00020000  // 支持多结果集
CLIENT_PS_MULTI_RESULTS  = 0x00040000  // 预处理语句支持多结果集
CLIENT_PLUGIN_AUTH       = 0x00080000  // 支持插件认证
CLIENT_CONNECT_ATTRS     = 0x00100000  // 连接属性
CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA = 0x00200000  // 认证数据使用变长编码
CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   = 0x00400000  // 可以处理过期密码
CLIENT_SESSION_TRACK     = 0x00800000  // 会话状态跟踪
CLIENT_DEPRECATE_EOF     = 0x01000000  // 废弃EOF包，使用OK包替代
CLIENT_ZSTD_COMPRESSION_ALGORITHM = 0x00010000  // 支持zstd压缩
```

## 服务器状态标志（Server Status Flags）

```c
SERVER_STATUS_IN_TRANS          = 0x0001  // 事务中
SERVER_STATUS_AUTOCOMMIT        = 0x0002  // 自动提交
SERVER_MORE_RESULTS_EXISTS      = 0x0008  // 还有更多结果
SERVER_STATUS_NO_GOOD_INDEX_USED = 0x0010  // 没有使用好的索引
SERVER_STATUS_NO_INDEX_USED     = 0x0020  // 没有使用索引
SERVER_STATUS_CURSOR_EXISTS     = 0x0040  // 只读游标存在
SERVER_STATUS_LAST_ROW_SENT     = 0x0080  // 最后一行已发送
SERVER_STATUS_DB_DROPPED        = 0x0100  // 数据库被删除
SERVER_STATUS_NO_BACKSLASH_ESCAPES = 0x0200  // 不使用反斜杠转义
SERVER_STATUS_METADATA_CHANGED  = 0x0400  // 元数据已改变
SERVER_QUERY_WAS_SLOW           = 0x0800  // 查询很慢
SERVER_PS_OUT_PARAMS            = 0x1000  // 预处理语句输出参数
SERVER_STATUS_IN_TRANS_READONLY = 0x2000  // 只读事务中
SERVER_SESSION_STATE_CHANGED    = 0x4000  // 会话状态已改变
```

## 字符集编号

常用字符集编号（1字节）：

```
1   = big5
2   = latin2  
3   = dec8
4   = cp850
5   = latin1
6   = hp8
7   = koi8r
8   = latin1 (default)
9   = latin2
10  = swe7
11  = ascii
12  = ujis
13  = sjis
14  = hebrew
15  = tis620
16  = euckr
17  = koi8u
18  = gb2312
19  = greek
20  = cp1250
21  = gbk
22  = latin5
23  = armscii8
24  = utf8
28  = cp866
29  = keybcs2
30  = macce
31  = macroman
32  = cp852
33  = latin7
34  = cp1251
35  = cp1256
36  = cp1257
37  = binary
38  = geostd8
39  = cp932
40  = eucjpms
45  = utf8mb4
63  = binary (default for binary data)
```

## 参考资料

1. [MySQL Protocol Documentation](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html)
2. Percona Server 8.4.3-3 源码：
   - `sql-common/net_serv.cc` - 包处理
   - `sql-common/client.cc` - 客户端实现
   - `sql/protocol_classic.cc` - 服务器端协议实现
   - `include/mysql_com.h` - 常量定义
