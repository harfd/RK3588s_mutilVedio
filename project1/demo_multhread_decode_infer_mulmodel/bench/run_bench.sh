#!/usr/bin/env bash
#
# DMA-BUF 零拷贝 vs 深拷贝 性能对比自动化脚本 (RK3588 Linux 板端运行)
#
# 从同一 bench 分支编出两个变体二进制，锁频固定环境，交替执行多轮，
# 外部采样 CPU / 内存 / 温度，收集应用自身的分阶段 CSV，最后做均值±方差汇总。
#
# 用法:
#   sudo ./run_bench.sh            # 完整流程: 构建 + 测试 + 汇总
#   sudo ./run_bench.sh --no-build # 跳过构建, 直接测已存在的二进制
#   sudo ./run_bench.sh --analyze  # 只对已有结果做汇总
#
set -euo pipefail

# ======================= 可配置区 =======================
PROJECT_DIR="${PROJECT_DIR:-$HOME/multi_video/RK3588s_mutilVedio/project1/demo_multhread_decode_infer_mulmodel}"
# 结果落在 bench/results 下 (已 gitignore)。如需减少板端 eMMC 写入可改指向 tmpfs。
RESULTS_ROOT="${RESULTS_ROOT:-$PROJECT_DIR/bench/results}"
RUN_TAG="$(date +%Y%m%d_%H%M%S)"
RUN_DIR="$RESULTS_ROOT/run_$RUN_TAG"

# 变体: 名称 -> CMake 开关
declare -A VARIANT_FLAGS=(
  [dma]="-DBENCH=ON -DTRANSFER_COPY=OFF"
  [copy]="-DBENCH=ON -DTRANSFER_COPY=ON"
)
VARIANTS=(dma copy)

# 统一配置文件; 工况(capped/uncapped)由环境变量 BENCH_UNCAPPED 区分, 无需两份配置。
BENCH_CONFIG="${BENCH_CONFIG:-$PROJECT_DIR/config_bench.ini}"
declare -A WORKLOAD_UNCAPPED=(
  [capped]=0     # 定帧率: 保留限速, 比等工作量下的 CPU/内存
  [uncapped]=1   # 不限速: 比吞吐天花板
)
WORKLOADS=(capped uncapped)
# 1=编码照跑但不发网络(默认, 无需 RTMP 服务器且去网络噪声); 0=真实推流
NULL_SINK="${NULL_SINK:-1}"

REPS="${REPS:-5}"              # 每 变体×工况 重复次数 (env 可覆盖)
WARMUP_S="${WARMUP_S:-30}"     # 预热(丢弃)秒数 (env 可覆盖)
WINDOW_S="${WINDOW_S:-120}"    # 稳态采样窗口秒数 (env 可覆盖)
# 快速冒烟: REPS=1 WARMUP_S=10 WINDOW_S=30 sudo ./run_bench.sh
TEMP_MAX_C=70          # 起跑温度上限(摄氏度), 高于此值先冷却
COOLDOWN_MAX_S=180     # 冷却最长等待
SAMPLE_INTERVAL=1      # 采样间隔(秒)
PERF_EVENTS="cycles,instructions,cache-misses,LLC-load-misses"

# 应用需支持: 环境变量 BENCH_RUN_SECONDS=N -> 运行 N 秒后自动优雅退出并 flush CSV
#            环境变量 BENCH_CSV_DIR=path -> 分阶段 CSV 输出目录
# (见脚本末尾"应用侧需要的最小改动"说明)
# =======================================================

log()  { echo "[$(date +%H:%M:%S)] $*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# ---- 环境锁定: 保存并设置 performance governor, 退出时恢复 ----
declare -a SAVED_GOV_FILES SAVED_GOV_VALS
lock_cpu_perf() {
  log "锁定 CPU governor -> performance"
  for g in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    [ -w "$g" ] || continue
    SAVED_GOV_FILES+=("$g"); SAVED_GOV_VALS+=("$(cat "$g")")
    echo performance > "$g" || true
  done
  # NPU/RGA/DMC devfreq 若可写也锁 performance (best-effort)
  for d in /sys/class/devfreq/*/governor; do
    [ -w "$d" ] || continue
    SAVED_GOV_FILES+=("$d"); SAVED_GOV_VALS+=("$(cat "$d")")
    echo performance > "$d" 2>/dev/null || true
  done
}
restore_gov() {
  for i in "${!SAVED_GOV_FILES[@]}"; do
    echo "${SAVED_GOV_VALS[$i]}" > "${SAVED_GOV_FILES[$i]}" 2>/dev/null || true
  done
}

# ---- 温度: 取所有 thermal_zone 最大值(摄氏度) ----
max_temp_c() {
  local m=0 t
  for z in /sys/class/thermal/thermal_zone*/temp; do
    [ -r "$z" ] || continue
    t=$(cat "$z"); [ "$t" -gt "$m" ] && m=$t
  done
  echo $(( m / 1000 ))
}

cooldown() {
  local waited=0
  while [ "$(max_temp_c)" -gt "$TEMP_MAX_C" ] && [ "$waited" -lt "$COOLDOWN_MAX_S" ]; do
    log "  温度 $(max_temp_c)C > ${TEMP_MAX_C}C, 冷却中..."; sleep 5; waited=$((waited+5))
  done
}

# ---- /proc 采样器: RSS/PSS/fd/温度/系统可用内存 -> CSV ----
sample_proc() {   # $1=pid  $2=outfile
  local pid=$1 out=$2
  echo "ts,rss_kb,pss_kb,fd,temp_c,mem_avail_kb" > "$out"
  while kill -0 "$pid" 2>/dev/null; do
    local rss pss fd temp mavail
    rss=$(awk '/VmRSS/{print $2}' "/proc/$pid/status" 2>/dev/null || echo)
    pss=$(awk '/^Pss:/{s+=$2} END{print s+0}' "/proc/$pid/smaps_rollup" 2>/dev/null || echo)
    fd=$(ls "/proc/$pid/fd" 2>/dev/null | wc -l)
    temp=$(max_temp_c)
    mavail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
    echo "$(date +%s.%N),${rss:-},${pss:-},${fd:-},${temp},${mavail}" >> "$out"
    sleep "$SAMPLE_INTERVAL"
  done
}

# ---- 单次 run ----
run_one() {   # $1=variant  $2=workload  $3=rep
  local variant=$1 workload=$2 rep=$3
  local bin="$PROJECT_DIR/build_$variant/myDemo"
  local cfg="$BENCH_CONFIG"
  local uncapped="${WORKLOAD_UNCAPPED[$workload]}"
  [ -x "$bin" ] || die "找不到二进制: $bin (先构建)"
  [ -f "$cfg" ] || die "找不到配置: $cfg"

  local od="$RUN_DIR/${variant}_${workload}_r${rep}"
  mkdir -p "$od"
  local total=$((WARMUP_S + WINDOW_S))

  log "==> $variant / $workload / rep$rep  (温度门限 ${TEMP_MAX_C}C)"
  cooldown
  sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
  echo "start_temp_c=$(max_temp_c)" > "$od/meta.txt"

  # 启动被测程序: 自终止 total 秒, 分阶段 CSV 落到 od/
  BENCH_RUN_SECONDS="$total" BENCH_CSV_DIR="$od" \
    BENCH_UNCAPPED="$uncapped" BENCH_NULL_SINK="$NULL_SINK" \
    "$bin" "$cfg" > "$od/app_stdout.log" 2>&1 &
  local pid=$!
  log "  pid=$pid, 运行 ${total}s (warmup ${WARMUP_S}s + window ${WINDOW_S}s)"

  # 外部采样器 (都在独立进程, 不干扰被测程序)
  sample_proc "$pid" "$od/sys_sample.csv" &
  local sampler=$!
  local pidstat_pid="" perf_pid=""
  if have pidstat; then
    pidstat -h -r -u -p "$pid" "$SAMPLE_INTERVAL" > "$od/pidstat.log" 2>/dev/null &
    pidstat_pid=$!
    pidstat -h -t -p "$pid" "$SAMPLE_INTERVAL" > "$od/pidstat_threads.log" 2>/dev/null &
  fi
  if have perf; then
    # 只统计稳态窗口: 先睡过预热, 再对剩余窗口 perf stat
    ( sleep "$WARMUP_S"; perf stat -p "$pid" -e "$PERF_EVENTS" -- sleep "$WINDOW_S" ) \
      > "$od/perf.txt" 2>&1 &
    perf_pid=$!
  fi

  wait "$pid" 2>/dev/null || true          # 程序自终止
  echo "end_temp_c=$(max_temp_c)" >> "$od/meta.txt"

  # 收尾采样器
  kill "$sampler" "$pidstat_pid" "$perf_pid" 2>/dev/null || true
  wait "$sampler" "$pidstat_pid" "$perf_pid" 2>/dev/null || true

  # 从应用日志提取投递/丢帧
  grep -Eo 'frames_sent[= ]+[0-9]+|frames_dropped[= ]+[0-9]+|copied_bytes[= ]+[0-9]+' \
    "$od/app_stdout.log" | tail -20 >> "$od/meta.txt" || true
  log "  完成 -> $od"
}

# ---- 构建两个变体 ----
build_all() {
  for v in "${VARIANTS[@]}"; do
    local bd="$PROJECT_DIR/build_$v"
    log "构建变体 $v (${VARIANT_FLAGS[$v]})"
    mkdir -p "$bd"
    ( cd "$bd" && cmake ${VARIANT_FLAGS[$v]} .. >/dev/null && make -j"$(nproc)" >/dev/null )
    [ -x "$bd/myDemo" ] || die "变体 $v 构建失败"
  done
}

# ---- 汇总: 对每个 变体×工况 求 CPU/内存 均值±标准差 ----
analyze() {
  local dir="${1:-$RUN_DIR}"
  local summary="$dir/summary.csv"
  echo "variant,workload,cpu_pct_mean,cpu_pct_std,rss_mb_mean,rss_mb_peak,pss_mb_mean,temp_c_peak" > "$summary"
  for variant in "${VARIANTS[@]}"; do
    for workload in "${WORKLOADS[@]}"; do
      # 跨 rep 汇总稳态窗口(跳过前 WARMUP 行)的 sys_sample.csv
      awk -v V="$variant" -v W="$workload" -v warm="$WARMUP_S" '
        FNR==1 { next }                                  # 跳表头
        FNR<=warm+1 { next }                             # 跳预热
        { n++; rss+=$2; rss2+=$2*$2; if($2>rssp)rssp=$2;
          pss+=$3; if($5>tp)tp=$5 }
        END{}
      ' "$dir"/${variant}_${workload}_r*/sys_sample.csv 2>/dev/null \
        | : # (占位, 内存统计在下方 python 更稳)
      # CPU 用 pidstat 的 %CPU 列, 跨 rep 求均值/方差
      python3 - "$dir" "$variant" "$workload" "$WARMUP_S" >> "$summary" <<'PY' || true
import sys,glob,statistics as st
d,variant,workload,warm=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4])
cpu=[]; rss=[]; pss=[]; tp=0
for f in glob.glob(f"{d}/{variant}_{workload}_r*/sys_sample.csv"):
    rows=open(f).read().splitlines()[1+warm:]
    for r in rows:
        c=r.split(',')
        try:
            rss.append(float(c[1])/1024);
            if c[2]: pss.append(float(c[2])/1024)
            tp=max(tp,float(c[4]))
        except: pass
for f in glob.glob(f"{d}/{variant}_{workload}_r*/pidstat.log"):
    for r in open(f):
        p=r.split()
        # pidstat -h -u: 末列附近为 %CPU; 取倒数第2列并容错
        try:
            val=float(p[-2]);  cpu.append(val)
        except: pass
def ms(x): return (st.mean(x), (st.pstdev(x) if len(x)>1 else 0.0)) if x else (0,0)
cm,csd=ms(cpu); rm,_=ms(rss); pm,_=ms(pss)
rp=max(rss) if rss else 0
print(f"{variant},{workload},{cm:.1f},{csd:.1f},{rm:.1f},{rp:.1f},{pm:.1f},{tp:.0f}")
PY
    done
  done
  log "汇总 -> $summary"; echo; cat "$summary"
}

# ======================= 主流程 =======================
main() {
  local do_build=1 only_analyze=0
  for a in "$@"; do
    case "$a" in
      --no-build) do_build=0 ;;
      --analyze)  only_analyze=1 ;;
      *) die "未知参数: $a" ;;
    esac
  done

  [ "$(id -u)" -eq 0 ] || log "警告: 非 root, drop_caches/锁频/perf 可能失败"

  if [ "$only_analyze" -eq 1 ]; then
    RUN_DIR="$(ls -dt "$RESULTS_ROOT"/run_* | head -1)"
    analyze "$RUN_DIR"; exit 0
  fi

  mkdir -p "$RUN_DIR"
  trap 'restore_gov; log "已恢复 governor"' EXIT
  lock_cpu_perf
  [ "$do_build" -eq 1 ] && build_all

  # A/B 交替执行以抵消热漂移: 外层 rep, 中层 workload, 内层变体每轮翻转顺序
  for rep in $(seq 1 "$REPS"); do
    for workload in "${WORKLOADS[@]}"; do
      if [ $((rep % 2)) -eq 0 ]; then order=(copy dma); else order=(dma copy); fi
      for variant in "${order[@]}"; do
        run_one "$variant" "$workload" "$rep"
      done
    done
  done

  analyze "$RUN_DIR"
  log "全部完成. 结果目录: $RUN_DIR"
}
main "$@"
