/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

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

void combineImage(StreamLoaderManager& stream_manager, int target_fps,
                  bool streaming_enabled)
{
    cv::Mat combined_image(1080, 1280, CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat last_combined_image;
    bool has_last_frame = false;
    const int frame_interval_ms = 1000 / target_fps;
    const int tile_width = 640;
    const int tile_height = 360;
    const size_t tile_size =
        static_cast<size_t>(tile_width) * tile_height * 3;

    struct DmaTileBuffer {
        int fd = -1;
        void* va = nullptr;
        cv::Mat view;
    };

    std::vector<DmaTileBuffer> tile_buffers(stream_manager.num_stream);
    for (int i = 0; i < stream_manager.num_stream; ++i) {
        if (dma_buf_alloc(DMA_HEAP_PATH, tile_size, &tile_buffers[i].fd,
                          &tile_buffers[i].va) == 0) {
            tile_buffers[i].view = cv::Mat(
                tile_height, tile_width, CV_8UC3, tile_buffers[i].va);
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

            cv::Mat resized_image;
            if (tile_buffers[i].va == nullptr) {
                cv::resize(local_image, resized_image,
                           cv::Size(tile_width, tile_height));
            } else {
                rga_buffer_t source_buffer = wrapbuffer_virtualaddr_t(
                    local_image.data, local_image.cols, local_image.rows,
                    local_image.cols, local_image.rows, RK_FORMAT_BGR_888);
                rga_buffer_t target_buffer = wrapbuffer_virtualaddr_t(
                    tile_buffers[i].va, tile_width, tile_height, tile_width,
                    tile_height, RK_FORMAT_BGR_888);
                const IM_STATUS resize_status =
                    imresize(source_buffer, target_buffer);
                if (resize_status != IM_STATUS_SUCCESS) {
                    cv::resize(local_image, resized_image,
                               cv::Size(tile_width, tile_height));
                } else {
                    resized_image = tile_buffers[i].view;
                }
            }

            const int row = i / 2;
            const int column = i % 2;
            const int x = column * tile_width;
            const int y = row * tile_height;
            resized_image.copyTo(combined_image(
                cv::Rect(x, y, tile_width, tile_height)));
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
            dma_buf_free(tile_size, &buffer.fd, buffer.va);
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
        std::lock_guard<std::mutex> image_lock(mutexes[stream_index]);
        images[stream_index] = std::move(person->ori_img);
    }
}

int main(int argc, char* argv[])
{
    (void)argc;
    (void)argv;

    AppConfig app_config;
    std::string config_error;
    if (!AppConfigLoader::load(APP_CONFIG_FILE, app_config, config_error)) {
        std::cerr << "Failed to load " << APP_CONFIG_FILE << ": "
                  << config_error << std::endl;
        return -1;
    }
    std::cout << "Loaded configuration: " << APP_CONFIG_FILE << std::endl;
    manager.configure(app_config.streams);

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
                              stream_config.fps, streaming_enabled);
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
