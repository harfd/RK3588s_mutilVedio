#!/bin/bash
# 运行 demo，确保能找到 install/lib 中的 SIP 库
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
export LD_LIBRARY_PATH="${PROJECT_ROOT}/install/lib:${LD_LIBRARY_PATH}"
exec "${PROJECT_ROOT}/build/demo" "$@"
