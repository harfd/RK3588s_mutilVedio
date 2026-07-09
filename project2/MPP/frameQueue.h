#ifndef _FRAMEQUEUE_
#define _FRAMEQUEUE_
#include <iostream>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

template <typename T>
class frameQueue
{
public:
    explicit frameQueue(int max_data_size) : max_size(max_data_size), shutdown_(false) {}

    bool push(T &&temp)
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        cond_not_full_.wait(lock, [this]
                            { return queue_.size() < max_size || shutdown_; });

        if (shutdown_)
            return false;

        queue_.emplace(std::move(temp));
        cond_not_empty_.notify_one();
        return true;
    }

    bool wait_and_pop(T &temp)
    {
        std::unique_lock<std::mutex> lock(this->mtx);
        cond_not_empty_.wait(lock, [this]
                             { return !queue_.empty() || shutdown_; });

        if (queue_.empty())
            return false;

        temp = std::move(queue_.front());
        queue_.pop();
        cond_not_full_.notify_one();
        return true;
    }

    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mtx); // 修正：使用mtx而不是mutex_
        shutdown_ = true;
        cond_not_empty_.notify_all();
        cond_not_full_.notify_all();
    }

    size_t size() const // 修正：返回size_t
    {
        std::lock_guard<std::mutex> lock(mtx);
        return queue_.size();
    }

    bool empty() const
    {
        std::lock_guard<std::mutex> lock(mtx);
        return queue_.empty();
    }

private:
    mutable std::mutex mtx; // 添加mutable以支持const成员函数
    std::condition_variable cond_not_empty_;
    std::condition_variable cond_not_full_;
    size_t max_size;
    bool shutdown_;
    std::queue<T> queue_;
};
#endif // _FRAMEQUEUE_