/* Copyright (C) 2007 Google Inc.
   Copyright (c) 2008, 2022, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "plugin/semisync/semisync.h"
#include "mysql/components/services/component_sys_var_service.h"

// 定义半同步复制的魔术字节，用于标识半同步复制的数据包
// Define the magic byte for semi-synchronous replication packets
const unsigned char ReplSemiSyncBase::kPacketMagicNum = 0xef;

// 定义同步标志，用于标识数据包是否需要同步
// Define the sync flag to indicate whether the packet requires synchronization
const unsigned char ReplSemiSyncBase::kPacketFlagSync = 0x01;

// 定义跟踪级别的常量，用于控制日志记录的详细程度
// Define constants for trace levels to control the verbosity of logging
const unsigned long Trace::kTraceGeneral = 0x0001;  // 一般跟踪信息
const unsigned long Trace::kTraceDetail = 0x0010;   // 详细跟踪信息
const unsigned long Trace::kTraceNetWait = 0x0020; // 网络等待相关的跟踪信息
const unsigned long Trace::kTraceFunction = 0x0040; // 函数调用的跟踪信息

// 定义同步头部，用于标识半同步复制的数据包
// Define the sync header for semi-synchronous replication packets
const unsigned char ReplSemiSyncBase::kSyncHeader[2] = {
    ReplSemiSyncBase::kPacketMagicNum, 0};

// 检查系统变量是否已定义
// Check if a system variable is defined
bool is_sysvar_defined(const char *name) {
  char buffer[256]; // 用于存储变量值的缓冲区
  void *value = buffer; // 指向缓冲区的指针
  size_t value_length = sizeof(buffer) - 1; // 缓冲区的长度减去 1
  auto registry_handle = mysql_plugin_registry_acquire(); // 获取插件注册表句柄
  assert(registry_handle != nullptr); // 确保句柄不为空

  // 创建一个服务对象，用于访问系统变量
  // Create a service object to access system variables
  my_service<SERVICE_TYPE(component_sys_variable_register)> svc(
      "component_sys_variable_register", registry_handle);

  // 如果变量不存在，返回 true 表示错误
  // Returns true on error, i.e., if the variable does *not* exist
  bool get_var_error =
      svc->get_variable("mysql_server", name, &value, &value_length);

  // 释放插件注册表句柄
  // Release the plugin registry handle
  mysql_plugin_registry_release(registry_handle);

  // 如果没有错误，返回 true 表示变量已定义；否则返回 false
  // Return true if the variable is defined, false otherwise
  return !get_var_error;
}
