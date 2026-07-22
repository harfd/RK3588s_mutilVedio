/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

// 基准测试埋点。未定义 BENCH 时全部退化为空操作, 生产构建零开销。
// 运行期通过环境变量控制:
//   BENCH_RUN_SECONDS=N  运行 N 秒后 combineImage 优雅退出并 flush
//   BENCH_CSV_DIR=path   每线程分阶段计时 CSV 输出目录
//   BENCH_UNCAPPED=1     关闭合成限速与本地文件 throttle (测吞吐天花板)
//   BENCH_NULL_SINK=1    编码照跑但跳过网络发送 (无需 RTMP 服务器, 去网络噪声)
#pragma once

#include <cstdint>
#include <chrono>

#ifdef BENCH

#include <atomic>

extern std::atomic<uint64_t> g_copied_bytes;   // 深拷贝路径累计拷贝字节数

void bench_init();                              // main 启动时调用一次
bool bench_should_stop();                       // BENCH_RUN_SECONDS 到点
bool bench_uncapped();                          // BENCH_UNCAPPED
bool bench_null_sink();                         // BENCH_NULL_SINK
void bench_flush();                             // 退出前打印聚合并冲刷
void bench_record(const char *stage, int tag,
                  std::chrono::steady_clock::time_point t0,
                  std::chrono::steady_clock::time_point t1);

struct BenchScope
{
    const char *stage_;
    int tag_;
    std::chrono::steady_clock::time_point t0_;
    BenchScope(const char *stage, int tag)
        : stage_(stage), tag_(tag),
          t0_(std::chrono::steady_clock::now()) {}
    ~BenchScope() { bench_record(stage_, tag_, t0_,
                                 std::chrono::steady_clock::now()); }
};

#define BENCH_CAT2(a, b) a##b
#define BENCH_CAT(a, b) BENCH_CAT2(a, b)
// 在当前作用域放置一个计时器, 析构时记录 [进入,离开] 区间
#define BENCH_SCOPE(stage, tag) \
    BenchScope BENCH_CAT(bench_scope_, __LINE__)((stage), (tag))
#define BENCH_ADD_COPY(bytes) \
    g_copied_bytes.fetch_add((uint64_t)(bytes), std::memory_order_relaxed)

#else // !BENCH

static inline void bench_init() {}
static inline bool bench_should_stop() { return false; }
static inline bool bench_uncapped() { return false; }
static inline bool bench_null_sink() { return false; }
static inline void bench_flush() {}
#define BENCH_SCOPE(stage, tag) ((void)0)
#define BENCH_ADD_COPY(bytes) ((void)0)

#endif // BENCH

#ifdef TRANSFER_MODE_COPY
#include <vector>
#include <cstring>
// 深拷贝交接模型: 把 n 字节整帧复制到线程本地堆暂存, 复现非零拷贝交接的
// CPU/DDR 拷贝开销 (与深拷贝管线每级克隆一份整帧等价), 并计入 g_copied_bytes。
inline void copy_clone(const void *src, size_t n)
{
    static thread_local std::vector<uint8_t> staging;
    if (staging.size() < n)
        staging.resize(n);
    std::memcpy(staging.data(), src, n);
    BENCH_ADD_COPY(n);
}
#endif
