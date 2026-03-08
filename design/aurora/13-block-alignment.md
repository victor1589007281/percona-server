# Skill: 块对齐与原子写 (Block Alignment & Atomic Write)

## 1. 问题背景

取消 Double Write 后，不同数据类型的 IO 大小不一致：

| 数据类型 | 大小 | 特点 |
|----------|------|------|
| Redo Log | 512B - 数KB | 变长，高频追加 |
| Data Page | 16KB | 固定，随机写 |
| Binlog | 变长 (几十B - 数MB) | 变长，追加 |
| 元数据 | 小于 4KB | 低频 |

**问题**：
- 块存储通常以 4KB 或 512B 为原子写单位
- 16KB 页写入可能被撕裂 (partial write)
- 没有文件系统的 fsync 语义

**实现语言**: Golang (存储层)

## 2. 解决方案

### 2.1 存储层原子写保证

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    原子写架构                                                │
│                                                                             │
│  方案 1: 块存储原生支持                                                     │
│  ─────────────────────────────────────────────────────────────────────────  │
│  如果底层块存储支持 16KB 原子写 (如 NVMe with 4KB atomic):                   │
│  • 直接写入，无需额外处理                                                   │
│  • 依赖硬件/存储系统保证                                                    │
│                                                                             │
│  方案 2: 软件层面保证 (推荐)                                                │
│  ─────────────────────────────────────────────────────────────────────────  │
│  • Redo-only 写入: 计算层只写 Redo，存储层应用生成页                        │
│  • 页版本 + LSN: 通过 Redo 重建任意 LSN 的页                                │
│  • 无需 Double Write: 因为页由 Redo 派生，不是直接写入                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Redo-as-Source-of-Truth

```go
// 核心理念：Redo 是唯一的真相来源，页是 Redo 的物化视图

// 写入路径 (只写 Redo)
func (s *StorageLayer) WriteRedo(redo []byte, lsn uint64) error {
    // 1. Redo 追加写入 (顺序 IO，原子性易保证)
    if err := s.redoStore.Append(redo, lsn); err != nil {
        return err
    }
    
    // 2. 后台异步应用到页 (页是派生数据)
    s.applyQueue <- ApplyTask{redo: redo, lsn: lsn}
    
    return nil
}

// 读取路径 (页 = 基础页 + Redo 重建)
func (s *StorageLayer) ReadPage(key PageKey, minLSN uint64) (*Page, error) {
    // 1. 获取基础页 (可能是旧版本)
    basePage, baseLSN := s.pageStore.GetBase(key)
    
    if baseLSN >= minLSN {
        return basePage, nil
    }
    
    // 2. 应用增量 Redo 重建
    redos := s.redoStore.GetRange(key, baseLSN, minLSN)
    page := basePage.Clone()
    for _, redo := range redos {
        applyRedo(page, redo)
    }
    
    return page, nil
}
```

### 2.3 Redo 的原子写保证

```go
// Redo 块格式 (对齐到 512B 或 4KB)
type RedoBlock struct {
    Header   RedoBlockHeader  // 固定头
    Records  []byte           // Redo 记录
    Padding  []byte           // 填充到对齐边界
    Checksum uint32           // CRC32
}

type RedoBlockHeader struct {
    Magic       uint32  // 魔数
    BlockNo     uint64  // 块序号
    FirstLSN    uint64  // 块内第一条记录 LSN
    LastLSN     uint64  // 块内最后一条记录 LSN
    DataLen     uint32  // 有效数据长度
    Flags       uint32  // 标志位
}

const (
    BLOCK_SIZE = 4096  // 对齐到 4KB
)

func (w *RedoWriter) Write(redo []byte, lsn uint64) error {
    // 1. 追加到当前块
    if w.currentBlock.Remaining() < len(redo) {
        // 块满，提交当前块
        if err := w.flushBlock(); err != nil {
            return err
        }
    }
    
    w.currentBlock.Append(redo, lsn)
    return nil
}

func (w *RedoWriter) flushBlock() error {
    // 1. 填充到对齐边界
    w.currentBlock.Pad(BLOCK_SIZE)
    
    // 2. 计算校验和
    w.currentBlock.Checksum = crc32(w.currentBlock.Data())
    
    // 3. 原子写入 (4KB 对齐，块存储保证原子)
    return w.blockIO.WriteAligned(w.currentBlock.ToBytes())
}
```

### 2.4 页物化策略

```go
// 页的物化是可选的优化，不是正确性要求
type PageMaterializer struct {
    interval    time.Duration  // 物化间隔
    threshold   int            // 累积 Redo 数量阈值
}

func (m *PageMaterializer) ShouldMaterialize(key PageKey) bool {
    pendingRedos := m.redoStore.CountPendingRedos(key)
    
    // 策略 1: Redo 累积过多
    if pendingRedos > m.threshold {
        return true
    }
    
    // 策略 2: 热点页定期物化
    if m.isHotPage(key) && m.timeSinceLastMaterialize(key) > m.interval {
        return true
    }
    
    return false
}

func (m *PageMaterializer) Materialize(key PageKey) error {
    // 1. 读取并重建最新页
    page, err := m.storageLayer.ReadPage(key, m.currentLSN)
    if err != nil {
        return err
    }
    
    // 2. 写入页存储 (这个写入可以接受失败，因为可以重建)
    // 使用校验和检测部分写
    page.Checksum = crc32(page.Data)
    return m.pageStore.Write(key, page)
}

// 读取时验证页完整性
func (s *PageStore) Read(key PageKey) (*Page, error) {
    page := s.readRaw(key)
    
    // 校验和验证
    if page.Checksum != crc32(page.Data) {
        // 页损坏，从 Redo 重建
        return nil, ErrPageCorrupted
    }
    
    return page, nil
}
```

## 3. Binlog 的块对齐

```go
// Binlog Event 可能跨块，需要特殊处理
type BinlogBlock struct {
    Header    BinlogBlockHeader
    Events    []byte
    Padding   []byte
    Checksum  uint32
}

type BinlogBlockHeader struct {
    Magic         uint32
    BlockNo       uint64
    FirstEventPos uint64  // 第一个完整事件的位置
    LastEventPos  uint64  // 最后一个完整事件的位置
    Flags         uint32  // CONTINUES_FROM_PREV, CONTINUES_TO_NEXT
}

const (
    FLAG_CONTINUES_FROM_PREV = 1 << 0  // 从上一块继续
    FLAG_CONTINUES_TO_NEXT   = 1 << 1  // 延续到下一块
)

// 大事件跨块写入
func (w *BinlogWriter) WriteLargeEvent(event *BinlogEvent) error {
    data := event.Serialize()
    offset := 0
    
    for offset < len(data) {
        remaining := w.currentBlock.Remaining()
        toWrite := min(remaining, len(data)-offset)
        
        w.currentBlock.Append(data[offset : offset+toWrite])
        offset += toWrite
        
        if offset < len(data) {
            // 标记继续到下一块
            w.currentBlock.Header.Flags |= FLAG_CONTINUES_TO_NEXT
            w.flushBlock()
            
            // 新块标记从上一块继续
            w.currentBlock.Header.Flags |= FLAG_CONTINUES_FROM_PREV
        }
    }
    
    return nil
}
```

## 4. 崩溃恢复

```go
func (s *StorageLayer) Recover() error {
    // 1. 扫描 Redo 块，找到最后一个完整块
    lastValidLSN := s.redoStore.FindLastValidLSN()
    
    // 2. 验证页存储
    // 不需要特殊处理，因为页是从 Redo 派生的
    // 任何损坏的页都可以重建
    
    // 3. 设置恢复点
    s.appliedLSN = lastValidLSN
    
    // 4. 重新应用未物化的 Redo
    go s.applyPendingRedos()
    
    return nil
}

func (s *RedoStore) FindLastValidLSN() uint64 {
    // 从后向前扫描，找到最后一个校验和正确的块
    for blockNo := s.lastBlockNo; blockNo >= 0; blockNo-- {
        block := s.readBlock(blockNo)
        
        if block.Checksum == crc32(block.Data()) {
            return block.Header.LastLSN
        }
        
        // 校验和失败，继续向前
    }
    return 0
}
```

## 5. 配置

```go
type BlockAlignmentConfig struct {
    // 块大小
    RedoBlockSize   int  `default:"4096"`   // 4KB
    BinlogBlockSize int  `default:"4096"`   // 4KB
    PageSize        int  `default:"16384"`  // 16KB
    
    // 物化策略
    MaterializeThreshold int           `default:"100"`    // 累积 100 条 Redo
    MaterializeInterval  time.Duration `default:"10s"`    // 10 秒
    
    // 校验
    EnableChecksum bool `default:"true"`
}
```

## 6. 开发任务

- [ ] 实现 RedoBlock 格式
- [ ] 实现 Redo 对齐写入
- [ ] 实现 Binlog 跨块处理
- [ ] 实现页物化策略
- [ ] 实现崩溃恢复
- [ ] 校验和验证
- [ ] 性能测试

## 7. 参考

- 主文档: IO 优化部分
- Aurora 论文: "The log is the database"
