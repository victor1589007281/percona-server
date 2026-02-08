#!/bin/bash
# ============================================================
# 数据目录初始化脚本
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

log_step "初始化数据目录..."
log_info "数据目录: ${PS_DATA_DIR}"

# 备份已有数据目录
if [ -d "${PS_DATA_DIR}" ]; then
    BACKUP_DIR="${PS_DATA_DIR}.backup.$(date +%Y%m%d_%H%M%S)"
    log_warn "数据目录已存在,备份到: ${BACKUP_DIR}"
    mv "${PS_DATA_DIR}" "${BACKUP_DIR}"
fi

# 创建必要目录
mkdir -p "${PS_DATA_DIR}"
mkdir -p "${PS_LOG_DIR}"

# 初始化数据目录
log_step "执行 mysqld --initialize-insecure..."

"${MYSQLD}" \
    --initialize-insecure \
    --user=$(whoami) \
    --basedir="${PS_INSTALL_DIR}" \
    --datadir="${PS_DATA_DIR}" \
    --log-error="${PS_LOG_DIR}/error.log"

log_info "数据目录初始化完成!"
log_info "注意: 使用了 --initialize-insecure,root 密码为空"
log_info "下一步: 运行 ./start.sh 启动 MySQL"
