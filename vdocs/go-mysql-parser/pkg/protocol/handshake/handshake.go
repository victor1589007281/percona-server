package handshake

import (
"bytes"
"crypto/rand"
"io"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

// ParseHandshakeV10 解析服务器握手包 (Protocol Version 10)
func ParseHandshakeV10(payload []byte) (*types.HandshakeV10, error) {
r := bytes.NewReader(payload)
h := &types.HandshakeV10{}

// 协议版本 (1字节)
version, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
h.ProtocolVersion = version

// 服务器版本字符串 (null-terminated)
serverVersion, err := utils.ReadNullTerminatedString(r)
if err != nil {
return nil, err
}
h.ServerVersion = serverVersion

// 连接ID (4字节)
connectionID, err := utils.ReadUint32(r)
if err != nil {
return nil, err
}
h.ConnectionID = connectionID

// 认证数据第一部分 (8字节)
authPluginDataPart1, err := utils.ReadBytes(r, 8)
if err != nil {
return nil, err
}

// Filler (1字节, 0x00)
if _, err := utils.ReadUint8(r); err != nil {
return nil, err
}

// 能力标志低2字节
capLow, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}

// 字符集 (1字节)
charset, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
h.CharacterSet = charset

// 状态标志 (2字节)
status, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
h.StatusFlags = status

// 能力标志高2字节
capHigh, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
h.CapabilityFlags = uint32(capLow) | (uint32(capHigh) << 16)

// 认证数据总长度 (1字节)
authLen, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
h.AuthPluginDataLen = authLen

// 保留字段 (10字节)
if _, err := utils.ReadBytes(r, 10); err != nil {
return nil, err
}

// 如果支持CLIENT_SECURE_CONNECTION，读取认证数据第二部分
if h.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 {
// 认证数据第二部分长度 = max(13, authLen - 8)
part2Len := 13
if authLen > 8 {
part2Len = int(authLen) - 8
}
if part2Len > 0 {
authPluginDataPart2, err := utils.ReadBytes(r, part2Len)
if err != nil {
return nil, err
}
// 组合完整的认证数据（去掉末尾的0）
h.AuthPluginData = append(authPluginDataPart1, authPluginDataPart2[:len(authPluginDataPart2)-1]...)
}

// 认证插件名 (null-terminated)
authPluginName, err := utils.ReadNullTerminatedString(r)
if err != nil {
return nil, err
}
h.AuthPluginName = authPluginName
} else {
h.AuthPluginData = authPluginDataPart1
}

return h, nil
}

// BuildHandshakeV10 构建服务器握手包
func BuildHandshakeV10(h *types.HandshakeV10) ([]byte, error) {
var buf bytes.Buffer

// 协议版本
if err := utils.WriteUint8(&buf, h.ProtocolVersion); err != nil {
return nil, err
}

// 服务器版本
if err := utils.WriteNullTerminatedString(&buf, h.ServerVersion); err != nil {
return nil, err
}

// 连接ID
if err := utils.WriteUint32(&buf, h.ConnectionID); err != nil {
return nil, err
}

// 认证数据第一部分 (8字节)
if len(h.AuthPluginData) < 8 {
// 如果没有提供，生成随机数据
authData := make([]byte, types.SCRAMBLE_LENGTH)
if _, err := rand.Read(authData); err != nil {
return nil, err
}
h.AuthPluginData = authData
}
if err := utils.WriteBytes(&buf, h.AuthPluginData[:8]); err != nil {
return nil, err
}

// Filler (0x00)
if err := utils.WriteUint8(&buf, 0); err != nil {
return nil, err
}

// 能力标志低2字节
if err := utils.WriteUint16(&buf, uint16(h.CapabilityFlags)); err != nil {
return nil, err
}

// 字符集
if err := utils.WriteUint8(&buf, h.CharacterSet); err != nil {
return nil, err
}

// 状态标志
if err := utils.WriteUint16(&buf, h.StatusFlags); err != nil {
return nil, err
}

// 能力标志高2字节
if err := utils.WriteUint16(&buf, uint16(h.CapabilityFlags>>16)); err != nil {
return nil, err
}

// 认证数据总长度
if err := utils.WriteUint8(&buf, h.AuthPluginDataLen); err != nil {
return nil, err
}

// 保留字段 (10字节, 全0)
if err := utils.WriteBytes(&buf, make([]byte, 10)); err != nil {
return nil, err
}

// 认证数据第二部分
if h.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 && len(h.AuthPluginData) >= types.SCRAMBLE_LENGTH {
// 写入第二部分 (12字节) + null terminator
part2 := append(h.AuthPluginData[8:types.SCRAMBLE_LENGTH], 0)
if err := utils.WriteBytes(&buf, part2); err != nil {
return nil, err
}

// 认证插件名
if err := utils.WriteNullTerminatedString(&buf, h.AuthPluginName); err != nil {
return nil, err
}
}

return buf.Bytes(), nil
}

// ParseHandshakeResponse41 解析客户端握手响应包
func ParseHandshakeResponse41(payload []byte) (*types.HandshakeResponse41, error) {
r := bytes.NewReader(payload)
resp := &types.HandshakeResponse41{}

// 能力标志 (4字节)
cap, err := utils.ReadUint32(r)
if err != nil {
return nil, err
}
resp.CapabilityFlags = cap

// 最大包大小 (4字节)
maxPacket, err := utils.ReadUint32(r)
if err != nil {
return nil, err
}
resp.MaxPacketSize = maxPacket

// 字符集 (1字节)
charset, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
resp.CharacterSet = charset

// Filler (23字节)
if _, err := utils.ReadBytes(r, 23); err != nil {
return nil, err
}

// 用户名 (null-terminated)
username, err := utils.ReadNullTerminatedString(r)
if err != nil {
return nil, err
}
resp.Username = username

// 认证响应数据
if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA != 0 {
// length-encoded string
authResp, err := utils.ReadLengthEncodedString(r)
if err != nil {
return nil, err
}
resp.AuthResponse = []byte(authResp)
} else if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 {
// 1字节长度 + 数据
authLen, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
authResp, err := utils.ReadBytes(r, int(authLen))
if err != nil {
return nil, err
}
resp.AuthResponse = authResp
} else {
// null-terminated string
authResp, err := utils.ReadNullTerminatedString(r)
if err != nil {
return nil, err
}
resp.AuthResponse = []byte(authResp)
}

// 数据库名 (可选, null-terminated)
if resp.CapabilityFlags&types.CLIENT_CONNECT_WITH_DB != 0 {
database, err := utils.ReadNullTerminatedString(r)
if err != nil && err != io.EOF {
return nil, err
}
resp.Database = database
}

// 认证插件名 (可选, null-terminated)
if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 {
pluginName, err := utils.ReadNullTerminatedString(r)
if err != nil && err != io.EOF {
return nil, err
}
resp.AuthPluginName = pluginName
}

// 连接属性 (可选)
if resp.CapabilityFlags&types.CLIENT_CONNECT_ATTRS != 0 {
// TODO: 解析连接属性
}

return resp, nil
}

// BuildHandshakeResponse41 构建客户端握手响应包
func BuildHandshakeResponse41(resp *types.HandshakeResponse41) ([]byte, error) {
var buf bytes.Buffer

// 能力标志
if err := utils.WriteUint32(&buf, resp.CapabilityFlags); err != nil {
return nil, err
}

// 最大包大小
if err := utils.WriteUint32(&buf, resp.MaxPacketSize); err != nil {
return nil, err
}

// 字符集
if err := utils.WriteUint8(&buf, resp.CharacterSet); err != nil {
return nil, err
}

// Filler (23字节, 全0)
if err := utils.WriteBytes(&buf, make([]byte, 23)); err != nil {
return nil, err
}

// 用户名
if err := utils.WriteNullTerminatedString(&buf, resp.Username); err != nil {
return nil, err
}

// 认证响应数据
if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA != 0 {
if err := utils.WriteLengthEncodedString(&buf, string(resp.AuthResponse)); err != nil {
return nil, err
}
} else if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 {
if err := utils.WriteUint8(&buf, byte(len(resp.AuthResponse))); err != nil {
return nil, err
}
if err := utils.WriteBytes(&buf, resp.AuthResponse); err != nil {
return nil, err
}
} else {
if err := utils.WriteBytes(&buf, resp.AuthResponse); err != nil {
return nil, err
}
if err := utils.WriteUint8(&buf, 0); err != nil {
return nil, err
}
}

// 数据库名
if resp.CapabilityFlags&types.CLIENT_CONNECT_WITH_DB != 0 && resp.Database != "" {
if err := utils.WriteNullTerminatedString(&buf, resp.Database); err != nil {
return nil, err
}
}

// 认证插件名
if resp.CapabilityFlags&types.CLIENT_PLUGIN_AUTH != 0 && resp.AuthPluginName != "" {
if err := utils.WriteNullTerminatedString(&buf, resp.AuthPluginName); err != nil {
return nil, err
}
}

return buf.Bytes(), nil
}
