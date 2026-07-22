/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "bench_probe.hpp"

// 未开启 BENCH 时本翻译单元为空, 不产生任何符号与开销。
#ifdef BENCH

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>
#include <sys/syscall.h>

std::atomic<uint64_t> g_copied_bytes{0};

namespace
{
std::chrono::steady_clock::time_point g_start;
long g_run_seconds = 0;
std::string g_csv_dir;
bool g_uncapped = false;
bool g_null_sink = false;
bool g_inited = false;

long tid_now() { return static_cast<long>(syscall(SYS_gettid)); }

long env_long(const char *key, long defv)
{
    const char *v = getenv(key);
    return v ? strtol(v, nullptr, 10) : defv;
}

// 每线程独立 CSV, 周期性 fflush 兼顾硬 kill; 线程退出(join)时 fclose 冲刷。
struct ThreadCsv
{
    FILE *fp = nullptr;
    int count = 0;
    ThreadCsv()
    {
        if (g_csv_dir.empty())
            return;
        char path[512];
        snprintf(path, sizeof(path), "%s/stage_tid%ld.csv",
                 g_csv_dir.c_str(), tid_now());
        fp = fopen(path, "w");
        if (fp)
            fprintf(fp, "t_start_us,dur_us,tag,stage\n");
    }
    ~ThreadCsv()
    {
        if (fp)
        {
            fflush(fp);
            fclose(fp);
            fp = nullptr;
        }
    }
};
thread_local ThreadCsv t_csv;
} // namespace

void bench_init()
{
    if (g_inited)
        return;
    g_inited = true;
    g_start = std::chrono::steady_clock::now();
    const char *d = getenv("BENCH_CSV_DIR");
    g_csv_dir = d ? d : "";
    g_run_seconds = env_long("BENCH_RUN_SECONDS", 0);
    g_uncapped = env_long("BENCH_UNCAPPED", 0) != 0;
    g_null_sink = env_long("BENCH_NULL_SINK", 0) != 0;
    fprintf(stdout,
            "[bench] init csv_dir=%s run_s=%ld uncapped=%d null_sink=%d\n",
            g_csv_dir.c_str(), g_run_seconds, (int)g_uncapped,
            (int)g_null_sink);
    fflush(stdout);
}

bool bench_should_stop()
{
    if (g_run_seconds <= 0)
        return false;
    const auto el = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - g_start)
                        .count();
    return el >= g_run_seconds;
}

bool bench_uncapped() { return g_uncapped; }
bool bench_null_sink() { return g_null_sink; }

void bench_record(const char *stage, int tag,
                  std::chrono::steady_clock::time_point t0,
                  std::chrono::steady_clock::time_point t1)
{
    if (!t_csv.fp)
        return;
    const auto us0 = std::chrono::duration_cast<std::chrono::microseconds>(
                         t0 - g_start)
                         .count();
    const auto dur = std::chrono::duration_cast<std::chrono::microseconds>(
                         t1 - t0)
                         .count();
    fprintf(t_csv.fp, "%lld,%lld,%d,%s\n",
            (long long)us0, (long long)dur, tag, stage);
    if ((++t_csv.count & 63) == 0)
        fflush(t_csv.fp);
}

void bench_flush()
{
    if (t_csv.fp)
        fflush(t_csv.fp);
    fprintf(stdout, "[bench] copied_bytes=%llu\n",
            (unsigned long long)g_copied_bytes.load());
    fflush(stdout);
}

#endif // BENCH
