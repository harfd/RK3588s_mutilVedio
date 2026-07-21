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

    int open(const std::string &device_path, int requested_width = 0,
             int requested_height = 0, int requested_fps = 0,
             bool use_nv21 = false, bool auto_white_balance = false);
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
        uint32_t rga_handle = 0;
    };

    static constexpr int kDefaultRequestedWidth = 4224;
    static constexpr int kDefaultRequestedHeight = 3136;
    static constexpr int kOutputWidth = 1280;
    static constexpr int kOutputHeight = 960;
    static constexpr uint32_t kCaptureBufferCount = 4;

    int configureDevice();
    int allocateBuffers();
    int queueBuffer(uint32_t index);
    bool processBuffer(uint32_t index, Mbuffer &output);
    void applyAutoWhiteBalance(cv::Mat &image);
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
    int requested_width_ = kDefaultRequestedWidth;
    int requested_height_ = kDefaultRequestedHeight;
    int requested_fps_ = 0;
    bool use_nv21_ = false;
    bool full_range_ = false;
    bool auto_white_balance_ = false;
    cv::Vec3f white_balance_gains_ = cv::Vec3f(1.0f, 1.0f, 1.0f);

    std::vector<DmaBuffer> capture_buffers_;
    DmaBuffer scaled_nv12_;
    uint64_t captured_frame_count_ = 0;
    uint32_t poll_timeout_count_ = 0;
};
