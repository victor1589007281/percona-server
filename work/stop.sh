#!/bin/bash
# ============================================================
# MySQL 停止脚本
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 检查 PID 文件
if [ ! -f "${PS_PID_FILE}" ]; then
    log_warn "PID 文件不存在,MySQL 可能未运行"
    
    # 尝试通过端口查找进程
    PID=$(lsof -ti:${PS_PORT} 2>/dev/null || true)
    if [ -n "${PID}" ]; then
        log_info "找到监听端口 ${PS_PORT} 的进程: ${PID}"
        log_step "正在停止..."
        kill "${PID}"
        sleep 2
        if kill -0 "${PID}" 2>/dev/null; then
            log_warn "进程未响应,强制终止..."
            kill -9 "${PID}"
        fi
        log_info "MySQL 已停止"
    fi
    exit 0
fi

PID=$(cat "${PS_PID_FILE}" 2>/dev/null)
if [ -z "${PID}" ]; then
    log_warn "PID 文件为空"
    rm -f "${PS_PID_FILE}"
    exit 0
fi

if ! kill -0 "${PID}" 2>/dev/null; then
    log_warn "进程 ${PID} 不存在"
    rm -f "${PS_PID_FILE}"
    exit 0
fi

log_step "停止 MySQL (PID: ${PID})..."

# 优雅停止
kill "${PID}"

# 等待停止
for i in {1..30}; do
    if ! kill -0 "${PID}" 2>/dev/null; then
        rm -f "${PS_PID_FILE}"
        log_info "MySQL 已停止"
        exit 0
    fi
    sleep 1
    echo -n "."
done

echo ""
log_warn "MySQL 未能在30秒内停止,强制终止..."
kill -9 "${PID}"
rm -f "${PS_PID_FILE}"
log_info "MySQL 已强制终止"
