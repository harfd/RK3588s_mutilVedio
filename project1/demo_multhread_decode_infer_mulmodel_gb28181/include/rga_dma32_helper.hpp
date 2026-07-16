#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "im2d.h"
#include "dma_alloc.h"

class RgaDma32Buffer {
public:
    RgaDma32Buffer() = default;
    ~RgaDma32Buffer() { release(); }

    bool ensure(size_t bytes) {
        if (bytes == 0) return false;
        if (va_ && size_ >= bytes && handle_ > 0) return true;
        release();

        int ret = dma_buf_alloc(DMA_HEAP_DMA32_UNCACHED_PATH, bytes, &fd_, &va_);
        if (ret < 0 || fd_ < 0 || va_ == nullptr) {
            fd_ = -1;
            va_ = nullptr;
            size_ = 0;
            return false;
        }

        handle_ = importbuffer_fd(fd_, static_cast<int>(bytes));
        if (handle_ == 0) {
            dma_buf_free(bytes, &fd_, va_);
            va_ = nullptr;
            size_ = 0;
            return false;
        }

        size_ = bytes;
        return true;
    }

    void release() {
        if (handle_ > 0) {
            releasebuffer_handle(handle_);
            handle_ = 0;
        }
        if (va_ != nullptr && fd_ >= 0) {
            dma_buf_free(size_, &fd_, va_);
        }
        va_ = nullptr;
        fd_ = -1;
        size_ = 0;
    }

    void *data() { return va_; }
    const void *data() const { return va_; }
    int fd() const { return fd_; }
    size_t size() const { return size_; }
    rga_buffer_handle_t handle() const { return handle_; }

private:
    int fd_ = -1;
    void *va_ = nullptr;
    size_t size_ = 0;
    rga_buffer_handle_t handle_ = 0;
};
