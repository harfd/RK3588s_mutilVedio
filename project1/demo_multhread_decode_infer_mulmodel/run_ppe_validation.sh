#!/usr/bin/env bash
# 在 RK3588 板端编译并运行单模型 PPE 验证。
# 用法：
#   bash run_ppe_validation.sh int8
#   bash run_ppe_validation.sh fp
# 已经重新编译过时可使用：NO_BUILD=1 bash run_ppe_validation.sh int8
set -euo pipefail

cd "$(dirname "$0")"

MODEL_KIND="${1:-int8}"
case "$MODEL_KIND" in
  int8)
    CONFIG="config_ppe_int8.ini"
    ;;
  fp)
    CONFIG="config_ppe_fp.ini"
    ;;
  *)
    echo "用法: bash run_ppe_validation.sh [int8|fp]" >&2
    exit 2
    ;;
esac

if [ "${NO_BUILD:-0}" != "1" ]; then
  cmake -S . -B build
  cmake --build build -j"$(nproc)"
fi

[ -x build/myDemo ] || {
  echo "[!] 找不到 build/myDemo，请先去掉 NO_BUILD 重新编译" >&2
  exit 1
}

echo "[*] 验证模型类型: $MODEL_KIND"
echo "[*] 使用配置: $(pwd)/$CONFIG"

# RKNN/RGA/MPP 设备节点在当前板端需要 root 权限。构建阶段保持普通用户，
# 只提升最终进程权限，避免 build/ 目录产生 root 所有的中间文件。
if [ "$(id -u)" -eq 0 ]; then
  exec ./build/myDemo "$CONFIG"
fi
exec sudo -- ./build/myDemo "$CONFIG"
