/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "mpp_encoder.h"
#include "bench_probe.hpp"
#include "im2d.h"
#include <stdio.h>
#include <string.h>
#include <thread>
#include <chrono>

MppEncoder::MppEncoder()
    : mpp_ctx_(NULL), mpp_mpi_(NULL), enc_cfg_(NULL),
      input_buffer_(NULL),
      width_(0), height_(0), fps_(0), bitrate_(0),
      mpp_type_(MPP_VIDEO_CodingAVC), initialized_(false) {
}

MppEncoder::~MppEncoder() {
    Release();
}

int MppEncoder::Init(int width, int height, int fps, int bitrate, int codec_type) {
    MPP_RET ret = MPP_OK;
    MppBufferInfo input_info = {};

    width_ = width;
    height_ = height;
    fps_ = fps;
    bitrate_ = bitrate;

    if (codec_type == 264) {
        mpp_type_ = MPP_VIDEO_CodingAVC;
    } else if (codec_type == 265) {
        mpp_type_ = MPP_VIDEO_CodingHEVC;
    } else {
        fprintf(stderr, "Unsupported codec type: %d\n", codec_type);
        return -1;
    }

    // 创建 MPP 上下文
    ret = mpp_create(&mpp_ctx_, &mpp_mpi_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_create failed ret %d\n", ret);
        return -1;
    }

    // 初始化编码器
    ret = mpp_init(mpp_ctx_, MPP_CTX_ENC, mpp_type_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_init failed ret %d\n", ret);
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
        return -1;
    }

    // 创建编码配置
    ret = mpp_enc_cfg_init(&enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_enc_cfg_init failed ret %d\n", ret);
        goto FAIL;
    }

    // 获取默认配置
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_CFG, enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "get enc cfg failed ret %d\n", ret);
        goto FAIL;
    }

    // 基本图像参数
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:width", width);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:height", height);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:hor_stride", width);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:ver_stride", height);
    mpp_enc_cfg_set_s32(enc_cfg_, "prep:format", MPP_FMT_YUV420SP);

    // 码率控制：简单的 VBR 设置，偏向速度
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:mode", MPP_ENC_RC_MODE_VBR);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_target", bitrate);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_max", bitrate * 2);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:bps_min", bitrate / 2);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_num", fps);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(enc_cfg_, "rc:gop", fps);

    if (mpp_type_ == MPP_VIDEO_CodingAVC) {
        mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", MPP_VIDEO_CodingAVC);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:profile", 66);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:level", 40);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:cabac_en", 0);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_init", 26);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_min", 20);
        mpp_enc_cfg_set_s32(enc_cfg_, "h264:qp_max", 35);
    } else if (mpp_type_ == MPP_VIDEO_CodingHEVC) {
        mpp_enc_cfg_set_s32(enc_cfg_, "codec:type", MPP_VIDEO_CodingHEVC);
        mpp_enc_cfg_set_s32(enc_cfg_, "h265:profile", 1);
        mpp_enc_cfg_set_s32(enc_cfg_, "h265:level", 120);
    }

    // 应用配置
    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_SET_CFG, enc_cfg_);
    if (ret != MPP_OK) {
        fprintf(stderr, "set enc cfg failed ret %d\n", ret);
        goto FAIL;
    }

    input_nv12_ = DmaImageBuffer::createNv12(width, height);
    if (!input_nv12_) {
        fprintf(stderr, "failed to allocate encoder NV12 DMA-BUF\n");
        goto FAIL;
    }

    input_info.type = MPP_BUFFER_TYPE_EXT_DMA;
    input_info.size = input_nv12_->size();
    // EXT_DMA 只通过 DMA-BUF fd 导入，不能同时传入用户态虚拟地址。
    input_info.ptr = NULL;
    input_info.hnd = NULL;
    input_info.fd = input_nv12_->fd();
    input_info.index = 0;
    fprintf(stdout, "Importing encoder DMA-BUF into MPP: fd=%d, size=%zu\n",
            input_info.fd, input_info.size);
    ret = mpp_buffer_import(&input_buffer_, &input_info);
    if (ret != MPP_OK || !input_buffer_) {
        fprintf(stderr, "failed to import encoder DMA-BUF into MPP ret %d\n", ret);
        goto FAIL;
    }

    initialized_ = true;
    fprintf(stdout, "MPP encoder DMA input ready: fd=%d, NV12 %dx%d\n",
            input_nv12_->fd(), width_, height_);
    return 0;

FAIL:
    if (input_buffer_) {
        mpp_buffer_put(input_buffer_);
        input_buffer_ = NULL;
    }
    input_nv12_.reset();
    if (enc_cfg_) {
        mpp_enc_cfg_deinit(enc_cfg_);
        enc_cfg_ = NULL;
    }
    if (mpp_ctx_) {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
    }
    initialized_ = false;
    return -1;
}

int MppEncoder::EncodeFrame(
    const std::shared_ptr<DmaImageBuffer> &bgr_frame,
    uint8_t *packet_data, int *packet_size) {
    if (!initialized_) {
        fprintf(stderr, "Encoder not initialized\n");
        return -1;
    }

    if (!bgr_frame || !bgr_frame->valid() ||
        bgr_frame->format() != RK_FORMAT_BGR_888) {
        fprintf(stderr, "Invalid BGR DMA-BUF passed to encoder\n");
        return -1;
    }
    if (bgr_frame->width() != width_ || bgr_frame->height() != height_) {
        fprintf(stderr, "Frame size mismatch: %dx%d vs %dx%d\n",
                bgr_frame->width(), bgr_frame->height(), width_, height_);
        return -1;
    }
    if (!input_nv12_ || !input_buffer_ ||
        !bgr_frame->syncForDevice() || !input_nv12_->syncForDevice()) {
        fprintf(stderr, "Encoder DMA-BUF is unavailable\n");
        return -1;
    }

    BENCH_SCOPE("encode", -1);   // T6: RGA BGR->NV12 + MPP H.264 编码

    rga_buffer_t source = wrapbuffer_handle_t(
        bgr_frame->rgaHandle(), bgr_frame->width(), bgr_frame->height(),
        bgr_frame->widthStride(), bgr_frame->heightStride(),
        RK_FORMAT_BGR_888);
    rga_buffer_t destination = wrapbuffer_handle_t(
        input_nv12_->rgaHandle(), width_, height_,
        input_nv12_->widthStride(), input_nv12_->heightStride(),
        RK_FORMAT_YCbCr_420_SP);
    const IM_STATUS status = imcvtcolor(
        source, destination, RK_FORMAT_BGR_888,
        RK_FORMAT_YCbCr_420_SP, IM_RGB_TO_YUV_BT601_LIMIT);
    if (status != IM_STATUS_SUCCESS) {
        fprintf(stderr, "RGA DMA BGR->NV12 conversion failed (%d: %s)\n",
                static_cast<int>(status), imStrError_t(status));
        return -1;
    }

    return EncodeNv12(packet_data, packet_size);
}

int MppEncoder::EncodeNv12(uint8_t *packet_data, int *packet_size) {
    if (!initialized_) {
        fprintf(stderr, "Encoder not initialized\n");
        return -1;
    }

    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        fprintf(stderr, "mpp_frame_init failed\n");
        return -1;
    }

    mpp_frame_set_width(frame, width_);
    mpp_frame_set_height(frame, height_);
    mpp_frame_set_hor_stride(frame, width_);
    mpp_frame_set_ver_stride(frame, height_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_buffer(frame, input_buffer_);
    mpp_frame_set_eos(frame, 0);

    ret = mpp_mpi_->encode_put_frame(mpp_ctx_, frame);
    if (ret != MPP_OK) {
        fprintf(stderr, "encode_put_frame failed ret %d\n", ret);
        mpp_frame_deinit(&frame);
        return -1;
    }

    // 轮询获取编码输出，避免帧在编码器内堆积导致后续卡顿
    const int max_retries = 50;
    for (int r = 0; r < max_retries; r++) {
        ret = mpp_mpi_->encode_get_packet(mpp_ctx_, &packet);
        if (ret == MPP_OK && packet) {
            break;
        }
        if (ret != MPP_ERR_TIMEOUT) {
            *packet_size = 0;
            mpp_frame_deinit(&frame);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (ret != MPP_OK || !packet) {
        *packet_size = 0;
        mpp_frame_deinit(&frame);
        return 0;
    }

    void* pkt_data = mpp_packet_get_data(packet);
    size_t pkt_size = mpp_packet_get_length(packet);

    if (pkt_data && pkt_size > 0) {
        if (packet_data && *packet_size >= (int)pkt_size) {
            memcpy(packet_data, pkt_data, pkt_size);
            *packet_size = pkt_size;
        } else {
            *packet_size = 0;
            mpp_packet_deinit(&packet);
            mpp_frame_deinit(&frame);
            return -1;
        }
    } else {
        *packet_size = 0;
    }

    mpp_packet_deinit(&packet);
    mpp_frame_deinit(&frame);

    return 0;
}

int MppEncoder::GetHeader(uint8_t* header_data, int* header_size) {
    if (!initialized_) {
        fprintf(stderr, "Encoder not initialized\n");
        return -1;
    }

    MPP_RET ret = MPP_OK;
    MppPacket packet = NULL;

    ret = mpp_mpi_->control(mpp_ctx_, MPP_ENC_GET_EXTRA_INFO, &packet);
    if (ret != MPP_OK || !packet) {
        fprintf(stderr, "get extra info failed ret %d\n", ret);
        return -1;
    }

    void* pkt_data = mpp_packet_get_data(packet);
    size_t pkt_size = mpp_packet_get_length(packet);

    if (pkt_data && pkt_size > 0) {
        if (header_data && *header_size >= (int)pkt_size) {
            memcpy(header_data, pkt_data, pkt_size);
            *header_size = pkt_size;
            ret = MPP_OK;
        } else {
            fprintf(stderr, "Header buffer too small: %d < %zu\n", *header_size, pkt_size);
            *header_size = 0;
            ret = MPP_NOK;
        }
    } else {
        *header_size = 0;
        ret = MPP_NOK;
    }

    mpp_packet_deinit(&packet);
    return (ret == MPP_OK) ? 0 : -1;
}

void MppEncoder::Release() {
    if (enc_cfg_) {
        mpp_enc_cfg_deinit(enc_cfg_);
        enc_cfg_ = NULL;
    }

    if (input_buffer_) {
        mpp_buffer_put(input_buffer_);
        input_buffer_ = NULL;
    }
    input_nv12_.reset();

    if (mpp_ctx_) {
        mpp_destroy(mpp_ctx_);
        mpp_ctx_ = NULL;
    }

    initialized_ = false;
}
