#!/usr/bin/env bash
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN_DEMO_SH="${PROJECT_ROOT}/run_demo.sh"

if [[ -f "${PROJECT_ROOT}/demo.env" ]]; then
    # shellcheck disable=SC1091
    source "${PROJECT_ROOT}/demo.env"
fi

if [[ ! -x "${RUN_DEMO_SH}" ]]; then
    echo "Runner not found: ${RUN_DEMO_SH}"
    exit 1
fi

MODE="${1:-all}"
if [[ $# -gt 0 ]]; then
    shift
fi

export MYDEMO_RTMP_URL="${MYDEMO_RTMP_URL:-rtmp://192.168.1.25/live/livestream}"
export MYDEMO_RTSP_URL="${MYDEMO_RTSP_URL:-rtsp://192.168.1.25:8554/mystream}"
export MYDEMO_GB_CONFIG="${MYDEMO_GB_CONFIG:-${PROJECT_ROOT}/gb28181.conf}"

export MYDEMO_ENABLE_RTMP=0
export MYDEMO_ENABLE_RTSP=0
export MYDEMO_ENABLE_GB28181=0

enable_mode() {
    case "$1" in
        rtmp)
            export MYDEMO_ENABLE_RTMP=1
            ;;
        rtsp)
            export MYDEMO_ENABLE_RTSP=1
            ;;
        gb28181)
            export MYDEMO_ENABLE_GB28181=1
            ;;
        all)
            export MYDEMO_ENABLE_RTMP=1
            export MYDEMO_ENABLE_RTSP=1
            export MYDEMO_ENABLE_GB28181=1
            ;;
        none|"")
            ;;
        *)
            echo "Unsupported mode: $1"
            echo "Usage: ./run_streams.sh [rtmp|rtsp|gb28181|all|rtmp,rtsp|rtmp,gb28181|rtsp,gb28181] [run_demo args...]"
            exit 1
            ;;
    esac
}

IFS=',' read -r -a MODES <<< "${MODE}"
for item in "${MODES[@]}"; do
    enable_mode "${item}"
done

echo "Streaming config:"
echo "  RTMP    : ${MYDEMO_ENABLE_RTMP} (${MYDEMO_RTMP_URL})"
echo "  RTSP    : ${MYDEMO_ENABLE_RTSP} (${MYDEMO_RTSP_URL})"
echo "  GB28181 : ${MYDEMO_ENABLE_GB28181} (${MYDEMO_GB_CONFIG})"
echo

exec "${RUN_DEMO_SH}" "$@"
