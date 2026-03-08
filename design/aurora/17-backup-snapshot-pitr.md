# Skill: 备份、快照与 PITR (Backup, Snapshot & PITR)

## 1. 模块职责

提供数据保护能力：
- **快照 (Snapshot)**: 瞬间一致性视图，用于创建备份
- **备份 (Backup)**: 数据导出，用于跨集群恢复
- **PITR**: 恢复到任意时间点

**实现语言**: Golang

## 2. 架构概览

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    备份与恢复架构                                            │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                         快照管理                                       │ │
│  │                                                                        │ │
│  │  • 基于 LSN 的逻辑快照 (无物理拷贝)                                   │ │
│  │  • 利用存储层的多版本能力                                             │ │
│  │  • 瞬间完成，不影响正常 IO                                            │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                              │                                              │
│                              ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                         备份导出                                       │ │
│  │                                                                        │ │
│  │  • 从快照导出数据页                                                   │ │
│  │  • 导出 Redo 范围                                                     │ │
│  │  • 支持增量备份                                                       │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                              │                                              │
│                              ▼                                              │
│  ┌───────────────────────────────────────────────────────────────────────┐ │
│  │                         恢复                                           │ │
│  │                                                                        │ │
│  │  • 全量恢复: 从备份恢复                                               │ │
│  │  • PITR: 应用 Redo 到指定时间点                                       │ │
│  │  • 克隆: 从快照创建新实例                                             │ │
│  └───────────────────────────────────────────────────────────────────────┘ │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 3. 快照设计

### 3.1 逻辑快照 (基于 LSN)

```go
// 快照是一个逻辑概念，不复制数据
type Snapshot struct {
    ID             string
    Name           string
    CreateTime     time.Time
    
    // 快照点
    LSN            uint64     // 快照时的 LSN
    Timestamp      uint64     // 快照时的时间戳
    GTID           string     // 快照时的 GTID Set
    
    // 元数据
    Tablespaces    []TablespaceSnapshot
    SchemaVersions map[uint32]uint64  // space_id -> schema_version
    
    // 状态
    State          SnapshotState  // CREATING, ACTIVE, DELETING
}

type TablespaceSnapshot struct {
    SpaceID       uint32
    SchemaVersion uint64
    TableName     string
    PageCount     uint64
}

// 创建快照 (瞬间完成)
func (s *SnapshotManager) CreateSnapshot(name string) (*Snapshot, error) {
    // 1. 获取当前一致性点
    currentLSN := s.storageLayer.GetDurableLSN()
    currentTS := s.storageLayer.GetTimestamp(currentLSN)
    currentGTID := s.storageLayer.GetGTIDSet()
    
    // 2. 收集元数据
    tablespaces := s.metadataService.ListTablespaces()
    schemaVersions := make(map[uint32]uint64)
    
    var tsSnapshots []TablespaceSnapshot
    for _, ts := range tablespaces {
        schemaVersions[ts.SpaceID] = ts.SchemaVersion
        tsSnapshots = append(tsSnapshots, TablespaceSnapshot{
            SpaceID:       ts.SpaceID,
            SchemaVersion: ts.SchemaVersion,
            TableName:     ts.TableName,
            PageCount:     ts.PageCount,
        })
    }
    
    // 3. 创建快照记录 (不复制数据!)
    snapshot := &Snapshot{
        ID:             uuid.New().String(),
        Name:           name,
        CreateTime:     time.Now(),
        LSN:            currentLSN,
        Timestamp:      currentTS,
        GTID:           currentGTID,
        Tablespaces:    tsSnapshots,
        SchemaVersions: schemaVersions,
        State:          SnapshotActive,
    }
    
    // 4. 注册快照 (通知存储层保留该 LSN 之前的数据)
    s.storageLayer.RegisterSnapshot(snapshot.LSN)
    
    // 5. 持久化快照元数据
    s.snapshotStore.Save(snapshot)
    
    return snapshot, nil
}
```

### 3.2 快照读取

```go
// 从快照读取数据 (用于备份导出)
func (s *SnapshotManager) ReadPageFromSnapshot(
    snapshot *Snapshot,
    spaceID uint32,
    pageNo uint32) (*Page, error) {
    
    // 获取快照时的 schema version
    schemaVersion := snapshot.SchemaVersions[spaceID]
    
    // 读取指定 LSN 版本的页
    return s.storageLayer.ReadPageAtLSN(
        spaceID,
        schemaVersion,
        pageNo,
        snapshot.LSN,  // 读取不超过此 LSN 的版本
    )
}
```

### 3.3 快照保留策略

```go
type SnapshotRetentionPolicy struct {
    MaxSnapshots      int           // 最多保留快照数
    MaxAge            time.Duration // 最长保留时间
    MinRetainCount    int           // 最少保留数量
}

func (s *SnapshotManager) CleanupSnapshots() {
    snapshots := s.snapshotStore.List()
    
    // 按时间排序 (旧的在前)
    sort.Slice(snapshots, func(i, j int) bool {
        return snapshots[i].CreateTime.Before(snapshots[j].CreateTime)
    })
    
    toDelete := []string{}
    
    for i, snap := range snapshots {
        remaining := len(snapshots) - i
        
        // 保留最少数量
        if remaining <= s.policy.MinRetainCount {
            break
        }
        
        // 超过最大数量
        if len(snapshots) > s.policy.MaxSnapshots {
            toDelete = append(toDelete, snap.ID)
            continue
        }
        
        // 超过最大年龄
        if time.Since(snap.CreateTime) > s.policy.MaxAge {
            toDelete = append(toDelete, snap.ID)
        }
    }
    
    for _, id := range toDelete {
        s.DeleteSnapshot(id)
    }
}
```

## 4. 备份设计

### 4.1 全量备份

```go
type BackupManager struct {
    snapshotMgr    *SnapshotManager
    storageLayer   *StorageLayer
    backupStorage  BackupStorage  // S3, 本地, etc.
}

// 全量备份
func (b *BackupManager) CreateFullBackup(name string) (*Backup, error) {
    // 1. 创建快照
    snapshot, err := b.snapshotMgr.CreateSnapshot(name + "_snapshot")
    if err != nil {
        return nil, err
    }
    
    backup := &Backup{
        ID:          uuid.New().String(),
        Name:        name,
        Type:        BackupFull,
        SnapshotID:  snapshot.ID,
        LSN:         snapshot.LSN,
        GTID:        snapshot.GTID,
        State:       BackupInProgress,
    }
    
    // 2. 导出数据页
    for _, ts := range snapshot.Tablespaces {
        if err := b.exportTablespace(backup, snapshot, &ts); err != nil {
            backup.State = BackupFailed
            return backup, err
        }
    }
    
    // 3. 导出元数据
    b.exportMetadata(backup, snapshot)
    
    backup.State = BackupCompleted
    backup.CompletedTime = time.Now()
    
    return backup, nil
}

func (b *BackupManager) exportTablespace(
    backup *Backup,
    snapshot *Snapshot,
    ts *TablespaceSnapshot) error {
    
    // 创建备份文件
    writer := b.backupStorage.CreateWriter(
        fmt.Sprintf("%s/%s.ibd", backup.ID, ts.TableName))
    defer writer.Close()
    
    // 导出所有页
    for pageNo := uint32(0); pageNo < uint32(ts.PageCount); pageNo++ {
        page, err := b.snapshotMgr.ReadPageFromSnapshot(
            snapshot, ts.SpaceID, pageNo)
        if err != nil {
            return err
        }
        
        // 压缩写入
        compressed := lz4.Compress(page.Data)
        writer.Write(compressed)
    }
    
    return nil
}
```

### 4.2 增量备份

```go
// 增量备份：只导出上次备份后变化的数据
func (b *BackupManager) CreateIncrementalBackup(
    name string,
    baseLSN uint64) (*Backup, error) {
    
    // 1. 创建快照
    snapshot, _ := b.snapshotMgr.CreateSnapshot(name + "_snapshot")
    
    backup := &Backup{
        ID:       uuid.New().String(),
        Name:     name,
        Type:     BackupIncremental,
        BaseLSN:  baseLSN,
        LSN:      snapshot.LSN,
        GTID:     snapshot.GTID,
    }
    
    // 2. 导出变化的页 (通过 Redo 识别)
    changedPages := b.findChangedPages(baseLSN, snapshot.LSN)
    
    for _, pageKey := range changedPages {
        page, _ := b.snapshotMgr.ReadPageFromSnapshot(
            snapshot, pageKey.SpaceID, pageKey.PageNo)
        
        b.exportPage(backup, pageKey, page)
    }
    
    // 3. 导出 Redo 范围
    b.exportRedoRange(backup, baseLSN, snapshot.LSN)
    
    return backup, nil
}

func (b *BackupManager) findChangedPages(fromLSN, toLSN uint64) []PageKey {
    // 扫描 Redo 日志，收集修改的页
    changedPages := make(map[PageKey]bool)
    
    b.storageLayer.ScanRedo(fromLSN, toLSN, func(rec *RedoRecord) {
        key := PageKey{SpaceID: rec.SpaceID, PageNo: rec.PageNo}
        changedPages[key] = true
    })
    
    var result []PageKey
    for key := range changedPages {
        result = append(result, key)
    }
    return result
}
```

## 5. PITR (Point-In-Time Recovery)

### 5.1 PITR 流程

```go
type PITRManager struct {
    backupStorage  BackupStorage
    storageLayer   *StorageLayer
}

// 恢复到指定时间点
func (p *PITRManager) RecoverToTimestamp(
    targetTimestamp time.Time,
    targetDB string) error {
    
    // 1. 找到最近的全量备份
    backup := p.findNearestBackup(targetTimestamp)
    if backup == nil {
        return ErrNoBackupFound
    }
    
    // 2. 恢复全量备份
    if err := p.restoreFullBackup(backup, targetDB); err != nil {
        return err
    }
    
    // 3. 应用增量备份 (如果有)
    incrementals := p.findIncrementalBackups(backup.LSN, targetTimestamp)
    for _, incr := range incrementals {
        if err := p.applyIncrementalBackup(incr, targetDB); err != nil {
            return err
        }
    }
    
    // 4. 应用 Redo 到目标时间点
    targetLSN := p.timestampToLSN(targetTimestamp)
    if err := p.applyRedoUntil(targetDB, targetLSN); err != nil {
        return err
    }
    
    return nil
}

func (p *PITRManager) restoreFullBackup(
    backup *Backup,
    targetDB string) error {
    
    // 1. 创建目标表空间
    for _, ts := range backup.Tablespaces {
        p.storageLayer.CreateTablespace(targetDB, ts.TableName)
    }
    
    // 2. 恢复数据页
    for _, ts := range backup.Tablespaces {
        reader := p.backupStorage.OpenReader(
            fmt.Sprintf("%s/%s.ibd", backup.ID, ts.TableName))
        defer reader.Close()
        
        for pageNo := uint32(0); ; pageNo++ {
            compressed, err := reader.Read()
            if err == io.EOF {
                break
            }
            
            pageData := lz4.Decompress(compressed)
            p.storageLayer.WritePage(ts.SpaceID, pageNo, pageData)
        }
    }
    
    return nil
}

func (p *PITRManager) applyRedoUntil(
    targetDB string,
    targetLSN uint64) error {
    
    // 从备份的 LSN 开始应用 Redo
    currentLSN := p.getCurrentLSN(targetDB)
    
    return p.storageLayer.ApplyRedoRange(
        targetDB,
        currentLSN,
        targetLSN,
    )
}
```

### 5.2 基于 GTID 的 PITR

```go
// 恢复到指定 GTID
func (p *PITRManager) RecoverToGTID(
    targetGTID string,
    targetDB string) error {
    
    // 1. 找到包含该 GTID 的备份
    backup := p.findBackupContainingGTID(targetGTID)
    
    // 2. 恢复备份
    p.restoreFullBackup(backup, targetDB)
    
    // 3. 应用 Redo 直到目标 GTID
    return p.applyRedoUntilGTID(targetDB, targetGTID)
}

func (p *PITRManager) applyRedoUntilGTID(
    targetDB string,
    targetGTID string) error {
    
    targetSet, _ := ParseGTIDSet(targetGTID)
    
    // 逐条应用 Redo，直到达到目标 GTID
    return p.storageLayer.ScanRedo(0, MaxLSN, func(rec *RedoRecord) bool {
        // 应用 Redo
        p.applyRedo(rec)
        
        // 检查是否达到目标
        if rec.Type == MLOG_TRX_COMMIT {
            currentSet := p.getCurrentGTIDSet()
            if currentSet.Contains(targetSet) {
                return false  // 停止
            }
        }
        return true  // 继续
    })
}
```

## 6. 克隆 (Clone)

```go
// 从快照创建新实例 (用于扩容 RO)
func (c *CloneManager) CloneFromSnapshot(
    snapshot *Snapshot,
    newInstanceID string) error {
    
    // 1. 注册新实例的元数据
    c.metadataService.RegisterInstance(newInstanceID, snapshot)
    
    // 2. 配置存储层共享
    // 新实例与源实例共享快照点之前的数据 (COW)
    c.storageLayer.ShareSnapshot(snapshot, newInstanceID)
    
    // 3. 启动新实例
    return c.computeLayer.StartROInstance(newInstanceID, snapshot.LSN)
}
```

## 7. 备份存储

```go
// 备份存储接口
type BackupStorage interface {
    CreateWriter(path string) (io.WriteCloser, error)
    OpenReader(path string) (io.ReadCloser, error)
    Delete(path string) error
    List(prefix string) ([]string, error)
}

// S3 实现
type S3BackupStorage struct {
    client *s3.Client
    bucket string
}

// 本地存储实现
type LocalBackupStorage struct {
    basePath string
}
```

## 8. 配置

```go
type BackupConfig struct {
    // 快照
    SnapshotRetentionDays int `default:"7"`
    MaxSnapshots          int `default:"100"`
    
    // 备份
    BackupStorageType     string `default:"s3"`
    BackupBucket          string
    BackupPath            string
    CompressionLevel      int    `default:"3"`
    
    // PITR
    RedoRetentionDays     int    `default:"7"`
    
    // 调度
    AutoBackupEnabled     bool          `default:"true"`
    AutoBackupSchedule    string        `default:"0 2 * * *"`  // 每天 2 点
    IncrementalInterval   time.Duration `default:"1h"`
}
```

## 9. 开发任务

- [ ] 实现逻辑快照
- [ ] 实现全量备份
- [ ] 实现增量备份
- [ ] 实现 PITR
- [ ] 实现克隆
- [ ] S3 备份存储
- [ ] 备份调度
- [ ] 备份验证
- [ ] 监控指标

## 10. 参考

- Aurora 论文: Backup 部分
- MySQL 物理备份文档
