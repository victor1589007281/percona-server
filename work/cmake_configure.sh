#!/bin/bash
# ============================================================
# CMake 配置脚本 - 轻量化编译 (仅 mysqld + InnoDB)
# 禁用不必要的组件以加速编译和减少资源占用
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/config.sh"

# 参数处理
BUILD_TYPE="${1:-$PS_BUILD_TYPE}"

log_step "开始 CMake 配置..."
log_info "源码目录: ${PS_SOURCE_DIR}"
log_info "构建目录: ${PS_BUILD_DIR}"
log_info "编译类型: ${BUILD_TYPE}"

# 创建构建目录
mkdir -p "${PS_BUILD_DIR}"
cd "${PS_BUILD_DIR}"

# ============================================================
# CMake 配置参数
# 核心原则: 仅保留 mysqld + InnoDB 核心功能
# ============================================================

CMAKE_OPTS=(
    # 基础配置
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"
    -DCMAKE_INSTALL_PREFIX="${PS_INSTALL_DIR}"
    -DMYSQL_DATADIR="${PS_DATA_DIR}"
    
    # OpenSSL 配置
    -DWITH_SSL="${OPENSSL_ROOT_DIR}"
    
    # ============================================================
    # 禁用不必要的组件 (大幅减少编译时间)
    # ============================================================
    
    # 禁用 MySQL Router (节省大量编译时间)
    -DWITH_ROUTER=OFF
    
    # 禁用单元测试 (开发时可按需开启)
    -DWITH_UNIT_TESTS=OFF
    
    # 禁用 NDB Cluster
    -DWITH_NDB=OFF
    -DWITH_NDBCLUSTER=OFF
    
    # 禁用 RocksDB 存储引擎 (专注 InnoDB)
    -DWITH_ROCKSDB=OFF
    
    # 禁用 TokuDB (如果存在)
    -DWITH_TOKUDB=OFF
    
    # 禁用 Federated 存储引擎
    -DWITH_FEDERATED_STORAGE_ENGINE=OFF
    
    # 禁用 Archive 存储引擎
    -DWITH_ARCHIVE_STORAGE_ENGINE=OFF
    
    # 禁用 Blackhole 存储引擎
    -DWITH_BLACKHOLE_STORAGE_ENGINE=OFF
    
    # 禁用 Example 存储引擎
    -DWITH_EXAMPLE_STORAGE_ENGINE=OFF
    
    # 禁用 Memcached
    -DWITH_INNODB_MEMCACHED=OFF
    
    # 禁用不常用的认证插件
    -DWITH_AUTHENTICATION_LDAP=OFF
    -DWITH_AUTHENTICATION_KERBEROS=OFF
    -DWITH_AUTHENTICATION_WEBAUTHN=OFF
    
    # 禁用 PAM 认证
    -DWITH_PAM=OFF
    
    # 禁用 Percona 特定的 LDAP
    -DWITH_PERCONA_AUTHENTICATION_LDAP=OFF
    
    # 禁用 Telemetry
    -DWITH_PERCONA_TELEMETRY=OFF
    
    # 禁用 JavaScript 引擎 (V8)
    -DWITH_JS_LANG=OFF
    
    # 禁用不必要的插件
    -DWITH_LOCK_ORDER=OFF
    
    # 禁用 LTO (链接时优化) - Debug时不需要
    -DWITH_LTO=OFF
    
    # 禁用 KMIP Keyring (避免依赖问题)
    -DWITH_COMPONENT_KEYRING_KMIP=OFF
    
    # 禁用 KMS Keyring
    -DWITH_COMPONENT_KEYRING_KMS=OFF
    
    # ============================================================
    # 保留必要的组件
    # ============================================================
    
    # 保留 InnoDB (核心)
    -DWITH_INNOBASE_STORAGE_ENGINE=ON
    
    # 保留 MyISAM (系统表需要)
    -DWITH_MYISAM_STORAGE_ENGINE=ON
    
    # 保留 Heap (临时表需要)
    -DWITH_HEAP_STORAGE_ENGINE=ON
    
    # 使用 bundled 库简化依赖
    -DWITH_ZLIB=bundled
    -DWITH_ZSTD=bundled
    -DWITH_LZ4=bundled
    -DWITH_LIBEVENT=bundled
    -DWITH_PROTOBUF=bundled
    -DWITH_RAPIDJSON=bundled
    -DWITH_ICU=bundled
    -DWITH_FIDO=bundled
    
    # 允许源内编译
    -DFORCE_INSOURCE_BUILD=1
    
    # 特性集
    -DFEATURE_SET=community
)

# Debug 模式特定配置
if [ "${BUILD_TYPE}" = "Debug" ]; then
    CMAKE_OPTS+=(
        -DWITH_DEBUG=ON
        -DDEBUG_EXTNAME=OFF
    )
fi

log_step "执行 CMake 配置..."
log_info "CMake 参数:"
for opt in "${CMAKE_OPTS[@]}"; do
    echo "  $opt"
done

# 执行 CMake
cmake "${PS_SOURCE_DIR}" "${CMAKE_OPTS[@]}"

log_info "CMake 配置完成!"
log_info "下一步: 运行 ./build.sh 开始编译"
