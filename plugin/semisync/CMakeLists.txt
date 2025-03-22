# Copyright (c) 2006, 2022, Oracle and/or its affiliates.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

# -DMYSQL_SERVER：定义 MYSQL_SERVER 宏，表明这是 MySQL 服务器端的代码。
# -DLOG_SUBSYSTEM_TAG="Repl"：定义日志子系统标签为 Repl，用于标识这是复制相关的日志。
# -DLOG_COMPONENT_TAG="semisync"：定义日志组件标签为 semisync，用于标识这是半同步复制插件的日志。
ADD_DEFINITIONS(-DMYSQL_SERVER)
ADD_DEFINITIONS(-DLOG_SUBSYSTEM_TAG="Repl")
ADD_DEFINITIONS(-DLOG_COMPONENT_TAG="semisync")

# 这是一个 MySQL 特定的 CMake 宏，用于禁用构建过程中因缺少某些配置文件而产生的警告。
DISABLE_MISSING_PROFILE_WARNING()

# MYSQL_ADD_PLUGIN：这是 MySQL 的 CMake 宏，用于定义插件的构建规则。
# 插件名称：semisync_source，表示这是主库端的半同步复制插件。
# 源文件：
#    semisync.cc：公共代码。
#    semisync_source.cc：主库端的核心逻辑。
#    semisync_source_ack_receiver.cc：处理从库确认（ACK）的逻辑。
#    semisync_source_plugin.cc：插件的初始化和注册逻辑。
# MODULE_ONLY：表示这是一个模块化插件。
# MODULE_OUTPUT_NAME：生成的插件文件名为 semisync_source。
# VISIBILITY_HIDDEN：隐藏符号，减少符号冲突的可能性。

MYSQL_ADD_PLUGIN(semisync_source
  semisync.cc
  semisync_source.cc
  semisync_source_ack_receiver.cc
  semisync_source_plugin.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "semisync_source"
  VISIBILITY_HIDDEN
  )

# 插件名称：semisync_replica，表示这是从库端的半同步复制插件。
# 源文件：
#    semisync.cc：公共代码。
#    semisync_replica.cc：从库端的核心逻辑。
#    semisync_replica_plugin.cc：插件的初始化和注册逻辑。
# 其他选项与 semisync_source 类似。

MYSQL_ADD_PLUGIN(semisync_replica
  semisync.cc
  semisync_replica.cc
  semisync_replica_plugin.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "semisync_replica"
  VISIBILITY_HIDDEN
  )

#  semisync_master 和 semisync_slave 是旧版插件的实现，分别对应主库和从库。
#  使用了 semisync_source_plugin_old.cc 和 semisync_replica_plugin_old.cc，这些文件可能是为了兼容旧版本的 MySQL。

MYSQL_ADD_PLUGIN(semisync_master
  semisync.cc
  semisync_source.cc
  semisync_source_ack_receiver.cc
  semisync_source_plugin_old.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "semisync_master"
  VISIBILITY_HIDDEN
  )

MYSQL_ADD_PLUGIN(semisync_slave
  semisync.cc
  semisync_replica.cc
  semisync_replica_plugin_old.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "semisync_slave"
  VISIBILITY_HIDDEN
  )
