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
# 从脚本自身位置推导项目目录(bench 的上一级), 避免 sudo 下 $HOME=/root 及目录名差异问题。
SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PROJECT_DIR="${PROJECT_DIR:-$(dirname "$SCRIPT_DIR")}"
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
  python3 - "$dir" "$WARMUP_S" "$WINDOW_S" "$summary" <<'PY' || true
import sys, glob, os, re, statistics as st, collections
d, warm, window, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]

def pidstat(path):
    # pidstat -h: $1时间 $2UID $3PID $4%usr $5%system $6%guest $7%wait $8%CPU
    u=[]; s=[]; c=[]; n=0
    try: f=open(path, encoding="utf-8", errors="replace")
    except OSError: return u,s,c
    for line in f:
        line=line.strip()
        if not line or line[0]=='#' or line.startswith("Linux"): continue
        p=line.split()
        if len(p)<8: continue
        try: uu=float(p[3]); ss=float(p[4]); cc=float(p[7])
        except ValueError: continue
        n+=1
        if n<=warm: continue           # 跳预热(约 1 行/秒)
        u.append(uu); s.append(ss); c.append(cc)
    return u,s,c

def sysmem(path):
    rss=[]; pss=[]; tp=0.0
    try: rows=open(path,encoding="utf-8",errors="replace").read().splitlines()[1:]
    except OSError: return rss,pss,tp
    for i,r in enumerate(rows):
        cc=r.split(",")
        if len(cc)<5 or not cc[1]: continue
        if i<warm: continue
        try:
            rss.append(float(cc[1])/1024)
            if cc[2]: pss.append(float(cc[2])/1024)
            tp=max(tp,float(cc[4]))
        except ValueError: pass
    return rss,pss,tp

def copied(path):
    try:
        for line in open(path,encoding="utf-8",errors="replace"):
            if "copied_bytes=" in line:
                return int(line.split("copied_bytes=")[1].split()[0])
    except OSError: pass
    return 0

def stages(rundir):
    durs=collections.defaultdict(list); warm_us=warm*1_000_000
    for f in glob.glob(os.path.join(rundir,"stage_tid*.csv")):
        for line in open(f,encoding="utf-8",errors="replace"):
            cc=line.split(",")
            if len(cc)!=4 or cc[0]=="t_start_us": continue
            try: t0=int(cc[0]); dd=int(cc[1]); sg=cc[3].strip()
            except ValueError: continue
            if t0<warm_us: continue
            durs[sg].append(dd)
    return durs

runs=collections.defaultdict(list)
for rd in sorted(glob.glob(os.path.join(d,"*_r*"))):
    if not os.path.isdir(rd): continue
    m=re.match(r'(.+)_(capped|uncapped)_r\d+$', os.path.basename(rd))
    if m: runs[(m.group(1),m.group(2))].append(rd)

STAGES=["decode_cvt","rknn_pre","rknn_set","rknn_run","composite","enc_cvt","encode",
        "clone_t1","clone_t5","clone_t6"]
mean=lambda x: st.mean(x) if x else 0.0
pstd=lambda x: st.pstdev(x) if len(x)>1 else 0.0

rows=[]
for key in sorted(runs):
    v,w=key
    cpu=[];usr=[];sysc=[];rssL=[];pssL=[];rpk=[];cpd=[];fps=[]
    stg=collections.defaultdict(list)
    for rd in runs[key]:
        u,s,c=pidstat(os.path.join(rd,"pidstat.log"))
        if c: cpu.append(mean(c)); usr.append(mean(u)); sysc.append(mean(s))
        rss,pss,tp=sysmem(os.path.join(rd,"sys_sample.csv"))
        if rss: rssL.append(mean(rss)); pssL.append(mean(pss)); rpk.append(max(rss))
        cpd.append(copied(os.path.join(rd,"app_stdout.log")))
        du=stages(rd); fps.append(len(du.get("composite",[]))/window)
        for sg,xs in du.items():
            if xs: stg[sg].append(mean(xs))
    r={"variant":v,"workload":w,"cpu":mean(cpu),"cpu_sd":pstd(cpu),"usr":mean(usr),
       "sys":mean(sysc),"rss":mean(rssL),"pss":mean(pssL),"rss_pk":mean(rpk),
       "copied_gb":mean([x/1e9 for x in cpd]),"fps":mean(fps)}
    for sg in STAGES: r[sg]=mean(stg[sg]) if sg in stg else 0.0
    rows.append(r)

cols=["variant","workload","cpu","cpu_sd","usr","sys","rss","pss","rss_pk","copied_gb","fps"]+STAGES
with open(out,"w",encoding="utf-8") as f:
    f.write(",".join(cols)+"\n")
    for r in rows:
        f.write(",".join(f"{r[c]:.2f}" if isinstance(r[c],float) else str(r[c]) for c in cols)+"\n")

print(f"{'variant/workload':<18}{'CPU%':>8}{'usr%':>7}{'sys%':>7}{'RSS':>8}{'PSS':>8}{'copGB':>8}{'fps':>8}")
for r in rows:
    print(f"{r['variant']+'/'+r['workload']:<18}{r['cpu']:>8.1f}{r['usr']:>7.1f}{r['sys']:>7.1f}"
          f"{r['rss']:>8.0f}{r['pss']:>8.0f}{r['copied_gb']:>8.1f}{r['fps']:>8.1f}")
print()
print(f"{'variant/workload':<18}"+"".join(f"{s[:9]:>10}" for s in STAGES)+"   (us)")
for r in rows:
    print(f"{r['variant']+'/'+r['workload']:<18}"+"".join(f"{r[s]:>10.0f}" for s in STAGES))
print(f"\nsummary.csv -> {out}")
PY
  log "汇总 -> $summary"
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
