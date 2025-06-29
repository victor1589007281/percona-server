# 如何源码编译运行调试mysqld
## 准备
1. 环境
   1.1 操作系统：Ubuntu 24.04
   ```bash
   uname -a
   ```
   ```
   Linux victor-mini-pc 6.11.0-25-generic #25~24.04.1-Ubuntu SMP    PREEMPT_DYNAMIC Tue Apr 15 17:20:50 UTC 2 x86_64 x86_64 x86_64    GNU/Linux
   ```
   1.2 官方文档：https://docs.percona.com/percona-server/8.4/compile-percona-server.html

2. 源码
   2.1. 从Percona Server 8.4.3-3下载源码

```bash
gh repo clone percona/percona-server
cd percona-server
git branch -a|grep release-8.4.3-3
git fetch origin release-8.4.3-3:release-8.4.3-3
git checkout release-8.4.3-3
```

## 编译
0. 初始化更新子模块
   ```bash
   git submodule update --init --recursive
   # 可选 仅更新
   git submodule update
   ```
1. 安装依赖

   ```bash
      sudo apt update
      sudo apt install -y cmake gcc g++ bison libncurses-dev libssl-dev libaio-dev libcurl4-openssl-dev libevent-dev libz-dev
   ```
2. 编译
删除 percona-server 源码目录下所有 CMake 相关的缓存和临时文件（如 CMakeCache.txt、CMakeFiles、Makefile、cmake_install.cmake 等）。
然后在源码目录外的 build 目录重新 cmake(如果不在源码目录外，可能会遇到各种奇怪的问题，如重复的宏定义等)。

   ```bash
      cd ~/work/mysql/mysql8433/percona-server && rm -rf CMakeCache.txt CMakeFiles Makefile cmake_install.cmake && cd ~/work/mysql/mysql8433/build_percona && cmake ../percona-server -DCMAKE_BUILD_TYPE=Release

   make -j$(nproc)
   ```
 * clion 默认编译问题
  * 版本
    * 调整cmakelist
    cmake_minimum_required(VERSION  3.5)


## 运行
1. 创建数据目录
   ```bash
   # 进入到编译目录，我们在编译目录下创建数据目录
   cd build
   rm -rf data
   mkdir -p data
   ```
2. 初始化数据库
   ```bash
   ./bin/mysqld --initialize-insecure  --datadir=./data 
   ```
* 问题
* 初始化失败，已经存在运行的MySQL
```bash
# 查看是否有mysql进程
ps -ef|grep mysqld
# 杀掉进程
pkill mysqld
# 清空数据目录
rm -rf ./data/*
```
```
2025-06-28T08:54:51.038311Z 0 [System] [MY-015017] [Server] MySQL Server Initialization - start.
2025-06-28T08:54:51.039542Z 0 [System] [MY-013169] [Server] /home/victor/work/mysql/mysql8433/build_percona_clion/runtime_output_directory/mysqld (mysqld 8.4.3-3-debug) initializing of server in progress as process 2983351
2025-06-28T08:54:51.041196Z 0 [ERROR] [MY-010457] [Server] --initialize specified but the data directory has files in it. Aborting.
2025-06-28T08:54:51.041200Z 0 [ERROR] [MY-013236] [Server] The designated data directory /home/victor/work/mysql/mysql8433/build_percona_clion/data/ is unusable. You can remove all files that the server added to it.
2025-06-28T08:54:51.041236Z 0 [ERROR] [MY-010119] [Server] Aborting
2025-06-28T08:54:51.041772Z 0 [System] [MY-015018] [Server] MySQL Server Initialization - end.
```
3. 启动数据库
   ```bash
   # 或者点击IDE 的debug或者run
   ./mysqld_safe --datadir=../data --pid-file=../data/mysqld.pid
   ```
4. 连接数据库
   ```bash
   mysql -u root -p
   ```
## 调试
1. 安装gdb
   ```bash
   sudo apt-get install gdb
   ```
2. 启动gdb
   ```bash
   gdb ./bin/mysqld
   ```
3. 在gdb中设置断点
   ```
   (gdb) break mysql_main
   ```
4. 运行数据库
   ```
   (gdb) run
   ```
5. 连接gdb
   ```
   (gdb) attach <pid>
   ```
6. 在gdb中调试
   ```
   (gdb) continue
   ```
