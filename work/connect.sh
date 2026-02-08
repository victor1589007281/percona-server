#!/bin/bash
# ============================================================
# 快速连接脚本
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 查找 mysql 客户端
MYSQL_CLIENT="${PS_INSTALL_DIR}/bin/mysql"
if [ ! -f "${MYSQL_CLIENT}" ]; then
    MYSQL_CLIENT=$(which mysql 2>/dev/null || echo "")
fi

if [ -z "${MYSQL_CLIENT}" ] || [ ! -f "${MYSQL_CLIENT}" ]; then
    log_error "未找到 mysql 客户端"
    log_info "请安装: brew install mysql-client"
    log_info "或使用编译后的客户端: ${PS_INSTALL_DIR}/bin/mysql"
    exit 1
fi

# 检查 MySQL 是否运行
if [ ! -S "${PS_SOCKET_FILE}" ]; then
    log_error "MySQL 未运行,请先启动: ./start.sh"
    exit 1
fi

log_info "连接到 MySQL..."
"${MYSQL_CLIENT}" -S "${PS_SOCKET_FILE}" -u root "$@"
