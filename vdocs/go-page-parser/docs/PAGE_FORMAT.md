# InnoDB Page格式详解

基于Percona Server 8.4.3-3源码分析

## Page概述

InnoDB以Page为单位管理数据，默认Page大小为16KB（16384字节）。

## Page Header结构 (38字节)

| 偏移 | 长度 | 字段名 | 说明 |
|------|------|--------|------|
| 0 | 4 | FIL_PAGE_SPACE_OR_CHKSUM | Checksum |
| 4 | 4 | FIL_PAGE_OFFSET | Page Number |
| 8 | 4 | FIL_PAGE_PREV | Previous Page |
| 12 | 4 | FIL_PAGE_NEXT | Next Page |
| 16 | 8 | FIL_PAGE_LSN | LSN |
| 24 | 2 | FIL_PAGE_TYPE | Page Type |
| 26 | 8 | FIL_PAGE_FILE_FLUSH_LSN | Flush LSN |
| 34 | 4 | FIL_PAGE_SPACE_ID | Space ID |

## Page Trailer (8字节)

| 偏移 | 长度 | 说明 |
|------|------|------|
| -8 | 4 | Old Checksum |
| -4 | 4 | LSN Low 32 bits |

## 参考

- Percona Server 8.4.3-3源码
- `storage/innobase/include/fil0types.h`
