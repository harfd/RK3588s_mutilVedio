/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef PROJECT2_FRAME_QUEUE_H
#define PROJECT2_FRAME_QUEUE_H

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class FrameQueue
{
public:
    explicit FrameQueue(int max_data_size) : max_size(max_data_size), shutdown_(false) {}

    bool push(T &&temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)
            return false;
        queue_.emplace(std::move(temp));
        cond_not_empty_.notify_one();
        return true;
    }
    bool push(const T &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_not_full_.wait(lock, [this] { return queue_.size() < max_size || shutdown_; });
        if (shutdown_)
            return false;
        queue_.emplace(temp);
        cond_not_empty_.notify_one();
        return true;
    }
    bool wait_and_pop(T &temp)
    {
        std::unique_lock<std::mutex> lock(mtx_);
        cond_not_empty_.wait(lock, [this] { return !queue_.empty() || shutdown_; });
        if (shutdown_ && queue_.empty())
            return false;
        // 对于 cv::Mat，使用拷贝而不是 move，避免 Mat 内部结构损坏
        temp = queue_.front();
        queue_.pop();
        cond_not_full_.notify_one();
        return true;
    }
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        shutdown_ = true;
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }
    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }
    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::queue<T> tmp;
        std::swap(queue_, tmp);
        cond_not_full_.notify_all();
    }
    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.empty();
    }
    void reset(int new_max_size)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        max_size = new_max_size;
        queue_ = std::queue<T>();
        shutdown_ = false;
        cond_not_empty_.notify_all();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
    size_t max_size;
    bool shutdown_;
    std::queue<T> queue_;
};

#endif /* PROJECT2_FRAME_QUEUE_H */
