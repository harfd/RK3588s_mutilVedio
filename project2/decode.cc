/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include "decode.h"
MpiEncTestData encoder_params;
MpiEncTestData *encoder_params_ptr = &encoder_params;

MPP_RET test_ctx_init(MpiEncTestData **p_ptr, MpiEncTestCmd *cmd)
{

    MPP_RET ret = MPP_OK;
    MpiEncTestData *p = *p_ptr;
    if (!p)
    {
        p = mpp_calloc(MpiEncTestData, 1);
        if (!p)
        {
            printf("malloc failed\n");
            return MPP_ERR_MALLOC;
        }
        *p_ptr = p; // 返回新分配的结构体
    }
    p->frm_buf = NULL;
    p->width = cmd->width;
    p->height = cmd->height;
    // MPP_ALIGN,如果分配123字节，但是因为需要内存对齐，就会自动分配128
    p->hor_stride = MPP_ALIGN(cmd->width, 16); // 内存中为每行分配的字节数
    p->ver_stride = MPP_ALIGN(cmd->height, 16);
    p->fmt = cmd->format;
    p->type = cmd->type;
    p->num_frames = cmd->num_frames;
    if (p->fmt == MPP_FMT_YUV420P)
    {
        p->frame_size = p->hor_stride * p->ver_stride * 3 / 2;
    }
    else
    {
        printf("传入类型错误\n");
    }

    p->packet_size = p->width * p->height;
    return ret;
}

// 设置编码器参数
// 说明：1-输入控制配置；2-码率控制配置；3-协议控制配置；4-SEI模式配置
MPP_RET test_mpp_setup(MpiEncTestData *p)
{
    MPP_RET ret;
    MppApi *mpi;
    MppCtx ctx;
    MppEncCodecCfg *codec_cfg; // 协议控制配置
    MppEncPrepCfg *prep_cfg;   // 输入控制配置
    MppEncRcCfg *rc_cfg;       // 码率控制配置
    mpi = p->mpi;
    ctx = p->ctx;
    codec_cfg = &p->condec_cfg;
    prep_cfg = &p->prep_cfg;
    rc_cfg = &p->rc_cfg;
    p->fps = 30;          // 帧率设置为30
    p->gop = 30;          // 一组gop大小设置为60
    p->bps = 4096 * 1024; // 码率设置为4M，分辨率大概为720P

    // 添加 slice 分割配置

    // 1--输入控制配置

    prep_cfg->change = MPP_ENC_PREP_CFG_CHANGE_INPUT | MPP_ENC_PREP_CFG_CHANGE_ROTATION | MPP_ENC_PREP_CFG_CHANGE_FORMAT;
    prep_cfg->width = p->width;
    prep_cfg->height = p->height;
    prep_cfg->hor_stride = p->hor_stride;
    prep_cfg->ver_stride = p->ver_stride;
    prep_cfg->format = p->fmt;
    prep_cfg->rotation = MPP_ENC_ROT_0;
    ret = mpi->control(ctx, MPP_ENC_SET_PREP_CFG, prep_cfg);
    if (ret)
    {
        printf("mpi control enc set prep cfg failed ret %d\n", ret);
        return ret;
    }

    // 2、码率控制设置
    // 码率控制设置
    rc_cfg->change = MPP_ENC_RC_CFG_CHANGE_ALL; // 标记所有参数需要更新
    rc_cfg->rc_mode = MPP_ENC_RC_MODE_VBR;      // 可变码率模式
    rc_cfg->quality = MPP_ENC_RC_QUALITY_CQP;   // CQP 模式（需在 VBR 下）

    // 帧率配置（必须有效值）
    rc_cfg->fps_in_flex = 0;  // 固定帧率
    rc_cfg->fps_in_num = 30;  // 必须 > 0（如30fps）
    rc_cfg->fps_in_denom = 1; // 分母
    rc_cfg->fps_out_flex = 0; // 输出帧率（同输入）
    rc_cfg->fps_out_num = 30;
    rc_cfg->fps_out_denom = 1;

    // GOP 和跳过帧配置
    rc_cfg->gop = 30;     // 关键帧间隔
    rc_cfg->skip_cnt = 0; // 不跳帧

    // CQP 模式需设置 QP 范围（必须！）
    rc_cfg->qp_min = 20; // 最小QP（0-51）
    rc_cfg->qp_max = 51; // 最大QP

    // VBR 模式下可选的码率参数（CQP模式下这些参数可能被忽略）
    rc_cfg->bps_target = p->bps;  // 目标码率（可选）
    rc_cfg->bps_max = p->bps * 2; // 最大码率（可选）
    rc_cfg->bps_min = p->bps / 2; // 最小码率（可选）

    ret = mpi->control(ctx, MPP_ENC_SET_RC_CFG, rc_cfg);
    if (ret != MPP_OK)
    {
        printf("MPP_ENC_SET_RC_CFG failed! ret=%d\n", ret);
        return ret;
    }

    // 3--协议控制配置
    codec_cfg->coding = p->type; // 设置编码格式

    // codec_cfg->h264.change = MPP_ENC_H264_CFG_CHANGE_PROFILE | MPP_ENC_H264_CFG_CHANGE_ENTROPY | MPP_ENC_H264_CFG_CHANGE_TRANS_8x8;
    //  是否需要B帧
    /*
        66：Baseline Profile（低复杂度，无 B帧/CABAC）。

        77：Main Profile（支持 B帧和 CABAC，广泛使用）。

        100：High Profile（支持 8x8 变换，更高压缩率）
    */
    // 此处不需要B帧，否则后面推到国标平台会出现卡顿的情况
    codec_cfg->h264.profile = 66;

    /*
     * H.264 level_idc parameter
     * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
     * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
     * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
     * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
     * 50 / 51 / 52         - 4K@30fps
     */
    codec_cfg->h264.level = 31;
    // 配置熵编码模式
    codec_cfg->h264.entropy_coding_mode = 1; //// 1=CABAC, 0=CAVLC
    codec_cfg->h264.cabac_init_idc = 0;      // // CABAC初始化参数
    ret = mpi->control(ctx, MPP_ENC_SET_CODEC_CFG, codec_cfg);
    if (ret)
    {
        printf("mpi control enc set codec cfg failed ret %d\n", ret);
        return ret;
    }

    // 4--SEI模式配置
    p->sei_mode = MPP_ENC_SEI_MODE_DISABLE; // 每帧视频数据都插入一次SEI信息 默认
    ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &p->sei_mode);
    if (ret)
    {
        printf("mpi control enc set sei cfg failed ret %d\n", ret);
        return ret;
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_DEFAULT; // 只在IDR帧生成头信息
    ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
    if (ret)
    {
        printf("mpi control enc set header mode failed ret %d\n", ret);
        return ret;
    }
    return ret;
}

// 功能：将YUV420视频帧数据填充到MPP buffer
// 说明：使用16字节对齐，MPP可以实现零拷贝，提高效率
// 强化 read_yuv_buffer 的边界检查
int read_yuv_buffer(RK_U8 *buf, cv::Mat &yuvImg, RK_U32 width, RK_U32 height)
{
    // 计算理论所需大小
    size_t y_size = width * height;
    size_t uv_size = y_size / 4;
    size_t total_needed = y_size + 2 * uv_size;

    // 验证目标缓冲区大小
    size_t buf_size = MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16) * 3 / 2;
    if (buf_size < total_needed)
    {
        throw std::runtime_error("Target buffer too small");
    }

    // 验证源数据大小
    if (yuvImg.total() * yuvImg.elemSize() < total_needed)
    {
        throw std::runtime_error("Source YUV data incomplete");
    }

    // 安全拷贝（添加范围检查）
    RK_U8 *buf_y = buf;
    RK_U8 *buf_u = buf + MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16);
    RK_U8 *buf_v = buf_u + MPP_ALIGN(width, 16) * MPP_ALIGN(height, 16) / 4;

    const RK_U8 *src = yuvImg.data;
    memcpy(buf_y, src, y_size);
    memcpy(buf_u, src + y_size, uv_size);
    memcpy(buf_v, src + y_size + uv_size, uv_size);
    return 0;
}

// 进行mpp编码
/*
    @param yuvImg:输出的数据
*/
MPP_RET test_mpp_run_yuv(Mat yuvImg, MppApi *mpi, MppCtx &ctx,
                         unsigned char *&H264_buf, int &length)
{
    // 参数检查
    if (!mpi || !encoder_params_ptr || yuvImg.empty())
    {
        printf("Invalid parameters\n");
        return MPP_ERR_NULL_PTR;
    }

    MpiEncTestData *p = encoder_params_ptr;
    MPP_RET ret = MPP_OK;
    MppFrame frame = NULL;
    MppPacket packet = NULL;

    auto encode_start = std::chrono::high_resolution_clock::now();

    // 1. 准备输入数据
    void *buf = mpp_buffer_get_ptr(p->frm_buf);
    if (!buf)
    {
        printf("Failed to get frame buffer pointer\n");
        return MPP_ERR_NULL_PTR;
    }

    // 2. 读取YUV数据
    if (read_yuv_buffer((RK_U8 *)buf, yuvImg, p->width, p->height) != 0)
    {
        printf("Failed to read yuv buffer\n");
        return MPP_ERR_VALUE;
    }

    // 3. 初始化并配置帧
    ret = mpp_frame_init(&frame);
    if (ret)
    {
        printf("mpp_frame_init failed: %d\n", ret);
        goto FAIL;
    }

    mpp_frame_set_width(frame, p->width);
    mpp_frame_set_height(frame, p->height);
    mpp_frame_set_hor_stride(frame, p->hor_stride);
    mpp_frame_set_ver_stride(frame, p->ver_stride);
    mpp_frame_set_fmt(frame, p->fmt);
    mpp_frame_set_buffer(frame, p->frm_buf);
    mpp_frame_set_eos(frame, p->frm_eos);

    // 4. 编码处理
    ret = mpi->encode_put_frame(ctx, frame);
    if (ret)
    {
        printf("encode_put_frame failed: %d\n", ret);
        goto FAIL;
    }

    // 5. 获取编码结果
    ret = mpi->encode_get_packet(ctx, &packet);
    if (ret)
    {
        printf("encode_get_packet failed: %d\n", ret);
        goto FAIL;
    }

    if (packet)
    {
        // 6. 处理编码数据
        void *ptr = mpp_packet_get_pos(packet);
        size_t len = mpp_packet_get_length(packet);

        if (!ptr || len <= 0)
        {
            printf("Invalid packet data\n");
            ret = MPP_ERR_VALUE;
            goto FAIL;
        }

        // 7. 确保输出缓冲区足够大
        if (H264_buf)
        {
            free(H264_buf); // 释放旧缓冲区
            H264_buf = nullptr;
        }

        H264_buf = (unsigned char *)malloc(len);
        if (!H264_buf)
        {
            printf("Failed to allocate output buffer\n");
            ret = MPP_ERR_MALLOC;
            goto FAIL;
        }

        memcpy(H264_buf, ptr, len);
        length = len;

        // 8. 调试信息
        static int frame_count = 0;
        frame_count++;
        auto encode_end = std::chrono::high_resolution_clock::now();
        auto encode_duration = std::chrono::duration_cast<std::chrono::microseconds>(encode_end - encode_start);

        // printf("Encoded frame %d, size: %zu, time: %lld us\n",
        //        frame_count, len, encode_duration.count());

        // 9. 更新统计信息
        p->pkt_eos = mpp_packet_get_eos(packet);
        p->stream_size += len;
        p->frame_count++;

        if (p->pkt_eos)
        {
            printf("Found last packet\n");
            mpp_assert(p->frm_eos);
        }
    }

FAIL:
    // 10. 资源清理
    if (frame)
    {
        mpp_frame_deinit(&frame);
    }
    if (packet)
    {
        mpp_packet_deinit(&packet);
    }

    return ret;
}

// 初始化编码器并且获得sps和pps
MpiEncTestData *test_mpp_run_yuv_init(MpiEncTestData *p, int width, int height)
{
    printf("init \n");

    MPP_RET ret;
    MpiEncTestCmd cmd;
    cmd.width = width;
    cmd.height = height;
    cmd.type = MPP_VIDEO_CodingAVC;
    cmd.format = MPP_FMT_YUV420P;
    cmd.num_frames = 0;
    ret = test_ctx_init(&p, &cmd);
    if (ret)
    {
        printf("test data init failed ret %d\n", ret);
        return p;
    }
    // 申请缓冲区存储待存储的数据
    ret = mpp_buffer_get(NULL, &p->frm_buf, p->frame_size);
    if (ret)
    {
        printf("failed to get buffer for input frame ret %d\n", ret);
        return p;
    }
    ret = mpp_create(&p->ctx, &p->mpi);
    if (ret)
    {
        printf("mpp_create failed ret %d\n", ret);
        return p;
    }
    printf("ctx = %p, mpi = %p\n", p->ctx, p->mpi);
    ret = mpp_init(p->ctx, MPP_CTX_ENC, p->type);
    if (ret)
    {
        printf("mpp_init failed ret %d\n", ret);
        return p;
    }
    // 设置参数
    ret = test_mpp_setup(p);

    if (ret)
    {
        printf("test mpp setup failed ret %d\n", ret);
        return p;
    }
    mpi = p->mpi;
    ctx = p->ctx;

    return p;
}

MppEncoder &MppEncoder::instance()
{
    static MppEncoder encoder;
    return encoder;
}

// 将YUV420格式图像帧编码为H264数据包
// 首帧需要有 SPS 和 PPS
MPP_RET MppEncoder::encode(int width, int height, Mat yuv_frame,
                           char *(&encode_buf), size_t &encode_length)
{
    unsigned char *H264_buf = NULL;
    int H264_buf_length = 0;
    MPP_RET ret = MPP_OK;

    if (first_frame_flg.load())
    {
        printf("first_packet! \n");
        // 初始化编码器
        encoder_params_ptr = test_mpp_run_yuv_init(encoder_params_ptr, width, height);
        // 编码第一帧（关键帧）, 参数不变的情况下，只有一个 sps 和 pps
        ret = test_mpp_run_yuv(yuv_frame, mpi, ctx, H264_buf, H264_buf_length);
        if (ret)
        {
            printf("test_mpp_run_yuv first frame failed: %d\n", ret);
        }
        else
        {
            memcpy(encode_buf, H264_buf, H264_buf_length);
            encode_length = static_cast<size_t>(H264_buf_length);
            first_frame_flg.store(false);
        }
    }
    else
    {
        ret = test_mpp_run_yuv(yuv_frame, mpi, ctx, H264_buf, H264_buf_length);
        if (ret)
        {
            printf("test_mpp_run_yuv frame failed: %d\n", ret);
        }
        else
        {
            memcpy(encode_buf, H264_buf, H264_buf_length);
            encode_length = static_cast<size_t>(H264_buf_length);
        }
    }

    if (H264_buf)
    {
        delete H264_buf;
        H264_buf = nullptr;
    }

    return ret;
}

// 兼容旧接口：直接通过 MppEncoder 封装调用
void YuvtoH264(int width, int height, Mat yuv_frame, char *(&encode_buf), size_t &encode_length)
{
    (void)MppEncoder::instance().encode(width, height, yuv_frame, encode_buf, encode_length);
}
/**
 * @brief 分割H.264数据流为独立的NALU单元
 * @param data 输入H.264数据指针
 * @param size 输入数据大小(字节)
 * @return 包含所有NALU的vector，每个NALU包含起始码
 */
std::vector<H264Frame> splitNalus(const char *packet, int packetSize)
{
    std::vector<H264Frame> nalus;
    size_t pos = 0;
    size_t m_startCodeSize = 0;
    while (pos < packetSize)
    {
        // 查找起始码（0x00 00 01 或 0x00 00 00 01）
        if (pos + 3 > packetSize)
            break;
        size_t startCodeSize = 0;
        // 检查 3-Byte 起始码
        if (packet[pos] == 0x00 && packet[pos + 1] == 0x00 && packet[pos + 2] == 0x01)
        {
            startCodeSize = 3;
            m_startCodeSize = startCodeSize;
        }
        // 检查 4-Byte 起始码
        else if (pos + 4 <= packetSize &&
                 packet[pos] == 0x00 && packet[pos + 1] == 0x00 &&
                 packet[pos + 2] == 0x00 && packet[pos + 3] == 0x01)
        {
            startCodeSize = 4;
            m_startCodeSize = startCodeSize;
        }
        else
        {
            pos++; // 未找到起始码，继续搜索
            continue;
        }

        // 记录当前 NALU 的起始位置（包含起始码）
        size_t naluStart = pos;
        pos += startCodeSize; // 跳过起始码

        // 查找下一个起始码（确定当前 NALU 的结束位置）
        while (pos < packetSize)
        {
            if (pos + 3 <= packetSize &&
                packet[pos] == 0x00 && packet[pos + 1] == 0x00 && packet[pos + 2] == 0x01)
            {
                break;
            }
            if (pos + 4 <= packetSize &&
                packet[pos] == 0x00 && packet[pos + 1] == 0x00 &&
                packet[pos + 2] == 0x00 && packet[pos + 3] == 0x01)
            {
                break;
            }
            pos++;
        }

        // 提取 NALU（包含起始码）
        size_t naluSize = pos - naluStart;
        if (naluSize > 0)
        {
            H264Frame nalu;
            nalu.data.resize(naluSize);
            memcpy(nalu.data.data(), packet + naluStart, naluSize);
            nalu.startSize = m_startCodeSize;
            nalus.push_back(std::move(nalu));
        }
    }
    return nalus;
}
