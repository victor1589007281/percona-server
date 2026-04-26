/**
  @file storage/innobase/flashback/flashback_purge_guard.cc
  @brief Flashback Purge Guard — RAII 保护器，确保闪回期间 Purge 线程被暂停

  闪回操作依赖 Undo Log 版本链来重建历史数据版本。
  如果 Purge 线程在闪回过程中清理了 Undo 记录，会导致
  DB_MISSING_HISTORY 错误，闪回无法完成。

  使用模式参考 row0quiesce.cc 中的 trx_purge_stop/run 用法:
  - 构造时检查 Purge 状态，若非 DISABLED 则调用 trx_purge_stop()
  - 析构时若 Purge 之前未被禁用，则调用 trx_purge_run()
*/

#include "flashback_purge_guard.h"

#include "trx0purge.h"

/** 构造函数：暂停 Purge 线程
 *
 * WHY: 参考 row0quiesce.cc 中的使用模式，先检查 purge 状态。
 *      如果 Purge 已经被禁用 (PURGE_STATE_DISABLED)，说明系统没有
 *      启动 Purge 线程 (--innodb-purge-threads=0)，此时无需也不应该
 *      调用 trx_purge_stop() (内部有 ut_a 断言要求 srv_n_purge_threads > 0)。
 */
FlashbackPurgeGuard::FlashbackPurgeGuard()
    : m_purge_was_disabled(false) {
  purge_state_t state = trx_purge_state();

  if (state == PURGE_STATE_DISABLED) {
    /* Purge 未启动，无需暂停 */
    m_purge_was_disabled = true;
    return;
  }

  ut_ad(state != PURGE_STATE_INIT);
  ut_ad(state != PURGE_STATE_EXIT);

  trx_purge_stop();
}

/** 析构函数：恢复 Purge 线程
 *
 * 仅在构造时 Purge 未被禁用的情况下调用 trx_purge_run()。
 * WHY: 如果 Purge 本来就是 DISABLED 状态，调用 trx_purge_run()
 *      会触发 ut_error (switch case PURGE_STATE_DISABLED → ut_error)。
 */
FlashbackPurgeGuard::~FlashbackPurgeGuard() {
  if (m_purge_was_disabled) {
    return;
  }

  trx_purge_run();
}
