/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "streaming_manager.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

namespace {
bool fillCodecParameters(AVStream* stream, int width, int height)
{
    if (!stream || !stream->codecpar) {
        return false;
    }

    AVCodecParameters* codecpar = stream->codecpar;
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = AV_CODEC_ID_H264;
    codecpar->width = width;
    codecpar->height = height;
    codecpar->format = AV_PIX_FMT_YUV420P;
    stream->time_base = AVRational{1, 1};
    return true;
}

bool attachEncoderExtradata(AVCodecParameters* codecpar, MppEncoder* encoder)
{
    if (!codecpar || !encoder) {
        return false;
    }

    uint8_t header_buf[1024];
    int header_size = sizeof(header_buf);
    if (encoder->GetHeader(header_buf, &header_size) != 0 || header_size <= 0) {
        std::cerr << "Warning: failed to get H.264 extra info from MPP encoder" << std::endl;
        return false;
    }

    codecpar->extradata = static_cast<uint8_t*>(av_malloc(header_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codecpar->extradata) {
        std::cerr << "Failed to allocate extradata buffer" << std::endl;
        return false;
    }

    memcpy(codecpar->extradata, header_buf, header_size);
    memset(codecpar->extradata + header_size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    codecpar->extradata_size = header_size;
    std::cout << "H.264 extradata from MPP, size: " << header_size << " bytes" << std::endl;
    return true;
}
}

StreamingManager::StreamingManager()
    : streaming_active_(false)
    , should_stop_(false)
    , rtmp_context_(nullptr)
    , rtsp_context_(nullptr)
    , mpp_encoder_(nullptr)
    , frame_index_(0)
    , frame_count_(0)
{
}

StreamingManager::~StreamingManager() {
    stopStreaming();
}

bool StreamingManager::initialize(const StreamingConfig& config) {
    config_ = config;
    stats_.last_frame_time = std::chrono::system_clock::now();
    frame_index_ = 0;
    frame_count_ = 0;

    const bool need_network_output = config_.enable_rtmp || config_.enable_rtsp;
    if (!need_network_output) {
        return true;
    }

    avformat_network_init();

    if (!ensureEncoderInitialized()) {
        return false;
    }

    if (config_.enable_rtmp) {
        if (!initializeRTMP()) {
            std::cerr << "Failed to initialize RTMP streaming" << std::endl;
            closeOutput(rtsp_context_, "RTSP");
            return false;
        }
    }

    if (config_.enable_rtsp) {
        if (!initializeRTSP()) {
            std::cerr << "Failed to initialize RTSP streaming" << std::endl;
            closeOutput(rtmp_context_, "RTMP");
            return false;
        }
    }

    return true;
}

void StreamingManager::addStreamingData(const StreamingData& data) {
    std::lock_guard<std::mutex> lock(queue_mutex_);

    if (streaming_queue_.size() > 10) {
        streaming_queue_.pop();
        stats_.frames_dropped++;
    }

    streaming_queue_.push(data);
    queue_cv_.notify_one();

    static int frame_count = 0;
    if (++frame_count % 100 == 0) {
        std::cout << "Added frame to streaming queue, queue size: " << streaming_queue_.size() << std::endl;
    }
}

void StreamingManager::startStreaming() {
    if (streaming_active_.load()) {
        return;
    }

    streaming_active_ = true;
    should_stop_ = false;
    streaming_thread_ = std::thread(&StreamingManager::streamingWorker, this);

    std::cout << "Streaming started" << std::endl;
}

void StreamingManager::stopStreaming() {
    if (!streaming_active_.load()) {
        closeOutput(rtmp_context_, "RTMP");
        closeOutput(rtsp_context_, "RTSP");
        if (mpp_encoder_) {
            mpp_encoder_->Release();
            delete mpp_encoder_;
            mpp_encoder_ = nullptr;
        }
        return;
    }

    should_stop_ = true;
    queue_cv_.notify_all();

    if (streaming_thread_.joinable()) {
        streaming_thread_.join();
    }

    streaming_active_ = false;

    closeOutput(rtmp_context_, "RTMP");
    closeOutput(rtsp_context_, "RTSP");

    if (mpp_encoder_) {
        mpp_encoder_->Release();
        delete mpp_encoder_;
        mpp_encoder_ = nullptr;
    }

    std::cout << "Streaming stopped" << std::endl;
}

void StreamingManager::streamingWorker() {
    cv::Mat last_frame;
    bool has_last_frame = false;

    while (!should_stop_.load()) {
        StreamingData data;
        bool got_data = false;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            auto timeout = std::chrono::milliseconds(1000 / config_.fps);
            if (queue_cv_.wait_for(lock, timeout, [this] {
                return !streaming_queue_.empty() || should_stop_.load();
            })) {
                if (should_stop_.load()) {
                    break;
                }

                if (!streaming_queue_.empty()) {
                    data = streaming_queue_.front();
                    streaming_queue_.pop();
                    got_data = true;
                }
            } else {
                if (has_last_frame && !last_frame.empty()) {
                    data.frame = last_frame.clone();
                    data.stream_id = 0;
                    data.timestamp = std::chrono::system_clock::now();
                    memset(&data.person_results, 0, sizeof(detect_result_group_t));
                    memset(&data.helmet_results, 0, sizeof(detect_result_group_t));
                    memset(&data.tired_results, 0, sizeof(detect_result_group_t));
                    memset(&data.callplay_results, 0, sizeof(detect_result_group_t));
                    got_data = true;
                } else {
                    continue;
                }
            }
        }

        if (!got_data) {
            continue;
        }

        cv::Mat frame = data.frame.clone();

        if (config_.draw_detections) {
            drawDetections(frame, data);
        }

        if (frame.cols != config_.width || frame.rows != config_.height) {
            cv::resize(frame, frame, cv::Size(config_.width, config_.height));
        }

        last_frame = frame.clone();
        has_last_frame = true;

        bool success = false;
        if (config_.enable_rtmp || config_.enable_rtsp) {
            std::vector<uint8_t> encoded_frame;
            int packet_size = 0;
            if (encodeFrame(frame, encoded_frame, packet_size)) {
                if (config_.enable_rtmp) {
                    success |= writeEncodedFrame(rtmp_context_, encoded_frame.data(), packet_size, "RTMP");
                }
                if (config_.enable_rtsp) {
                    success |= writeEncodedFrame(rtsp_context_, encoded_frame.data(), packet_size, "RTSP");
                }
                frame_index_++;
            }
        }

        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (success) {
                stats_.frames_sent++;
            } else {
                stats_.frames_dropped++;
            }

            auto now = std::chrono::system_clock::now();
            frame_count_++;
            if (frame_count_ % config_.fps == 0) {
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - stats_.last_frame_time).count();
                if (duration > 0) {
                    stats_.fps = (config_.fps * 1000.0) / duration;
                }
                stats_.last_frame_time = now;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / config_.fps));
    }
}

void StreamingManager::drawDetections(cv::Mat& frame, const StreamingData& data) {
    for (int i = 0; i < data.person_results.count; i++) {
        const auto& result = data.person_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 255, 0), 2);
        cv::putText(frame, "Person",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 0), 1);
    }

    for (int i = 0; i < data.helmet_results.count; i++) {
        const auto& result = data.helmet_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 0, 255), 2);
        cv::putText(frame, "Helmet",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255), 1);
    }

    for (int i = 0; i < data.tired_results.count; i++) {
        const auto& result = data.tired_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(0, 255, 255), 2);
        cv::putText(frame, "Tired",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 255, 255), 1);
    }

    for (int i = 0; i < data.callplay_results.count; i++) {
        const auto& result = data.callplay_results.results[i];
        cv::rectangle(frame,
            cv::Point(result.box.left, result.box.top),
            cv::Point(result.box.right, result.box.bottom),
            cv::Scalar(255, 0, 0), 2);
        cv::putText(frame, "Call/Play",
            cv::Point(result.box.left, result.box.top - 10),
            cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255, 0, 0), 1);
    }

    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

    cv::putText(frame, ss.str(), cv::Point(10, 30),
        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(255, 255, 255), 2);

    std::string stats = "P:" + std::to_string(data.person_results.count) +
                       " H:" + std::to_string(data.helmet_results.count) +
                       " T:" + std::to_string(data.tired_results.count) +
                       " C:" + std::to_string(data.callplay_results.count);

    cv::putText(frame, stats, cv::Point(10, 60),
        cv::FONT_HERSHEY_SIMPLEX, 0.6, cv::Scalar(255, 255, 255), 2);
}

std::string StreamingManager::createDetectionJSON(const StreamingData& data) {
    std::stringstream json;
    json << "{";
    json << "\"stream_id\":" << data.stream_id << ",";
    json << "\"timestamp\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
        data.timestamp.time_since_epoch()).count() << ",";

    json << "\"detections\":{";
    json << "\"person\":" << data.person_results.count << ",";
    json << "\"helmet\":" << data.helmet_results.count << ",";
    json << "\"tired\":" << data.tired_results.count << ",";
    json << "\"callplay\":" << data.callplay_results.count;
    json << "}";

    json << "}";
    return json.str();
}

bool StreamingManager::ensureEncoderInitialized() {
    if (mpp_encoder_) {
        return true;
    }

    mpp_encoder_ = new MppEncoder();
    if (mpp_encoder_->Init(config_.width, config_.height, config_.fps, config_.bitrate, 264) != 0) {
        std::cerr << "Failed to init MPP encoder" << std::endl;
        delete mpp_encoder_;
        mpp_encoder_ = nullptr;
        return false;
    }

    return true;
}

bool StreamingManager::initializeOutput(const char* format_name, const std::string& output_url, void*& opaque_context, bool use_rtsp_options) {
    if (output_url.empty()) {
        std::cerr << format_name << " output URL is empty" << std::endl;
        return false;
    }

    AVFormatContext* fmt_ctx = nullptr;
    if (avformat_alloc_output_context2(&fmt_ctx, nullptr, format_name, output_url.c_str()) < 0 || !fmt_ctx) {
        std::cerr << "Could not create " << format_name << " output context for " << output_url << std::endl;
        return false;
    }

    AVStream* stream = avformat_new_stream(fmt_ctx, nullptr);
    if (!stream || !fillCodecParameters(stream, config_.width, config_.height)) {
        std::cerr << "Could not create stream for " << format_name << std::endl;
        avformat_free_context(fmt_ctx);
        return false;
    }

    stream->time_base = AVRational{1, config_.fps};
    if (!attachEncoderExtradata(stream->codecpar, mpp_encoder_)) {
        avformat_free_context(fmt_ctx);
        return false;
    }

    AVDictionary* options = nullptr;
    if (use_rtsp_options) {
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "muxdelay", "0.1", 0);
    }

    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&fmt_ctx->pb, output_url.c_str(), AVIO_FLAG_WRITE, nullptr, &options) < 0) {
            std::cerr << "Could not open output URL: " << output_url << std::endl;
            av_dict_free(&options);
            avformat_free_context(fmt_ctx);
            return false;
        }
    }

    if (avformat_write_header(fmt_ctx, &options) < 0) {
        std::cerr << "Error occurred when writing header to " << format_name << std::endl;
        av_dict_free(&options);
        if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb) {
            avio_closep(&fmt_ctx->pb);
        }
        avformat_free_context(fmt_ctx);
        return false;
    }

    av_dict_free(&options);
    opaque_context = fmt_ctx;
    return true;
}

bool StreamingManager::initializeRTMP() {
    return initializeOutput("flv", config_.rtmp_url, rtmp_context_, false);
}

bool StreamingManager::initializeRTSP() {
    return initializeOutput("rtsp", config_.rtsp_url, rtsp_context_, true);
}

bool StreamingManager::encodeFrame(const cv::Mat& frame, std::vector<uint8_t>& encoded_frame, int& packet_size) {
    if (!mpp_encoder_) {
        return false;
    }

    if (!frame.isContinuous()) {
        std::cerr << "Frame is not continuous, skip" << std::endl;
        return false;
    }

    const int max_packet_size = config_.width * config_.height * 2;
    encoded_frame.resize(max_packet_size);
    packet_size = max_packet_size;

    int ret = mpp_encoder_->EncodeFrame(
        frame.data,
        frame.cols,
        frame.rows,
        encoded_frame.data(),
        &packet_size,
        static_cast<int>(frame.step)
    );

    if (ret != 0 || packet_size <= 0) {
        return false;
    }

    encoded_frame.resize(packet_size);
    return true;
}

bool StreamingManager::writeEncodedFrame(void* opaque_context, const uint8_t* data, int size, const char* output_name) {
    if (!opaque_context || !data || size <= 0) {
        return false;
    }

    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(opaque_context);
    if (fmt_ctx->nb_streams == 0) {
        return false;
    }

    AVStream* stream = fmt_ctx->streams[0];
    AVPacket pkt = {};
    pkt.data = const_cast<uint8_t*>(data);
    pkt.size = size;
    pkt.stream_index = stream->index;
    pkt.pts = frame_index_;
    pkt.dts = frame_index_;
    pkt.duration = 1;

    AVRational src_tb{1, config_.fps};
    av_packet_rescale_ts(&pkt, src_tb, stream->time_base);

    const int ret = av_interleaved_write_frame(fmt_ctx, &pkt);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "Error writing MPP-encoded frame to " << output_name << ": " << errbuf << std::endl;
        return false;
    }

    static int send_count = 0;
    if (++send_count % 100 == 0) {
        std::cout << "Sent frame " << send_count
                  << " to " << output_name
                  << " (MPP encoded, size=" << size << " bytes)" << std::endl;
    }

    return true;
}

void StreamingManager::closeOutput(void*& opaque_context, const char* output_name) {
    if (!opaque_context) {
        return;
    }

    AVFormatContext* fmt_ctx = static_cast<AVFormatContext*>(opaque_context);
    av_write_trailer(fmt_ctx);
    if (!(fmt_ctx->oformat->flags & AVFMT_NOFILE) && fmt_ctx->pb) {
        avio_closep(&fmt_ctx->pb);
    }
    avformat_free_context(fmt_ctx);
    opaque_context = nullptr;
    std::cout << output_name << " output closed" << std::endl;
}

StreamingManager::StreamingStats StreamingManager::getStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
}
