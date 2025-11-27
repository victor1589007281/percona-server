# 使用指南

## 1. 命令行工具使用

### ibdinfo - IBD文件信息查看工具

#### 基本用法

查看IBD文件基本信息：
```bash
./bin/ibdinfo -file /path/to/your/file.ibd
```

查看特定Page的详细信息：
```bash
./bin/ibdinfo -file /path/to/your/file.ibd -page 1
```

验证所有Page的Checksum：
```bash
./bin/ibdinfo -file /path/to/your/file.ibd -verify
```

#### 输出示例

```
=== IBD文件信息 ===
文件路径: test.ibd
Page大小: 16384 bytes (16 KB)
Page总数: 100

=== FSP Header (Page 0) ===
Page类型: 0x0008 (FSP_HDR)
Checksum: 0x12345678
LSN: 123456789
Space ID: 1

FSP SpaceID: 1
FSP Size: 100 pages
FSP FreeLimit: 50
FSP Flags: 0x00000001
```

## 2. Go Package使用

### 读取IBD文件

```go
package main

import (
    "fmt"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
)

func main() {
    // 打开IBD文件
    r, err := reader.Open("test.ibd")
    if err != nil {
        panic(err)
    }
    defer r.Close()
    
    // 获取文件信息
    fmt.Printf("Page大小: %d\n", r.GetPageSize())
    fmt.Printf("Page总数: %d\n", r.GetPageCount())
    
    // 读取Page 0
    page, err := r.ReadPage(0)
    if err != nil {
        panic(err)
    }
    
    fmt.Printf("Page号: %d\n", page.PageNumber)
    fmt.Printf("Page类型: 0x%04X\n", page.PageType)
}
```

### 解析FSP Header

```go
package main

import (
    "fmt"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/fsp"
)

func main() {
    r, _ := reader.Open("test.ibd")
    defer r.Close()
    
    // 读取Page 0
    page0, _ := r.ReadPage(0)
    
    // 解析FSP Header
    fspHeader, err := fsp.ParseFSPHeader(page0)
    if err != nil {
        panic(err)
    }
    
    fmt.Printf("Space ID: %d\n", fspHeader.SpaceID)
    fmt.Printf("Size: %d pages\n", fspHeader.Size)
    fmt.Printf("Free Limit: %d\n", fspHeader.FreeLimit)
}
```

### 修改Page并更新Checksum

```go
package main

import (
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/modifier"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/writer"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
    // 读取Page
    r, _ := reader.Open("test.ibd")
    page, _ := r.ReadPage(1)
    r.Close()
    
    // 创建修改器
    mod := modifier.NewModifier(page)
    
    // 修改Space ID
    mod.SetSpaceID(999)
    
    // 修改Page号
    mod.SetPageNumber(100)
    
    // 写入自定义数据
    customData := []byte{0x01, 0x02, 0x03, 0x04}
    mod.WriteData(100, customData)
    
    // 更新Checksum (CRC32)
    mod.UpdateChecksum(types.ChecksumCRC32)
    
    // 写回文件
    w, _ := writer.Open("test.ibd", r.GetPageSize())
    defer w.Close()
    
    w.WritePage(mod.GetPage())
    w.Sync()
}
```

### 验证Checksum

```go
package main

import (
    "fmt"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/checksum"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/types"
)

func main() {
    r, _ := reader.Open("test.ibd")
    defer r.Close()
    
    page, _ := r.ReadPage(0)
    
    // 验证CRC32 Checksum
    if checksum.Verify(page.Data, types.ChecksumCRC32) {
        fmt.Println("Checksum有效")
    } else {
        fmt.Println("Checksum无效")
    }
    
    // 手动计算Checksum
    crc32 := checksum.CalculateCRC32(page.Data)
    fmt.Printf("CRC32: 0x%08X\n", crc32)
    
    innodb := checksum.CalculateInnoDBChecksum(page.Data)
    fmt.Printf("InnoDB Checksum: 0x%08X\n", innodb)
}
```

### 批量读取Page

```go
package main

import (
    "fmt"
    "github.com/percona/percona-server/vdocs/go-page-parser/pkg/reader"
)

func main() {
    r, _ := reader.Open("test.ibd")
    defer r.Close()
    
    // 从Page 0开始读取10个Page
    pages, err := r.ReadPages(0, 10)
    if err != nil {
        panic(err)
    }
    
    for i, page := range pages {
        fmt.Printf("Page %d: Type=0x%04X, LSN=%d\n", 
            i, page.PageType, page.LSN)
    }
}
```

## 3. Page类型说明

| 类型值 | 名称 | 说明 |
|--------|------|------|
| 0x0000 | ALLOCATED | 新分配的页 |
| 0x0002 | UNDO_LOG | Undo日志页 |
| 0x0003 | INODE | Inode页 |
| 0x0005 | IBUF_BITMAP | Insert buffer位图 |
| 0x0008 | FSP_HDR | File space header |
| 0x0009 | XDES | Extent descriptor page |
| 0x000A | BLOB | BLOB页 |
| 0x0011 | SDI | Serialized Dictionary Information |
| 0x45BF | INDEX | B-tree索引页 |

## 4. Checksum算法

支持以下Checksum算法：

1. **ChecksumCRC32** - CRC32算法（推荐）
2. **ChecksumInnoDB** - InnoDB自定义算法（兼容性）
3. **ChecksumNone** - 无Checksum

## 5. 注意事项

1. **备份数据**：在修改IBD文件之前，务必备份原始文件
2. **停止MySQL**：修改IBD文件时，确保MySQL已停止
3. **Checksum一致性**：修改Page后必须更新Checksum
4. **Page大小**：默认16KB，但可能是4KB、8KB、32KB或64KB
5. **字节序**：InnoDB使用Big Endian字节序

## 6. 故障排查

### 无法打开IBD文件
- 检查文件路径是否正确
- 检查文件权限
- 确认文件是有效的IBD文件

### Checksum验证失败
- 文件可能已损坏
- Page大小可能不正确
- 尝试不同的Checksum算法

### Page读取超出范围
- 确认Page号在有效范围内
- 使用GetPageCount()获取总Page数
