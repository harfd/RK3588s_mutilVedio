/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "stream_loader.h"
#include "im2d.h"
#include <chrono>
#include <string>
#include <thread>

// 判断是否为 Annex B 格式
// 该函数并没有使用
int is_annexb(const uint8_t *buf, size_t buf_size)
{
    // Annex B 格式以 0x000001 或 0x00000001 开头
    if (buf_size >= 4)
    {
        if ((buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) ||
            (buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x01))
        {
            return 1; // 是 Annex B 格式
        }
    }
    return 0; // 不是 Annex B 格式
}


void mpp_decoder_frame_callback(void *buffer, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)
{
    Mbuffer *mbuffer = (Mbuffer *)buffer;
    size_t yuv_size = (size_t)width * height * 3 / 2;

    mbuffer->yuv_work.resize(yuv_size);
    uint8_t *yuv_data = mbuffer->yuv_work.data();

    uint8_t *base_y = (uint8_t *)data;
    uint8_t *base_c = base_y + (size_t)width_stride * height_stride;
    int idx = 0;

    for (int i = 0; i < height; i++, base_y += width_stride)
    {
        memcpy(yuv_data + idx, base_y, width);
        idx += width;
    }

    for (int i = 0; i < height / 2; i++, base_c += width_stride)
    {
        memcpy(yuv_data + idx, base_c, width);
        idx += width;
    }

    mbuffer->bgr_work.create(height, width, CV_8UC3);
    if (mbuffer->bgr_work.empty()) {
        return;
    }

    // 优先使用 RGA 硬件加速 NV12->BGR，失败则回退 OpenCV
    rga_buffer_t src_buf = wrapbuffer_virtualaddr_t(yuv_data, width, height, width, height, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst_buf = wrapbuffer_virtualaddr_t(mbuffer->bgr_work.data, width, height, width, height, RK_FORMAT_BGR_888);

    IM_STATUS status = imcvtcolor(src_buf, dst_buf,
                                  RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                                  IM_COLOR_SPACE_DEFAULT);

    if (status != IM_STATUS_SUCCESS) {
        static int fallback_count = 0;
        if (fallback_count++ < 3) {
            fprintf(stderr, "RGA NV12->BGR failed (%d), using OpenCV fallback\n", (int)status);
        }
        cv::Mat yuvMat(height + height / 2, width, CV_8UC1, yuv_data);
        cv::cvtColor(yuvMat, mbuffer->bgr_work, cv::COLOR_YUV2BGR_NV12);
    }

    std::unique_lock<std::mutex> mlock(mbuffer->mtx);
    mbuffer->img = std::move(mbuffer->bgr_work);
    mlock.unlock();

    // 每输出一帧限速，解决一包多帧导致的倍速
    if (mbuffer->throttle && mbuffer->frame_interval_ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(mbuffer->frame_interval_ms));
}

void StreamLoader::close()
{
    if (is_v4l2_camera_)
    {
        std::lock_guard<std::mutex> lock(buffer.mtx);
        buffer.img.release();
        camera_.close();
        return;
    }

    decoder.Reset();
    if (bsf_ctx)
    {
        av_bsf_free(&bsf_ctx);
    }
    bsf = nullptr;
    isnotAnnexB = false;
    if (temp_pkt)
    {
        av_packet_free(&temp_pkt); // 释放 temp_pkt 并将指针置为 nullptr
    }

    if (fmtCtx)
    {
        avformat_close_input(&fmtCtx); // 关闭输入流
        fmtCtx = nullptr;              // 确保指针在关闭后被设置为 nullptr
    }

    if (codecPar)
    {
        avcodec_parameters_free(&codecPar); // 释放 codecPar 结构
    }
    if (options)
    {
        av_dict_free(&options);
    }
}

bool StreamLoader::read_frame()
{
    if (is_v4l2_camera_)
    {
        const bool ok = camera_.captureFrame(buffer, stopFlag);
        if (!ok)
            status = -1;
        return ok;
    }

    using namespace std::chrono_literals;
    int eof_retry = 0;           // 连续 av_read_frame 失败次数（EOF 时递增）
    int no_frame_count = 0;      // 已读视频包但解码未出帧的次数，防止异常时死循环
    const int MAX_EOF_RETRY = 10;       // EOF 时最多重试次数，超过则触发 reconnect
    const int MAX_PACKETS_NO_FRAME = 100; // 连续读包未出帧的上限，避免异常流导致死循环

    while (true)
    {
        int x = av_read_frame(fmtCtx, temp_pkt);
        if (x < 0)
        {
            status = x;
            eof_retry++;
            if (eof_retry >= MAX_EOF_RETRY) {
                return false;  // 确认 EOF，触发 reconnect 循环播放
            }
            std::this_thread::sleep_for(2ms);
            av_packet_unref(temp_pkt);
            continue;
        }

        eof_retry = 0;  // 成功读到包，重置 EOF 计数

        if (temp_pkt->stream_index != videoStreamIndex)
        {
            av_packet_unref(temp_pkt);
            continue;
        }

        // 视频包
        if (isnotAnnexB)
        {
            int ret = av_bsf_send_packet(bsf_ctx, temp_pkt);
            if (ret < 0)
            {
                fprintf(stderr, "Error sending packet to filter\n");
                av_packet_unref(temp_pkt);
                return false;
            }
            ret = av_bsf_receive_packet(bsf_ctx, temp_pkt);
            if (ret < 0)
            {
                fprintf(stderr, "Error receiving packet from filter\n");
                av_packet_unref(temp_pkt);
                return false;
            }
        }

        bool decode_success = decoder.Decode(temp_pkt->data, temp_pkt->size, 0);
        av_packet_unref(temp_pkt);

        if (decode_success)
        {
            status = 0;
            return true;
        }

        no_frame_count++;
        if (no_frame_count >= MAX_PACKETS_NO_FRAME)
        {
            // 异常：连续多包无输出，避免死循环
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
}

StreamLoader::StreamLoader(const StreamSourceConfig& source, int id)
    : source_(source)
{
    stream_loader_id = id;
    std::cout << "StreamLoader: " << std::to_string(id) << std::endl;
    callback = mpp_decoder_frame_callback;
    if (source_.type == InputSourceType::Camera)
    {
        stream_url_ = source_.device_path.empty()
                          ? "/dev/video" + std::to_string(source_.device_index)
                          : source_.device_path;
    }
    else
    {
        stream_url_ = source_.url;
    }
    status = 0;
    stopFlag = false;
}

StreamLoader::~StreamLoader()
{
    std::cout << "destory stream loader: " << stream_loader_id << std::endl;
    close();
    // delete mat_ptr;
}

int StreamLoader::open()
{
    is_v4l2_camera_ = source_.type == InputSourceType::Camera;
    if (is_v4l2_camera_)
    {
        buffer.throttle = false;
        buffer.frame_interval_ms = 0;
        is_local_file_ = false;

        const int ret = camera_.open(stream_url_, source_.width,
                                     source_.height, source_.fps,
                                     source_.chroma_order == "vu");
        status = ret;
        if (ret == 0)
        {
            width = camera_.sourceWidth();
            height = camera_.sourceHeight();
        }
        return ret;
    }

    close();
    temp_pkt = av_packet_alloc();
    // av_init_packet is deprecated in FFmpeg 7.x, av_packet_alloc() already initializes the packet
    codecPar = avcodec_parameters_alloc();
    // pFrame = av_frame_alloc();
    // temp_frame = av_frame_alloc();
    if (!temp_pkt || !codecPar)
    {
        std::cerr << "allocate FFmpeg input structures failed" << std::endl;
        close();
        return -1;
    }

    if (source_.type == InputSourceType::Rtsp)
    {
        av_dict_set(&options, "rtbufsize", "8192000", 0);
        av_dict_set(&options, "rtsp_transport", "tcp", 0);
        av_dict_set(&options, "stimeout", "2000000", 0);
        av_dict_set(&options, "max_delay", "500000", 0);
    }

    if (avformat_open_input(&fmtCtx, stream_url_.c_str(), NULL, &options) != 0)
    {
        std::cout << "open " << input_source_type_name(source_.type)
                  << " source failed: " << stream_url_ << std::endl;
        close();
        return -1;
    }
    // 查找RTSP流信息
    if (avformat_find_stream_info(fmtCtx, NULL) < 0)
    {
        close();
        return -1;
    }

    // 打印视频相关信息
    av_dump_format(fmtCtx, 0, stream_url_.c_str(), 0);
    // 获取视频的信息
    videoStreamIndex = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; i++)
    {
        if (fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            width = fmtCtx->streams[i]->codecpar->width;
            height = fmtCtx->streams[i]->codecpar->height;
            videoStreamIndex = i;
            break;
        }
    }
    std::cout << "videoindex: " << videoStreamIndex << std::endl;
    if (videoStreamIndex < 0)
    {
        close();
        return -2;
    }

    AVCodecID input_format = fmtCtx->streams[videoStreamIndex]->codecpar->codec_id;
    int required_decoder_type = 0;
    const char* bitstream_filter = nullptr;
    if (input_format == AV_CODEC_ID_H264)
    {
        required_decoder_type = 264;
        bitstream_filter = "h264_mp4toannexb";
    }
    else if (input_format == AV_CODEC_ID_HEVC)
    {
        required_decoder_type = 265;
        bitstream_filter = "hevc_mp4toannexb";
    }
    else
    {
        std::cerr << "MPP input only supports H.264/H.265, codec id="
                  << input_format << std::endl;
        close();
        return -3;
    }

    if (!decoder_initialized_)
    {
        const int ret = decoder.Init(required_decoder_type, source_.fps,
                                     &(this->buffer), stream_loader_id);
        if (ret <= 0)
        {
            std::cerr << "initialize MPP decoder failed" << std::endl;
            close();
            return -3;
        }
        decoder_initialized_ = true;
        decoder_type_ = required_decoder_type;
    }
    else if (decoder_type_ != required_decoder_type)
    {
        std::cerr << "codec changed after reconnect; restart is required"
                  << std::endl;
        close();
        return -3;
    }

    if (source_.type == InputSourceType::Mp4)
    {
            bsf = av_bsf_get_by_name(bitstream_filter);
            if (!bsf)
            {
                fprintf(stderr, "Could not find MP4 Annex-B filter\n");
                close();
                return -3;
            }

            // 初始化比特流过滤器上下文
            if (av_bsf_alloc(bsf, &bsf_ctx) < 0)
            {
                fprintf(stderr, "Could not allocate bsf context\n");
                close();
                return -3;
            }
            // 设置过滤器参数
            avcodec_parameters_copy(
                bsf_ctx->par_in,
                fmtCtx->streams[videoStreamIndex]->codecpar);
            bsf_ctx->time_base_in = fmtCtx->streams[videoStreamIndex]->time_base;

            if (av_bsf_init(bsf_ctx) < 0)
            {
                fprintf(stderr, "Could not initialize bsf context\n");
                close();
                return -3;
            }
            isnotAnnexB = true;
    }

    decoder.SetCallback(this->callback);
    avcodec_parameters_copy(codecPar, fmtCtx->streams[videoStreamIndex]->codecpar);

    // 获取源视频帧率，用于本地文件限速
    AVStream *st = fmtCtx->streams[videoStreamIndex];
    double fps = av_q2d(st->avg_frame_rate);
    if (fps <= 0) fps = av_q2d(st->r_frame_rate);
    if (fps <= 0) fps = 25.0;
    source_fps_ = fps;

    is_local_file_ = source_.type == InputSourceType::Mp4;

    if (is_local_file_ && source_fps_ > 0) {
        buffer.throttle = true;
        buffer.frame_interval_ms = (int)(1000.0 / source_fps_);
    } else {
        buffer.throttle = false;
        buffer.frame_interval_ms = 0;
    }

    status = 0;
    return 0;
}

void StreamLoader::operator()()
{
    while (!stopFlag)
    {
        if (open() != 0)
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(source_.reconnect_interval_ms));
            continue;
        }

        while (!stopFlag && read_frame())
        {
        }
        close();
        if (stopFlag || (source_.type == InputSourceType::Mp4 && !source_.loop))
        {
            break;
        }
        std::cout << "reopening " << input_source_type_name(source_.type)
                  << " source, stream=" << stream_loader_id << std::endl;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(source_.reconnect_interval_ms));
    }
    stopFlag = true;
}

// ==================================================================================================

void StreamLoaderManager::configure(const vector<StreamSourceConfig>& sources)
{
    sources_ = sources;
    num_stream = static_cast<int>(sources_.size());
}

bool StreamLoaderManager::load_stream(int id)
{
    if (id < 0 || id >= static_cast<int>(sources_.size()))
    {
        std::cerr << "invalid stream id: " << id << std::endl;
        return false;
    }
    std::cout << "Loading stream id: " << id << std::endl;
    StreamLoader *loader = new StreamLoader(sources_[id], id);
    stream_loaders.push_back(loader);
    threads.emplace_back(std::thread(std::ref(*loader)));
    return true;
}

// 卸载流
void StreamLoaderManager::unload_stream(int id)
{
    if (id < 0 || id >= static_cast<int>(stream_loaders.size()))
        return;
    std::cout << "Unloading stream id: " << id << std::endl;
    stream_loaders[id]->stopFlag = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if(threads[id].joinable())
        threads[id].join();
    delete stream_loaders[id];
}
