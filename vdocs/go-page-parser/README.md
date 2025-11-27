# InnoDB IBD & Page Parser - 完整版

这是一个用Golang实现的完整InnoDB IBD文件和Page解析、修改工具包，基于Percona Server 8.4.3-3源码，支持**所有Page类型**的解析和修改。

## ✨ 特性

### 核心功能
- ✅ **完整的Page类型支持** - 支持所有19种InnoDB Page类型
- ✅ 读取IBD文件中的任意Page
- ✅ 解析所有Page类型的详细结构
- ✅ 修改Page数据并自动更新Checksum
- ✅ 写回IBD文件
- ✅ 通用Page工厂和自动类型识别
- ✅ Checksum计算和验证（CRC32和InnoDB算法）

### 开发质量
- ✅ 完整的单元测试（平均覆盖率70%+）
- ✅ 详细的文档和使用示例
- ✅ 命令行工具
- ✅ 多个实战示例程序

## 📋 支持的Page类型

| Page类型 | 代码 | 说明 | 支持状态 |
|---------|------|------|---------|
| FSP_HDR | 0x0008 | File Space Header | ✅ 完全支持 |
| INDEX | 0x45BF | B-tree索引页 | ✅ 完全支持 |
| UNDO_LOG | 0x0002 | Undo日志页 | ✅ 完全支持 |
| INODE | 0x0003 | Segment Inode页 | ✅ 完全支持 |
| XDES | 0x0009 | Extent Descriptor | ✅ 完全支持 |
| TRX_SYS | 0x0007 | 事务系统页 | ✅ 完全支持 |
| RSEG_ARRAY | 0x0015 | Rollback Segment Array | ✅ 完全支持 |
| LOB_FIRST | 0x0018 | LOB第一页 | ✅ 完全支持 |
| LOB_DATA | 0x0017 | LOB数据页 | ✅ 完全支持 |
| LOB_INDEX | 0x0016 | LOB索引页 | ✅ 完全支持 |
| ZLOB_FIRST | 0x0019 | 压缩LOB第一页 | ✅ 完全支持 |
| ZLOB_DATA | 0x001A | 压缩LOB数据页 | ✅ 完全支持 |
| ZLOB_INDEX | 0x001B | 压缩LOB索引页 | ✅ 完全支持 |
| SDI | 0x0011 | 数据字典信息 | ✅ 完全支持 |
| ALLOCATED | 0x0000 | 新分配的页 | ✅ 支持 |
| IBUF_BITMAP | 0x0005 | Insert Buffer位图 | ✅ 支持 |
| BLOB | 0x000A | BLOB页 | ✅ 支持 |
| SYS | 0x0006 | 系统页 | ✅ 支持 |
| ZLOB_FRAG | 0x001C | 压缩LOB片段 | ✅ 支持 |

## �� 项目结构

```
vdocs/go-page-parser/
├── README.md                          # 项目说明
├── go.mod                             # Go模块定义
├── docs/                              # 文档目录
│   ├── PAGE_FORMAT.md                 # InnoDB Page格式文档
│   ├── USAGE.md                       # 使用指南
│   └── ALL_PAGES.md                   # 所有Page类型详解
├── pkg/                               # 核心包
│   ├── types/                         # 类型定义和常量
│   │   ├── constants.go               # InnoDB常量定义
│   │   ├── page_constants.go          # Page常量
│   │   ├── models.go                  # 数据模型
│   │   └── page_models.go             # Page模型
│   ├── checksum/                      # Checksum计算
│   ├── reader/                        # IBD文件读取器
│   ├── parser/                        # Page解析器
│   │   ├── factory/                   # 通用Page工厂
│   │   ├── fsp/                       # FSP Header解析
│   │   ├── index/                     # Index Page解析
│   │   ├── undo/                      # Undo Log解析
│   │   ├── inode/                     # Inode Page解析
│   │   ├── xdes/                      # XDES Page解析
│   │   ├── trxsys/                    # TRX_SYS解析
│   │   ├── rseg/                      # RSEG_ARRAY解析
│   │   ├── lob/                       # LOB Pages解析
│   │   └── sdi/                       # SDI Page解析
│   ├── modifier/                      # Page修改器
│   └── writer/                        # IBD文件写入器
├── cmd/                               # 命令行工具
│   └── ibdinfo/                       # IBD信息查看工具
├── examples/                          # 示例代码
│   ├── modify_page.go                 # 基础Page修改示例
│   ├── analyze_all_pages.go           # 完整IBD分析工具
│   └── modify_index_page.go           # Index Page修改示例
└── bin/                               # 编译输出
    ├── ibdinfo                        # IBD信息工具
    ├── analyze_all_pages              # IBD分析工具
    ├── modify_index_page              # Index Page修改工具
    └── modify_page                    # 通用Page修改工具
```

## 🚀 快速开始

### 1. 编译

```bash
# 编译所有工具
go build -o bin/ibdinfo ./cmd/ibdinfo/
go build -o bin/analyze_all_pages ./examples/analyze_all_pages.go
go build -o bin/modify_index_page ./examples/modify_index_page.go
go build -o bin/modify_page ./examples/modify_page.go
```

### 2. 运行测试

```bash
# 运行所有单元测试
go test -v ./...

# 查看测试覆盖率
go test -cover ./...
```

### 3. 使用命令行工具

#### 查看IBD文件信息
```bash
./bin/ibdinfo -file /path/to/table.ibd
```

#### 分析所有Page类型
```bash
./bin/analyze_all_pages /path/to/table.ibd
```

输出示例：
```
=== 分析IBD文件: test.ibd ===

文件信息:
  Page大小: 16384 bytes (16 KB)
  Page总数: 1000

=== Page类型统计 ===
  FSP_HDR             :     1 pages (0.10%)
  INDEX               :   850 pages (85.00%)
  INODE               :     5 pages (0.50%)
  SDI                 :     1 pages (0.10%)
  ALLOCATED           :   143 pages (14.30%)
```

#### 修改Index Page（演示）
```bash
./bin/modify_index_page /path/to/table.ibd 4
```

## 💻 编程使用

### 使用通用Page工厂（推荐）

```go
import (
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/factory"
)

// 打开IBD文件
r, _ := reader.Open("test.ibd")
defer r.Close()

// 读取Page
page, _ := r.ReadPage(pageNum)

// 自动识别和解析
parsed, _ := factory.ParsePage(page)
fmt.Printf("Page类型: %s\n", parsed.GetTypeName())

// 根据类型处理
switch parsed.GetPageType() {
case types.FIL_PAGE_INDEX:
    var indexPage *types.IndexPage
    factory.GetSpecificPage(parsed, &indexPage)
    fmt.Printf("索引层级: %d, 记录数: %d\n",
        indexPage.Header.Level,
        indexPage.Header.NRecs)
    
case types.FIL_PAGE_TYPE_FSP_HDR:
    var fspHeader *types.FSPHeader
    factory.GetSpecificPage(parsed, &fspHeader)
    fmt.Printf("表空间大小: %d pages\n", fspHeader.Size)
}
```

### 解析特定Page类型

#### Index Page (B-tree索引页)
```go
import "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/index"

page, _ := reader.ReadPage(pageNum)
indexPage, _ := index.ParseIndexPage(page)

fmt.Printf("Level: %d\n", indexPage.Header.Level)
fmt.Printf("Records: %d\n", indexPage.Header.NRecs)
fmt.Printf("Index ID: %d\n", indexPage.Header.IndexID)

// 获取目录槽
slots := index.GetDirectorySlots(indexPage)
```

#### Undo Log Page
```go
import "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/undo"

page, _ := reader.ReadPage(pageNum)
undoPage, _ := undo.ParseUndoPage(page)

// 解析Undo Log Header
logHeader, _ := undo.ParseUndoLogHeader(page, offset)
fmt.Printf("Trx ID: %d\n", logHeader.TrxID)
```

#### LOB Pages
```go
import "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/lob"

// LOB First Page
page, _ := reader.ReadPage(pageNum)
lobFirst, _ := lob.ParseLOBFirstPage(page)
fmt.Printf("LOB长度: %d\n", lobFirst.LOBLen)

// LOB Data Page
lobData, _ := lob.ParseLOBDataPage(page)
fmt.Printf("数据: %d bytes\n", len(lobData.Data))

// LOB Index Page
lobIndex, _ := lob.ParseLOBIndexPage(page)
for _, entry := range lobIndex.Entries {
    fmt.Printf("Page: %d, Length: %d\n", entry.PageNo, entry.Length)
}
```

### 修改Page

```go
import (
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/modifier"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/writer"
)

// 读取Page
page, _ := reader.ReadPage(pageNum)

// 创建修改器
mod := modifier.NewModifier(page)

// 修改数据
mod.SetSpaceID(999)
mod.SetPageNumber(100)

// 更新Checksum
mod.UpdateChecksum(types.ChecksumCRC32)

// 写回文件
w, _ := writer.Open("test.ibd", reader.GetPageSize())
defer w.Close()
w.WritePage(mod.GetPage())
w.Sync()
```

## 📖 文档

- [所有Page类型详解](docs/ALL_PAGES.md) - 19种Page类型的完整说明
- [Page格式文档](docs/PAGE_FORMAT.md) - InnoDB Page内部结构
- [使用指南](docs/USAGE.md) - 详细的API文档和示例

## 📊 测试覆盖率

```
pkg/types         100.0%  ✅
pkg/parser/fsp    100.0%  ✅
pkg/modifier       93.8%  ✅
pkg/checksum       93.5%  ✅
pkg/reader         76.0%  ✅
pkg/parser/index   71.0%  ✅
pkg/parser/undo    51.6%  ✅
pkg/parser/factory 14.1%  ⚠️
```

## 🎯 使用场景

### 1. 数据恢复
```go
// 修复损坏的Page Checksum
page, _ := reader.ReadPage(damagedPageNum)
mod := modifier.NewModifier(page)
mod.UpdateChecksum(types.ChecksumCRC32)
writer.WritePage(page)
```

### 2. 数据分析
```go
// 统计表空间中的Page类型分布
pageTypes := make(map[string]int)
for i := uint32(0); i < reader.GetPageCount(); i++ {
    page, _ := reader.ReadPage(i)
    pageTypes[factory.GetPageTypeName(page.PageType)]++
}
```

### 3. 性能调优
```go
// 分析Index Page的碎片率
indexPage, _ := index.ParseIndexPage(page)
fragmentationRate := float64(indexPage.Header.Garbage) / 
                     float64(indexPage.Header.HeapTop) * 100
fmt.Printf("碎片率: %.2f%%\n", fragmentationRate)
```

### 4. 安全审计
```go
// 检查所有Undo Log的事务ID
undoPage, _ := undo.ParseUndoPage(page)
logHeader, _ := undo.ParseUndoLogHeader(page, offset)
if logHeader.TrxID > suspiciousThreshold {
    fmt.Printf("发现可疑事务: %d\n", logHeader.TrxID)
}
```

## ⚠️ 重要提醒

1. **备份数据**: 修改IBD文件前务必备份
2. **停止MySQL**: 修改时确保MySQL已停止
3. **更新Checksum**: 修改Page后必须更新Checksum
4. **了解结构**: 深入理解InnoDB结构，避免破坏数据完整性
5. **测试环境**: 先在测试环境验证，再应用到生产环境

## 🔧 技术细节

### InnoDB Page结构

每个Page包含三部分：
1. **FIL Header** (38 bytes) - 所有Page通用
2. **Page Data** (变长) - 根据Page类型不同
3. **FIL Trailer** (8 bytes) - 所有Page通用

### Checksum算法

支持两种算法：
- **CRC32**: 标准CRC32算法（推荐）
- **InnoDB**: InnoDB自定义算法（兼容性）

### Page大小

支持的Page大小：
- 4 KB (4096 bytes)
- 8 KB (8192 bytes)
- 16 KB (16384 bytes) - 默认
- 32 KB (32768 bytes)
- 64 KB (65536 bytes)

## 🤝 参考资料

本项目基于以下源码实现：
- Percona Server 8.4.3-3 源码
- `storage/innobase/include/fil0types.h` - Page Header定义
- `storage/innobase/include/page0types.h` - Index Page定义
- `storage/innobase/include/trx0undo.h` - Undo Log定义
- `storage/innobase/include/fsp0fsp.h` - FSP Header定义
- `storage/innobase/buf/checksum.cc` - Checksum实现

## �� 项目统计

- **Go源文件**: 30+ 个
- **代码行数**: 3000+ 行
- **测试文件**: 10+ 个
- **支持的Page类型**: 19 种
- **平均测试覆盖率**: 70%+

## 📝 许可证

本项目遵循与Percona Server相同的许可证。

## 👨‍💻 作者

基于Percona Server 8.4.3-3源码实现

---

**最后更新**: 2025年11月23日

**版本**: v2.0 - 完整版（支持所有Page类型）
