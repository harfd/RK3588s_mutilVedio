/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app_config.h"
#include "detection_fusion_manager.h"
#include "realtime_logger.h"
#include "im2d.h"
#include "include/dma_alloc.hpp"
#include "rknnPool.hpp"
#include "stream_loader.h"
#include "streaming_manager.h"

#ifndef APP_CONFIG_FILE
#error "APP_CONFIG_FILE must point to demo_multhread_decode_infer_mulmodel/config_user.ini"
#endif

StreamLoaderManager& manager = StreamLoaderManager::getInstance();
std::vector<std::unique_ptr<rknn_lite>> rk_pool;
std::vector<std::thread> rk_threads;
std::vector<cv::Mat> images(6);
std::vector<std::mutex> mutexes(6);
StreamingManager streaming_manager;
DetectionFusionManager fusion_manager;

namespace
{
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

StreamLayout getStreamLayout(int stream_id, int camera_stream_index)
{
    if (stream_id == camera_stream_index)
        return {0, kTileTopPadding, kTileWidth, 2 * kTileHeight};

    if (camera_stream_index < 0)
    {
        const int row = stream_id / 2;
        const int column = stream_id % 2;
        return {column * kTileWidth,
                kTileTopPadding + row * kTileHeight,
                kTileWidth, kTileHeight};
    }

    const int thumbnail_index = stream_id < camera_stream_index
                                    ? stream_id
                                    : stream_id - 1;
    switch (thumbnail_index)
    {
    case 0:
        return {kTileWidth, kTileTopPadding, kTileWidth, kTileHeight};
    case 1:
        return {kTileWidth, kTileTopPadding + kTileHeight, kTileWidth, kTileHeight};
    case 2:
        return {0, kTileTopPadding + 2 * kTileHeight, kTileWidth, kTileHeight};
    case 3:
        return {kTileWidth, kTileTopPadding + 2 * kTileHeight, kTileWidth, kTileHeight};
    default:
        return {0, 0, 0, 0};
    }
}

int findCameraStreamIndex(const std::vector<StreamSourceConfig>& sources)
{
    for (size_t i = 0; i < sources.size(); ++i)
    {
        if (sources[i].type == InputSourceType::Camera)
            return static_cast<int>(i);
    }
    return -1;
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

void combineImage(StreamLoaderManager& stream_manager, int target_fps,
                  bool streaming_enabled, int camera_stream_index)
{
    cv::Mat combined_image(kCompositeHeight, kCompositeWidth,
                           CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat last_combined_image;
    bool has_last_frame = false;
    const int frame_interval_ms = 1000 / target_fps;
    struct DmaTileBuffer {
        int fd = -1;
        void *va = nullptr;
        int width = 0;
        int height = 0;
        cv::Mat view;
    };
    std::vector<DmaTileBuffer> tile_buffers(stream_manager.num_stream);
    for (int i = 0; i < stream_manager.num_stream; ++i) {
        const StreamLayout layout = getStreamLayout(i, camera_stream_index);
        tile_buffers[i].width = layout.width;
        tile_buffers[i].height = layout.height;
        const size_t tile_size = static_cast<size_t>(layout.width) * layout.height * 3;
        if (dma_buf_alloc(DMA_HEAP_PATH, tile_size, &tile_buffers[i].fd, &tile_buffers[i].va) == 0) {
            tile_buffers[i].view = cv::Mat(layout.height, layout.width,
                                           CV_8UC3, tile_buffers[i].va);
        }
    }

    auto last_frame_time = std::chrono::steady_clock::now();
    while (true) {
        auto current_time = std::chrono::steady_clock::now();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                current_time - last_frame_time)
                .count();
        if (elapsed < frame_interval_ms) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(frame_interval_ms - elapsed));
            current_time = std::chrono::steady_clock::now();
        }
        last_frame_time = current_time;

        bool has_new_frame = false;
        for (int i = 0; i < stream_manager.num_stream; ++i) {
            cv::Mat local_image;
            {
                std::lock_guard<std::mutex> lock(mutexes[i]);
                if (images[i].empty()) {
                    continue;
                }
                local_image = std::move(images[i]);
                images[i] = cv::Mat();
            }

            const StreamLayout layout = getStreamLayout(i, camera_stream_index);
            cv::Mat resize_source = local_image;
            if (i == camera_stream_index)
                resize_source = centerCropToAspect(local_image, layout.width, layout.height);

            cv::Mat resized_image;
            int src_w = resize_source.cols, src_h = resize_source.rows;
            bool tile_cpu_access_active = false;

            if (tile_buffers[i].va == nullptr) {
                cv::resize(resize_source, resized_image,
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
                    resized_image = tile_buffers[i].view;
                    tile_cpu_access_active = true;
                } else {
                    cv::resize(resize_source, resized_image,
                               cv::Size(layout.width, layout.height),
                               0.0, 0.0, cv::INTER_AREA);
                }
            }

            if (i == camera_stream_index)
                sharpenCameraTile(resized_image);

            resized_image.copyTo(combined_image(cv::Rect(
                layout.x, layout.y, layout.width, layout.height)));
            if (tile_cpu_access_active)
                dma_sync_cpu_to_device(tile_buffers[i].fd);
            has_new_frame = true;
        }

        cv::Mat frame_to_send;
        if (has_new_frame) {
            last_combined_image = combined_image.clone();
            frame_to_send = last_combined_image;
            has_last_frame = true;
        } else if (has_last_frame) {
            frame_to_send = last_combined_image;
        } else {
            frame_to_send = combined_image.clone();
        }
        if (streaming_enabled) {
            StreamingData stream_data;
            stream_data.stream_id = 0;
            stream_data.frame = frame_to_send;
            stream_data.use_dma = false;
            stream_data.timestamp = std::chrono::system_clock::now();
            memset(&stream_data.person_results, 0,
                   sizeof(detect_result_group_t));
            memset(&stream_data.helmet_results, 0,
                   sizeof(detect_result_group_t));
            memset(&stream_data.tired_results, 0,
                   sizeof(detect_result_group_t));
            memset(&stream_data.callplay_results, 0,
                   sizeof(detect_result_group_t));
            streaming_manager.addStreamingData(stream_data);
        }
    }

    for (auto& buffer : tile_buffers) {
        if (buffer.fd >= 0) {
            const size_t buffer_size =
                static_cast<size_t>(buffer.width) * buffer.height * 3;
            dma_buf_free(buffer_size, &buffer.fd, buffer.va);
        }
    }
}

void rknn_infer(rknn_lite* person, rknn_lite* helmet, rknn_lite* tired,
                rknn_lite* callplay, int stream_index, int infer_interval,
                bool draw_detections)
{
    dpool::ThreadPool pool(4);
    detect_result_group_t person_results;
    detect_result_group_t helmet_results;
    detect_result_group_t tired_results;
    detect_result_group_t callplay_results;
    memset(&person_results, 0, sizeof(detect_result_group_t));
    memset(&helmet_results, 0, sizeof(detect_result_group_t));
    memset(&tired_results, 0, sizeof(detect_result_group_t));
    memset(&callplay_results, 0, sizeof(detect_result_group_t));

    int frame_count = 0;
    bool has_inference_result = false;
    std::vector<FusedDetection> last_fused;
    bool first_input_logged = false;
    bool first_output_logged = false;

    while (!manager.stream_loaders[stream_index]->stopFlag) {
        std::unique_lock<std::mutex> lock(
            manager.stream_loaders[stream_index]->buffer.mtx);
        if (manager.stream_loaders[stream_index]->buffer.img.empty()) {
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        person->ori_img =
            manager.stream_loaders[stream_index]->buffer.img.clone();
        helmet->ori_img = person->ori_img.clone();
        if (tired) {
            tired->ori_img = person->ori_img.clone();
        }
        callplay->ori_img = person->ori_img.clone();
        lock.unlock();

        if (!first_input_logged)
        {
            std::cout << "Inference stream " << stream_index
                      << " received first frame: "
                      << person->ori_img.cols << "x" << person->ori_img.rows
                      << std::endl;
            first_input_logged = true;
        }

        ++frame_count;
        const bool do_infer =
            !has_inference_result ||
            ((frame_count - 1) % infer_interval == 0);
        if (do_infer) {
            memset(&person_results, 0, sizeof(detect_result_group_t));
            memset(&helmet_results, 0, sizeof(detect_result_group_t));
            memset(&tired_results, 0, sizeof(detect_result_group_t));
            memset(&callplay_results, 0, sizeof(detect_result_group_t));

            auto person_future =
                pool.submit([&]() { person->interf(person_results); });
            auto helmet_future =
                pool.submit([&]() { helmet->interf(helmet_results); });
            std::future<void> tired_future;
            if (tired) {
                tired_future =
                    pool.submit([&]() { tired->interf(tired_results); });
            }
            auto callplay_future =
                pool.submit([&]() { callplay->interf(callplay_results); });

            person_future.get();
            helmet_future.get();
            if (tired) {
                tired_future.get();
            }
            callplay_future.get();

            last_fused = fusion_manager.fuseDetections(
                person_results, helmet_results, tired_results,
                callplay_results);
            has_inference_result = true;
        }

        if (draw_detections) {
            fusion_manager.drawFusedDetections(person->ori_img, last_fused);
        }
        {
            std::lock_guard<std::mutex> image_lock(mutexes[stream_index]);
            images[stream_index] = std::move(person->ori_img);
        }

        if (!first_output_logged)
        {
            std::cout << "Inference stream " << stream_index
                      << " published first frame to compositor" << std::endl;
            first_output_logged = true;
        }
    }
}

int main(int argc, char* argv[])
{
    RealtimeLogger realtime_logger;
    if (!realtime_logger.start())
        std::cerr << "Realtime file logging is unavailable; continuing with console output"
                  << std::endl;

    if (argc > 2)
    {
        std::cerr << "Usage: " << argv[0]
                  << " [config_file|stream_count]" << std::endl;
        return -1;
    }

    std::string config_path = APP_CONFIG_FILE;
    int stream_count_override = -1;
    if (argc == 2)
    {
        const std::string argument = argv[1];
        const bool is_number = !argument.empty() &&
            std::all_of(argument.begin(), argument.end(),
                        [](unsigned char ch) { return std::isdigit(ch) != 0; });
        if (is_number)
            stream_count_override = std::stoi(argument);
        else
            config_path = argument;
    }

    AppConfig app_config;
    std::string config_error;
    if (!AppConfigLoader::load(config_path, app_config, config_error)) {
        std::cerr << "Failed to load " << config_path << ": "
                  << config_error << std::endl;
        return -1;
    }
    if (stream_count_override > 0)
    {
        if (stream_count_override > static_cast<int>(app_config.streams.size()))
        {
            std::cerr << "stream_count override exceeds enabled sources in config"
                      << std::endl;
            return -1;
        }
        app_config.streams.resize(stream_count_override);
    }
    if (app_config.streams.empty() ||
        app_config.streams.size() > static_cast<size_t>(kLayoutStreamCount) ||
        app_config.streams.size() > images.size())
    {
        std::cerr << "configured stream count must be between 1 and "
                  << kLayoutStreamCount << std::endl;
        return -1;
    }

    std::cout << "Loaded configuration: " << config_path << std::endl;
    manager.configure(app_config.streams);
    const int camera_stream_index = findCameraStreamIndex(app_config.streams);

    FusionConfig fusion_config;
    fusion_config.iou_threshold =
        app_config.inference.fusion_iou_threshold;
    fusion_config.iom_threshold =
        app_config.inference.fusion_iom_threshold;
    fusion_config.confidence_threshold =
        app_config.inference.fusion_confidence_threshold;
    fusion_manager.setConfig(fusion_config);

    StreamingConfig stream_config;
    stream_config.rtmp_url = app_config.streaming.rtmp_url;
    stream_config.rtsp_url = app_config.streaming.rtsp_url;
    stream_config.width = app_config.streaming.width;
    stream_config.height = app_config.streaming.height;
    stream_config.fps = app_config.streaming.fps;
    stream_config.bitrate = app_config.streaming.bitrate;
    stream_config.enable_rtmp = app_config.streaming.enable_rtmp;
    stream_config.enable_rtsp = app_config.streaming.enable_rtsp;
    stream_config.draw_detections = app_config.streaming.draw_detections;
    const bool streaming_enabled =
        stream_config.enable_rtmp || stream_config.enable_rtsp;

    if (streaming_enabled &&
        !streaming_manager.initialize(stream_config)) {
        std::cerr << "Failed to initialize streaming manager" << std::endl;
        return -1;
    }
    if (streaming_enabled) {
        streaming_manager.startStreaming();
    }

    for (int i = 0; i < manager.num_stream; ++i) {
        if (!manager.load_stream(i)) {
            std::cerr << "Failed to load stream " << i << std::endl;
            return -1;
        }

        auto person = std::make_unique<rknn_lite>(
            app_config.inference.person_model_path,
            app_config.inference.person_core,
            app_config.inference.person_class_count, 0,
            app_config.inference.confidence_threshold,
            app_config.inference.nms_threshold);
        auto helmet = std::make_unique<rknn_lite>(
            app_config.inference.helmet_model_path,
            app_config.inference.helmet_core,
            app_config.inference.helmet_class_count, 1,
            app_config.inference.confidence_threshold,
            app_config.inference.nms_threshold);
        auto callplay = std::make_unique<rknn_lite>(
            app_config.inference.callplay_model_path,
            app_config.inference.callplay_core,
            app_config.inference.callplay_class_count, 3,
            app_config.inference.confidence_threshold,
            app_config.inference.nms_threshold);

        rknn_lite* person_ptr = person.get();
        rknn_lite* helmet_ptr = helmet.get();
        rknn_lite* callplay_ptr = callplay.get();
        rk_pool.push_back(std::move(person));
        rk_pool.push_back(std::move(helmet));
        rk_pool.push_back(std::move(callplay));
        rk_threads.emplace_back(
            rknn_infer, person_ptr, helmet_ptr, nullptr, callplay_ptr, i,
            app_config.global.infer_interval,
            app_config.streaming.draw_detections);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::thread reader_thread(combineImage, std::ref(manager),
                              stream_config.fps, streaming_enabled,
                              camera_stream_index);
    reader_thread.join();

    if (streaming_enabled) {
        streaming_manager.stopStreaming();
    }
    for (int i = 0; i < manager.num_stream; ++i) {
        manager.unload_stream(i);
    }
    for (auto& thread : rk_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    rk_pool.clear();
    return 0;
}
