/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

class DmaImageBuffer
{
public:
    using Ptr = std::shared_ptr<DmaImageBuffer>;

    static Ptr create(int width, int height, int width_stride,
                      int height_stride, int format, size_t size,
                      const std::string &heap_path = std::string());
    static Ptr createBgr(int width, int height,
                         const std::string &heap_path = std::string());
    static Ptr createNv12(int width, int height,
                          const std::string &heap_path = std::string());

    ~DmaImageBuffer();

    DmaImageBuffer(const DmaImageBuffer &) = delete;
    DmaImageBuffer &operator=(const DmaImageBuffer &) = delete;

    bool valid() const;
    bool syncForCpu();
    bool syncForDevice();
    cv::Mat bgrView() const;

    int fd() const { return fd_; }
    void *data() const { return va_; }
    uint32_t rgaHandle() const { return rga_handle_; }
    size_t size() const { return size_; }
    int width() const { return width_; }
    int height() const { return height_; }
    int widthStride() const { return width_stride_; }
    int heightStride() const { return height_stride_; }
    int format() const { return format_; }

private:
    DmaImageBuffer(int width, int height, int width_stride,
                   int height_stride, int format, size_t size);

    int fd_ = -1;
    void *va_ = nullptr;
    uint32_t rga_handle_ = 0;
    size_t size_ = 0;
    int width_ = 0;
    int height_ = 0;
    int width_stride_ = 0;
    int height_stride_ = 0;
    int format_ = 0;
    bool cpu_access_active_ = false;
    std::mutex sync_mutex_;
};

class DmaImagePool
{
public:
    bool configure(size_t count, int width, int height,
                   int width_stride, int height_stride,
                   int format, size_t buffer_size,
                   const std::string &heap_path = std::string());
    DmaImageBuffer::Ptr acquire();
    void clear();

private:
    std::mutex mutex_;
    std::vector<DmaImageBuffer::Ptr> buffers_;
    int width_ = 0;
    int height_ = 0;
    int width_stride_ = 0;
    int height_stride_ = 0;
    int format_ = 0;
    size_t buffer_size_ = 0;
    std::string heap_path_;
};
