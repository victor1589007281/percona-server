# 存储引擎代码分析笔记

## 概述
存储引擎层负责数据的存储和检索，是MySQL架构中的重要组成部分。

## 主要存储引擎

### 1. InnoDB
- 位置：`storage/innobase/`
- 特点：事务性存储引擎，支持ACID特性

### 2. MyISAM
- 位置：`storage/myisam/`
- 特点：非事务性存储引擎，查询性能优秀

### 3. Memory
- 位置：`storage/heap/`
- 特点：内存表存储引擎

## 待分析文件
- [ ] storage/innobase/handler/ha_innodb.cc
- [ ] storage/innobase/btr/btr0btr.cc
- [ ] storage/myisam/ha_myisam.cc 
 