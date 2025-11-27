package main

import (
"fmt"
"log"
"net"

"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/command"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/handshake"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/protocol/response"
"github.com/percona/percona-server/vdocs/go-mysql-parser/pkg/types"
)

func main() {
fmt.Println("MySQL协议解析器 - 模拟客户端示例")
fmt.Println("===================================")

// 连接到MySQL服务器（这里演示协议解析，不真实连接）
conn, err := net.Dial("tcp", "127.0.0.1:3306")
if err != nil {
log.Printf("连接失败（这是正常的，因为没有运行MySQL服务器）: %v", err)
log.Println("\n以下展示协议包的生成和解析示例...")
demonstrateProtocol()
return
}
defer conn.Close()

// 如果真的连接上了，执行握手
if err := performHandshake(conn); err != nil {
log.Fatalf("握手失败: %v", err)
}

// 执行查询
if err := executeQuery(conn, "SELECT * FROM users"); err != nil {
log.Fatalf("查询失败: %v", err)
}
}

func performHandshake(conn net.Conn) error {
reader := protocol.NewPacketReader(conn)
writer := protocol.NewPacketWriter(conn)

// 1. 读取服务器握手包
packet, err := reader.ReadPacket()
if err != nil {
return fmt.Errorf("读取握手包失败: %w", err)
}

handshakePkt, err := handshake.ParseHandshakeV10(packet.Payload)
if err != nil {
return fmt.Errorf("解析握手包失败: %w", err)
}

fmt.Printf("服务器版本: %s\n", handshakePkt.ServerVersion)
fmt.Printf("连接ID: %d\n", handshakePkt.ConnectionID)

// 2. 发送握手响应
resp := &types.HandshakeResponse41{
CapabilityFlags: types.CLIENT_PROTOCOL_41 | types.CLIENT_PLUGIN_AUTH,
MaxPacketSize:   types.MaxPacketSize,
CharacterSet:    types.CHARSET_UTF8MB4,
Username:        "root",
AuthResponse:    []byte{}, // 实际应该计算认证响应
Database:        "test",
AuthPluginName:  "mysql_native_password",
}

responsePayload, err := handshake.BuildHandshakeResponse41(resp)
if err != nil {
return fmt.Errorf("构建握手响应失败: %w", err)
}

if err := writer.WritePacket(responsePayload); err != nil {
return fmt.Errorf("发送握手响应失败: %w", err)
}

// 3. 读取认证结果
packet, err = reader.ReadPacket()
if err != nil {
return fmt.Errorf("读取认证结果失败: %w", err)
}

if protocol.IsERRPacket(packet.Payload) {
errPkt, _ := response.ParseERRPacket(packet.Payload, types.CLIENT_PROTOCOL_41)
return fmt.Errorf("认证失败: %s", errPkt.ErrorMessage)
}

fmt.Println("✅ 握手成功")
return nil
}

func executeQuery(conn net.Conn, query string) error {
writer := protocol.NewPacketWriter(conn)
reader := protocol.NewPacketReader(conn)

// 发送查询命令
cmdPayload, err := command.BuildQueryCommand(query)
if err != nil {
return fmt.Errorf("构建查询命令失败: %w", err)
}

if err := writer.WritePacket(cmdPayload); err != nil {
return fmt.Errorf("发送查询命令失败: %w", err)
}

fmt.Printf("执行查询: %s\n", query)

// 读取响应
packet, err := reader.ReadPacket()
if err != nil {
return fmt.Errorf("读取响应失败: %w", err)
}

if protocol.IsERRPacket(packet.Payload) {
errPkt, _ := response.ParseERRPacket(packet.Payload, types.CLIENT_PROTOCOL_41)
return fmt.Errorf("查询失败: %s", errPkt.ErrorMessage)
}

if protocol.IsOKPacket(packet.Payload) {
okPkt, _ := response.ParseOKPacket(packet.Payload, types.CLIENT_PROTOCOL_41)
fmt.Printf("✅ 查询成功: 影响行数=%d\n", okPkt.AffectedRows)
return nil
}

// 结果集
fmt.Println("✅ 接收到结果集")
return nil
}

func demonstrateProtocol() {
fmt.Println("\n📦 协议包生成示例:")
fmt.Println("-------------------")

// 演示各种命令包的生成
queries := []string{
"SELECT * FROM users",
"INSERT INTO users VALUES (1, 'Alice')",
"UPDATE users SET name='Bob' WHERE id=1",
}

for _, query := range queries {
payload, err := command.BuildQueryCommand(query)
if err != nil {
log.Printf("生成失败: %v", err)
continue
}
fmt.Printf("✓ %s -> %d bytes\n", query, len(payload))
}

// 演示其他命令
fmt.Println("\n📦 其他命令包:")
fmt.Println("-------------------")

pingPayload, _ := command.BuildPingCommand()
fmt.Printf("✓ PING命令 -> %d bytes\n", len(pingPayload))

quitPayload, _ := command.BuildQuitCommand()
fmt.Printf("✓ QUIT命令 -> %d bytes\n", len(quitPayload))

initdbPayload, _ := command.BuildInitDBCommand("mydb")
fmt.Printf("✓ 切换数据库 -> %d bytes\n", len(initdbPayload))

fmt.Println("\n✅ 示例完成！")
fmt.Println("\n💡 提示: 要实际连接MySQL服务器，请确保:")
fmt.Println("   1. MySQL服务器正在运行")
fmt.Println("   2. 端口3306可访问")
fmt.Println("   3. 有正确的认证信息")
}
