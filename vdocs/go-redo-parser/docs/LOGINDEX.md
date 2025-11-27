# LogIndex 实现文档

**位置：** `vdocs/go-redo-parser/pkg/logindex/`

## 概述

LogIndex 是一个高性能的索引结构，用于 PolarDB 风格的 Redo Log 管理。它使用 Swiss Table + B+Tree 的组合设计，提供快速的 LSN 查找和范围查询能力。

## 架构设计

```
LogIndex (Swiss Table)
    ├─ Key: PageID (SpaceID + PageNo)
    └─ Value: LSNBPlusTree
              ├─ Insert LSN
              ├─ Query single LSN
              ├─ Query LSN range [start, end]
              └─ Delete LSNs before threshold
```

### 核心组件

1. **Swiss Table** (`swiss_table.go`)
   - 基于 Google Swiss Table 设计的高性能哈希表
   - 开放定址法 + 二次探测
   - 87.5% 负载因子
   - O(1) 平均查找时间

2. **B+Tree** (`bplustree.go`)
   - Degree 16 优化（适应 CPU 缓存行）
   - 叶子节点链表支持高效范围查询
   - 支持插入、查询、删除操作
   - O(log N) 时间复杂度

3. **LogIndex** (`logindex.go`)
   - 组合 Swiss Table 和 B+Tree
   - 线程安全的并发访问
   - 支持批量操作
   - 提供统计和监控接口

## 使用方法

### 基本使用

```go
package main

import (
    "fmt"
    "github.com/percona/go-redo-parser/pkg/logindex"
)

func main() {
    // 创建 LogIndex
    idx := logindex.NewLogIndex()
    
    // 插入 LSN
    idx.Insert(1, 100, 1000)  // spaceID=1, pageNo=100, lsn=1000
    idx.Insert(1, 100, 1050)
    idx.Insert(1, 100, 1100)
    
    // 查询单个 LSN
    exists := idx.Contains(1, 100, 1050)
    fmt.Printf("LSN 1050 exists: %v\n", exists)
    
    // 查询所有 LSN
    lsns := idx.Query(1, 100)
    fmt.Printf("All LSNs: %v\n", lsns)
    
    // 范围查询
    rangeResult := idx.QueryRange(1, 100, 1000, 1080)
    fmt.Printf("LSNs in range [1000, 1080]: %v\n", rangeResult)
    
    // 获取 Page LSN 范围
    minLSN, maxLSN, exists := idx.GetPageLSNRange(1, 100)
    if exists {
        fmt.Printf("Page LSN range: [%d, %d]\n", minLSN, maxLSN)
    }
    
    // 清理旧 LSN
    purged := idx.PurgeBefore(1050)
    fmt.Printf("Purged %d LSNs\n", purged)
    
    // 获取统计信息
    stats := idx.GetStats()
    fmt.Printf("Stats: %s\n", stats.String())
}
```

### 批量操作

```go
// 批量插入
lsns := []uint64{1000, 1100, 1200, 1300, 1400}
err := idx.BatchInsert(1, 100, lsns)
if err != nil {
    panic(err)
}

// 遍历所有 Page
idx.ForEachPage(func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool {
    fmt.Printf("Page (%d, %d): %d LSNs, Range=[%d, %d]\n",
        spaceID, pageNo, count, minLSN, maxLSN)
    return true // 继续迭代
})
```

### PolarDB 场景模拟

参见 `examples/polardb_simulation.go`：

```go
// Phase 1: 主节点生成 Redo Log
for txn := 0; txn < numTransactions; txn++ {
    // 每个事务修改多个 Page
    for i := 0; i < numPages; i++ {
        currentLSN++
        idx.Insert(spaceID, pageNo, currentLSN)
    }
}

// Phase 2: 只读节点回放
// 获取 Page 的 LSN 范围
minLSN, maxLSN, _ := idx.GetPageLSNRange(spaceID, pageNo)

// 查询需要回放的 LSN
lsnsToReplay := idx.QueryRange(spaceID, pageNo, pageLSN+1, maxLSN)

// Phase 3: 检查点和清理
purged := idx.PurgeBefore(minCheckpointLSN)
```

## API 文档

### PageID

```go
type PageID struct {
    SpaceID uint32  // 表空间 ID
    PageNo  uint32  // Page 号
}
```

### LogIndex 主要方法

#### 创建和初始化

```go
// 创建默认容量的 LogIndex
func NewLogIndex() *LogIndex

// 创建指定容量的 LogIndex
func NewLogIndexWithCapacity(capacity int) *LogIndex
```

#### 插入操作

```go
// 插入单个 LSN
func (idx *LogIndex) Insert(spaceID, pageNo uint32, lsn uint64) error

// 批量插入 LSN
func (idx *LogIndex) BatchInsert(spaceID, pageNo uint32, lsns []uint64) error
```

#### 查询操作

```go
// 检查 LSN 是否存在
func (idx *LogIndex) Contains(spaceID, pageNo uint32, lsn uint64) bool

// 查询所有 LSN
func (idx *LogIndex) Query(spaceID, pageNo uint32) []uint64

// 范围查询
func (idx *LogIndex) QueryRange(spaceID, pageNo uint32, startLSN, endLSN uint64) []uint64

// 获取 Page 的 LSN 范围
func (idx *LogIndex) GetPageLSNRange(spaceID, pageNo uint32) (minLSN, maxLSN uint64, exists bool)
```

#### 删除操作

```go
// 删除指定 LSN 之前的所有记录
func (idx *LogIndex) PurgeBefore(threshold uint64) int
```

#### 统计和监控

```go
// 获取当前最大 LSN
func (idx *LogIndex) GetCurrentLSN() uint64

// 获取 Page 数量
func (idx *LogIndex) GetPageCount() int

// 获取总 LSN 数量
func (idx *LogIndex) GetTotalLSNCount() int

// 获取统计信息
func (idx *LogIndex) GetStats() Stats

// 估算内存占用
func (idx *LogIndex) GetMemoryUsage() int64
```

#### 迭代和清理

```go
// 遍历所有 Page
func (idx *LogIndex) ForEachPage(fn func(spaceID, pageNo uint32, minLSN, maxLSN uint64, count int) bool)

// 清空所有数据
func (idx *LogIndex) Clear()
```

### Stats 结构

```go
type Stats struct {
    PageCount  int     // Page 数量
    TotalLSNs  int     // 总 LSN 数
    CurrentLSN uint64  // 当前最大 LSN
    MinLSN     uint64  // 全局最小 LSN
    MaxLSN     uint64  // 全局最大 LSN
}
```

## 性能特性

### 时间复杂度

| 操作 | 平均时间复杂度 | 最坏时间复杂度 |
|------|--------------|--------------|
| Insert | O(1) + O(log N) | O(N) + O(N) |
| Contains | O(1) + O(log N) | O(N) + O(N) |
| Query | O(1) + O(log N + K) | O(N) + O(N + K) |
| QueryRange | O(1) + O(log N + K) | O(N) + O(N + K) |
| PurgeBefore | O(M × log N) | O(M × N) |

其中：
- N = 每个 Page 的 LSN 数量
- K = 查询结果数量
- M = Page 总数

### 性能基准（预期）

在现代硬件上（8核 CPU）：

```
Swiss Table:
  Insert:  ~250 ns/op    120 B/op    2 allocs/op
  Get:     ~150 ns/op      0 B/op    0 allocs/op

B+Tree:
  Insert:      ~400 ns/op    200 B/op    3 allocs/op
  Contains:    ~300 ns/op      0 B/op    0 allocs/op
  RangeQuery:  ~1200 ns/op   512 B/op    5 allocs/op

LogIndex:
  Insert:      ~600 ns/op    300 B/op    5 allocs/op
  QueryRange:  ~2500 ns/op  1024 B/op    8 allocs/op
  Contains:    ~500 ns/op      0 B/op    0 allocs/op
  PurgeBefore: ~100 μs (1000 pages)
```

### 内存占用

对于 100TB 数据库：
- 约 6.7 亿个 Page
- 热点 Page 10%：6700 万个
- 平均每个热点 Page 20 个 LSN
- **总内存占用：~8GB**（约 0.008% 数据量）

## 测试

### 运行测试

```bash
cd vdocs/go-redo-parser

# 运行所有 LogIndex 测试
go test -v ./pkg/logindex/...

# 运行测试并显示覆盖率
go test -cover ./pkg/logindex/...

# 运行基准测试
go test -bench=. -benchmem ./pkg/logindex/...

# 使用 Makefile
make test
make bench
```

### 测试覆盖

- ✅ Swiss Table: 11 个测试用例 + 2 个基准测试
- ✅ B+Tree: 12 个测试用例 + 3 个基准测试
- ✅ LogIndex: 22 个测试用例 + 4 个基准测试
- ✅ 总计：45+ 测试用例

### 示例程序

```bash
# 基本使用示例
go run examples/basic_usage.go

# 性能测试
go run examples/performance_demo.go

# PolarDB 场景模拟
go run examples/polardb_simulation.go
```

## 设计决策

### 为什么选择 Swiss Table？

1. **性能优越**：开放定址法比链式哈希表更快
2. **缓存友好**：更好的内存局部性
3. **负载因子**：87.5% 提供速度和内存的最佳平衡
4. **SIMD 优化**：未来可以使用 SIMD 加速探测

### 为什么选择 B+Tree？

1. **范围查询**：PolarDB 场景中常见的操作
2. **有序存储**：自然维护 LSN 顺序
3. **叶子链表**：支持高效的全扫描
4. **缓存友好**：Degree 16 适应 64 字节缓存行

### 为什么不用其他数据结构？

- **Skip List**：虽然简单，但缓存局部性不如 B+Tree
- **Red-Black Tree**：更复杂，无明显优势
- **简单数组**：O(N) 插入，不可扩展

## 应用场景

### 1. PolarDB 风格的 Redo Log 管理

- 主节点写入 Redo Log
- LogIndex 维护 Page → LSN 映射
- 只读节点按需查询和回放

### 2. 只读节点 Page 回放

```go
// 1. 从 PolarFS 读取 Page
page := polarfs.ReadPage(spaceID, pageNo)
pageLSN := page.Header.LSN

// 2. 查询需要回放的 LSN
maxLSN := idx.GetPageLSNRange(spaceID, pageNo)
lsnsToReplay := idx.QueryRange(spaceID, pageNo, pageLSN+1, maxLSN)

// 3. 回放 Redo Log
for _, lsn := range lsnsToReplay {
    redoRecord := polarfs.ReadRedoLog(lsn)
    applyRedoToPage(page, redoRecord)
}
```

### 3. 检查点和内存管理

```go
// 定期清理旧 LSN
minCheckpointLSN := getMinCheckpointLSN() // 所有只读节点的最小 LSN
purged := idx.PurgeBefore(minCheckpointLSN)
log.Printf("Purged %d LSNs, saved %.2f MB memory", 
    purged, float64(purged*16)/(1024*1024))
```

## 与 PolarDB 架构文档的对应关系

详见 `vdocs/design/PolarDB_Architecture.md` 中的：
- **5.1.7a LogIndex 数据结构优化**
- **5.1.7b LogIndex 物理存储**

本实现对应文档中的 **Swiss Table + B+Tree** 方案。

## 进一步的优化

### 可能的改进方向

1. **SIMD 加速**：Swiss Table 探测使用 SIMD 指令
2. **内存池**：减少小对象分配开销
3. **持久化**：支持 LogIndex 的磁盘持久化
4. **压缩**：LSN 差分编码
5. **分片**：更细粒度的锁

### 实验性功能

- 异步 Purge：后台 goroutine
- Prometheus 监控集成
- 快照和恢复功能

## 参考资料

- [Swiss Table 设计](https://abseil.io/blog/20180927-swisstables)
- [B+Tree 基础](https://en.wikipedia.org/wiki/B%2B_tree)
- [PolarDB 架构白皮书](https://www.alibabacloud.com/blog/polardb-architecture-and-mysql-compatibility_594838)
- [PolarDB 架构文档](../design/PolarDB_Architecture.md)

## 作者与许可

基于 Percona Server 和 PolarDB 架构设计实现。

License: GPL v2.0
