/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "realtime_logger.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace
{
std::string parentPath(const std::string &path)
{
    const size_t separator = path.find_last_of('/');
    if (separator == std::string::npos)
        return std::string();
    if (separator == 0)
        return "/";
    return path.substr(0, separator);
}

std::string baseName(const std::string &path)
{
    const size_t separator = path.find_last_of('/');
    return separator == std::string::npos ? path : path.substr(separator + 1);
}
} // namespace

RealtimeLogger::~RealtimeLogger()
{
    stop();
}

bool RealtimeLogger::start()
{
    if (started_)
        return true;

    const std::string log_directory = resolveLogDirectory();
    if (log_directory.empty())
    {
        std::cerr << "Failed to resolve log directory" << std::endl;
        return false;
    }

    struct stat directory_stat = {};
    if (mkdir(log_directory.c_str(), 0755) < 0 && errno != EEXIST)
    {
        std::cerr << "Failed to create log directory " << log_directory
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }
    if (stat(log_directory.c_str(), &directory_stat) < 0 ||
        !S_ISDIR(directory_stat.st_mode))
    {
        std::cerr << "Log path is not a directory: " << log_directory << std::endl;
        return false;
    }

    std::ostringstream file_name;
    file_name << "myDemo_" << makeTimestamp(false)
              << "_" << getpid() << ".log";
    log_path_ = log_directory + "/" + file_name.str();

    log_fd_ = ::open(log_path_.c_str(),
                     O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                     0644);
    if (log_fd_ < 0)
    {
        std::cerr << "Failed to open log file " << log_path_
                  << ": " << std::strerror(errno) << std::endl;
        return false;
    }

    original_stdout_fd_ = dup(STDOUT_FILENO);
    original_stderr_fd_ = dup(STDERR_FILENO);
    if (original_stdout_fd_ < 0 || original_stderr_fd_ < 0)
    {
        std::cerr << "Failed to duplicate stdout/stderr: "
                  << std::strerror(errno) << std::endl;
        closeFd(original_stdout_fd_);
        closeFd(original_stderr_fd_);
        closeFd(log_fd_);
        return false;
    }
    setCloseOnExec(original_stdout_fd_);
    setCloseOnExec(original_stderr_fd_);

    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0)
    {
        const int saved_errno = errno;
        if (stdout_pipe[0] >= 0) ::close(stdout_pipe[0]);
        if (stdout_pipe[1] >= 0) ::close(stdout_pipe[1]);
        if (stderr_pipe[0] >= 0) ::close(stderr_pipe[0]);
        if (stderr_pipe[1] >= 0) ::close(stderr_pipe[1]);
        closeFd(original_stdout_fd_);
        closeFd(original_stderr_fd_);
        closeFd(log_fd_);
        errno = saved_errno;
        std::cerr << "Failed to create logging pipes: "
                  << std::strerror(errno) << std::endl;
        return false;
    }
    setCloseOnExec(stdout_pipe[0]);
    setCloseOnExec(stdout_pipe[1]);
    setCloseOnExec(stderr_pipe[0]);
    setCloseOnExec(stderr_pipe[1]);

    fflush(nullptr);
    if (dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
        dup2(stderr_pipe[1], STDERR_FILENO) < 0)
    {
        const int saved_errno = errno;
        dup2(original_stdout_fd_, STDOUT_FILENO);
        dup2(original_stderr_fd_, STDERR_FILENO);
        ::close(stdout_pipe[0]);
        ::close(stdout_pipe[1]);
        ::close(stderr_pipe[0]);
        ::close(stderr_pipe[1]);
        closeFd(original_stdout_fd_);
        closeFd(original_stderr_fd_);
        closeFd(log_fd_);
        errno = saved_errno;
        std::cerr << "Failed to redirect stdout/stderr: "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    ::close(stdout_pipe[1]);
    ::close(stderr_pipe[1]);
    stdout_pipe_read_fd_ = stdout_pipe[0];
    stderr_pipe_read_fd_ = stderr_pipe[0];

    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    last_sync_time_ = std::chrono::steady_clock::now();
    started_ = true;

    try
    {
        stdout_thread_ = std::thread(&RealtimeLogger::readerLoop, this,
                                     stdout_pipe_read_fd_, original_stdout_fd_, "INFO");
        stderr_thread_ = std::thread(&RealtimeLogger::readerLoop, this,
                                     stderr_pipe_read_fd_, original_stderr_fd_, "ERROR");
    }
    catch (const std::exception &e)
    {
        stop();
        std::cerr << "Failed to start logging threads: " << e.what() << std::endl;
        return false;
    }

    const std::string latest_path = log_directory + "/latest.log";
    unlink(latest_path.c_str());
    if (symlink(baseName(log_path_).c_str(), latest_path.c_str()) < 0)
    {
        std::cerr << "Warning: failed to update " << latest_path
                  << ": " << std::strerror(errno) << std::endl;
    }

    std::cout << "Realtime log file: " << log_path_ << std::endl;
    return true;
}

void RealtimeLogger::stop()
{
    if (!started_)
        return;

    std::cout.flush();
    std::cerr.flush();
    fflush(nullptr);

    dup2(original_stdout_fd_, STDOUT_FILENO);
    dup2(original_stderr_fd_, STDERR_FILENO);

    if (stdout_thread_.joinable())
        stdout_thread_.join();
    if (stderr_thread_.joinable())
        stderr_thread_.join();

    closeFd(stdout_pipe_read_fd_);
    closeFd(stderr_pipe_read_fd_);

    {
        std::lock_guard<std::mutex> lock(log_mutex_);
        if (log_fd_ >= 0)
            fdatasync(log_fd_);
    }

    closeFd(original_stdout_fd_);
    closeFd(original_stderr_fd_);
    closeFd(log_fd_);
    started_ = false;
}

void RealtimeLogger::readerLoop(int read_fd, int console_fd, const char *level)
{
    char buffer[4096];
    std::string pending;

    while (true)
    {
        const ssize_t bytes_read = read(read_fd, buffer, sizeof(buffer));
        if (bytes_read > 0)
        {
            writeAll(console_fd, buffer, static_cast<size_t>(bytes_read));
            pending.append(buffer, static_cast<size_t>(bytes_read));

            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos)
            {
                writeLogLine(level, pending.substr(0, newline));
                pending.erase(0, newline + 1);
            }
            continue;
        }
        if (bytes_read < 0 && errno == EINTR)
            continue;
        break;
    }

    if (!pending.empty())
        writeLogLine(level, pending);
}

void RealtimeLogger::writeLogLine(const char *level, std::string line)
{
    if (!line.empty() && line.back() == '\r')
        line.pop_back();

    std::ostringstream formatted;
    formatted << '[' << makeTimestamp(true) << "] [" << level << "] "
              << line << '\n';
    const std::string output = formatted.str();

    std::lock_guard<std::mutex> lock(log_mutex_);
    if (log_fd_ < 0)
        return;

    writeAll(log_fd_, output.data(), output.size());
    const auto now = std::chrono::steady_clock::now();
    if (now - last_sync_time_ >= std::chrono::seconds(1))
    {
        fdatasync(log_fd_);
        last_sync_time_ = now;
    }
}

std::string RealtimeLogger::resolveLogDirectory()
{
    char executable_path[PATH_MAX] = {};
    const ssize_t length = readlink("/proc/self/exe", executable_path,
                                    sizeof(executable_path) - 1);
    if (length > 0)
    {
        executable_path[length] = '\0';
        const std::string build_directory = parentPath(executable_path);
        const std::string project_directory = parentPath(build_directory);
        if (!project_directory.empty())
            return project_directory + "/log";
    }

    char current_directory[PATH_MAX] = {};
    if (getcwd(current_directory, sizeof(current_directory)))
        return std::string(current_directory) + "/../log";
    return std::string();
}

std::string RealtimeLogger::makeTimestamp(bool include_milliseconds)
{
    const auto now = std::chrono::system_clock::now();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    struct tm local_time = {};
    localtime_r(&now_time, &local_time);

    std::ostringstream timestamp;
    if (include_milliseconds)
    {
        timestamp << std::put_time(&local_time, "%Y-%m-%d %H:%M:%S")
                  << '.' << std::setfill('0') << std::setw(3)
                  << milliseconds.count();
    }
    else
    {
        timestamp << std::put_time(&local_time, "%Y%m%d_%H%M%S");
    }
    return timestamp.str();
}

bool RealtimeLogger::writeAll(int fd, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size)
    {
        const ssize_t ret = write(fd, data + written, size - written);
        if (ret > 0)
        {
            written += static_cast<size_t>(ret);
            continue;
        }
        if (ret < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

void RealtimeLogger::setCloseOnExec(int fd)
{
    if (fd < 0)
        return;
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

void RealtimeLogger::closeFd(int &fd)
{
    if (fd >= 0)
    {
        ::close(fd);
        fd = -1;
    }
}
