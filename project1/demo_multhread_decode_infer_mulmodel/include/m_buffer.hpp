/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once

#include "dma_image.hpp"

#include <opencv2/opencv.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct Mbuffer{
    std::shared_ptr<DmaImageBuffer> dma_frame;
    std::unique_ptr<DmaImagePool> dma_pool;
    uint64_t sequence = 0;
    std::mutex mtx;
    // 本地文件限速：每输出一帧后 sleep，避免倍速（解码可能一包多帧）
    int frame_interval_ms = 0;  // 0 = 不限速
    bool throttle = false;
    // 预分配缓冲区，避免每帧 malloc
    std::vector<uint8_t> yuv_work;
};
