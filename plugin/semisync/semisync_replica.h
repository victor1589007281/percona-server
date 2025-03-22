/*
   Copyright (c) 2006, 2022, Oracle and/or its affiliates.

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

#ifndef SEMISYNC_REPLICA_H
#define SEMISYNC_REPLICA_H

#include "my_inttypes.h"
#include "plugin/semisync/semisync.h"


/**
   半同步复制从库的扩展类
   The extension class for the slave of semi-synchronous replication
*/
class ReplSemiSyncSlave : public ReplSemiSyncBase {
   public:
    // 构造函数，初始化从库的半同步复制状态为禁用
    // Constructor, initializes the slave's semi-sync replication state as disabled
    ReplSemiSyncSlave() : slave_enabled_(false) {}
  
    // 默认析构函数
    // Default destructor
    ~ReplSemiSyncSlave() = default;
  
    // 设置跟踪级别，用于控制日志记录的详细程度
    // Set the trace level to control the verbosity of logging
    void setTraceLevel(unsigned long trace_level) { trace_level_ = trace_level; }
  
    /**
       初始化该类，在 MySQL 参数初始化后调用。
       该函数应在引导时调用一次。
       Initialize this class after MySQL parameters are initialized.
       This function should be called once at bootstrap time.
    */
    int initObject();
  
    // 获取从库是否启用了半同步复制
    // Get whether semi-sync replication is enabled on the slave
    bool getSlaveEnabled() { return slave_enabled_; }
  
    // 设置从库是否启用半同步复制
    // Set whether semi-sync replication is enabled on the slave
    void setSlaveEnabled(bool enabled) { slave_enabled_ = enabled; }
  
    /**
       从库读取半同步数据包头部，并将元数据与有效负载分离。
       A slave reads the semi-sync packet header and separates the metadata
       from the payload data.
  
       输入参数：
       Input:
         header      - (IN)  数据包头部指针 / Packet header pointer
         total_len   - (IN)  数据包总长度：元数据 + 有效负载 / Total packet length: metadata + payload
         need_reply  - (IN)  主库是否等待回复 / Whether the master is waiting for the reply
         payload     - (IN)  有效负载：复制事件 / Payload: the replication event
         payload_len - (IN)  有效负载长度 / Payload length
  
       返回值：
       Return:
         0: 成功 / Success
         非零: 错误 / Error
    */
    int slaveReadSyncHeader(const char *header, unsigned long total_len,
                            bool *need_reply, const char **payload,
                            unsigned long *payload_len);
  
    /**
       从库向主库发送回复，指示其复制进度。
       它表明从库已接收到指定 binlog 位置之前的所有事件。
       A slave replies to the master indicating its replication progress.
       It indicates that the slave has received all events before the specified
       binlog position.
  
       输入参数：
       Input:
         mysql            - (IN)  MySQL 网络连接 / The MySQL network connection
         binlog_filename  - (IN)  回复点的 binlog 文件名 / The reply point's binlog file name
         binlog_filepos   - (IN)  回复点的 binlog 文件偏移量 / The reply point's binlog file offset
  
       返回值：
       Return:
         0: 成功 / Success
         非零: 错误 / Error
    */
    int slaveReply(MYSQL *mysql, const char *binlog_filename,
                   my_off_t binlog_filepos);
  
    // 启动半同步复制从库
    // Start the semi-synchronous replication slave
    int slaveStart(Binlog_relay_IO_param *param);
  
    // 停止半同步复制从库
    // Stop the semi-synchronous replication slave
    int slaveStop(Binlog_relay_IO_param *param);
  
   private:
    // 标记是否已调用 initObject
    // True when initObject has been called
    bool init_done_ = false;
  
    // 标记从库是否启用了半同步复制
    // True if semi-sync is enabled on the slave
    bool slave_enabled_ = false;
  
    // 用于发送回复的 MySQL 网络连接
    // Connection to send reply
    MYSQL *mysql_reply = nullptr;
  };
  
  /* 从库组件的系统变量和状态变量
     System and status variables for the slave component */
  extern bool rpl_semi_sync_replica_enabled; // 是否启用半同步复制
  extern unsigned long rpl_semi_sync_replica_trace_level; // 跟踪级别
  extern char rpl_semi_sync_replica_status; // 半同步复制状态
  
  #endif /* SEMISYNC_REPLICA_H */
