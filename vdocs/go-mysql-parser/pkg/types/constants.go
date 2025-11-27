package types

// 包相关常量
const (
// MaxPacketSize MySQL单个包的最大payload大小 (2^24 - 1)
MaxPacketSize = 0xFFFFFF // 16777215 字节

// PacketHeaderSize 包头大小：3字节长度 + 1字节序列号
PacketHeaderSize = 4
)

// 包类型标识字节
const (
// PacketOK OK包标识 (当包长度>7时，0x00或0xFE)
PacketOK byte = 0x00

// PacketEOF EOF包标识 (当包长度<9时，0xFE)
PacketEOF byte = 0xFE

// PacketERR ERR包标识
PacketERR byte = 0xFF
)

// 服务器命令类型 (enum_server_command)
const (
COM_SLEEP               byte = 0x00 // 内部使用
COM_QUIT                byte = 0x01 // 关闭连接
COM_INIT_DB             byte = 0x02 // 切换数据库
COM_QUERY               byte = 0x03 // SQL查询
COM_FIELD_LIST          byte = 0x04 // 获取字段列表（已弃用）
COM_CREATE_DB           byte = 0x05 // 创建数据库（已弃用）
COM_DROP_DB             byte = 0x06 // 删除数据库（已弃用）
COM_REFRESH             byte = 0x07 // 刷新（已移除）
COM_SHUTDOWN            byte = 0x08 // 关闭服务器（已移除）
COM_STATISTICS          byte = 0x09 // 获取统计信息
COM_PROCESS_INFO        byte = 0x0A // 获取进程列表（已移除）
COM_CONNECT             byte = 0x0B // 内部使用
COM_PROCESS_KILL        byte = 0x0C // 杀死进程（已移除）
COM_DEBUG               byte = 0x0D // 转储调试信息
COM_PING                byte = 0x0E // Ping服务器
COM_TIME                byte = 0x0F // 内部使用
COM_DELAYED_INSERT      byte = 0x10 // 延迟插入（已移除）
COM_CHANGE_USER         byte = 0x11 // 改变用户
COM_BINLOG_DUMP         byte = 0x12 // Binlog转储
COM_TABLE_DUMP          byte = 0x13 // 表转储
COM_CONNECT_OUT         byte = 0x14 // 内部使用
COM_REGISTER_SLAVE      byte = 0x15 // 注册从服务器
COM_STMT_PREPARE        byte = 0x16 // 预处理语句
COM_STMT_EXECUTE        byte = 0x17 // 执行预处理语句
COM_STMT_SEND_LONG_DATA byte = 0x18 // 发送长数据
COM_STMT_CLOSE          byte = 0x19 // 关闭预处理语句
COM_STMT_RESET          byte = 0x1A // 重置预处理语句
COM_SET_OPTION          byte = 0x1B // 设置选项
COM_STMT_FETCH          byte = 0x1C // 获取预处理语句结果
COM_DAEMON              byte = 0x1D // 守护进程
COM_BINLOG_DUMP_GTID    byte = 0x1E // GTID Binlog转储
COM_RESET_CONNECTION    byte = 0x1F // 重置连接
COM_CLONE               byte = 0x20 // 克隆
COM_END                 byte = 0x21 // 结束标记
)

// 客户端能力标志 (Client Capability Flags)
const (
CLIENT_LONG_PASSWORD                  uint32 = 1 << 0  // 0x00000001
CLIENT_FOUND_ROWS                     uint32 = 1 << 1  // 0x00000002
CLIENT_LONG_FLAG                      uint32 = 1 << 2  // 0x00000004
CLIENT_CONNECT_WITH_DB                uint32 = 1 << 3  // 0x00000008
CLIENT_NO_SCHEMA                      uint32 = 1 << 4  // 0x00000010
CLIENT_COMPRESS                       uint32 = 1 << 5  // 0x00000020
CLIENT_ODBC                           uint32 = 1 << 6  // 0x00000040
CLIENT_LOCAL_FILES                    uint32 = 1 << 7  // 0x00000080
CLIENT_IGNORE_SPACE                   uint32 = 1 << 8  // 0x00000100
CLIENT_PROTOCOL_41                    uint32 = 1 << 9  // 0x00000200
CLIENT_INTERACTIVE                    uint32 = 1 << 10 // 0x00000400
CLIENT_SSL                            uint32 = 1 << 11 // 0x00000800
CLIENT_IGNORE_SIGPIPE                 uint32 = 1 << 12 // 0x00001000
CLIENT_TRANSACTIONS                   uint32 = 1 << 13 // 0x00002000
CLIENT_RESERVED                       uint32 = 1 << 14 // 0x00004000
CLIENT_RESERVED2                      uint32 = 1 << 15 // 0x00008000
CLIENT_MULTI_STATEMENTS               uint32 = 1 << 16 // 0x00010000
CLIENT_MULTI_RESULTS                  uint32 = 1 << 17 // 0x00020000
CLIENT_PS_MULTI_RESULTS               uint32 = 1 << 18 // 0x00040000
CLIENT_PLUGIN_AUTH                    uint32 = 1 << 19 // 0x00080000
CLIENT_CONNECT_ATTRS                  uint32 = 1 << 20 // 0x00100000
CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA uint32 = 1 << 21 // 0x00200000
CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS   uint32 = 1 << 22 // 0x00400000
CLIENT_SESSION_TRACK                  uint32 = 1 << 23 // 0x00800000
CLIENT_DEPRECATE_EOF                  uint32 = 1 << 24 // 0x01000000
CLIENT_OPTIONAL_RESULTSET_METADATA    uint32 = 1 << 25 // 0x02000000
CLIENT_ZSTD_COMPRESSION_ALGORITHM     uint32 = 1 << 26 // 0x04000000
CLIENT_QUERY_ATTRIBUTES               uint32 = 1 << 27 // 0x08000000
CLIENT_MULTI_FACTOR_AUTHENTICATION    uint32 = 1 << 28 // 0x10000000
CLIENT_CAPABILITY_EXTENSION           uint32 = 1 << 29 // 0x20000000
CLIENT_SSL_VERIFY_SERVER_CERT         uint32 = 1 << 30 // 0x40000000
CLIENT_REMEMBER_OPTIONS               uint32 = 1 << 31 // 0x80000000
)

// 服务器状态标志 (Server Status Flags)
const (
SERVER_STATUS_IN_TRANS             uint16 = 0x0001 // 事务中
SERVER_STATUS_AUTOCOMMIT           uint16 = 0x0002 // 自动提交启用
SERVER_MORE_RESULTS_EXISTS         uint16 = 0x0008 // 还有更多结果
SERVER_STATUS_NO_GOOD_INDEX_USED   uint16 = 0x0010 // 没有使用好的索引
SERVER_STATUS_NO_INDEX_USED        uint16 = 0x0020 // 没有使用索引
SERVER_STATUS_CURSOR_EXISTS        uint16 = 0x0040 // 游标存在
SERVER_STATUS_LAST_ROW_SENT        uint16 = 0x0080 // 最后一行已发送
SERVER_STATUS_DB_DROPPED           uint16 = 0x0100 // 数据库已删除
SERVER_STATUS_NO_BACKSLASH_ESCAPES uint16 = 0x0200 // 不使用反斜杠转义
SERVER_STATUS_METADATA_CHANGED     uint16 = 0x0400 // 元数据已改变
SERVER_QUERY_WAS_SLOW              uint16 = 0x0800 // 查询很慢
SERVER_PS_OUT_PARAMS               uint16 = 0x1000 // 预处理语句输出参数
SERVER_STATUS_IN_TRANS_READONLY    uint16 = 0x2000 // 只读事务中
SERVER_SESSION_STATE_CHANGED       uint16 = 0x4000 // 会话状态已改变
)

// 字符集常量
const (
CHARSET_BIG5        byte = 1
CHARSET_LATIN2      byte = 2
CHARSET_DEC8        byte = 3
CHARSET_CP850       byte = 4
CHARSET_LATIN1      byte = 5
CHARSET_HP8         byte = 6
CHARSET_KOI8R       byte = 7
CHARSET_LATIN1_BIN  byte = 8
CHARSET_LATIN2_BIN  byte = 9
CHARSET_SWE7        byte = 10
CHARSET_ASCII       byte = 11
CHARSET_UJIS        byte = 12
CHARSET_SJIS        byte = 13
CHARSET_HEBREW      byte = 14
CHARSET_TIS620      byte = 15
CHARSET_EUCKR       byte = 16
CHARSET_KOI8U       byte = 17
CHARSET_GB2312      byte = 18
CHARSET_GREEK       byte = 19
CHARSET_CP1250      byte = 20
CHARSET_GBK         byte = 21
CHARSET_LATIN5      byte = 22
CHARSET_ARMSCII8    byte = 23
CHARSET_UTF8        byte = 24
CHARSET_UCS2        byte = 35
CHARSET_CP866       byte = 36
CHARSET_KEYBCS2     byte = 37
CHARSET_MACCE       byte = 38
CHARSET_MACROMAN    byte = 39
CHARSET_CP852       byte = 40
CHARSET_LATIN7      byte = 41
CHARSET_UTF8MB4     byte = 45
CHARSET_CP1251      byte = 51
CHARSET_UTF16       byte = 54
CHARSET_UTF16LE     byte = 56
CHARSET_CP1256      byte = 57
CHARSET_CP1257      byte = 59
CHARSET_UTF32       byte = 60
CHARSET_BINARY      byte = 63
CHARSET_GEOSTD8     byte = 92
CHARSET_CP932       byte = 95
CHARSET_EUCJPMS     byte = 97
CHARSET_UTF8MB4_BIN byte = 46
)

// MySQL字段类型 (Field Types)
const (
MYSQL_TYPE_DECIMAL     byte = 0x00
MYSQL_TYPE_TINY        byte = 0x01
MYSQL_TYPE_SHORT       byte = 0x02
MYSQL_TYPE_LONG        byte = 0x03
MYSQL_TYPE_FLOAT       byte = 0x04
MYSQL_TYPE_DOUBLE      byte = 0x05
MYSQL_TYPE_NULL        byte = 0x06
MYSQL_TYPE_TIMESTAMP   byte = 0x07
MYSQL_TYPE_LONGLONG    byte = 0x08
MYSQL_TYPE_INT24       byte = 0x09
MYSQL_TYPE_DATE        byte = 0x0A
MYSQL_TYPE_TIME        byte = 0x0B
MYSQL_TYPE_DATETIME    byte = 0x0C
MYSQL_TYPE_YEAR        byte = 0x0D
MYSQL_TYPE_NEWDATE     byte = 0x0E
MYSQL_TYPE_VARCHAR     byte = 0x0F
MYSQL_TYPE_BIT         byte = 0x10
MYSQL_TYPE_TIMESTAMP2  byte = 0x11
MYSQL_TYPE_DATETIME2   byte = 0x12
MYSQL_TYPE_TIME2       byte = 0x13
MYSQL_TYPE_JSON        byte = 0xF5
MYSQL_TYPE_NEWDECIMAL  byte = 0xF6
MYSQL_TYPE_ENUM        byte = 0xF7
MYSQL_TYPE_SET         byte = 0xF8
MYSQL_TYPE_TINY_BLOB   byte = 0xF9
MYSQL_TYPE_MEDIUM_BLOB byte = 0xFA
MYSQL_TYPE_LONG_BLOB   byte = 0xFB
MYSQL_TYPE_BLOB        byte = 0xFC
MYSQL_TYPE_VAR_STRING  byte = 0xFD
MYSQL_TYPE_STRING      byte = 0xFE
MYSQL_TYPE_GEOMETRY    byte = 0xFF
)

// 字段标志 (Field Flags)
const (
NOT_NULL_FLAG         uint32 = 1 << 0  // 字段不能为NULL
PRI_KEY_FLAG          uint32 = 1 << 1  // 字段是主键的一部分
UNIQUE_KEY_FLAG       uint32 = 1 << 2  // 字段是唯一键的一部分
MULTIPLE_KEY_FLAG     uint32 = 1 << 3  // 字段是键的一部分
BLOB_FLAG             uint32 = 1 << 4  // 字段是BLOB
UNSIGNED_FLAG         uint32 = 1 << 5  // 字段是无符号的
ZEROFILL_FLAG         uint32 = 1 << 6  // 字段是零填充的
BINARY_FLAG           uint32 = 1 << 7  // 字段是二进制的
ENUM_FLAG             uint32 = 1 << 8  // 字段是枚举
AUTO_INCREMENT_FLAG   uint32 = 1 << 9  // 字段是自增的
TIMESTAMP_FLAG        uint32 = 1 << 10 // 字段是时间戳
SET_FLAG              uint32 = 1 << 11 // 字段是集合
NO_DEFAULT_VALUE_FLAG uint32 = 1 << 12 // 字段没有默认值
ON_UPDATE_NOW_FLAG    uint32 = 1 << 13 // 字段在UPDATE时设置为NOW
NUM_FLAG              uint32 = 1 << 15 // 字段是数字
)

// 认证相关常量
const (
// SCRAMBLE_LENGTH 随机字符串长度（握手包中的认证数据）
SCRAMBLE_LENGTH = 20

// AUTH_PLUGIN_DATA_PART_1_LENGTH 认证数据第一部分长度
AUTH_PLUGIN_DATA_PART_1_LENGTH = 8

// PROTOCOL_VERSION MySQL协议版本号
PROTOCOL_VERSION = 10
)

// 默认端口
const (
// DEFAULT_MYSQL_PORT MySQL默认端口
DEFAULT_MYSQL_PORT = 3306
)
