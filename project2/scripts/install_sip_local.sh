#!/bin/bash
# 将 libosip2 和 libeXosip2 编译安装到项目 install 目录，实现自包含，他人拿到项目即可运行

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_PREFIX="$PROJECT_ROOT/install"
BUILD_DIR="$PROJECT_ROOT/build_sip_deps"
SOURCES_DIR="$PROJECT_ROOT/third_party/sip_sources"
mkdir -p "$BUILD_DIR"
mkdir -p "$SOURCES_DIR"
cd "$BUILD_DIR"

OSIP_VERSION="5.3.0"
EXOSIP_VERSION="5.3.0"

# 下载 tarball：优先本地 third_party/sip_sources，其次网络（含国内镜像）
fetch_tarball() {
    local name="$1"
    local out="$2"
    shift 2
    local urls=("$@")
    [ -f "$out" ] && return 0
    [ -f "$SOURCES_DIR/$name" ] && cp "$SOURCES_DIR/$name" "$out" && return 0
    for url in "${urls[@]}"; do
        (wget -q --timeout=60 -O "$out" "$url" 2>/dev/null || curl -sLf -o "$out" --connect-timeout 30 --max-time 120 "$url") && return 0
    done
    return 1
}

echo "=== 项目路径: $PROJECT_ROOT"
echo "=== 安装目录: $INSTALL_PREFIX"
echo ""

# 1. 编译安装 libosip2（eXosip 依赖）
if [ ! -f "$INSTALL_PREFIX/lib/libosip2.so" ] && [ ! -f "$INSTALL_PREFIX/lib/libosip2.a" ]; then
    echo ">>> 1/2 编译 libosip2-${OSIP_VERSION} ..."
    if [ ! -d "libosip2-${OSIP_VERSION}" ]; then
        if [ ! -f "libosip2-${OSIP_VERSION}.tar.gz" ]; then
            if ! fetch_tarball "libosip2-${OSIP_VERSION}.tar.gz" "libosip2-${OSIP_VERSION}.tar.gz" \
                "https://ghproxy.com/https://github.com/siptools/libosip2/releases/download/${OSIP_VERSION}/libosip2-${OSIP_VERSION}.tar.gz" \
                "https://github.com/siptools/libosip2/releases/download/${OSIP_VERSION}/libosip2-${OSIP_VERSION}.tar.gz" \
                "https://mirror.linux.org.au/linux/ubuntu/pool/universe/libo/libosip2/libosip2_${OSIP_VERSION}.orig.tar.gz" \
                "http://mirrors.ustc.edu.cn/debian/pool/main/libo/libosip2/libosip2_5.3.0.orig.tar.gz"; then
                echo "    下载失败。请手动将 libosip2-${OSIP_VERSION}.tar.gz 放入: $SOURCES_DIR"
                echo "    或执行: bash scripts/download_sip_sources.sh"
                exit 1
            fi
        fi
        tar xzf "libosip2-${OSIP_VERSION}.tar.gz"
    fi
    cd "libosip2-${OSIP_VERSION}"
    ./configure --prefix="$INSTALL_PREFIX" --enable-static --enable-shared
    make -j$(nproc)
    make install
    cd ..
    echo "    libosip2 安装完成"
else
    echo ">>> 1/2 libosip2 已存在，跳过"
fi

# 2. 编译安装 libeXosip2（若 install 中已有则跳过，否则重新编译以确保与 libosip2 匹配）
if [ ! -f "$INSTALL_PREFIX/lib/libeXosip2.so" ]; then
    echo ">>> 2/2 编译 libeXosip2-${EXOSIP_VERSION} ..."
    if [ ! -d "libeXosip2-${EXOSIP_VERSION}" ]; then
        if [ ! -f "libeXosip2-${EXOSIP_VERSION}.tar.gz" ]; then
            if ! fetch_tarball "libeXosip2-${EXOSIP_VERSION}.tar.gz" "libeXosip2-${EXOSIP_VERSION}.tar.gz" \
                "https://ghproxy.com/https://github.com/siptools/libeXosip2/releases/download/${EXOSIP_VERSION}/libeXosip2-${EXOSIP_VERSION}.tar.gz" \
                "https://github.com/siptools/libeXosip2/releases/download/${EXOSIP_VERSION}/libeXosip2-${EXOSIP_VERSION}.tar.gz" \
                "https://mirror.linux.org.au/linux/ubuntu/pool/universe/libe/libeXosip2/libeXosip2_5.3.0.orig.tar.gz"; then
                echo "    下载失败。请手动将 libeXosip2-${EXOSIP_VERSION}.tar.gz 放入: $SOURCES_DIR"
                exit 1
            fi
        fi
        tar xzf "libeXosip2-${EXOSIP_VERSION}.tar.gz"
    fi
    cd "libeXosip2-${EXOSIP_VERSION}"
    export PKG_CONFIG_PATH="$INSTALL_PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"
    export CPPFLAGS="-I$INSTALL_PREFIX/include"
    export LDFLAGS="-L$INSTALL_PREFIX/lib"
    ./configure --prefix="$INSTALL_PREFIX"
    make -j$(nproc)
    make install
    cd ..
    echo "    libeXosip2 安装完成"
else
    echo ">>> 2/2 install 中已有 libeXosip2，跳过"
    # 若 libeXosip2 来自其他机器，可能含有错误 RUNPATH，需用 patchelf 修复
    if command -v patchelf >/dev/null 2>&1; then
        CURRENT_RPATH=$(patchelf --print-rpath "$INSTALL_PREFIX/lib/libeXosip2.so.15" 2>/dev/null || true)
        if [ -n "$CURRENT_RPATH" ] && [[ "$CURRENT_RPATH" != *"$INSTALL_PREFIX"* ]]; then
            echo "    修复 libeXosip2 RUNPATH: $CURRENT_RPATH -> $INSTALL_PREFIX/lib"
            patchelf --set-rpath "$INSTALL_PREFIX/lib" "$INSTALL_PREFIX/lib/libeXosip2.so.15"
        fi
    fi
fi

echo ""
echo "=== SIP 库已安装到: $INSTALL_PREFIX"
echo "    include: $INSTALL_PREFIX/include"
echo "    lib:     $INSTALL_PREFIX/lib"
ls -la "$INSTALL_PREFIX/lib/" | grep -E "osip|eXosip" || true
echo ""
echo "请执行: cd $PROJECT_ROOT/build && rm -rf CMakeCache.txt CMakeFiles && cmake .. && make -j\$(nproc)"
echo "然后运行: ./build/demo  或  bash scripts/run_demo.sh"
