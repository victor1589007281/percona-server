package command

import (
"bytes"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

// BuildQueryCommand 构建COM_QUERY命令包
func BuildQueryCommand(query string) ([]byte, error) {
var buf bytes.Buffer

// 命令类型
if err := utils.WriteUint8(&buf, types.COM_QUERY); err != nil {
return nil, err
}

// SQL查询字符串
if _, err := buf.Write([]byte(query)); err != nil {
return nil, err
}

return buf.Bytes(), nil
}

// BuildQuitCommand 构建COM_QUIT命令包
func BuildQuitCommand() ([]byte, error) {
return []byte{types.COM_QUIT}, nil
}

// BuildPingCommand 构建COM_PING命令包
func BuildPingCommand() ([]byte, error) {
return []byte{types.COM_PING}, nil
}

// BuildInitDBCommand 构建COM_INIT_DB命令包
func BuildInitDBCommand(database string) ([]byte, error) {
var buf bytes.Buffer

// 命令类型
if err := utils.WriteUint8(&buf, types.COM_INIT_DB); err != nil {
return nil, err
}

// 数据库名
if _, err := buf.Write([]byte(database)); err != nil {
return nil, err
}

return buf.Bytes(), nil
}

// BuildStatisticsCommand 构建COM_STATISTICS命令包
func BuildStatisticsCommand() ([]byte, error) {
return []byte{types.COM_STATISTICS}, nil
}

// BuildResetConnectionCommand 构建COM_RESET_CONNECTION命令包
func BuildResetConnectionCommand() ([]byte, error) {
return []byte{types.COM_RESET_CONNECTION}, nil
}

// ParseCommandPacket 解析命令包
func ParseCommandPacket(payload []byte) (*types.CommandPacket, error) {
if len(payload) == 0 {
return nil, types.NewError(0, "", "empty command packet")
}

cmd := &types.CommandPacket{
Command: payload[0],
}

if len(payload) > 1 {
cmd.Payload = payload[1:]
}

return cmd, nil
}
