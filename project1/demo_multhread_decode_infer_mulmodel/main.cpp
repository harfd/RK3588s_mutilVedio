
/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <cstring>
#include <chrono>
#include <thread>
#include "stream_loader.h"
#include "rknnPool.hpp"
#include "streaming_manager.h"
#include "detection_fusion_manager.h"
#include "realtime_logger.h"
#include "im2d.h"
#include "include/dma_alloc.hpp"

const char *model_person = "../../model/person_relu.rknn";
const char *model_helmet = "../../model/helmet_relu.rknn";
//char *model_tired = "../../model/tired_relu.rknn";
const char *model_callplay = "../../model/callplay_relu.rknn";
StreamLoaderManager &manager = StreamLoaderManager::getInstance();
// 创建RKNN模型的集合，用于存储多个模型实例
vector<rknn_lite *> rk_pool;
// 创建线程池对象，使用n个线程
vector<std::thread> rk_threads;
// 用于存储显示图像的Mat
vector<cv::Mat> images(6);
// 管理images的互斥锁
vector<std::mutex> mutexes(6);

// 推流管理器
StreamingManager streaming_manager;

// 检测融合管理器
DetectionFusionManager fusion_manager;

namespace
{
constexpr int kCameraStreamId = 4;
constexpr int kCompositeWidth = 1280;
constexpr int kCompositeHeight = 1088;
constexpr int kTileWidth = 640;
constexpr int kTileHeight = 360;
constexpr int kTileTopPadding = (kCompositeHeight - 3 * kTileHeight) / 2;
constexpr int kLayoutStreamCount = 5;

struct StreamLayout
{
    int x;
    int y;
    int width;
    int height;
};

StreamLayout getStreamLayout(int stream_id)
{
    switch (stream_id)
    {
    case 0:
        return {kTileWidth, kTileTopPadding, kTileWidth, kTileHeight};
    case 1:
        return {kTileWidth, kTileTopPadding + kTileHeight, kTileWidth, kTileHeight};
    case 2:
        return {0, kTileTopPadding + 2 * kTileHeight, kTileWidth, kTileHeight};
    case 3:
        return {kTileWidth, kTileTopPadding + 2 * kTileHeight, kTileWidth, kTileHeight};
    case kCameraStreamId:
        return {0, kTileTopPadding, kTileWidth, 2 * kTileHeight};
    default:
        return {0, 0, 0, 0};
    }
}

cv::Mat centerCropToAspect(const cv::Mat &image, int target_width, int target_height)
{
    if (image.empty() || target_width <= 0 || target_height <= 0)
        return image;

    const int64_t source_aspect = static_cast<int64_t>(image.cols) * target_height;
    const int64_t target_aspect = static_cast<int64_t>(image.rows) * target_width;
    if (source_aspect == target_aspect)
        return image;

    int crop_width = image.cols;
    int crop_height = image.rows;
    if (source_aspect > target_aspect)
        crop_width = static_cast<int>(static_cast<int64_t>(image.rows) * target_width / target_height);
    else
        crop_height = static_cast<int>(static_cast<int64_t>(image.cols) * target_height / target_width);

    crop_width = std::max(2, crop_width & ~1);
    crop_height = std::max(2, crop_height & ~1);
    const int x = (image.cols - crop_width) / 2;
    const int y = (image.rows - crop_height) / 2;
    return image(cv::Rect(x, y, crop_width, crop_height));
}

void sharpenCameraTile(cv::Mat &image)
{
    if (image.empty())
        return;

    cv::Mat blurred;
    cv::GaussianBlur(image, blurred, cv::Size(0, 0), 0.8, 0.8, cv::BORDER_REPLICATE);
    cv::addWeighted(image, 1.2, blurred, -0.2, 0.0, image);
}
} // namespace

void combineImage(StreamLoaderManager &manager)
{
    cv::Mat combinedImage(kCompositeHeight, kCompositeWidth,
                          CV_8UC3, cv::Scalar(0, 0, 0)); // 初始化为黑色
    cv::Mat lastCombinedImage; // 保存最后一帧
    bool hasLastFrame = false;
    const int target_fps = 24; // 目标帧率
    const int frame_interval_ms = 1000 / target_fps; // 每帧间隔（毫秒）
    struct DmaTileBuffer {
        int fd = -1;
        void *va = nullptr;
        int width = 0;
        int height = 0;
        cv::Mat view;
    };
    std::vector<DmaTileBuffer> tile_buffers(manager.num_stream);
    for (int i = 0; i < manager.num_stream; ++i) {
        const StreamLayout layout = getStreamLayout(i);
        tile_buffers[i].width = layout.width;
        tile_buffers[i].height = layout.height;
        const size_t tile_size = static_cast<size_t>(layout.width) * layout.height * 3;
        if (dma_buf_alloc(DMA_HEAP_PATH, tile_size, &tile_buffers[i].fd, &tile_buffers[i].va) == 0) {
            tile_buffers[i].view = cv::Mat(layout.height, layout.width,
                                           CV_8UC3, tile_buffers[i].va);
        }
    }
    auto last_frame_time = std::chrono::steady_clock::now();
    
    while (true)
    {
        auto current_time = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_frame_time).count();
        
        if (elapsed < frame_interval_ms) {
            std::this_thread::sleep_for(std::chrono::milliseconds(frame_interval_ms - elapsed));
            current_time = std::chrono::steady_clock::now();
        }
        last_frame_time = current_time;
        
        bool hasNewFrame = false;

        for (int i = 0; i < manager.num_stream; ++i)
        {
            cv::Mat local_img;
            {
                std::lock_guard<std::mutex> lock(mutexes[i]);
                if (images[i].empty())
                    continue;
                local_img = std::move(images[i]);
                images[i] = cv::Mat();
            }

            const StreamLayout layout = getStreamLayout(i);
            cv::Mat resize_source = local_img;
            if (i == kCameraStreamId)
                resize_source = centerCropToAspect(local_img, layout.width, layout.height);

            cv::Mat resizedImage;
            int src_w = resize_source.cols, src_h = resize_source.rows;
            bool tile_cpu_access_active = false;

            if (tile_buffers[i].va == nullptr) {
                cv::resize(resize_source, resizedImage,
                           cv::Size(layout.width, layout.height),
                           0.0, 0.0, cv::INTER_AREA);
            } else {
                const int src_stride_pixels = static_cast<int>(resize_source.step / resize_source.elemSize());
                rga_buffer_t src_buf = wrapbuffer_virtualaddr_t(
                    resize_source.data, src_w, src_h,
                    src_stride_pixels, src_h, RK_FORMAT_BGR_888);
                rga_buffer_t dst_buf = wrapbuffer_virtualaddr(
                    tile_buffers[i].va, layout.width, layout.height,
                    RK_FORMAT_BGR_888);
                IM_STATUS status = imresize(src_buf, dst_buf);
                if (status == IM_STATUS_SUCCESS &&
                    dma_sync_device_to_cpu(tile_buffers[i].fd) == 0) {
                    resizedImage = tile_buffers[i].view;
                    tile_cpu_access_active = true;
                } else {
                    cv::resize(resize_source, resizedImage,
                               cv::Size(layout.width, layout.height),
                               0.0, 0.0, cv::INTER_AREA);
                }
            }

            if (i == kCameraStreamId)
                sharpenCameraTile(resizedImage);

            resizedImage.copyTo(combinedImage(cv::Rect(
                layout.x, layout.y, layout.width, layout.height)));
            if (tile_cpu_access_active)
                dma_sync_cpu_to_device(tile_buffers[i].fd);
            hasNewFrame = true;
        }

        cv::Mat frameToSend;
        if (hasNewFrame) {
            lastCombinedImage = combinedImage.clone();
            frameToSend = lastCombinedImage;
            hasLastFrame = true;
        } else if (hasLastFrame) {
            frameToSend = lastCombinedImage;
        } else {
            frameToSend = combinedImage.clone();
        }
        
        StreamingData stream_data;
        stream_data.stream_id = 0;
        stream_data.frame = frameToSend;
        stream_data.use_dma = false;
        stream_data.timestamp = std::chrono::system_clock::now();
        memset(&stream_data.person_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.helmet_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.tired_results, 0, sizeof(detect_result_group_t));
        memset(&stream_data.callplay_results, 0, sizeof(detect_result_group_t));
        
        streaming_manager.addStreamingData(stream_data);
    }
    for (auto &buf : tile_buffers) {
        if (buf.fd >= 0) {
            const size_t buffer_size = static_cast<size_t>(buf.width) * buf.height * 3;
            dma_buf_free(buffer_size, &buf.fd, buf.va);
        }
    }
    cv::destroyAllWindows(); // 销毁所有窗口
}

void rknn_infer(rknn_lite *p1, rknn_lite *p2, rknn_lite *p3, rknn_lite *p4, int i)
{
    dpool::ThreadPool pool(4);

    detect_result_group_t g1, g2, g3, g4;
    memset(&g1, 0, sizeof(detect_result_group_t));
    memset(&g2, 0, sizeof(detect_result_group_t));
    memset(&g3, 0, sizeof(detect_result_group_t));
    memset(&g4, 0, sizeof(detect_result_group_t));

    // 跳帧推理：每 INFER_INTERVAL 帧做一次完整推理，中间帧复用上一帧检测结果，提高实时性
    const int INFER_INTERVAL = 2; 
    int frame_count = 0;
    std::vector<FusedDetection> last_fused;
    bool first_input_logged = false;
    bool first_output_logged = false;

    while (!manager.stream_loaders[i]->stopFlag)
    {
        std::unique_lock<std::mutex> lock(manager.stream_loaders[i]->buffer.mtx);
        if (manager.stream_loaders[i]->buffer.img.empty())
        {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        p1->ori_img = manager.stream_loaders[i]->buffer.img.clone();
        p2->ori_img = p1->ori_img;
        if (p3) p3->ori_img = p1->ori_img;
        p4->ori_img = p1->ori_img;
        lock.unlock();

        if (!first_input_logged)
        {
            std::cout << "Inference stream " << i << " received first frame: "
                      << p1->ori_img.cols << "x" << p1->ori_img.rows << std::endl;
            first_input_logged = true;
        }

        frame_count++;
        bool do_infer = (frame_count % INFER_INTERVAL == 1) || last_fused.empty();

        if (do_infer)
        {
            auto f1 = pool.submit([&]() { p1->interf(g1); });
            auto f2 = pool.submit([&]() { p2->interf(g2); });
            std::future<void> f3;
            if (p3) f3 = pool.submit([&]() { p3->interf(g3); });
            auto f4 = pool.submit([&]() { p4->interf(g4); });

            f1.get(); f2.get();
            if (p3) f3.get();
            f4.get();

            last_fused = fusion_manager.fuseDetections(g1, g2, g3, g4);
        }

        fusion_manager.drawFusedDetections(p1->ori_img, last_fused);

        std::unique_lock<std::mutex> lockimage(mutexes[i]);
        images[i] = std::move(p1->ori_img);
        lockimage.unlock();

        if (!first_output_logged)
        {
            std::cout << "Inference stream " << i
                      << " published first frame to compositor" << std::endl;
            first_output_logged = true;
        }
    }
}

int main(int argc, char *argv[])
{
    RealtimeLogger realtime_logger;
    if (!realtime_logger.start())
        std::cerr << "Realtime file logging is unavailable; continuing with console output"
                  << std::endl;

    if (argc != 2)
    {
        std::cerr << "Usage: " << argv[0] << " <stream_count>" << std::endl;
        return -1;
    }

    try
    {
        manager.num_stream = std::stoi(argv[1]);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Invalid stream count: " << e.what() << std::endl;
        return -1;
    }

    const size_t max_stream_count = std::min(
        {manager.urls.size(), images.size(), static_cast<size_t>(kLayoutStreamCount)});
    if (manager.num_stream < 1 ||
        static_cast<size_t>(manager.num_stream) > max_stream_count)
    {
        std::cerr << "stream_count must be between 1 and "
                  << max_stream_count << std::endl;
        return -1;
    }
    
    // 初始化推流配置
    StreamingConfig stream_config;
    stream_config.rtmp_url = "rtmp://192.168.0.198/live/livestream"; // 替换为实际的RTMP地址
    stream_config.width = kCompositeWidth;
    stream_config.height = kCompositeHeight;
    stream_config.fps = 24;
    stream_config.bitrate = 5000000;
    stream_config.enable_rtmp = true;
    stream_config.draw_detections = true;
    
    // 初始化推流管理器
    if (!streaming_manager.initialize(stream_config)) {
        std::cerr << "Failed to initialize streaming manager" << std::endl;
        return -1;
    }
    
    // 启动推流
    streaming_manager.startStreaming();
    
    // 解码与推理线程
    for (int i = 0; i < manager.num_stream; ++i)
    {
        manager.load_stream(i);
        // 不同模型绑定不同 NPU 核心，使 ThreadPool 内 person/helmet/callplay 可并行推理
        rknn_lite *ptr1 = new rknn_lite(model_person, 0, 1, 0);
        rknn_lite *ptr2 = new rknn_lite(model_helmet, 1, 2, 1);
        rknn_lite *ptr3 = nullptr;
        rknn_lite *ptr4 = new rknn_lite(model_callplay, 2, 2, 3);
        rk_threads.push_back(std::thread(rknn_infer, ptr1, ptr2, ptr3, ptr4, i));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::thread readerThread(combineImage, std::ref(manager));
    readerThread.join();

    // 停止推流
    streaming_manager.stopStreaming();
    
    for (int i = 0; i < manager.num_stream; ++i)
    {
        manager.unload_stream(i);
    }
    for (auto &t : rk_threads)
    {
        if (t.joinable())
            t.join();
    }

    return 0;
}
