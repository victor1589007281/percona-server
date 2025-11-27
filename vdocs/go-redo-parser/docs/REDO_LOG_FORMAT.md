# InnoDB Redo Log 格式文档 (8.0.30+)

> 基于 Percona Server 8.4.3-3 源码分析

## 概述

InnoDB Redo Log 是一个 Write-Ahead Log (WAL)，记录了对数据页的所有修改操作。

## 关键概念

### LSN (Log Sequence Number)
- 类型: uint64
- 起始值: 8192
- 最大值: 2^63 - 1

### Log Block
- 大小: 512 字节
- 结构: Header(12) + Data(496) + Trailer(4)

## Log Block 结构

### Block Header (12 字节)
- Offset 0-3: hdr_no (block number)
- Offset 4-5: data_len (bytes written)
- Offset 6-7: first_rec_group (first mtr start offset)
- Offset 8-11: epoch_no (epoch number)

### Block Trailer (4 字节)
- Checksum (uint32)

## MLOG Record 类型

简单类型:
- MLOG_1BYTE = 1
- MLOG_2BYTES = 2
- MLOG_4BYTES = 4
- MLOG_8BYTES = 8

复杂类型:
- MLOG_REC_INSERT = 67
- MLOG_PAGE_CREATE = 19
- MLOG_WRITE_STRING = 30
- MLOG_MULTI_REC_END = 31

详见源码 mtr0types.h
