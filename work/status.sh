#!/bin/bash
# ============================================================
# MySQL 状态检查脚本
# ============================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

echo "=============================================="
echo "Percona Server 开发环境状态"
echo "=============================================="
echo ""

# 检查 mysqld 编译状态
echo "【编译状态】"
if [ -f "${PS_INSTALL_DIR}/bin/mysqld" ]; then
    echo "  ✅ mysqld 已编译: ${PS_INSTALL_DIR}/bin/mysqld"
    MYSQLD_VERSION=$("${PS_INSTALL_DIR}/bin/mysqld" --version 2>/dev/null | head -1 || echo "版本获取失败")
    echo "  版本: ${MYSQLD_VERSION}"
elif [ -f "${PS_BUILD_DIR}/bin/mysqld" ]; then
    echo "  ✅ mysqld 已编译: ${PS_BUILD_DIR}/bin/mysqld"
else
    echo "  ❌ mysqld 未编译"
fi
echo ""

# 检查数据目录
echo "【数据目录】"
if [ -d "${PS_DATA_DIR}/mysql" ]; then
    echo "  ✅ 数据目录已初始化: ${PS_DATA_DIR}"
    DATA_SIZE=$(du -sh "${PS_DATA_DIR}" 2>/dev/null | cut -f1)
    echo "  大小: ${DATA_SIZE}"
else
    echo "  ❌ 数据目录未初始化"
fi
echo ""

# 检查运行状态
echo "【运行状态】"
if [ -f "${PS_PID_FILE}" ]; then
    PID=$(cat "${PS_PID_FILE}" 2>/dev/null)
    if [ -n "${PID}" ] && kill -0 "${PID}" 2>/dev/null; then
        echo "  ✅ MySQL 正在运行 (PID: ${PID})"
        echo "  端口: ${PS_PORT}"
        echo "  Socket: ${PS_SOCKET_FILE}"
        
        # 检查连接
        if [ -S "${PS_SOCKET_FILE}" ]; then
            MYSQL_CLIENT="${PS_INSTALL_DIR}/bin/mysql"
            if [ ! -f "${MYSQL_CLIENT}" ]; then
                MYSQL_CLIENT=$(which mysql 2>/dev/null || echo "")
            fi
            if [ -n "${MYSQL_CLIENT}" ] && [ -f "${MYSQL_CLIENT}" ]; then
                UPTIME=$("${MYSQL_CLIENT}" -S "${PS_SOCKET_FILE}" -u root -e "SHOW STATUS LIKE 'Uptime'" 2>/dev/null | grep Uptime | awk '{print $2}')
                if [ -n "${UPTIME}" ]; then
                    echo "  运行时间: ${UPTIME} 秒"
                fi
            fi
        fi
    else
        echo "  ❌ MySQL 未运行 (PID 文件存在但进程不存在)"
    fi
else
    echo "  ❌ MySQL 未运行"
fi
echo ""

# 显示连接信息
echo "【连接信息】"
echo "  Socket: mysql -S ${PS_SOCKET_FILE} -u root"
echo "  TCP:    mysql -h 127.0.0.1 -P ${PS_PORT} -u root"
echo ""

# 显示日志位置
echo "【日志位置】"
echo "  错误日志: ${PS_LOG_DIR}/error.log"
echo "  通用日志: ${PS_LOG_DIR}/general.log"
echo "  慢查询日志: ${PS_LOG_DIR}/slow.log"
echo ""
