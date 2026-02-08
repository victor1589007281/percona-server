#!/bin/bash
# ============================================================
# Percona Server 轻量化编译配置
# 针对 Mac 本地开发环境优化
# ============================================================

# 源码目录 (自动检测)
export PS_SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# 工作目录
export PS_WORK_DIR="${PS_SOURCE_DIR}/work"

# 构建目录
export PS_BUILD_DIR="${PS_WORK_DIR}/build"

# 安装目录
export PS_INSTALL_DIR="${PS_WORK_DIR}/install"

# 数据目录
export PS_DATA_DIR="${PS_WORK_DIR}/data"

# 日志目录
export PS_LOG_DIR="${PS_WORK_DIR}/logs"

# PID 文件
export PS_PID_FILE="${PS_WORK_DIR}/mysqld.pid"

# Socket 文件
export PS_SOCKET_FILE="${PS_WORK_DIR}/mysql.sock"

# 端口号 (使用非标准端口避免冲突)
export PS_PORT=33060

# OpenSSL 路径 (Homebrew)
export OPENSSL_ROOT_DIR="/opt/homebrew/opt/openssl@3"

# 并行编译数 (Mac上建议CPU核心数/2以节省资源)
export PS_PARALLEL_JOBS=$(( $(sysctl -n hw.ncpu) / 2 ))
if [ "$PS_PARALLEL_JOBS" -lt 2 ]; then
    export PS_PARALLEL_JOBS=2
fi

# 编译类型: Debug / RelWithDebInfo / Release
export PS_BUILD_TYPE="RelWithDebInfo"

# 调试版本编译类型
export PS_DEBUG_BUILD_TYPE="Debug"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${BLUE}[STEP]${NC} $1"
}
