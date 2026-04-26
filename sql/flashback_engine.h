/* Copyright (c) 2025, Percona and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef FLASHBACK_ENGINE_INCLUDED
#define FLASHBACK_ENGINE_INCLUDED

/**
  @file sql/flashback_engine.h
  @brief IFlashbackEngine 接口定义 - 所有闪回引擎的抽象基类.

  遵循接口隔离原则, 该接口仅暴露闪回执行所需的最小方法集.
  具体引擎 (UndoFlashbackEngine / BinlogFlashbackEngine) 实现此接口.
*/

#include "flashback_types.h"

class THD;

namespace flashback {

/**
  闪回引擎抽象接口

  所有闪回引擎 (Undo / Binlog) 必须实现此接口.
  调度器 (FlashbackScheduler) 通过此接口与具体引擎交互,
  实现依赖倒置原则.
*/
class IFlashbackEngine {
 public:
  virtual ~IFlashbackEngine() = default;

  /**
    返回引擎类型标识.
    @return FlashbackEngineType::UNDO 或 BINLOG
  */
  virtual FlashbackEngineType type() const = 0;

  /**
    检查该引擎是否支持给定的闪回请求.

    @param req  闪回请求参数
    @return true 支持, false 不支持
  */
  virtual bool supports(const FlashbackRequest &req) const = 0;

  /**
    执行闪回操作.

    @param thd    线程句柄
    @param req    闪回请求参数
    @param result 输出: 闪回结果统计
    @return true 失败 (result 中包含错误信息)
            false 成功
  */
  virtual bool execute(THD *thd, const FlashbackRequest &req,
                       FlashbackResult &result) = 0;

  /**
    返回该引擎支持的最大闪回窗口 (秒).
    @return 最大窗口秒数
  */
  virtual ulonglong max_window_seconds() const = 0;

  /**
    返回该引擎可用的最早时间戳.
    @return 最早可用时间戳 (unixtime), 0 表示无记录
  */
  virtual my_time_t oldest_available_time() const = 0;
};

} // namespace flashback

#endif /* FLASHBACK_ENGINE_INCLUDED */
