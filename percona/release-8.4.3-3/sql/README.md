# SQL层代码分析笔记

## 概述
SQL层是MySQL的核心组件，负责SQL语句的解析、优化和执行。

## 主要模块

### 1. 解析器 (Parser)
- 位置：`sql/sql_lex.cc`, `sql/sql_yacc.yy`
- 功能：将SQL文本转换为语法树

### 2. 优化器 (Optimizer)
- 位置：`sql/sql_optimizer.cc`
- 功能：生成最优执行计划

### 3. 执行器 (Executor)
- 位置：`sql/sql_executor.cc`
- 功能：执行优化后的查询计划

## 待分析文件
- [ ] sql/sql_lex.cc
- [ ] sql/sql_yacc.yy
- [ ] sql/sql_optimizer.cc
- [ ] sql/sql_executor.cc 
 