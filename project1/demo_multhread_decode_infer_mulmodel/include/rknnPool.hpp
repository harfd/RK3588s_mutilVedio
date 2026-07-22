/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#ifndef _rknnPool_H
#define _rknnPool_H

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <queue>
#include <vector>
#include <iostream>
#include <string>
#include "rga.h"
#include "im2d.h"
#include "RgaUtils.h"
#include "rknn_api.h"
#include "postprocess.h"
#include "dma_image.hpp"
#include "bench_probe.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/imgproc.hpp"
#include "ThreadPool.hpp"
using cv::Mat;
using std::queue;
using std::vector;

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz);
static unsigned char *load_model(const char *filename, int *model_size);

class rknn_lite
{
private:
    rknn_context rkModel = 0;
    unsigned char *model_data = nullptr;
    rknn_sdk_version version;
    rknn_input_output_num io_num;
    rknn_tensor_attr *input_attrs;
    rknn_tensor_attr *output_attrs;
    int ret;
    int channel = 3;
    int width = 0;
    int height = 0;
    int class_num = 0;
    float box_conf_threshold_ = BOX_THRESH;
    float nms_threshold_ = NMS_THRESH;
    std::shared_ptr<DmaImageBuffer> input_dma_;
    std::shared_ptr<DmaImageBuffer> source_dma_;
    rknn_tensor_mem *input_mem_ = nullptr;
    int model_id_ = -1;   // 基准埋点标签(模型角色: 0=person/1=helmet/3=callplay)
    int input_stride_ = 0;
#ifdef TRANSFER_MODE_COPY
    std::vector<uint8_t> input_rgb_;  // 深拷贝: 堆上 RGB 输入, 每帧经 rknn_inputs_set 拷入
#endif

public:
    Mat ori_img;
    void setInputFrame(const std::shared_ptr<DmaImageBuffer> &frame);
    int interf(detect_result_group_t &detect_result_group);
    rknn_lite(const std::string& model_name, int n, int class_num, int id,
              float box_conf_threshold, float nms_threshold);
    ~rknn_lite();
};

rknn_lite::rknn_lite(const std::string& model_name, int n, int class_num,
                     int id, float box_conf_threshold, float nms_threshold)
{
    this->class_num = class_num;
    model_id_ = id;
    box_conf_threshold_ = box_conf_threshold;
    nms_threshold_ = nms_threshold;
    /* Create the neural network */
    printf("Loading model id = %d\n", id);
    int model_data_size = 0;
    // 读取模型文件数据
    model_data = load_model(model_name.c_str(), &model_data_size);
    if (!model_data || model_data_size <= 0)
    {
        fprintf(stderr, "Failed to load RKNN model: %s\n", model_name.c_str());
        exit(-1);
    }
    // 通过模型文件初始化rknn类
    ret = rknn_init(&rkModel, model_data, model_data_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        exit(-1);
    }
    rknn_core_mask core_mask;
    if (n == 0)
        core_mask = RKNN_NPU_CORE_0;
    else if (n == 1)
        core_mask = RKNN_NPU_CORE_1;
    else
        core_mask = RKNN_NPU_CORE_2;
    ret = rknn_set_core_mask(rkModel, core_mask);
    if (ret < 0)
    {
        printf("rknn_set_core_mask error ret=%d\n", ret);
        exit(-1);
    }

    // 初始化rknn类的版本
    ret = rknn_query(rkModel, RKNN_QUERY_SDK_VERSION, &version, sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        exit(-1);
    }

    // 获取模型的输入参数
    ret = rknn_query(rkModel, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        exit(-1);
    }

    // 设置输入数组
    input_attrs = new rknn_tensor_attr[io_num.n_input];
    memset(input_attrs, 0, sizeof(rknn_tensor_attr) * io_num.n_input);
    for (uint32_t i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(rkModel, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            exit(-1);
        }
    }

    // 设置输出数组
    output_attrs = new rknn_tensor_attr[io_num.n_output];
    memset(output_attrs, 0, sizeof(rknn_tensor_attr) * io_num.n_output);
    for (uint32_t i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(rkModel, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
    }

    // 设置输入参数
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        channel = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
        width = input_attrs[0].dims[3];
    }
    else
    {
        height = input_attrs[0].dims[1];
        width = input_attrs[0].dims[2];
        channel = input_attrs[0].dims[3];
    }
    if (channel != 3)
    {
        fprintf(stderr, "DMA preprocessing requires a 3-channel RKNN input\n");
        exit(-1);
    }

    // 模型输入张量参数。
    input_attrs[0].type = RKNN_TENSOR_UINT8;
    input_attrs[0].fmt = RKNN_TENSOR_NHWC;
    input_attrs[0].pass_through = 0;
    const int input_stride = input_attrs[0].w_stride > 0
                                 ? static_cast<int>(input_attrs[0].w_stride)
                                 : width;
    input_stride_ = input_stride;
    const size_t minimum_input_size =
        static_cast<size_t>(input_stride) * height * channel;
    const size_t input_size = std::max(
        minimum_input_size,
        static_cast<size_t>(input_attrs[0].size_with_stride));
#ifndef TRANSFER_MODE_COPY
    // DMA 零拷贝: RGA 直接写入外部 DMA-BUF, RKNN 通过 fd 读取, 运行时不复制输入。
    input_dma_ = DmaImageBuffer::create(
        width, height, input_stride, height,
        RK_FORMAT_RGB_888, input_size);
    if (!input_dma_)
    {
        fprintf(stderr, "Failed to allocate RKNN input DMA-BUF\n");
        exit(-1);
    }
    input_mem_ = rknn_create_mem_from_fd(
        rkModel, input_dma_->fd(), input_dma_->data(),
        static_cast<uint32_t>(input_dma_->size()), 0);
    if (!input_mem_)
    {
        fprintf(stderr, "rknn_create_mem_from_fd failed\n");
        exit(-1);
    }
    ret = rknn_set_io_mem(rkModel, input_mem_, &input_attrs[0]);
    if (ret < 0)
    {
        fprintf(stderr, "rknn_set_io_mem failed ret=%d\n", ret);
        exit(-1);
    }
    printf("RKNN model id=%d input DMA-BUF fd=%d, tensor=%dx%dx%d, stride=%d\n",
           id, input_dma_->fd(), width, height, channel, input_stride);
#else
    // 深拷贝: RGA 写入堆缓冲, 每帧经 rknn_inputs_set 复制进 RKNN 内部输入。
    input_rgb_.resize(input_size);
    printf("RKNN model id=%d input HEAP copy, tensor=%dx%dx%d, stride=%d\n",
           id, width, height, channel, input_stride);
#endif
}

rknn_lite::~rknn_lite()
{
    source_dma_.reset();
    if (input_mem_)
    {
        rknn_destroy_mem(rkModel, input_mem_);
        input_mem_ = nullptr;
    }
    input_dma_.reset();
    ret = rknn_destroy(rkModel);
    delete[] input_attrs;
    delete[] output_attrs;
    if (model_data)
        free(model_data);
}

void rknn_lite::setInputFrame(
    const std::shared_ptr<DmaImageBuffer> &frame)
{
    source_dma_ = frame;
    ori_img = frame ? frame->bgrView() : cv::Mat();
}

int rknn_lite::interf(detect_result_group_t &detect_result_group)
{
#ifndef TRANSFER_MODE_COPY
    if (!source_dma_ || !input_dma_ ||
        !source_dma_->syncForDevice() || !input_dma_->syncForDevice())
        return -1;
#else
    if (!source_dma_ || !source_dma_->syncForDevice())
        return -1;
#endif

    const int img_width = source_dma_->width();
    const int img_height = source_dma_->height();
#ifndef TRANSFER_MODE_COPY
    rga_buffer_t source = wrapbuffer_handle_t(
        source_dma_->rgaHandle(), img_width, img_height,
        source_dma_->widthStride(), source_dma_->heightStride(),
        RK_FORMAT_BGR_888);
    rga_buffer_t destination = wrapbuffer_handle_t(
        input_dma_->rgaHandle(), width, height,
        input_dma_->widthStride(), input_dma_->heightStride(),
        RK_FORMAT_RGB_888);
#else
    // 深拷贝: RGA 通过虚拟地址访问(无 fd 零拷贝), 目标为堆缓冲。
    rga_buffer_t source = wrapbuffer_virtualaddr_t(
        source_dma_->data(), img_width, img_height,
        source_dma_->widthStride(), source_dma_->heightStride(),
        RK_FORMAT_BGR_888);
    rga_buffer_t destination = wrapbuffer_virtualaddr_t(
        input_rgb_.data(), width, height,
        input_stride_, height, RK_FORMAT_RGB_888);
#endif
    const rga_buffer_t empty_buffer = {};
    const im_rect source_rect = {0, 0, img_width, img_height};
    const im_rect destination_rect = {0, 0, width, height};
    const im_rect empty_rect = {};
    IM_STATUS rga_status;
    {
        BENCH_SCOPE("rknn_pre", model_id_);   // T3: RGA BGR->RGB 预处理
        rga_status = improcess(
            source, destination, empty_buffer,
            source_rect, destination_rect, empty_rect, IM_SYNC);
    }
    if (rga_status != IM_STATUS_SUCCESS)
    {
        fprintf(stderr, "RGA RKNN input preprocessing failed (%d: %s)\n",
                static_cast<int>(rga_status), imStrError_t(rga_status));
        return -1;
    }

    if (io_num.n_output < 3)
    {
        fprintf(stderr, "RKNN model returned only %u output tensor(s), expected at least 3\n",
                io_num.n_output);
        return -1;
    }

#ifdef TRANSFER_MODE_COPY
    // 深拷贝: 每帧把堆上的 RGB 输入复制进 RKNN 内部输入张量。
    {
        BENCH_SCOPE("rknn_set", model_id_);
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = static_cast<uint32_t>(input_rgb_.size());
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].pass_through = 0;
        inputs[0].buf = input_rgb_.data();
        ret = rknn_inputs_set(rkModel, 1, inputs);
        BENCH_ADD_COPY(input_rgb_.size());
    }
    if (ret < 0)
    {
        fprintf(stderr, "rknn_inputs_set failed ret=%d\n", ret);
        return -1;
    }
#endif

    // 设置输出
    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < io_num.n_output; i++)
        outputs[i].want_float = 0; // 调用npu进行推演
    {
        BENCH_SCOPE("rknn_run", model_id_);   // T3: NPU 推理 (两变体应一致)
        ret = rknn_run(rkModel, NULL);
    }
    if (ret < 0)
    {
        fprintf(stderr, "rknn_run failed ret=%d\n", ret);
        return -1;
    }

    // 获取npu的推演输出结果
    ret = rknn_outputs_get(rkModel, io_num.n_output, outputs, NULL);
    if (ret < 0)
    {
        fprintf(stderr, "rknn_outputs_get failed ret=%d\n", ret);
        return -1;
    }
    if (!outputs[0].buf || !outputs[1].buf || !outputs[2].buf)
    {
        fprintf(stderr, "rknn_outputs_get returned a null output buffer\n");
        rknn_outputs_release(rkModel, io_num.n_output, outputs);
        return -1;
    }

    float scale_w = (float)width / img_width;
    float scale_h = (float)height / img_height;

    std::vector<float> out_scales;
    std::vector<int32_t> out_zps;
    for (uint32_t i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    const int postprocess_ret = post_process(
        (int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf,
        (int8_t *)outputs[2].buf, height, width,
        box_conf_threshold_, nms_threshold_, scale_w, scale_h,
        out_zps, out_scales, &detect_result_group, class_num);

    ret = rknn_outputs_release(rkModel, io_num.n_output, outputs);
    if (postprocess_ret != 0)
    {
        fprintf(stderr, "RKNN post_process failed ret=%d\n", postprocess_ret);
        return -1;
    }
    if (ret < 0)
    {
        fprintf(stderr, "rknn_outputs_release failed ret=%d\n", ret);
        return -1;
    }

    return 0;
}

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(const char *filename, int *model_size)
{
    FILE *fp;
    unsigned char *data;

    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

#endif
