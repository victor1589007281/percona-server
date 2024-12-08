# build
```text
-DDOWNLOAD_BOOST=1
-DWITH_BOOST=/Users/victor/mysql/percona_8.0.32/boost_1_77_0
-DCMAKE_BUILD_TYPE=Debug
-DCMAKE_INSTALL_PREFIX=/Users/victor/mysql/percona_8.0.32
-DMYSQL_DATADIR=/Users/victor/mysql/percona_8.0.32/data
-DSYSCONFDIR=/Users/victor/mysql/percona_8.0.32
-DMYSQL_UNIX_ADDR=/Users/victor/mysql/percona_8.0.32/data/mysql.sock
-DMYSQL_MAINTAINER_MODE=false
-DWITH_SSL=/usr/local/Cellar/openssl@1.1/1.1.1t
-DWITH_DEBUG=1
-DWITH_UNIT_TESTS=OFF
-DWITH_LDAP=system
-DLDAP_INCLUDE_DIR=/opt/local/include
-DLDAP_LIBRARY=/opt/local/lib/libldap.dylib
-DLBER_LIBRARY=/opt/local/lib/liblber.dylib
```
```text
--initialize-insecure
--datadir=/Users/victor/mysql/percona_8.0.32/data
--socket=/Users/victor/mysql/percona_8.0.32/data/mysql.sock
--pid-file=/Users/victor/mysql/percona_8.0.32/data/mysql.pid
--user=victor
--port=3306
--bind-address=127.0.0.1
--log-error=/Users/victor/mysql/percona_8.0.32/log/error.log
--console
--skip-grant-tables
--skip-networking
--debug-sync-timeout=0
--gdb
--debug
--core-file
--general-log=1
--general-log-file=/Users/victor/mysql/percona_8.0.32/log/general.log
```
## Note
1. 编辑.gitignore 忽略.idea
2. 拉取git submodules.如果有多余的文件残留，则删除分支重来
```shell
git submodule init
git submodule update
```
3. 修复代码
```text
https://bugs.freebsd.org/bugzilla/attachment.cgi?id=245615&action=diff
```
4. 目录准备
```text
build  //编译目录
percona_dir /data /log //数据目录
```
5. cmake 后，指定build，不用全部，全部在LDAP中会过不去
```text
LDAP 用macports安装
```
6. 
