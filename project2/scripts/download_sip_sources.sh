#!/bin/bash
# 将 libosip2 和 libeXosip2 源码包下载到 third_party/sip_sources，便于无网络环境或项目分发

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCES_DIR="$PROJECT_ROOT/third_party/sip_sources"
mkdir -p "$SOURCES_DIR"
cd "$SOURCES_DIR"

OSIP_VERSION="5.3.0"
EXOSIP_VERSION="5.3.0"

fetch() {
    local name="$1"
    shift
    local urls=("$@")
    [ -f "$name" ] && echo "  已有: $name" && return 0
    for url in "${urls[@]}"; do
        echo "  尝试: $url"
        if wget -q --timeout=60 -O "$name" "$url" 2>/dev/null || curl -sLf -o "$name" --connect-timeout 30 --max-time 120 "$url" 2>/dev/null; then
            echo "  下载成功: $name"
            return 0
        fi
    done
    return 1
}

echo "=== 下载 SIP 依赖到: $SOURCES_DIR"
echo ""

echo ">>> libosip2-${OSIP_VERSION}.tar.gz"
if ! fetch "libosip2-${OSIP_VERSION}.tar.gz" \
    "http://mirrors.ustc.edu.cn/debian/pool/main/libo/libosip2/libosip2_5.3.0.orig.tar.gz" \
    "https://ghproxy.com/https://github.com/siptools/libosip2/releases/download/${OSIP_VERSION}/libosip2-${OSIP_VERSION}.tar.gz" \
    "https://github.com/siptools/libosip2/releases/download/${OSIP_VERSION}/libosip2-${OSIP_VERSION}.tar.gz" \
    "https://mirror.linux.org.au/linux/ubuntu/pool/universe/libo/libosip2/libosip2_${OSIP_VERSION}.orig.tar.gz"; then
    echo "  失败。请手动下载并放到: $SOURCES_DIR"
    exit 1
fi

echo ""
echo ">>> libeXosip2-${EXOSIP_VERSION}.tar.gz"
if ! fetch "libeXosip2-${EXOSIP_VERSION}.tar.gz" \
    "https://ghproxy.com/https://github.com/siptools/libeXosip2/releases/download/${EXOSIP_VERSION}/libeXosip2-${EXOSIP_VERSION}.tar.gz" \
    "https://github.com/siptools/libeXosip2/releases/download/${EXOSIP_VERSION}/libeXosip2-${EXOSIP_VERSION}.tar.gz" \
    "https://mirror.linux.org.au/linux/ubuntu/pool/universe/libe/libeXosip2/libeXosip2_5.3.0.orig.tar.gz"; then
    echo "  失败。请手动下载并放到: $SOURCES_DIR"
    exit 1
fi

echo ""
echo "=== 完成。可执行: bash scripts/install_sip_local.sh"
