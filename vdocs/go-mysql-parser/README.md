# Go MySQL Protocol Parser

这是一个用Go语言实现的MySQL二进制通信协议解析器和生成器，基于Percona Server 8.4.3-3源码分析开发。

## 功能特性

- ✅ **完整的协议支持**：支持MySQL 4.1+协议
- ✅ **握手和认证**：实现握手包的生成和解析
- ✅ **命令执行**：支持常用MySQL命令（COM_QUERY、COM_PING等）
- ✅ **结果集解析**：完整的结果集列定义和行数据解析
- ✅ **包管理**：自动处理分包和sequence_id管理
- ✅ **编解码工具**：length-encoded integer/string的完整实现

## 项目结构

```
go-mysql-parser/
├── pkg/
│   ├── types/           # 类型和常量定义
│   ├── utils/           # 编解码工具函数
│   └── protocol/        # 协议实现
│       ├── handshake/   # 握手协议
│       ├── command/     # 命令协议
│       ├── response/    # 响应协议（OK/ERR/EOF）
│       └── resultset/   # 结果集协议
├── cmd/
│   ├── client/          # 模拟客户端
│   └── server/          # 模拟服务器
├── examples/            # 示例代码
└── docs/                # 详细文档
```

## 测试

```bash
# 运行所有测试
go test ./...

# 运行特定包的测试
go test ./pkg/protocol/handshake

# 运行带覆盖率的测试
go test -cover ./...
```

## 参考文档

- [MySQL官方协议文档](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html)
- [Percona Server 8.4.3-3源码](https://github.com/percona/percona-server)

## License

MIT License
