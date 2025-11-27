package main

import (
main.go "bytes"
main.go "crypto/rand"
main.go "fmt"
main.go "log"
main.go "net"

main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/command"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/handshake"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/response"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/resultset"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
main.go "github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/utils"
)

func main() {
main.go fmt.Println("MySQL协议解析器 - 模拟服务器示例")
main.go fmt.Println("===================================")

main.go listener, err := net.Listen("tcp", ":13306")
main.go if err != nil {
main.go main.go log.Fatalf("监听失败: %v", err)
main.go }
main.go defer listener.Close()

main.go fmt.Println("✅ 服务器已启动，监听端口 13306")
main.go fmt.Println("等待客户端连接...")

main.go for {
main.go main.go conn, err := listener.Accept()
main.go main.go if err != nil {
main.go main.go main.go log.Printf("接受连接失败: %v", err)
main.go main.go main.go continue
main.go main.go }

main.go main.go fmt.Printf("\n🔗 新连接来自: %s\n", conn.RemoteAddr())
main.go main.go go handleConnection(conn)
main.go }
}

func handleConnection(conn net.Conn) {
main.go defer conn.Close()

main.go reader := protocol.NewPacketReader(conn)
main.go writer := protocol.NewPacketWriter(conn)

main.go // 1. 发送握手包
main.go if err := sendHandshake(writer); err != nil {
main.go main.go log.Printf("发送握手包失败: %v", err)
main.go main.go return
main.go }
main.go fmt.Println("✓ 已发送握手包")

main.go // 2. 接收握手响应
main.go packet, err := reader.ReadPacket()
main.go if err != nil {
main.go main.go log.Printf("读取握手响应失败: %v", err)
main.go main.go return
main.go }

main.go handshakeResp, err := handshake.ParseHandshakeResponse41(packet.Payload)
main.go if err != nil {
main.go main.go log.Printf("解析握手响应失败: %v", err)
main.go main.go return
main.go }

main.go fmt.Printf("✓ 客户端用户名: %s\n", handshakeResp.Username)
main.go if handshakeResp.Database != "" {
main.go main.go fmt.Printf("✓ 客户端数据库: %s\n", handshakeResp.Database)
main.go }

main.go // 3. 发送认证成功的OK包
main.go okPayload, err := response.BuildOKPacket(&types.OKPacket{
main.go main.go Header:          types.PacketOK,
main.go main.go AffectedRows:    0,
main.go main.go LastInsertID:    0,
main.go main.go StatusFlags:     types.SERVER_STATUS_AUTOCOMMIT,
main.go main.go Warnings:        0,
main.go main.go Info:            "",
main.go }, types.CLIENT_PROTOCOL_41)
main.go if err != nil {
main.go main.go log.Printf("构建OK包失败: %v", err)
main.go main.go return
main.go }

main.go if err := writer.WritePacket(okPayload); err != nil {
main.go main.go log.Printf("发送OK包失败: %v", err)
main.go main.go return
main.go }
main.go fmt.Println("✓ 认证成功")

main.go // 4. 处理命令
main.go for {
main.go main.go packet, err := reader.ReadPacket()
main.go main.go if err != nil {
main.go main.go main.go fmt.Println("✓ 客户端断开连接")
main.go main.go main.go return
main.go main.go }

main.go main.go cmd, err := command.ParseCommandPacket(packet.Payload)
main.go main.go if err != nil {
main.go main.go main.go log.Printf("解析命令失败: %v", err)
main.go main.go main.go continue
main.go main.go }

main.go main.go if err := handleCommand(writer, cmd); err != nil {
main.go main.go main.go log.Printf("处理命令失败: %v", err)
main.go main.go main.go return
main.go main.go }
main.go }
}

func sendHandshake(writer *protocol.PacketWriter) error {
main.go // 生成认证数据
main.go authData := make([]byte, types.SCRAMBLE_LENGTH)
main.go if _, err := rand.Read(authData); err != nil {
main.go main.go return err
main.go }

main.go h := &types.HandshakeV10{
main.go main.go ProtocolVersion: types.PROTOCOL_VERSION,
main.go main.go ServerVersion:   "8.4.3-3-Percona-Go-Parser",
main.go main.go ConnectionID:    1,
main.go main.go AuthPluginData:  authData,
main.go main.go CapabilityFlags: types.CLIENT_PROTOCOL_41 | 
main.go main.go main.go types.CLIENT_PLUGIN_AUTH |
main.go main.go main.go types.CLIENT_DEPRECATE_EOF,
main.go main.go CharacterSet:     types.CHARSET_UTF8MB4,
main.go main.go StatusFlags:      types.SERVER_STATUS_AUTOCOMMIT,
main.go main.go AuthPluginName:   "mysql_native_password",
main.go main.go AuthPluginDataLen: types.SCRAMBLE_LENGTH + 1,
main.go }

main.go payload, err := handshake.BuildHandshakeV10(h)
main.go if err != nil {
main.go main.go return err
main.go }

main.go return writer.WritePacket(payload)
}

func handleCommand(writer *protocol.PacketWriter, cmd *types.CommandPacket) error {
main.go fmt.Printf("📨 收到命令: 0x%02X ", cmd.Command)

main.go switch cmd.Command {
main.go case types.COM_QUIT:
main.go main.go fmt.Println("(QUIT)")
main.go main.go return fmt.Errorf("client quit")

main.go case types.COM_QUERY:
main.go main.go query := string(cmd.Payload)
main.go main.go fmt.Printf("(QUERY: %s)\n", query)
main.go main.go return handleQuery(writer, query)

main.go case types.COM_PING:
main.go main.go fmt.Println("(PING)")
main.go main.go return sendOK(writer)

main.go case types.COM_INIT_DB:
main.go main.go database := string(cmd.Payload)
main.go main.go fmt.Printf("(INIT_DB: %s)\n", database)
main.go main.go return sendOK(writer)

main.go default:
main.go main.go fmt.Printf("(UNKNOWN)\n")
main.go main.go return sendError(writer, 1047, "Unknown command")
main.go }
}

func handleQuery(writer *protocol.PacketWriter, query string) error {
main.go // 简单模拟：返回一个示例结果集
main.go if len(query) > 6 && query[:6] == "SELECT" {
main.go main.go return sendResultSet(writer)
main.go }

main.go // 其他查询返回OK包
main.go return sendOK(writer)
}

func sendOK(writer *protocol.PacketWriter) error {
main.go okPayload, err := response.BuildOKPacket(&types.OKPacket{
main.go main.go Header:       types.PacketOK,
main.go main.go AffectedRows: 1,
main.go main.go LastInsertID: 0,
main.go main.go StatusFlags:  types.SERVER_STATUS_AUTOCOMMIT,
main.go main.go Warnings:     0,
main.go main.go Info:         "",
main.go }, types.CLIENT_PROTOCOL_41)
main.go if err != nil {
main.go main.go return err
main.go }

main.go return writer.WritePacket(okPayload)
}

func sendError(writer *protocol.PacketWriter, code uint16, message string) error {
main.go errPayload, err := response.BuildERRPacket(&types.ERRPacket{
main.go main.go Header:       types.PacketERR,
main.go main.go ErrorCode:    code,
main.go main.go SQLState:     "HY000",
main.go main.go ErrorMessage: message,
main.go }, types.CLIENT_PROTOCOL_41)
main.go if err != nil {
main.go main.go return err
main.go }

main.go return writer.WritePacket(errPayload)
}

func sendResultSet(writer *protocol.PacketWriter) error {
main.go // 1. 发送列数量
main.go var buf bytes.Buffer
main.go if err := utils.WriteLengthEncodedInteger(&buf, 2); err != nil {
main.go main.go return err
main.go }
main.go if err := writer.WritePacket(buf.Bytes()); err != nil {
main.go main.go return err
main.go }

main.go // 2. 发送列定义
main.go columns := []types.ColumnDefinition{
main.go main.go {
main.go main.go main.go Catalog:      "def",
main.go main.go main.go Schema:       "test",
main.go main.go main.go Table:        "users",
main.go main.go main.go OrgTable:     "users",
main.go main.go main.go Name:         "id",
main.go main.go main.go OrgName:      "id",
main.go main.go main.go CharacterSet: types.CHARSET_BINARY,
main.go main.go main.go ColumnLength: 11,
main.go main.go main.go ColumnType:   types.MYSQL_TYPE_LONG,
main.go main.go main.go Flags:        types.NOT_NULL_FLAG | types.PRI_KEY_FLAG,
main.go main.go main.go Decimals:     0,
main.go main.go },
main.go main.go {
main.go main.go main.go Catalog:      "def",
main.go main.go main.go Schema:       "test",
main.go main.go main.go Table:        "users",
main.go main.go main.go OrgTable:     "users",
main.go main.go main.go Name:         "name",
main.go main.go main.go OrgName:      "name",
main.go main.go main.go CharacterSet: types.CHARSET_UTF8MB4,
main.go main.go main.go ColumnLength: 255,
main.go main.go main.go ColumnType:   types.MYSQL_TYPE_VAR_STRING,
main.go main.go main.go Flags:        0,
main.go main.go main.go Decimals:     0,
main.go main.go },
main.go }

main.go for _, col := range columns {
main.go main.go colPayload, err := resultset.BuildColumnDefinition(&col)
main.go main.go if err != nil {
main.go main.go main.go return err
main.go main.go }
main.go main.go if err := writer.WritePacket(colPayload); err != nil {
main.go main.go main.go return err
main.go main.go }
main.go }

main.go // 3. 发送EOF包（如果需要）
main.go eofPayload, err := response.BuildEOFPacket(&types.EOFPacket{
main.go main.go Header:      types.PacketEOF,
main.go main.go Warnings:    0,
main.go main.go StatusFlags: types.SERVER_STATUS_AUTOCOMMIT,
main.go })
main.go if err != nil {
main.go main.go return err
main.go }
main.go if err := writer.WritePacket(eofPayload); err != nil {
main.go main.go return err
main.go }

main.go // 4. 发送行数据
main.go rows := []types.ResultSetRow{
main.go main.go {Values: []interface{}{"1", "Alice"}},
main.go main.go {Values: []interface{}{"2", "Bob"}},
main.go }

main.go for _, row := range rows {
main.go main.go rowPayload, err := resultset.BuildTextResultSetRow(&row)
main.go main.go if err != nil {
main.go main.go main.go return err
main.go main.go }
main.go main.go if err := writer.WritePacket(rowPayload); err != nil {
main.go main.go main.go return err
main.go main.go }
main.go }

main.go // 5. 发送最后的EOF包
main.go return writer.WritePacket(eofPayload)
}
