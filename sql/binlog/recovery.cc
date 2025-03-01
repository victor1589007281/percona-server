/* Copyright (c) 2022, Oracle and/or its affiliates.

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

#include "sql/binlog/recovery.h"

#include "sql/binlog/tools/iterators.h"  // binlog::tools::Iterator
#include "sql/raii/sentry.h"             // raii::Sentry<>
#include "sql/xa/xid_extract.h"          // xa::XID_extractor

binlog::Binlog_recovery::Binlog_recovery(Binlog_file_reader &binlog_file_reader)
    : m_reader{binlog_file_reader},
      m_mem_root{key_memory_binlog_recover_exec,
                 static_cast<size_t>(my_getpagesize())},
      m_set_alloc{&m_mem_root},
      m_map_alloc{&m_mem_root},
      m_internal_xids{m_set_alloc},
      m_external_xids{m_map_alloc} {}

my_off_t binlog::Binlog_recovery::get_valid_pos() const {
  return this->m_valid_pos;
}

bool binlog::Binlog_recovery::has_failures() const {
  return this->m_is_malformed || this->m_no_engine_recovery;
  // 返回是否有失败
  // 如果 m_is_malformed 或 m_no_engine_recovery 为 true，则表示有失败
}

bool binlog::Binlog_recovery::is_binlog_malformed() const {
  return this->m_is_malformed;
}

bool binlog::Binlog_recovery::has_engine_recovery_failed() const {
  return this->m_no_engine_recovery;
}

std::string const &binlog::Binlog_recovery::get_failure_message() const {
  return this->m_failure_message;
}

binlog::Binlog_recovery &binlog::Binlog_recovery::recover() {
  binlog::tools::Iterator it{&this->m_reader}; // 创建迭代器
  it.set_copy_event_buffer(); // 设置复制事件缓冲区
  this->m_valid_pos = this->m_reader.position(); // 设置有效位置

  for (Log_event *ev = it.begin(); ev != it.end(); ev = it.next()) { // 遍历日志事件
    switch (ev->get_type_code()) { // 获取事件类型代码
      case binary_log::QUERY_EVENT: { // 查询事件
        this->process_query_event(dynamic_cast<Query_log_event &>(*ev)); // 处理查询事件
        break;
      }
      case binary_log::XID_EVENT: { // XID 事件
        this->process_xid_event(dynamic_cast<Xid_log_event &>(*ev)); // 处理 XID 事件
        break;
      }
      case binary_log::XA_PREPARE_LOG_EVENT: { // XA 预处理日志事件
        this->process_xa_prepare_event(
            dynamic_cast<XA_prepare_log_event &>(*ev)); // 处理 XA 预处理日志事件
        break;
      }
      default: { // 其他事件
        break;
      }
    }

    // Whenever the current position is at a transaction boundary, save it
    // to m_valid_pos
    // 每当当前位置在事务边界时，将其保存到 m_valid_pos
    if (!this->m_is_malformed && !this->m_in_transaction &&
        !is_gtid_event(ev) && !is_session_control_event(ev))
      this->m_valid_pos = this->m_reader.position(); // 更新有效位置

    delete ev; // 删除事件
    ev = nullptr; // 将事件指针置为空
    this->m_is_malformed = it.has_error() || this->m_is_malformed; // 检查是否有错误
    if (this->m_is_malformed) break; // 如果有错误，退出循环
  }

  if (!this->m_is_malformed && total_ha_2pc > 1) { // 如果没有错误且总共 2PC 大于 1
    Xa_state_list xa_list{this->m_external_xids}; // 创建 XA 状态列表
    this->m_no_engine_recovery = ha_recover(&this->m_internal_xids, &xa_list); // 恢复存储引擎
    if (this->m_no_engine_recovery) { // 如果恢复失败
      this->m_failure_message.assign("Recovery failed in storage engines"); // 设置失败消息
    }
  }

  return (*this); // 返回恢复对象
}

void binlog::Binlog_recovery::process_query_event(Query_log_event const &ev) {
  std::string query{ev.query};

  if (query == "BEGIN" || query.find("XA START") == 0)
    this->process_start();

  else if (query == "COMMIT")
    this->process_commit();

  else if (query == "ROLLBACK")
    this->process_rollback();

  else if (is_atomic_ddl_event(&ev))
    this->process_atomic_ddl(ev);

  else if (query.find("XA COMMIT") == 0)
    this->process_xa_commit(query);

  else if (query.find("XA ROLLBACK") == 0)
    this->process_xa_rollback(query);
}

void binlog::Binlog_recovery::process_xid_event(Xid_log_event const &ev) {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Xid_log_event outside the boundary of a sequence of events "
        "representing an active transaction");
    return;
  }
  this->m_in_transaction = false;
  if (!this->m_internal_xids.insert(ev.xid).second) {
    this->m_is_malformed = true;
    this->m_failure_message.assign("Xid_log_event holds an invalid XID");
  }
}

void binlog::Binlog_recovery::process_xa_prepare_event(
    XA_prepare_log_event const &ev) {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "XA_prepare_log_event outside the boundary of a sequence of events "
        "representing an active transaction");
    return;
  }

  this->m_in_transaction = false;

  XID xid;
  xid = ev.get_xid();
  auto found = this->m_external_xids.find(xid);
  if (found != this->m_external_xids.end()) {
    assert(found->second != enum_ha_recover_xa_state::PREPARED_IN_SE);
    if (found->second == enum_ha_recover_xa_state::PREPARED_IN_TC) {
      // If it was found already, must have been committed or rolled back, it
      // can't be in prepared state
      this->m_is_malformed = true;
      this->m_failure_message.assign(
          "XA_prepare_log_event holds an invalid XID");
      return;
    }
  }

  this->m_external_xids[xid] =
      ev.is_one_phase() ? enum_ha_recover_xa_state::COMMITTED_WITH_ONEPHASE
                        : enum_ha_recover_xa_state::PREPARED_IN_TC;
}

void binlog::Binlog_recovery::process_start() {
  this->m_is_malformed = this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `BEGIN/XA START` inside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = true;
}

void binlog::Binlog_recovery::process_commit() {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `COMMIT` outside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = false;
}

void binlog::Binlog_recovery::process_rollback() {
  this->m_is_malformed = !this->m_in_transaction;
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `ROLLBACK` outside the boundary of a "
        "sequence of events representing an active transaction");
  this->m_in_transaction = false;
}

void binlog::Binlog_recovery::process_atomic_ddl(Query_log_event const &ev) {
  this->m_is_malformed = this->m_in_transaction;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log event containing a DDL inside the boundary of a sequence of "
        "events representing an active transaction");
    return;
  }
  if (!this->m_internal_xids.insert(ev.ddl_xid).second) {
    this->m_is_malformed = true;
    this->m_failure_message.assign(
        "Query_log_event containing a DDL holds an invalid XID");
  }
}

void binlog::Binlog_recovery::process_xa_commit(std::string const &query) {
  this->m_is_malformed = this->m_in_transaction;
  this->m_in_transaction = false;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log_event containing `XA COMMIT` inside the boundary of a "
        "sequence of events representing a transaction not yet in prepared "
        "state");
    return;
  }
  this->add_external_xid(query, enum_ha_recover_xa_state::COMMITTED);
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `XA COMMIT` holds an invalid XID");
}

void binlog::Binlog_recovery::process_xa_rollback(std::string const &query) {
  this->m_is_malformed = this->m_in_transaction;
  this->m_in_transaction = false;
  if (this->m_is_malformed) {
    this->m_failure_message.assign(
        "Query_log_event containing `XA ROLLBACK` inside the boundary of a "
        "sequence of events representing a transaction not yet in prepared "
        "state");
    return;
  }
  this->add_external_xid(query, enum_ha_recover_xa_state::ROLLEDBACK);
  if (this->m_is_malformed)
    this->m_failure_message.assign(
        "Query_log_event containing `XA ROLLBACK` holds an invalid XID");
}

void binlog::Binlog_recovery::add_external_xid(std::string const &query,
                                               enum_ha_recover_xa_state state) {
  xa::XID_extractor tokenizer{query, 1};
  if (tokenizer.size() == 0) {
    this->m_is_malformed = true;
    return;
  }

  auto found = this->m_external_xids.find(tokenizer[0]);
  if (found != this->m_external_xids.end()) {
    assert(found->second != enum_ha_recover_xa_state::PREPARED_IN_SE);
    if (found->second != enum_ha_recover_xa_state::PREPARED_IN_TC) {
      // If it was found already, it needs to be in prepared in TC state
      this->m_is_malformed = true;
      return;
    }
  }

  this->m_external_xids[tokenizer[0]] = state;
}
