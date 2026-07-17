/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>

class RealtimeLogger
{
public:
    RealtimeLogger() = default;
    ~RealtimeLogger();

    RealtimeLogger(const RealtimeLogger &) = delete;
    RealtimeLogger &operator=(const RealtimeLogger &) = delete;

    bool start();
    void stop();

    bool isStarted() const { return started_; }
    const std::string &logPath() const { return log_path_; }

private:
    static std::string resolveLogDirectory();
    static std::string makeTimestamp(bool include_milliseconds);
    static bool writeAll(int fd, const char *data, size_t size);
    static void setCloseOnExec(int fd);

    void readerLoop(int read_fd, int console_fd, const char *level);
    void writeLogLine(const char *level, std::string line);
    void closeFd(int &fd);

    int log_fd_ = -1;
    int original_stdout_fd_ = -1;
    int original_stderr_fd_ = -1;
    int stdout_pipe_read_fd_ = -1;
    int stderr_pipe_read_fd_ = -1;

    std::thread stdout_thread_;
    std::thread stderr_thread_;
    std::mutex log_mutex_;
    std::chrono::steady_clock::time_point last_sync_time_;

    std::string log_path_;
    bool started_ = false;
};
