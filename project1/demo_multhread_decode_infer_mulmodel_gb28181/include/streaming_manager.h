/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once
#include <opencv2/opencv.hpp>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <vector>
#include "postprocess.h"
#include <condition_variable>
#include "mpp_encoder.h"

struct StreamingData {
    int stream_id;
    cv::Mat frame;
    detect_result_group_t person_results;
    detect_result_group_t helmet_results;
    detect_result_group_t tired_results;
    detect_result_group_t callplay_results;
    std::chrono::system_clock::time_point timestamp;
};

struct StreamingConfig {
    std::string rtmp_url;
    std::string rtsp_url;
    int width = 1280;
    int height = 720;
    int fps = 25;
    int bitrate = 2000000;
    bool enable_rtmp = true;
    bool enable_gb28181 = false;
    bool enable_rtsp = false;
    bool draw_detections = true;
};

class StreamingManager {
public:
    StreamingManager();
    ~StreamingManager();

    bool initialize(const StreamingConfig& config);
    void addStreamingData(const StreamingData& data);
    void startStreaming();
    void stopStreaming();

    bool isStreaming() const { return streaming_active_.load(); }

    struct StreamingStats {
        int frames_sent = 0;
        int frames_dropped = 0;
        double fps = 0.0;
        std::chrono::system_clock::time_point last_frame_time;
    };
    StreamingStats getStats() const;

private:
    void streamingWorker();
    void drawDetections(cv::Mat& frame, const StreamingData& data);
    std::string createDetectionJSON(const StreamingData& data);

    bool initializeRTMP();
    bool initializeRTSP();
    bool initializeOutput(const char* format_name, const std::string& output_url, void*& opaque_context, bool use_rtsp_options);
    bool ensureEncoderInitialized();
    bool encodeFrame(const cv::Mat& frame, std::vector<uint8_t>& encoded_frame, int& packet_size);
    bool writeEncodedFrame(void* opaque_context, const uint8_t* data, int size, const char* output_name);
    void closeOutput(void*& opaque_context, const char* output_name);

private:
    StreamingConfig config_;
    std::atomic<bool> streaming_active_;
    std::atomic<bool> should_stop_;

    std::queue<StreamingData> streaming_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread streaming_thread_;

    void* rtmp_context_;
    void* rtsp_context_;
    MppEncoder* mpp_encoder_;
    int64_t frame_index_;

    mutable std::mutex stats_mutex_;
    StreamingStats stats_;

    std::chrono::system_clock::time_point last_fps_time_;
    int frame_count_;
};
