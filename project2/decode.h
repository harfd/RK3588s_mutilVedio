/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef _DECODE_H_
#define _DECODE_H_
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include "frame_queue.h"
#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "rk_type.h"
#include "mpp_err.h"
#include "mpp_log.h"
#include "mpp_common.h"
#include "mpp_mem.h"
#include "mpp_debug.h"
#include <opencv2/opencv.hpp>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdint>
using namespace cv;

typedef struct
{
    MppCodingType type;    // 编码类型
    RK_U32 width;          // 视频帧的宽度，需要按十六字节对齐
    RK_U32 height;         // 视频帧的高度，需要按十六字节对齐
    MppFrameFormat format; // 视频帧的输入格式
    RK_U32 num_frames;
} MpiEncTestCmd; // 编码的一些设置

typedef struct
{
    /*--------流控制标志--------*/
    RK_U32 frm_eos;     // 输入帧结束标志，置1表示没有更多的帧输入
    RK_U32 pkt_eos;     // 输出包结束的标志，置1表示编码器已经输出最后的数据包
    RK_U32 frame_count; // 已经编码的帧数的计数器
    RK_U32 stream_size; // 已经编码的数据总大小

    /*-------输入/输出文件----------*/
    FILE *fp_input;
    FILE *fp_output;
    /*--------编码资源---------*/
    MppBuffer frm_buf;      // 帧缓冲区，输入帧
    MppEncSeiMode sei_mode; // SEI帧，如 MPP_ENC_SEI_MODE_ONE_FRAME 表示每帧携带 SEI。

    /*------MPP 基础上下文---------*/
    MppCtx ctx;
    MppApi *mpi;
    MppEncPrepCfg prep_cfg;    // 输入控制配置
    MppEncRcCfg rc_cfg;        // 码率控制配置
    MppEncCodecCfg condec_cfg; // 协议控制配置
    /*--------编码参数-----------*/
    RK_U32 width;
    RK_U32 height;
    RK_U32 hor_stride;  // 水平 stride
    RK_U32 ver_stride;  // 垂直stride
    MppFrameFormat fmt; // 输入帧格式
    MppCodingType type; // 编码类型
    RK_U32 num_frames;  // 计划编码的总帧数（0 表示无限编码）
    /*----------资源与缓冲区---------*/
    size_t frame_size;  // 单帧数据大小（计算方式：hor_stride * ver_stride * 3 / 2，YUV420P 格式）。
    size_t packet_size; // 输出数据包缓冲区大小（通常 ≥ width * height，防止溢出）。
    /*---------码率控制运行时参数--------*/
    RK_S32 gop; // GOP 长度
    RK_S32 fps; // 帧率
    RK_S32 bps; // 目标码率
} MpiEncTestData;

static MppApi *mpi = NULL;
static MppCtx ctx = NULL;
extern std::atomic<bool> first_frame_flg;
struct H264Frame
{
    std::vector<std::uint8_t> data; // NALU 数据（包含 Header）
    size_t startSize{};             // 起始码长度
};

// 简单的 MPP 编码器封装类
class MppEncoder
{
public:
    static MppEncoder &instance();

    // 将一帧 YUV420 图像编码为 H264 数据
    MPP_RET encode(int width, int height, Mat yuv_frame,
                   char *(&encode_buf), size_t &encode_length);

private:
    MppEncoder() = default;
};

void print_nalus_in_packet(MppPacket packet);
void YuvtoH264(int width, int height, Mat yuv_frame, char *(&encode_buf), size_t &encode_length);
std::vector<H264Frame> splitNalus(const char *packet, int packetSize);
#endif
