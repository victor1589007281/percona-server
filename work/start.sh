#!/bin/bash
# ============================================================
# MySQL 启动脚本
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 检查 mysqld 是否存在
MYSQLD="${PS_INSTALL_DIR}/bin/mysqld"
if [ ! -f "${MYSQLD}" ]; then
    MYSQLD="${PS_BUILD_DIR}/bin/mysqld"
fi

if [ ! -f "${MYSQLD}" ]; then
    log_error "未找到 mysqld,请先编译: ./build.sh"
    exit 1
fi

# 检查数据目录
if [ ! -d "${PS_DATA_DIR}/mysql" ]; then
    log_error "数据目录未初始化,请先运行: ./init_data.sh"
    exit 1
fi

# 检查是否已经在运行
if [ -f "${PS_PID_FILE}" ]; then
    PID=$(cat "${PS_PID_FILE}" 2>/dev/null)
    if [ -n "${PID}" ] && kill -0 "${PID}" 2>/dev/null; then
        log_warn "MySQL 已在运行 (PID: ${PID})"
        log_info "如需重启,请先运行: ./stop.sh"
        exit 0
    else
        rm -f "${PS_PID_FILE}"
    fi
fi

log_step "启动 MySQL..."
log_info "端口: ${PS_PORT}"
log_info "Socket: ${PS_SOCKET_FILE}"

# 启动 MySQL
"${MYSQLD}" \
    --user=$(whoami) \
    --basedir="${PS_INSTALL_DIR}" \
    --datadir="${PS_DATA_DIR}" \
    --port="${PS_PORT}" \
    --socket="${PS_SOCKET_FILE}" \
    --pid-file="${PS_PID_FILE}" \
    --log-error="${PS_LOG_DIR}/error.log" \
    --general-log=ON \
    --general-log-file="${PS_LOG_DIR}/general.log" \
    --slow-query-log=ON \
    --slow-query-log-file="${PS_LOG_DIR}/slow.log" \
    --innodb-buffer-pool-size=128M \
    --innodb-log-file-size=48M \
    --innodb-flush-log-at-trx-commit=2 \
    --sync-binlog=0 \
    --skip-mysqlx \
    &

# 等待启动
log_step "等待 MySQL 启动..."
for i in {1..30}; do
    if [ -S "${PS_SOCKET_FILE}" ]; then
        log_info "MySQL 启动成功!"
        log_info "连接命令: mysql -S ${PS_SOCKET_FILE} -u root"
        log_info "或: mysql -h 127.0.0.1 -P ${PS_PORT} -u root"
        exit 0
    fi
    sleep 1
    echo -n "."
done

echo ""
log_error "MySQL 启动超时,请检查日志: ${PS_LOG_DIR}/error.log"
exit 1
