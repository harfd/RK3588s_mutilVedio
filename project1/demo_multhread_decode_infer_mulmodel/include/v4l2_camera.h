/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once

#include "m_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class V4L2Camera
{
public:
    V4L2Camera() = default;
    ~V4L2Camera();

    V4L2Camera(const V4L2Camera &) = delete;
    V4L2Camera &operator=(const V4L2Camera &) = delete;

    int open(const std::string &device_path);
    bool captureFrame(Mbuffer &output, const std::atomic<bool> &stop_flag);
    void close();

    bool isOpen() const { return camera_fd_ >= 0 && streaming_; }
    int sourceWidth() const { return source_width_; }
    int sourceHeight() const { return source_height_; }

private:
    struct DmaBuffer
    {
        int fd = -1;
        void *va = nullptr;
        size_t size = 0;
    };

    static constexpr int kRequestedWidth = 4224;
    static constexpr int kRequestedHeight = 3136;
    static constexpr int kOutputWidth = 1280;
    static constexpr int kOutputHeight = 960;
    static constexpr uint32_t kCaptureBufferCount = 4;

    int configureDevice();
    int allocateBuffers();
    int queueBuffer(uint32_t index);
    bool processBuffer(uint32_t index, Mbuffer &output);
    void releaseBuffer(DmaBuffer &buffer);

    int camera_fd_ = -1;
    std::string device_path_;
    uint32_t buffer_type_ = 0;
    uint32_t plane_count_ = 1;
    bool is_multiplanar_ = false;
    bool streaming_ = false;

    int source_width_ = 0;
    int source_height_ = 0;
    int source_stride_ = 0;
    size_t source_size_ = 0;

    std::vector<DmaBuffer> capture_buffers_;
    DmaBuffer scaled_nv12_;
    DmaBuffer output_bgr_;
    cv::Mat output_bgr_view_;
    bool output_cpu_access_active_ = false;
};
