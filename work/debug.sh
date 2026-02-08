#!/bin/bash
# ============================================================
# 调试启动脚本 - 支持 lldb 附加调试
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 参数处理
MODE="${1:-foreground}"  # foreground, lldb, attach

# 检查 mysqld 是否存在 (优先使用 Debug 版本)
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

# 停止已运行的实例
if [ -f "${PS_PID_FILE}" ]; then
    PID=$(cat "${PS_PID_FILE}" 2>/dev/null)
    if [ -n "${PID}" ] && kill -0 "${PID}" 2>/dev/null; then
        log_warn "停止已运行的 MySQL 实例..."
        "${SCRIPT_DIR}/stop.sh"
        sleep 2
    fi
fi

# 通用 MySQL 参数
MYSQL_ARGS=(
    --user=$(whoami)
    --basedir="${PS_INSTALL_DIR}"
    --datadir="${PS_DATA_DIR}"
    --port="${PS_PORT}"
    --socket="${PS_SOCKET_FILE}"
    --pid-file="${PS_PID_FILE}"
    --log-error="${PS_LOG_DIR}/error.log"
    --general-log=ON
    --general-log-file="${PS_LOG_DIR}/general.log"
    --innodb-buffer-pool-size=128M
    --innodb-log-file-size=48M
    --innodb-flush-log-at-trx-commit=2
    --skip-mysqlx
    # Debug 友好的参数
    --gdb  # 允许 core dump 和调试
    --core-file
)

case "${MODE}" in
    "foreground"|"fg")
        # 前台运行 (Ctrl+C 可停止)
        log_step "前台启动 MySQL (按 Ctrl+C 停止)..."
        log_info "端口: ${PS_PORT}"
        log_info "Socket: ${PS_SOCKET_FILE}"
        "${MYSQLD}" "${MYSQL_ARGS[@]}"
        ;;
        
    "lldb")
        # 使用 lldb 启动调试
        log_step "使用 lldb 启动调试..."
        log_info "端口: ${PS_PORT}"
        log_info "Socket: ${PS_SOCKET_FILE}"
        echo ""
        log_info "常用 lldb 命令:"
        echo "  run                    - 开始运行"
        echo "  breakpoint set -n <func> - 设置函数断点"
        echo "  bt                     - 显示调用栈"
        echo "  frame variable         - 显示当前帧变量"
        echo "  continue               - 继续执行"
        echo "  quit                   - 退出"
        echo ""
        
        # 生成 lldb 命令文件
        LLDB_INIT="${PS_WORK_DIR}/.lldb_init"
        cat > "${LLDB_INIT}" << EOF
# 设置参数
settings set -- target.run-args ${MYSQL_ARGS[@]}

# 常用 InnoDB 断点 (取消注释以启用)
# breakpoint set -n row_search_mvcc
# breakpoint set -n btr_cur_search_to_nth_level
# breakpoint set -n buf_page_get_gen
# breakpoint set -n trx_commit

# SQL 执行相关断点
# breakpoint set -n mysql_execute_command
# breakpoint set -n dispatch_command

echo "MySQL 调试环境已准备,输入 'run' 开始"
EOF
        
        lldb -s "${LLDB_INIT}" "${MYSQLD}"
        ;;
        
    "attach")
        # 附加到已运行的 mysqld
        if [ ! -f "${PS_PID_FILE}" ]; then
            log_error "MySQL 未运行,请先启动: ./start.sh"
            exit 1
        fi
        
        PID=$(cat "${PS_PID_FILE}")
        if ! kill -0 "${PID}" 2>/dev/null; then
            log_error "进程 ${PID} 不存在"
            exit 1
        fi
        
        log_step "附加到 MySQL 进程 (PID: ${PID})..."
        log_warn "附加后进程将暂停,输入 'continue' 恢复执行"
        lldb -p "${PID}"
        ;;
        
    *)
        echo "用法: $0 [foreground|lldb|attach]"
        echo ""
        echo "模式说明:"
        echo "  foreground (fg) - 前台运行,Ctrl+C 停止"
        echo "  lldb            - 使用 lldb 启动调试"
        echo "  attach          - 附加到已运行的 mysqld 进程"
        exit 1
        ;;
esac
