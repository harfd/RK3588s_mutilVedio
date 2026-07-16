#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sys/mman.h>
#include <unistd.h>

class RgaLowMemBuffer {
public:
    RgaLowMemBuffer() = default;
    ~RgaLowMemBuffer() { release(); }

    bool ensure(size_t bytes) {
        if (bytes == 0) return false;
        if (ptr_ && capacity_ >= bytes) return true;
        release();

#if defined(MAP_32BIT)
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        if (p != MAP_FAILED) {
            ptr_ = p;
            capacity_ = bytes;
            use_mmap_ = true;
            return true;
        }
#endif
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            ptr_ = nullptr;
            capacity_ = 0;
            use_mmap_ = false;
            return false;
        }

        ptr_ = p;
        capacity_ = bytes;
        use_mmap_ = true;
        return true;
    }

    void* data() { return ptr_; }
    const void* data() const { return ptr_; }
    size_t size() const { return capacity_; }

private:
    void release() {
        if (ptr_ && use_mmap_) {
            munmap(ptr_, capacity_);
        }
        ptr_ = nullptr;
        capacity_ = 0;
        use_mmap_ = false;
    }

private:
    void* ptr_ = nullptr;
    size_t capacity_ = 0;
    bool use_mmap_ = false;
};
