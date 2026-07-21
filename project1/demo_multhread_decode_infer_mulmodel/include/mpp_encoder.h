/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#ifndef MPP_ENCODER_H
#define MPP_ENCODER_H

#include <stdint.h>
#include <memory>
#include "dma_image.hpp"
#include "rk_mpi.h"
#include "rk_venc_cfg.h"



class MppEncoder {
public:
    MppEncoder();
    ~MppEncoder();

    // 初始化编码器
    // width/height: 编码分辨率
    // fps:          帧率
    // bitrate:      码率 (bps)
    // codec_type:   264(H.264) / 265(H.265)
    int Init(int width, int height, int fps, int bitrate, int codec_type = 264);

    // RGA 从 BGR DMA-BUF 转 NV12 DMA-BUF，MPP 直接导入同一 fd 编码。
    int EncodeFrame(const std::shared_ptr<DmaImageBuffer> &bgr_frame,
                    uint8_t *packet_data, int *packet_size);

    // 获取 SPS / PPS 等头信息（可用于 FFmpeg extradata）
    int GetHeader(uint8_t* header_data, int* header_size);

    // 释放资源
    void Release();

private:
    MppCtx          mpp_ctx_;
    MppApi*         mpp_mpi_;
    MppEncCfg       enc_cfg_;
    MppBuffer       input_buffer_;
    std::shared_ptr<DmaImageBuffer> input_nv12_;

    int             width_;
    int             height_;
    int             fps_;
    int             bitrate_;
    MppCodingType   mpp_type_;
    bool            initialized_;

    int EncodeNv12(uint8_t *packet_data, int *packet_size);
};

#endif // MPP_ENCODER_H
