#ifndef __MEMORY_POOL_HPP__
#define __MEMORY_POOL_HPP__

#include <mutex>
#include <queue>
#include <vector>

class MemoryPool {
public:
    MemoryPool(size_t block_size, size_t block_count) 
        : block_size_(block_size), block_count_(block_count) {
        // 预分配内存
        buffer_ = new uint8_t[block_size * block_count];
        // 初始化空闲块列表
        for (size_t i = 0; i < block_count; i++) {
            free_blocks_.push(buffer_ + i * block_size);
        }
    }
    
    ~MemoryPool() {
        delete[] buffer_;
    }
    
    void* allocate() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_blocks_.empty()) {
            return nullptr; // 或扩容
        }
        void* block = free_blocks_.front();
        free_blocks_.pop();
        return block;
    }
    
    void deallocate(void* block) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_blocks_.push(block);
    }

private:
    size_t block_size_;
    size_t block_count_;
    uint8_t* buffer_;
    std::queue<void*> free_blocks_;
    std::mutex mutex_;
};

#endif /* __MEMORY_POOL_HPP__ */