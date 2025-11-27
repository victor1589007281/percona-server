package types

import "fmt"

// Packet 表示一个MySQL协议包
type Packet struct {
SequenceID byte   // 包序列号
Payload    []byte // 包负载数据
}

// PacketHeader 表示包头信息
type PacketHeader struct {
PayloadLength uint32 // Payload长度 (3字节)
SequenceID    byte   // 序列号 (1字节)
}

// HandshakeV10 服务器握手包 (Protocol Version 10)
type HandshakeV10 struct {
ProtocolVersion    byte     // 协议版本，通常是10
ServerVersion      string   // 服务器版本字符串
ConnectionID       uint32   // 连接ID（线程ID）
AuthPluginData     []byte   // 认证数据（20字节：8+12或8+13）
CapabilityFlags    uint32   // 服务器能力标志（4字节）
CharacterSet       byte     // 默认字符集
StatusFlags        uint16   // 服务器状态标志
AuthPluginName     string   // 认证插件名称
AuthPluginDataLen  byte     // 认证数据总长度
}

// HandshakeResponse41 客户端握手响应包 (Protocol 4.1)
type HandshakeResponse41 struct {
CapabilityFlags uint32            // 客户端能力标志
MaxPacketSize   uint32            // 最大包大小
CharacterSet    byte              // 字符集
Username        string            // 用户名
AuthResponse    []byte            // 认证响应数据
Database        string            // 数据库名（可选）
AuthPluginName  string            // 认证插件名（可选）
ConnectAttrs    map[string]string // 连接属性（可选）
}

// OKPacket OK响应包
type OKPacket struct {
Header          byte   // 0x00 或 0xFE
AffectedRows    uint64 // 受影响的行数
LastInsertID    uint64 // 最后插入的ID
StatusFlags     uint16 // 服务器状态标志
Warnings        uint16 // 警告数量
Info            string // 附加信息
SessionStateInfo string // 会话状态信息（如果有）
}

// ERRPacket 错误响应包
type ERRPacket struct {
Header       byte   // 0xFF
ErrorCode    uint16 // 错误代码
SQLState     string // SQL状态（5字节）
ErrorMessage string // 错误消息
}

// EOFPacket EOF响应包
type EOFPacket struct {
Header      byte   // 0xFE
Warnings    uint16 // 警告数量
StatusFlags uint16 // 服务器状态标志
}

// ColumnDefinition 列定义包
type ColumnDefinition struct {
Catalog      string // 目录名（通常是"def"）
Schema       string // 数据库名
Table        string // 表别名
OrgTable     string // 原始表名
Name         string // 列别名
OrgName      string // 原始列名
FixedLength  uint64 // 固定长度字段的长度
CharacterSet uint16 // 字符集编号
ColumnLength uint32 // 列长度
ColumnType   byte   // 列类型（MYSQL_TYPE_*）
Flags        uint16 // 列标志
Decimals     byte   // 小数位数
}

// ResultSetRow 结果集行数据
type ResultSetRow struct {
Values []interface{} // 列值（nil表示NULL）
}

// TextResultSet 文本协议结果集
type TextResultSet struct {
ColumnCount   uint64             // 列数量
Columns       []ColumnDefinition // 列定义
Rows          []ResultSetRow     // 行数据
EOF           EOFPacket          // EOF包
HasMoreResult bool               // 是否有更多结果集
}

// CommandPacket 命令包
type CommandPacket struct {
Command byte   // 命令类型 (COM_*)
Payload []byte // 命令数据
}

// PreparedStatement 预处理语句
type PreparedStatement struct {
StatementID  uint32             // 语句ID
NumColumns   uint16             // 列数量
NumParams    uint16             // 参数数量
WarningCount uint16             // 警告数量
Columns      []ColumnDefinition // 列定义
Params       []ColumnDefinition // 参数定义
}

// BinaryResultSetRow 二进制协议结果集行数据
type BinaryResultSetRow struct {
NullBitmap []byte        // NULL位图
Values     []interface{} // 列值
}

// CapabilitySet 能力标志集合辅助结构
type CapabilitySet uint32

// Has 检查是否包含指定的能力标志
func (c CapabilitySet) Has(flag uint32) bool {
return uint32(c)&flag != 0
}

// Set 设置能力标志
func (c *CapabilitySet) Set(flag uint32) {
*c = CapabilitySet(uint32(*c) | flag)
}

// Clear 清除能力标志
func (c *CapabilitySet) Clear(flag uint32) {
*c = CapabilitySet(uint32(*c) &^ flag)
}

// StatusFlagSet 状态标志集合辅助结构
type StatusFlagSet uint16

// Has 检查是否包含指定的状态标志
func (s StatusFlagSet) Has(flag uint16) bool {
return uint16(s)&flag != 0
}

// Set 设置状态标志
func (s *StatusFlagSet) Set(flag uint16) {
*s = StatusFlagSet(uint16(*s) | flag)
}

// Clear 清除状态标志
func (s *StatusFlagSet) Clear(flag uint16) {
*s = StatusFlagSet(uint16(*s) &^ flag)
}

// FieldFlagSet 字段标志集合辅助结构
type FieldFlagSet uint32

// Has 检查是否包含指定的字段标志
func (f FieldFlagSet) Has(flag uint32) bool {
return uint32(f)&flag != 0
}

// Set 设置字段标志
func (f *FieldFlagSet) Set(flag uint32) {
*f = FieldFlagSet(uint32(*f) | flag)
}

// Clear 清除字段标志
func (f *FieldFlagSet) Clear(flag uint32) {
*f = FieldFlagSet(uint32(*f) &^ flag)
}

// Error MySQL协议错误
type Error struct {
Code    uint16
State   string
Message string
}

func (e *Error) Error() string {
if e.State != "" {
return fmt.Sprintf("ERROR %d (%s): %s", e.Code, e.State, e.Message)
}
return fmt.Sprintf("ERROR %d: %s", e.Code, e.Message)
}

// NewError 创建MySQL协议错误
func NewError(code uint16, state string, message string) *Error {
return &Error{
Code:    code,
State:   state,
Message: message,
}
}
