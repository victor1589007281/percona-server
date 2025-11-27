# Quick Start Guide

## 快速开始指南

### 1. 项目结构

```
go-redo-parser/          # 项目根目录
├── pkg/                 # 核心库代码
│   ├── types/          # 类型定义 (LSN, LogBlock, LogRecord, MTR 等)
│   ├── utils/          # 工具函数 (压缩整数, checksum)
│   └── redolog/        # 解析和写入 (BlockParser, RecordParser, Reader, Writer)
├── cmd/                # 命令行工具
│   ├── parser/         # 解析器: 读取并显示 redo log 内容
│   └── writer/         # 写入器: 生成测试 redo log 文件
├── docs/               # 文档 (格式说明)
└── testdata/           # 测试数据文件
```

### 2. 快速开始（30 秒）

```bash
# 进入项目目录
cd /Users/huaquan.liang/Documents/GitHub/percona-server/vdocs/go-redo-parser

# 安装依赖（无外部依赖）
go mod tidy

# 生成测试 redo log
go run cmd/writer/main.go -output my_test.log

# 解析 redo log
go run cmd/parser/main.go -file my_test.log -verbose
```

### 3. 详细使用示例

#### 生成 Redo Log

```bash
# 基本用法（默认输出到 test_redo.log）
go run cmd/writer/main.go

# 指定输出文件
go run cmd/writer/main.go -output custom.log

# 指定起始 LSN
go run cmd/writer/main.go -output custom.log -start-lsn 10000
```

**输出示例**：
```
Creating redo log file: my_test.log
Starting LSN: 8192

Writing MTR 1: MLOG_1BYTE (single record)
Writing MTR 2: MLOG_4BYTES (single record)
Writing MTR 3: MLOG_WRITE_STRING + MLOG_2BYTES (multi-record)
Writing MTR 4: MLOG_PAGE_CREATE (single record)

Redo log creation complete!
Final LSN: 8560
Total bytes written: 368
```

#### 解析 Redo Log

```bash
# 基本解析（显示前 5 个 MTR）
go run cmd/parser/main.go -file my_test.log

# 详细模式（显示所有 record 详情）
go run cmd/parser/main.go -file my_test.log -verbose

# 显示统计信息
go run cmd/parser/main.go -file my_test.log -stats

# 从指定 LSN 开始解析
go run cmd/parser/main.go -file my_test.log -start-lsn 8200
```

**输出示例**：
```
Reading redo log file: my_test.log

File Information:
  File Size: 2560 bytes
  Format Version: 6
  Log UUID: 0x12345678
  Start LSN: 8192
  Creator: Go-Redo-Parser v1.0
  Flags: 0x00000000

Checkpoint 1:
  Checkpoint LSN: 8192

Recovery Statistics:
  Blocks Read: 1
  Records Parsed: 5
  MTRs Processed: 3
  Bytes Processed: 512

Found 3 MTRs
  MTR 1: LSN 8192-8193 (1 records)
  MTR 2: LSN 8198-8202 (1 records)
  MTR 3: LSN 8208-8234 (3 records)
```

### 4. 在代码中使用

#### 示例 1: 读取和解析 Redo Log

```go
package main

import (
    "fmt"
    "github.com/percona/go-redo-parser/pkg/redolog"
)

func main() {
    // 创建 reader
    reader := redolog.NewReader()
    
    // 读取整个文件
    mtrs, stats, err := reader.ReadFile("path/to/redo.log")
    if err != nil {
        panic(err)
    }
    
    // 显示统计
    fmt.Printf("解析了 %d 个 blocks, %d 条 records, %d 个 MTRs\n",
        stats.BlocksRead, stats.RecordsParsed, stats.MtrsProcessed)
    
    // 遍历 MTRs
    for i, mtr := range mtrs {
        fmt.Printf("MTR %d: LSN %d-%d, %d records\n",
            i+1, mtr.StartLsn, mtr.EndLsn, len(mtr.Records))
        
        // 遍历每个 MTR 中的 records
        for j, record := range mtr.Records {
            fmt.Printf("  Record %d: Type=%s, Space=%d, Page=%d\n",
                j+1, record.Type.String(), record.SpaceID, record.PageNo)
        }
    }
}
```

#### 示例 2: 生成 Redo Log

```go
package main

import (
    "github.com/percona/go-redo-parser/pkg/redolog"
    "github.com/percona/go-redo-parser/pkg/types"
)

func main() {
    // 创建 writer
    writer, err := redolog.NewWriter("output.log", types.LSN(8192))
    if err != nil {
        panic(err)
    }
    defer writer.Close()
    
    // 创建一个 MTR
    mtr := &types.MTR{
        Records: []*types.LogRecord{
            {
                Type:    types.Mlog1Byte | types.MlogSingleRecFlag,
                SpaceID: 0,
                PageNo:  100,
                Offset:  50,
                Data:    []byte{0x42},
            },
        },
    }
    
    // 写入 MTR
    if err := writer.WriteMTR(mtr); err != nil {
        panic(err)
    }
    
    fmt.Printf("写入完成，最终 LSN: %d\n", writer.GetCurrentLSN())
}
```

#### 示例 3: 解析特定类型的 Record

```go
package main

import (
    "fmt"
    "github.com/percona/go-redo-parser/pkg/redolog"
    "github.com/percona/go-redo-parser/pkg/types"
)

func main() {
    reader := redolog.NewReader()
    mtrs, _, _ := reader.ReadFile("redo.log")
    
    // 统计不同类型的 record
    typeCount := make(map[types.MlogType]int)
    
    for _, mtr := range mtrs {
        for _, record := range mtr.Records {
            baseType := record.Type.BaseType()
            typeCount[baseType]++
        }
    }
    
    // 显示统计
    fmt.Println("Record 类型统计:")
    for mType, count := range typeCount {
        fmt.Printf("  %s: %d\n", mType.String(), count)
    }
}
```

### 5. 运行测试

```bash
# 运行所有测试
go test ./...

# 运行特定包的测试
go test ./pkg/utils -v

# 查看测试覆盖率
go test -cover ./...

# 生成覆盖率报告
go test -coverprofile=coverage.out ./...
go tool cover -html=coverage.out
```

### 6. 常见问题

#### Q: 如何解析真实的 InnoDB redo log？
A: 找到 MySQL 数据目录下的 `#innodb_redo/#ib_redo*` 文件，然后使用 parser 解析：
```bash
go run cmd/parser/main.go -file /var/lib/mysql/#innodb_redo/#ib_redo0 -verbose
```

#### Q: 支持哪些 redo log 格式版本？
A: 主要支持 8.0.30+ 的格式（Log_format = 6），其他版本可能部分兼容。

#### Q: 如何处理大文件？
A: 使用 `-start-lsn` 参数从特定位置开始解析：
```bash
go run cmd/parser/main.go -file large.log -start-lsn 1000000
```

#### Q: 如何贡献代码？
A: 这是一个学习项目，欢迎提交 PR 改进功能。

### 7. 性能提示

- 大文件（>1GB）: 考虑使用流式解析或分段读取
- 内存占用：每个 block 占用 ~512 字节，合理控制一次读取的 block 数量
- 并发：可以实现多线程解析不同的 block

### 8. 调试技巧

```bash
# 查看文件头信息（不解析 records）
go run cmd/parser/main.go -file redo.log | head -15

# 只显示统计（不显示详细 records）
go run cmd/parser/main.go -file redo.log -stats

# 使用 verbose 模式查看所有细节
go run cmd/parser/main.go -file redo.log -verbose > output.txt
```

### 9. 相关资源

- [详细格式文档](docs/REDO_LOG_FORMAT.md)
- [项目 README](README.md)
- [项目总结](PROJECT_SUMMARY.md)
- [Percona Server 源码](https://github.com/percona/percona-server)

---

**提示**: 这是一个教育和调试工具，不建议用于生产环境的数据恢复。
