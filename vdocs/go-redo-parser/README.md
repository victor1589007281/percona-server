# Go Redo Parser

基于 Percona Server 8.4.3-3 源码实现的 InnoDB Redo Log 解析和生成工具包。

## 功能特性

- ✅ 完整的 redo log 格式解析（8.0.30+ 格式）
- ✅ 支持解析文件头、checkpoint 头、log blocks
- ✅ 支持解析所有主要的 MLOG record 类型
- ✅ 压缩整数编码/解码
- ✅ Block checksum 验证
- ✅ Redo log 写入器（生成测试数据）
- ✅ 命令行工具：parser 和 writer
- ✅ 单元测试

## 项目结构

```
go-redo-parser/
├── pkg/
│   ├── types/          # 类型定义和常量
│   ├── utils/          # 工具函数（压缩整数、checksum等）
│   └── redolog/        # 核心解析和写入逻辑
├── cmd/
│   ├── parser/         # 解析器命令行工具
│   └── writer/         # 写入器命令行工具
├── docs/               # 文档
└── testdata/           # 测试数据

```

## 安装

```bash
cd vdocs/go-redo-parser
go mod tidy
```

## 使用方法

### 1. 解析 Redo Log 文件

```bash
# 基本解析
go run cmd/parser/main.go -file /path/to/redo/log/file

# 显示详细信息
go run cmd/parser/main.go -file /path/to/redo/log/file -verbose

# 显示统计信息
go run cmd/parser/main.go -file /path/to/redo/log/file -stats

# 从指定 LSN 开始解析
go run cmd/parser/main.go -file /path/to/redo/log/file -start-lsn 100000
```

### 2. 生成测试 Redo Log

```bash
# 生成默认测试文件
go run cmd/writer/main.go

# 指定输出文件和起始 LSN
go run cmd/writer/main.go -output my_test.log -start-lsn 8192
```

### 3. 在代码中使用

```go
package main

import (
    "fmt"
    "github.com/percona/go-redo-parser/pkg/redolog"
    "github.com/percona/go-redo-parser/pkg/types"
)

func main() {
    // 创建 reader
    reader := redolog.NewReader()
    
    // 读取文件
    mtrs, stats, err := reader.ReadFile("path/to/redo.log")
    if err != nil {
        panic(err)
    }
    
    // 处理 MTRs
    for _, mtr := range mtrs {
        fmt.Printf("MTR: LSN %d-%d\n", mtr.StartLsn, mtr.EndLsn)
        for _, record := range mtr.Records {
            fmt.Printf("  Record: %s\n", record.Type.String())
        }
    }
    
    // 显示统计
    fmt.Printf("Blocks read: %d\n", stats.BlocksRead)
    fmt.Printf("Records parsed: %d\n", stats.RecordsParsed)
}
```

## API 文档

### 主要类型

#### LogBlock
```go
type LogBlock struct {
    Header   LogDataBlockHeader  // 12-byte header
    Data     []byte             // 496 bytes max
    Checksum uint32             // 4-byte trailer
}
```

#### LogRecord
```go
type LogRecord struct {
    Type    MlogType  // MLOG_* type
    SpaceID SpaceID   // Tablespace ID
    PageNo  PageNo    // Page number
    Offset  uint16    // Offset within page
    Data    []byte    // Record data
    LSN     LSN       // Record LSN
}
```

#### MTR (Mini-Transaction)
```go
type MTR struct {
    Records  []*LogRecord
    StartLsn LSN
    EndLsn   LSN
}
```

### 主要接口

#### BlockParser
```go
func NewBlockParser() *BlockParser
func (p *BlockParser) ParseBlock(blockData []byte) (*LogBlock, error)
func (p *BlockParser) IsEmptyBlock(block *LogBlock) bool
```

#### RecordParser
```go
func NewRecordParser() *RecordParser
func (p *RecordParser) ParseRecord(data []byte, currentLsn LSN) (*LogRecord, int, error)
func (p *RecordParser) ParseRecords(blockData []byte, startLsn LSN) ([]*LogRecord, error)
```

#### Reader
```go
func NewReader() *Reader
func (r *Reader) ReadFile(filePath string) ([]*MTR, *RecoveryStats, error)
func (r *Reader) ReadFromLSN(filePath string, startLsn LSN) ([]*MTR, error)
```

#### Writer
```go
func NewWriter(filePath string, startLsn LSN) (*Writer, error)
func (w *Writer) WriteRecord(record *LogRecord) error
func (w *Writer) WriteMTR(mtr *MTR) error
func (w *Writer) Close() error
```

## 测试

```bash
# 运行所有测试
go test ./...

# 运行特定包的测试
go test ./pkg/utils

# 运行测试并显示覆盖率
go test -cover ./...
```

## 示例输出

### Parser 输出示例

```
Reading redo log file: test_redo.log

File Information:
  File Size: 16384 bytes
  Format Version: 6
  Log UUID: 0x12345678
  Start LSN: 8192
  Creator: Go-Redo-Parser v1.0
  Flags: 0x00000000

Checkpoint 1:
  Checkpoint LSN: 8192

Checkpoint 2:
  Checkpoint LSN: 8192

Reading entire file...

Found 54 MTRs

  MTR 1: LSN 10240-10256 (1 records)
  MTR 2: LSN 10256-10273 (1 records)
  MTR 3: LSN 10273-10310 (2 records)
  MTR 4: LSN 10310-10325 (1 records)
  MTR 5: LSN 10325-10340 (1 records)
  ... and 49 more MTRs (use -verbose to see all)
```

## Redo Log 格式说明

### Block 结构（512 字节）

```
Offset  Size  Field
------  ----  -----
0       4     hdr_no (block number)
4       2     data_len (bytes written)
6       2     first_rec_group
8       4     epoch_no
12      496   data (log records)
508     4     checksum
```

### Record 格式

```
[Type (1)] [Space ID (compressed)] [Page No (compressed)] [Data...]
```

### 支持的 Record 类型

- `MLOG_1BYTE` (1): 写入 1 字节
- `MLOG_2BYTES` (2): 写入 2 字节
- `MLOG_4BYTES` (4): 写入 4 字节
- `MLOG_8BYTES` (8): 写入 8 字节
- `MLOG_WRITE_STRING` (30): 写入字符串
- `MLOG_PAGE_CREATE` (19): 创建页面
- `MLOG_REC_INSERT` (67): 插入记录
- `MLOG_REC_DELETE` (69): 删除记录
- `MLOG_MULTI_REC_END` (31): MTR 结束标记
- ... 等 70+ 种类型

### 压缩整数格式

```
1 byte:  0xxxxxxx               (< 128)
2 bytes: 10xxxxxx xxxxxxxx      (< 16K)
3 bytes: 110xxxxx ...           (< 2M)
4 bytes: 1110xxxx ...           (< 256M)
5 bytes: 11110000 ...           (>= 256M)
```

## 参考资料

- [InnoDB Redo Log Format Documentation](docs/REDO_LOG_FORMAT.md)
- [Percona Server 8.4.3-3 Source Code](https://github.com/percona/percona-server)
- MySQL 8.0 Reference Manual

## 作者

基于 Percona Server 8.4.3-3 源码实现

## 许可证

GPL v2.0
