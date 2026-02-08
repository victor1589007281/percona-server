#!/bin/bash
# ============================================================
# InnoDB 快速重编译脚本
# 专用于 InnoDB 代码修改后的快速迭代
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

JOBS="${1:-$PS_PARALLEL_JOBS}"

# 检查是否已配置
if [ ! -f "${PS_BUILD_DIR}/Makefile" ] && [ ! -f "${PS_BUILD_DIR}/build.ninja" ]; then
    log_error "未找到构建文件,请先运行 cmake_configure.sh"
    exit 1
fi

cd "${PS_BUILD_DIR}"

START_TIME=$(date +%s)

log_step "重编译 InnoDB 相关目标..."

# 编译 InnoDB 核心库
cmake --build . --target innobase -j${JOBS}

# 重新链接 mysqld
log_step "重新链接 mysqld..."
cmake --build . --target mysqld -j${JOBS}

# 复制到安装目录
if [ -d "${PS_INSTALL_DIR}/bin" ]; then
    log_step "更新安装目录..."
    cp "${PS_BUILD_DIR}/bin/mysqld" "${PS_INSTALL_DIR}/bin/mysqld"
fi

END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

log_info "InnoDB 重编译完成! 耗时: ${DURATION}秒"
log_info "如需测试修改,请重启 MySQL: ./stop.sh && ./start.sh"
