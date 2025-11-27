package response

import (
"bytes"
"io"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

// ParseOKPacket 解析OK包
func ParseOKPacket(payload []byte, capabilities uint32) (*types.OKPacket, error) {
r := bytes.NewReader(payload)
ok := &types.OKPacket{}

// Header (0x00 或 0xFE)
header, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
ok.Header = header

// 受影响的行数 (length-encoded integer)
affectedRows, err := utils.ReadLengthEncodedInteger(r)
if err != nil {
return nil, err
}
ok.AffectedRows = affectedRows

// 最后插入ID (length-encoded integer)
lastInsertID, err := utils.ReadLengthEncodedInteger(r)
if err != nil {
return nil, err
}
ok.LastInsertID = lastInsertID

// 状态标志 (2字节)
if capabilities&types.CLIENT_PROTOCOL_41 != 0 {
statusFlags, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
ok.StatusFlags = statusFlags

// 警告数量 (2字节)
warnings, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
ok.Warnings = warnings
} else if capabilities&types.CLIENT_TRANSACTIONS != 0 {
statusFlags, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
ok.StatusFlags = statusFlags
}

// 信息字符串
if capabilities&types.CLIENT_SESSION_TRACK != 0 {
info, err := utils.ReadLengthEncodedString(r)
if err != nil && err != io.EOF {
return nil, err
}
ok.Info = info

// 会话状态信息
if ok.StatusFlags&types.SERVER_SESSION_STATE_CHANGED != 0 {
sessionInfo, err := utils.ReadLengthEncodedString(r)
if err != nil && err != io.EOF {
return nil, err
}
ok.SessionStateInfo = sessionInfo
}
} else {
// 读取剩余的所有字节作为info
remaining, _ := io.ReadAll(r)
ok.Info = string(remaining)
}

return ok, nil
}

// BuildOKPacket 构建OK包
func BuildOKPacket(ok *types.OKPacket, capabilities uint32) ([]byte, error) {
var buf bytes.Buffer

// Header
if err := utils.WriteUint8(&buf, ok.Header); err != nil {
return nil, err
}

// 受影响的行数
if err := utils.WriteLengthEncodedInteger(&buf, ok.AffectedRows); err != nil {
return nil, err
}

// 最后插入ID
if err := utils.WriteLengthEncodedInteger(&buf, ok.LastInsertID); err != nil {
return nil, err
}

// 状态标志和警告
if capabilities&types.CLIENT_PROTOCOL_41 != 0 {
if err := utils.WriteUint16(&buf, ok.StatusFlags); err != nil {
return nil, err
}
if err := utils.WriteUint16(&buf, ok.Warnings); err != nil {
return nil, err
}
} else if capabilities&types.CLIENT_TRANSACTIONS != 0 {
if err := utils.WriteUint16(&buf, ok.StatusFlags); err != nil {
return nil, err
}
}

// 信息字符串
if capabilities&types.CLIENT_SESSION_TRACK != 0 {
if err := utils.WriteLengthEncodedString(&buf, ok.Info); err != nil {
return nil, err
}
if ok.StatusFlags&types.SERVER_SESSION_STATE_CHANGED != 0 {
if err := utils.WriteLengthEncodedString(&buf, ok.SessionStateInfo); err != nil {
return nil, err
}
}
} else if ok.Info != "" {
if _, err := buf.Write([]byte(ok.Info)); err != nil {
return nil, err
}
}

return buf.Bytes(), nil
}

// ParseERRPacket 解析ERR包
func ParseERRPacket(payload []byte, capabilities uint32) (*types.ERRPacket, error) {
r := bytes.NewReader(payload)
err := &types.ERRPacket{}

// Header (0xFF)
header, e := utils.ReadUint8(r)
if e != nil {
return nil, e
}
err.Header = header

// 错误代码 (2字节)
errorCode, e := utils.ReadUint16(r)
if e != nil {
return nil, e
}
err.ErrorCode = errorCode

// SQL状态标记和SQL状态
if capabilities&types.CLIENT_PROTOCOL_41 != 0 {
// SQL状态标记 ('#')
if _, e := utils.ReadUint8(r); e != nil {
return nil, e
}
// SQL状态 (5字节)
sqlState, e := utils.ReadFixedLengthString(r, 5)
if e != nil {
return nil, e
}
err.SQLState = sqlState
}

// 错误消息
remaining, _ := io.ReadAll(r)
err.ErrorMessage = string(remaining)

return err, nil
}

// BuildERRPacket 构建ERR包
func BuildERRPacket(err *types.ERRPacket, capabilities uint32) ([]byte, error) {
var buf bytes.Buffer

// Header
if e := utils.WriteUint8(&buf, types.PacketERR); e != nil {
return nil, e
}

// 错误代码
if e := utils.WriteUint16(&buf, err.ErrorCode); e != nil {
return nil, e
}

// SQL状态
if capabilities&types.CLIENT_PROTOCOL_41 != 0 {
// SQL状态标记
if e := utils.WriteUint8(&buf, '#'); e != nil {
return nil, e
}
// SQL状态
if e := utils.WriteFixedLengthString(&buf, err.SQLState, 5); e != nil {
return nil, e
}
}

// 错误消息
if _, e := buf.Write([]byte(err.ErrorMessage)); e != nil {
return nil, e
}

return buf.Bytes(), nil
}

// ParseEOFPacket 解析EOF包
func ParseEOFPacket(payload []byte, capabilities uint32) (*types.EOFPacket, error) {
r := bytes.NewReader(payload)
eof := &types.EOFPacket{}

// Header (0xFE)
header, err := utils.ReadUint8(r)
if err != nil {
return nil, err
}
eof.Header = header

// 警告数量 (2字节)
warnings, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
eof.Warnings = warnings

// 状态标志 (2字节)
statusFlags, err := utils.ReadUint16(r)
if err != nil {
return nil, err
}
eof.StatusFlags = statusFlags

return eof, nil
}

// BuildEOFPacket 构建EOF包
func BuildEOFPacket(eof *types.EOFPacket) ([]byte, error) {
var buf bytes.Buffer

// Header
if err := utils.WriteUint8(&buf, types.PacketEOF); err != nil {
return nil, err
}

// 警告数量
if err := utils.WriteUint16(&buf, eof.Warnings); err != nil {
return nil, err
}

// 状态标志
if err := utils.WriteUint16(&buf, eof.StatusFlags); err != nil {
return nil, err
}

return buf.Bytes(), nil
}
