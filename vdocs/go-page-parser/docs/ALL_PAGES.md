# InnoDB所有Page类型详解

本文档详细说明了InnoDB支持的所有Page类型及其解析方法。

## Page类型列表

### 1. FSP_HDR (0x0008) - File Space Header

**用途**: Tablespace的第一个page，包含整个表空间的元数据

**关键字段**:
- SpaceID: 表空间ID
- Size: 表空间大小（page数）
- FreeLimit: 空闲page限制
- SpaceFlags: 表空间标志（包含page大小等信息）
- FragNUsed: 已使用碎片page数
- SegID: 下一个segment ID

**解析示例**:
```go
page, _ := reader.ReadPage(0) // FSP Header总是在page 0
fspHeader, err := fsp.ParseFSPHeader(page)
fmt.Printf("Space ID: %d, Size: %d pages\n", fspHeader.SpaceID, fspHeader.Size)
```

### 2. INDEX (0x45BF) - B-tree Index Page

**用途**: B-tree索引页，存储索引数据和记录

**关键字段**:
- NDirSlots: 目录槽数量
- HeapTop: 堆顶指针
- NHeap: 堆中记录数
- NRecs: 用户记录数
- Level: B-tree层级（0为叶子节点）
- IndexID: 索引ID
- MaxTrxID: 最大事务ID
- IsCompact: 是否为紧凑格式

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
indexPage, err := index.ParseIndexPage(page)
fmt.Printf("Level: %d, Records: %d, Index ID: %d\n", 
    indexPage.Header.Level,
    indexPage.Header.NRecs,
    indexPage.Header.IndexID)

// 获取目录槽
slots := index.GetDirectorySlots(indexPage)
```

### 3. UNDO_LOG (0x0002) - Undo Log Page

**用途**: 存储事务的undo信息，用于回滚和MVCC

**关键字段**:
- PageType: TRX_UNDO_INSERT或TRX_UNDO_UPDATE
- PageStart: 最新事务undo记录开始位置
- PageFree: 第一个空闲字节偏移

**Undo Log Header字段**:
- TrxID: 事务ID
- TrxNo: 事务编号
- LogStart: 第一个undo记录偏移
- Flags: Undo标志（XID、GTID等）
- TableID: 表ID

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
undoPage, err := undo.ParseUndoPage(page)

// 解析undo log header
logHeader, err := undo.ParseUndoLogHeader(page, offset)
fmt.Printf("Trx ID: %d, Table ID: %d\n", logHeader.TrxID, logHeader.TableID)
```

### 4. INODE (0x0003) - Inode Page

**用途**: 存储segment inode信息

**关键字段**:
- SegmentID: Segment ID
- NotFullNUsed: 非满extent中已使用page数
- MagicN: Magic number
- FragmentPages: 碎片page数组（32个）

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
inodePage, err := inode.ParseInodePage(page)

for _, inode := range inodePage.Inodes {
    fmt.Printf("Segment ID: %d, Not Full Used: %d\n",
        inode.SegmentID, inode.NotFullNUsed)
}
```

### 5. XDES (0x0009) - Extent Descriptor Page

**用途**: 描述extent的状态和所属segment

**关键字段**:
- SegmentID: 所属Segment ID
- State: Extent状态（FREE、FREE_FRAG、FULL_FRAG、FSEG）
- Bitmap: Page状态位图（描述64个page的状态）

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
xdesPage, err := xdes.ParseXDESPage(page)

for _, entry := range xdesPage.Entries {
    fmt.Printf("Segment: %d, State: %d\n", entry.SegmentID, entry.State)
}
```

### 6. TRX_SYS (0x0007) - Transaction System Page

**用途**: 事务系统头部，存储系统级事务信息

**关键字段**:
- TrxIDStore: 事务ID存储
- RSegs: Rollback segment page号数组（128个）

**解析示例**:
```go
page, _ := reader.ReadPage(5) // TRX_SYS总是在page 5
trxsysPage, err := trxsys.ParseTRXSysPage(page)
fmt.Printf("Trx ID Store: %d\n", trxsysPage.Header.TrxIDStore)
```

### 7. RSEG_ARRAY (0x0015) - Rollback Segment Array

**用途**: 存储rollback segment page号数组

**解析示例**:
```go
page, _ := reader.ReadPage(3) // RSEG_ARRAY通常在page 3
rsegPage, err := rseg.ParseRSEGArrayPage(page)
fmt.Printf("Found %d rollback segments\n", len(rsegPage.RSegPageNos))
```

### 8. LOB Pages - Large Object Pages

#### 8.1 LOB_FIRST (0x0018) / ZLOB_FIRST (0x0019)

**用途**: LOB的第一个page，包含LOB元数据

**关键字段**:
- Version: LOB版本
- LOBLen: LOB总长度
- IndexPage: Index page号

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
lobFirst, err := lob.ParseLOBFirstPage(page)
fmt.Printf("LOB Length: %d, Index Page: %d\n",
    lobFirst.LOBLen, lobFirst.IndexPage)
```

#### 8.2 LOB_DATA (0x0017) / ZLOB_DATA (0x001A)

**用途**: 存储LOB实际数据

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
lobData, err := lob.ParseLOBDataPage(page)
fmt.Printf("Data length: %d bytes\n", len(lobData.Data))
```

#### 8.3 LOB_INDEX (0x0016) / ZLOB_INDEX (0x001B)

**用途**: LOB索引，指向LOB数据页

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
lobIndex, err := lob.ParseLOBIndexPage(page)

for _, entry := range lobIndex.Entries {
    fmt.Printf("Page: %d, Offset: %d, Length: %d\n",
        entry.PageNo, entry.Offset, entry.Length)
}
```

### 9. SDI (0x0011) - Serialized Dictionary Information

**用途**: 存储数据字典序列化信息（JSON格式）

**关键字段**:
- Version: SDI版本
- Type: SDI类型
- Compressed: 是否压缩
- DataLen: 数据长度
- Data: SDI数据（JSON）

**解析示例**:
```go
page, _ := reader.ReadPage(pageNum)
sdiPage, err := sdi.ParseSDIPage(page)
fmt.Printf("SDI Version: %d, Type: %d\n", sdiPage.Header.Version, sdiPage.Header.Type)
fmt.Printf("SDI Data: %s\n", string(sdiPage.Data))
```

### 10. 其他Page类型

- **ALLOCATED (0x0000)**: 新分配的page，尚未使用
- **IBUF_FREE_LIST (0x0004)**: Insert buffer空闲列表
- **IBUF_BITMAP (0x0005)**: Insert buffer位图
- **SYS (0x0006)**: 系统page
- **BLOB (0x000A)**: 未压缩BLOB page
- **ZLOB_FRAG (0x001C)**: 压缩LOB片段page

## 使用通用Page工厂

### 自动识别和解析Page类型

```go
import "github.com/percona/percona-server/vdocs/go-page-parser/pkg/parser/factory"

// 读取page
page, _ := reader.ReadPage(pageNum)

// 自动解析
parsed, err := factory.ParsePage(page)
if err != nil {
    panic(err)
}

fmt.Printf("Page Type: %s (0x%04X)\n", parsed.GetTypeName(), parsed.GetPageType())

// 获取具体类型
switch parsed.GetPageType() {
case types.FIL_PAGE_INDEX:
    var indexPage *types.IndexPage
    factory.GetSpecificPage(parsed, &indexPage)
    fmt.Printf("Index Level: %d\n", indexPage.Header.Level)
    
case types.FIL_PAGE_TYPE_FSP_HDR:
    var fspHeader *types.FSPHeader
    factory.GetSpecificPage(parsed, &fspHeader)
    fmt.Printf("Space Size: %d\n", fspHeader.Size)
}
```

### 批量分析IBD文件中的所有Page

```go
r, _ := reader.Open("test.ibd")
defer r.Close()

pageCount := r.GetPageCount()
pageTypes := make(map[string]int)

for i := uint32(0); i < pageCount; i++ {
    page, err := r.ReadPage(i)
    if err != nil {
        continue
    }
    
    typeName := factory.GetPageTypeName(page.PageType)
    pageTypes[typeName]++
}

fmt.Println("=== Page Type Statistics ===")
for typeName, count := range pageTypes {
    fmt.Printf("%s: %d pages\n", typeName, count)
}
```

## Page修改示例

### 修改Index Page

```go
page, _ := reader.ReadPage(pageNum)
indexPage, _ := index.ParseIndexPage(page)

// 修改Header
indexPage.Header.NRecs = 10
indexPage.Header.Level = 1

// 写回
index.WriteIndexPageHeader(page, indexPage.Header)

// 更新Checksum
mod := modifier.NewModifier(page)
mod.UpdateChecksum(types.ChecksumCRC32)

// 写入文件
w, _ := writer.Open("test.ibd", r.GetPageSize())
w.WritePage(page)
w.Sync()
```

### 修改Undo Log Header

```go
page, _ := reader.ReadPage(pageNum)

logHeader, _ := undo.ParseUndoLogHeader(page, offset)
logHeader.TrxID = 999999

undo.WriteUndoLogHeader(page, offset, logHeader)

// 更新Checksum
mod := modifier.NewModifier(page)
mod.UpdateChecksum(types.ChecksumCRC32)
```

## Page内部结构

### 通用Page结构

```
+-----------------------+
| FIL Header (38 bytes) |  // 所有page类型共有
+-----------------------+
| Page-specific data    |  // 根据page类型不同
|                       |
|   ...                 |
|                       |
+-----------------------+
| FIL Trailer (8 bytes) |  // 所有page类型共有
+-----------------------+
```

### FIL Header (38 bytes)

| 偏移 | 大小 | 字段 | 说明 |
|-----|------|------|------|
| 0 | 4 | Checksum | Checksum或Space ID |
| 4 | 4 | Page Offset | Page号 |
| 8 | 4 | Prev Page | 前一个Page |
| 12 | 4 | Next Page | 后一个Page |
| 16 | 8 | LSN | 日志序列号 |
| 24 | 2 | Page Type | Page类型 |
| 26 | 8 | Flush LSN | Flush日志序列号 |
| 34 | 4 | Space ID | 表空间ID |

### FIL Trailer (8 bytes)

| 偏移（从末尾） | 大小 | 字段 | 说明 |
|---------------|------|------|------|
| 8 | 4 | Old Checksum | Old checksum value |
| 4 | 4 | LSN Low | LSN低32位 |

## 注意事项

1. **Page大小**: 默认16KB，但可以是4KB、8KB、32KB或64KB
2. **Checksum验证**: 修改page后必须重新计算checksum
3. **字节序**: InnoDB使用Big Endian字节序
4. **事务一致性**: 修改page时要注意事务一致性
5. **备份**: 修改IBD文件前务必备份

## 性能提示

1. **批量读取**: 使用`ReadPages`批量读取page
2. **缓存解析结果**: 解析结果可以缓存重用
3. **避免频繁写入**: 批量修改后一次性写入
4. **使用正确的Page大小**: 从FSP Header读取实际page大小

## 参考

- Percona Server 8.4.3-3 源码
- MySQL InnoDB存储引擎文档
- InnoDB Page Format规范
