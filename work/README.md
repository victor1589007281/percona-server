# Percona Server 本地 Mac 开发环境

轻量化的 MySQL/InnoDB 源码编译调试方案，针对 Mac 本地开发优化。

## 📋 系统要求

```bash
# 安装必要依赖
brew install cmake openssl@3 pkg-config bison
```

## 🚀 快速开始

```bash
cd work/

# 1. 配置 CMake (首次/修改 CMake 配置后)
./cmake_configure.sh

# 2. 编译
./build.sh

# 3. 初始化数据目录 (首次)
./init_data.sh

# 4. 启动 MySQL
./start.sh

# 5. 连接测试
./connect.sh
```

## 📁 目录结构

```
work/
├── config.sh           # 全局配置
├── cmake_configure.sh  # CMake 配置脚本
├── build.sh            # 编译脚本
├── init_data.sh        # 数据目录初始化
├── start.sh            # 启动 MySQL
├── stop.sh             # 停止 MySQL
├── status.sh           # 查看状态
├── connect.sh          # 快速连接
├── debug.sh            # 调试启动
├── rebuild_innodb.sh   # InnoDB 快速重编译
├── build/              # CMake 构建目录
├── install/            # 安装目录
├── data/               # 数据目录
└── logs/               # 日志目录
```

## 🔧 编译命令

```bash
# 全量编译
./build.sh

# 仅编译 mysqld
./build.sh mysqld

# 仅编译 InnoDB
./build.sh innodb

# InnoDB 修改后快速重编译
./rebuild_innodb.sh

# 清理
./build.sh clean
```

## 🐛 调试方法

### 方法1: 前台运行
```bash
# 前台运行，Ctrl+C 停止
./debug.sh foreground
```

### 方法2: LLDB 启动调试
```bash
./debug.sh lldb

# 在 lldb 中:
(lldb) breakpoint set -n mysql_execute_command
(lldb) run
```

### 方法3: 附加到运行中的进程
```bash
# 先正常启动
./start.sh

# 然后附加调试
./debug.sh attach
```

### 常用 InnoDB 断点

```
# 行搜索
breakpoint set -n row_search_mvcc

# B+树搜索
breakpoint set -n btr_cur_search_to_nth_level

# Buffer Pool
breakpoint set -n buf_page_get_gen

# 事务提交
breakpoint set -n trx_commit

# SQL 执行
breakpoint set -n mysql_execute_command
breakpoint set -n dispatch_command
```

## ⚙️ 轻量化配置说明

为了加速编译和减少资源占用，已禁用以下组件：

| 禁用的组件 | 说明 |
|-----------|------|
| MySQL Router | 代理/路由组件 |
| RocksDB | 存储引擎 |
| NDB Cluster | 集群存储 |
| 单元测试 | 可按需开启 |
| LDAP/Kerberos | 认证插件 |
| Memcached | 缓存插件 |
| V8 JavaScript | 脚本引擎 |

保留的核心组件：
- ✅ **mysqld** - MySQL 服务器
- ✅ **InnoDB** - 核心存储引擎
- ✅ **MyISAM** - 系统表
- ✅ **mysql 客户端** - 连接工具

## 📊 资源占用

| 配置 | 完整编译 | 轻量化编译 |
|-----|---------|-----------|
| 编译时间 | ~60分钟 | ~20分钟 |
| 磁盘占用 | ~15GB | ~5GB |
| 内存占用 | ~8GB | ~4GB |

## 🔄 开发工作流

### 修改 InnoDB 代码

```bash
# 1. 修改代码
vim storage/innobase/row/row0sel.cc

# 2. 快速重编译
./rebuild_innodb.sh

# 3. 重启测试
./stop.sh && ./start.sh

# 4. 连接验证
./connect.sh
```

### Debug 编译

```bash
# 使用 Debug 模式配置
./cmake_configure.sh Debug

# 重新编译
./build.sh
```

## ⚠️ 常见问题

### 1. OpenSSL 找不到
```bash
# 确保 homebrew openssl 已安装
brew install openssl@3

# 检查路径
ls /opt/homebrew/opt/openssl@3
```

### 2. 编译内存不足
```bash
# 减少并行编译数
PS_PARALLEL_JOBS=2 ./build.sh
```

### 3. 启动失败
```bash
# 检查日志
cat logs/error.log

# 检查端口占用
lsof -i:33060
```

### 4. 权限问题
```bash
# 确保数据目录权限正确
chmod -R 755 data/
```

## 📝 配置修改

编辑 `config.sh` 可修改:
- `PS_PORT` - MySQL 端口 (默认 33060)
- `PS_BUILD_TYPE` - 编译类型
- `PS_PARALLEL_JOBS` - 并行编译数
