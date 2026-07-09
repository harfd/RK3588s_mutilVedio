#!/bin/bash
# 修复 install/lib 中 SIP 库的 RUNPATH（拷贝项目到其他路径后需执行）
# 依赖: apt install patchelf

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_LIB="$PROJECT_ROOT/install/lib"

if ! command -v patchelf >/dev/null 2>&1; then
    echo "请安装 patchelf: sudo apt install patchelf"
    exit 1
fi

for so in "$INSTALL_LIB"/libeXosip2.so.15 "$INSTALL_LIB"/libosip2.so.15 "$INSTALL_LIB"/libosipparser2.so.15; do
    [ -f "$so" ] || continue
    patchelf --set-rpath "$INSTALL_LIB" "$so"
    echo "已修复: $so"
done
echo "完成。可运行: ./build/demo"
