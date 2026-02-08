#!/bin/bash
# ============================================================
# 编译脚本 - 支持全量编译和增量编译
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 参数处理
TARGET="${1:-}"  # 可选: 指定编译目标 (mysqld, innodb, all)
JOBS="${2:-$PS_PARALLEL_JOBS}"

# 检查是否已配置
if [ ! -f "${PS_BUILD_DIR}/Makefile" ] && [ ! -f "${PS_BUILD_DIR}/build.ninja" ]; then
    log_error "未找到构建文件,请先运行 cmake_configure.sh"
    exit 1
fi

cd "${PS_BUILD_DIR}"

# 记录开始时间
START_TIME=$(date +%s)

case "${TARGET}" in
    "mysqld")
        # 仅编译 mysqld
        log_step "增量编译 mysqld..."
        cmake --build . --target mysqld -j${JOBS}
        ;;
    "innodb")
        # 仅编译 InnoDB 相关
        log_step "增量编译 InnoDB..."
        cmake --build . --target innobase -j${JOBS}
        ;;
    "sql")
        # 仅编译 SQL 层
        log_step "增量编译 SQL 层..."
        cmake --build . --target sql_main -j${JOBS}
        ;;
    "install")
        # 编译并安装
        log_step "编译并安装..."
        cmake --build . -j${JOBS}
        cmake --build . --target install
        ;;
    "clean")
        # 清理
        log_step "清理构建..."
        cmake --build . --target clean
        ;;
    *)
        # 全量编译
        log_step "全量编译 (使用 ${JOBS} 个并行任务)..."
        cmake --build . -j${JOBS}
        
        # 自动安装
        log_step "安装到 ${PS_INSTALL_DIR}..."
        cmake --build . --target install
        ;;
esac

# 计算耗时
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))
MINUTES=$((DURATION / 60))
SECONDS=$((DURATION % 60))

log_info "编译完成! 耗时: ${MINUTES}分${SECONDS}秒"

# 显示编译产物
if [ -f "${PS_BUILD_DIR}/bin/mysqld" ]; then
    log_info "mysqld 位置: ${PS_BUILD_DIR}/bin/mysqld"
fi

if [ -f "${PS_INSTALL_DIR}/bin/mysqld" ]; then
    log_info "安装位置: ${PS_INSTALL_DIR}/bin/mysqld"
fi
