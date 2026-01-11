# Aurora Storage Integration for InnoDB

This module provides Aurora-like distributed storage integration for InnoDB.

## Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         InnoDB                                    │
├──────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐               │
│  │ Log Writer  │  │ Buffer Pool │  │   Recovery  │               │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘               │
│         │                │                │                       │
│  ┌──────▼────────────────▼────────────────▼──────┐               │
│  │              Aurora Integration                │               │
│  │  ┌────────────┐  ┌────────────┐  ┌──────────┐ │               │
│  │  │RedoSender  │  │PageReader  │  │ReaderSync│ │               │
│  │  └─────┬──────┘  └─────┬──────┘  └────┬─────┘ │               │
│  │        │               │              │       │               │
│  │  ┌─────▼───────────────▼──────────────▼─────┐ │               │
│  │  │           Aurora Client (gRPC)            │ │               │
│  │  └────────────────────┬──────────────────────┘ │               │
│  └───────────────────────┼────────────────────────┘               │
└──────────────────────────┼────────────────────────────────────────┘
                           │
              ┌────────────▼────────────┐
              │    Storage Layer        │
              │   (6 Storage Nodes)     │
              └─────────────────────────┘
```

## Components

### aurora_config.h/cc
Configuration management for Aurora integration.

### aurora_types.h
Core type definitions (LSN, Page, RedoRecord, etc.).

### aurora_client.h/cc
gRPC client for communicating with storage and metadata services.

### aurora_redo_sender.h/cc
Intercepts InnoDB redo log writes and sends to Aurora storage.

### aurora_page_reader.h/cc
Reads pages from Aurora storage instead of local files.

### aurora_reader_sync.h/cc
Synchronizes reader instances with writer VDL.

### aurora.h/cc
Main entry point and InnoDB integration hooks.

## Integration Points

### Redo Log Writing
Modify `log0write.cc`:
```cpp
#include "aurora/aurora.h"

// In log_writer thread after writing to buffer:
if (aurora::aurora_is_active()) {
  aurora::aurora_on_redo_write(lsn, log_block, len);
}
```

### Page Reading
Modify `buf0flu.cc` or `fil0fil.cc`:
```cpp
#include "aurora/aurora.h"

// In page read path:
if (aurora::aurora_is_active()) {
  if (aurora::aurora_on_page_read(space_id, page_no, frame, UNIV_PAGE_SIZE)) {
    return; // Page read from Aurora
  }
}
// Fall through to local read
```

### Transaction Commit
Modify `trx0trx.cc`:
```cpp
#include "aurora/aurora.h"

// In trx_commit:
if (aurora::aurora_is_active()) {
  aurora::aurora_on_trx_commit(trx->commit_lsn, timeout_ms);
}
```

## MySQL System Variables

| Variable | Type | Default | Description |
|----------|------|---------|-------------|
| aurora_enabled | bool | OFF | Enable Aurora storage |
| aurora_mode | string | "" | "writer" or "reader" |
| aurora_instance_id | string | "" | Instance identifier |
| aurora_volume_id | string | "" | Storage volume ID |
| aurora_storage_nodes | string | "" | Comma-separated storage node addresses |
| aurora_metadata_nodes | string | "" | Comma-separated metadata node addresses |
| aurora_quorum_vw | int | 4 | Write quorum |
| aurora_quorum_vr | int | 3 | Read quorum |

## Building

1. Add to `storage/innobase/CMakeLists.txt`:
```cmake
ADD_SUBDIRECTORY(aurora)
TARGET_LINK_LIBRARIES(innobase aurora_storage)
```

2. Build with gRPC support:
```bash
cmake -DWITH_GRPC=ON ...
make
```

## Status

- [x] Core types and configuration
- [x] Client infrastructure
- [x] Redo sender (writer mode)
- [x] Page reader
- [x] Reader synchronization
- [ ] Full InnoDB integration
- [ ] gRPC implementation
- [ ] Performance optimization
