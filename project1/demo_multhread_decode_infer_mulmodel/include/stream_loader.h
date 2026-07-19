/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#pragma once
extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
}
#include "mpp_decoder.h"
#include "v4l2_camera.h"
#include <opencv2/opencv.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <queue>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <string>
using std::queue;
using std::vector;
#include "app_config.h"
#include "m_buffer.hpp"


using MppDecoderFrameCallback = std::function<void(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)>;


class StreamLoader
{
public:
    MppDecoder decoder;
    // 视频流的数据起始地址的索引
    int videoStreamIndex = -1;
    AVDictionary *options = NULL;
    AVFormatContext *fmtCtx = NULL;
    AVCodecParameters *codecPar = NULL;

    AVBSFContext *bsf_ctx = NULL;
    const AVBitStreamFilter *bsf = nullptr;

    // 是否已经读取到关键帧
    bool got_key_frame = false;
    // 存储接受流数据的临时结构，内存在构造函数中申请
    AVPacket *temp_pkt = nullptr;
    // 当前包的编号
    int current_pkt_id = 0;
    // 当前对象的唯一标识号
    int stream_loader_id;
    // 输入源配置
    StreamSourceConfig source_;
    StreamSourceConfig source_;
    std::string stream_url_;
    int width = 0;
    int height = 0;
    int status = 0;
    bool isnotAnnexB = false;
    bool decoder_initialized_ = false;
    int decoder_type_ = 0;
    MppDecoderFrameCallback callback;

    // 管理图像数据
    Mbuffer buffer;

    std::atomic<bool> stopFlag;

    // 本地文件播放时按源视频帧率限速，避免倍速
    double source_fps_ = 25.0;
    bool is_local_file_ = false;
    bool is_v4l2_camera_ = false;
    V4L2Camera camera_;

    void close();
    bool read_frame();
    StreamLoader(const StreamSourceConfig& source, int id);
    ~StreamLoader();
    int open();
    void operator()();
    void update_queue();
};

class StreamLoaderManager
{
public:
    int num_stream = 0;
    // 禁止拷贝构造和赋值操作
    StreamLoaderManager(const StreamLoaderManager &) = delete;
    StreamLoaderManager &operator=(const StreamLoaderManager &) = delete;

    // 获取单例实例的静态方法
    static StreamLoaderManager &getInstance()
    {
        static StreamLoaderManager instance; // C++11 保证了静态局部变量的线程安全性
        return instance;
    }

    void configure(const vector<StreamSourceConfig>& sources);
    // 加载流
    bool load_stream(int id);
    // 停止流
    // 该函数没有使用
    void unload_stream(int id);

    vector<StreamLoader *> stream_loaders;
    vector<std::thread> threads;
    vector<StreamSourceConfig> sources_;

private:
    // 私有构造函数，防止从外部创建对象
    StreamLoaderManager()
    {
        std::cout << "StreamLoaderManager created" << std::endl;
    }

    // 私有析构函数，防止外部删除对象
    ~StreamLoaderManager()
    {
        std::cout << "StreamLoaderManager destroyed" << std::endl;
    }
};
