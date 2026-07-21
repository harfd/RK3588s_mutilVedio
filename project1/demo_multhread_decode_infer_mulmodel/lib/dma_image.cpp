/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "dma_image.hpp"

#include "dma_alloc.hpp"
#include "im2d.h"

#include <iostream>
#include <utility>

DmaImageBuffer::DmaImageBuffer(int width, int height, int width_stride,
                               int height_stride, int format, size_t size)
    : size_(size), width_(width), height_(height),
      width_stride_(width_stride), height_stride_(height_stride),
      format_(format)
{
}

DmaImageBuffer::Ptr DmaImageBuffer::create(
    int width, int height, int width_stride, int height_stride,
    int format, size_t size, const std::string &heap_path)
{
    if (width <= 0 || height <= 0 || width_stride < width ||
        height_stride < height || size == 0)
        return nullptr;

    Ptr buffer(new DmaImageBuffer(width, height, width_stride,
                                  height_stride, format, size));
    const char *heap = heap_path.empty() ? DMA_HEAP_PATH : heap_path.c_str();
    if (dma_buf_alloc(heap, size, &buffer->fd_, &buffer->va_) < 0 ||
        buffer->fd_ < 0 || buffer->va_ == nullptr)
    {
        std::cerr << "Failed to allocate DMA image from " << heap << std::endl;
        return nullptr;
    }

    buffer->rga_handle_ = importbuffer_fd(
        buffer->fd_, static_cast<int>(buffer->size_));
    if (buffer->rga_handle_ == 0)
    {
        std::cerr << "Failed to import DMA image fd " << buffer->fd_
                  << " into RGA" << std::endl;
        return nullptr;
    }
    return buffer;
}

DmaImageBuffer::Ptr DmaImageBuffer::createBgr(
    int width, int height, const std::string &heap_path)
{
    const size_t size = static_cast<size_t>(width) * height * 3;
    return create(width, height, width, height, RK_FORMAT_BGR_888,
                  size, heap_path);
}

DmaImageBuffer::Ptr DmaImageBuffer::createNv12(
    int width, int height, const std::string &heap_path)
{
    const size_t size = static_cast<size_t>(width) * height * 3 / 2;
    return create(width, height, width, height, RK_FORMAT_YCbCr_420_SP,
                  size, heap_path);
}

DmaImageBuffer::~DmaImageBuffer()
{
    syncForDevice();
    if (rga_handle_ > 0)
    {
        releasebuffer_handle(rga_handle_);
        rga_handle_ = 0;
    }
    if (fd_ >= 0 && va_ != nullptr)
        dma_buf_free(size_, &fd_, va_);
    va_ = nullptr;
    size_ = 0;
}

bool DmaImageBuffer::valid() const
{
    return fd_ >= 0 && va_ != nullptr && rga_handle_ > 0;
}

bool DmaImageBuffer::syncForCpu()
{
    std::lock_guard<std::mutex> lock(sync_mutex_);
    if (cpu_access_active_)
        return true;
    if (dma_sync_device_to_cpu(fd_) < 0)
        return false;
    cpu_access_active_ = true;
    return true;
}

bool DmaImageBuffer::syncForDevice()
{
    std::lock_guard<std::mutex> lock(sync_mutex_);
    if (!cpu_access_active_)
        return true;
    if (dma_sync_cpu_to_device(fd_) < 0)
        return false;
    cpu_access_active_ = false;
    return true;
}

cv::Mat DmaImageBuffer::bgrView() const
{
    return cv::Mat(height_, width_, CV_8UC3, va_,
                   static_cast<size_t>(width_stride_) * 3);
}

bool DmaImagePool::configure(size_t count, int width, int height,
                             int width_stride, int height_stride,
                             int format, size_t buffer_size,
                             const std::string &heap_path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!buffers_.empty() && buffers_.size() == count &&
        width_ == width && height_ == height &&
        width_stride_ == width_stride &&
        height_stride_ == height_stride && format_ == format &&
        buffer_size_ == buffer_size && heap_path_ == heap_path)
        return true;

    std::vector<DmaImageBuffer::Ptr> new_buffers;
    new_buffers.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        auto buffer = DmaImageBuffer::create(
            width, height, width_stride, height_stride,
            format, buffer_size, heap_path);
        if (!buffer)
            return false;
        new_buffers.push_back(std::move(buffer));
    }

    buffers_ = std::move(new_buffers);
    width_ = width;
    height_ = height;
    width_stride_ = width_stride;
    height_stride_ = height_stride;
    format_ = format;
    buffer_size_ = buffer_size;
    heap_path_ = heap_path;
    return true;
}

DmaImageBuffer::Ptr DmaImagePool::acquire()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &buffer : buffers_)
    {
        if (buffer.use_count() == 1)
        {
            if (!buffer->syncForDevice())
                return nullptr;
            return buffer;
        }
    }
    return nullptr;
}

void DmaImagePool::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    buffers_.clear();
    width_ = 0;
    height_ = 0;
    width_stride_ = 0;
    height_stride_ = 0;
    format_ = 0;
    buffer_size_ = 0;
    heap_path_.clear();
}
