// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
inline int clamp(float value, int minimum, int maximum)
{
    return value > minimum
               ? (value < maximum ? static_cast<int>(value) : maximum)
               : minimum;
}

float calculateOverlap(float xmin0, float ymin0, float xmax0, float ymax0,
                       float xmin1, float ymin1, float xmax1, float ymax1)
{
    const float width =
        std::fmax(0.0f, std::fmin(xmax0, xmax1) - std::fmax(xmin0, xmin1) + 1.0f);
    const float height =
        std::fmax(0.0f, std::fmin(ymax0, ymax1) - std::fmax(ymin0, ymin1) + 1.0f);
    const float intersection = width * height;
    const float area =
        (xmax0 - xmin0 + 1.0f) * (ymax0 - ymin0 + 1.0f) +
        (xmax1 - xmin1 + 1.0f) * (ymax1 - ymin1 + 1.0f) -
        intersection;
    return area <= 0.0f ? 0.0f : intersection / area;
}

void nms(int valid_count, const std::vector<float> &boxes,
         const std::vector<int> &class_ids, std::vector<int> &order,
         int filter_id, float threshold)
{
    for (int i = 0; i < valid_count; ++i)
    {
        if (order[i] == -1 || class_ids[order[i]] != filter_id)
            continue;

        const int first = order[i];
        for (int j = i + 1; j < valid_count; ++j)
        {
            const int second = order[j];
            if (second == -1 || class_ids[second] != filter_id)
                continue;

            const float first_x2 = boxes[first * 4] + boxes[first * 4 + 2];
            const float first_y2 = boxes[first * 4 + 1] + boxes[first * 4 + 3];
            const float second_x2 = boxes[second * 4] + boxes[second * 4 + 2];
            const float second_y2 = boxes[second * 4 + 1] + boxes[second * 4 + 3];
            const float iou = calculateOverlap(
                boxes[first * 4], boxes[first * 4 + 1], first_x2, first_y2,
                boxes[second * 4], boxes[second * 4 + 1], second_x2, second_y2);
            if (iou > threshold)
                order[j] = -1;
        }
    }
}

float tensorValue(int8_t value, int32_t zero_point, float scale)
{
    return (static_cast<float>(value) - static_cast<float>(zero_point)) * scale;
}

float tensorValue(float value, int32_t, float)
{
    return value;
}

template <typename TensorType>
int processHead(const TensorType *input, const int *anchor,
                int grid_h, int grid_w, int stride,
                 std::vector<float> &boxes, std::vector<float> &scores,
                 std::vector<int> &class_ids, float threshold,
                 int32_t zero_point, float scale, int class_count,
                 const std::vector<std::string> &labels)
{
    int valid_count = 0;
    const int grid_length = grid_h * grid_w;
    const int attributes = 5 + class_count;

    for (int anchor_index = 0; anchor_index < 3; ++anchor_index)
    {
        for (int row = 0; row < grid_h; ++row)
        {
            for (int column = 0; column < grid_w; ++column)
            {
                const int cell = row * grid_w + column;
                const int offset = attributes * anchor_index * grid_length + cell;
                const TensorType *current = input + offset;
                const float object_confidence =
                    tensorValue(current[4 * grid_length], zero_point, scale);
                if (object_confidence < threshold)
                    continue;

                int best_class = -1;
                float best_class_probability = -1.0f;
                for (int class_id = 0; class_id < class_count; ++class_id)
                {
                    if (labels[class_id].empty())
                        continue;
                    const float probability = tensorValue(
                        current[(5 + class_id) * grid_length],
                        zero_point, scale);
                    if (probability > best_class_probability)
                    {
                        best_class = class_id;
                        best_class_probability = probability;
                    }
                }

                if (best_class < 0)
                    continue;
                const float confidence =
                    object_confidence * best_class_probability;
                // 保持原三模型流程的阈值语义：目标分数和类别分数分别过阈值。
                if (best_class_probability < threshold)
                    continue;

                float box_x =
                    tensorValue(current[0], zero_point, scale) * 2.0f - 0.5f;
                float box_y =
                    tensorValue(current[grid_length], zero_point, scale) * 2.0f - 0.5f;
                float box_w =
                    tensorValue(current[2 * grid_length], zero_point, scale) * 2.0f;
                float box_h =
                    tensorValue(current[3 * grid_length], zero_point, scale) * 2.0f;
                box_x = (box_x + column) * static_cast<float>(stride);
                box_y = (box_y + row) * static_cast<float>(stride);
                box_w = box_w * box_w * static_cast<float>(anchor[anchor_index * 2]);
                box_h = box_h * box_h * static_cast<float>(anchor[anchor_index * 2 + 1]);

                boxes.push_back(box_x - box_w / 2.0f);
                boxes.push_back(box_y - box_h / 2.0f);
                boxes.push_back(box_w);
                boxes.push_back(box_h);
                scores.push_back(confidence);
                class_ids.push_back(best_class);
                ++valid_count;
            }
        }
    }
    return valid_count;
}

template <typename TensorType>
int postProcessImpl(TensorType *input0, TensorType *input1, TensorType *input2,
                    int model_height, int model_width,
                    float confidence_threshold, float nms_threshold,
                    float scale_width, float scale_height,
                    const std::vector<int32_t> &zero_points,
                    const std::vector<float> &quant_scales,
                    detect_result_group_t *group, int class_count,
                    const std::vector<int> &anchors,
                    const std::vector<std::string> &labels)
{
    if (!input0 || !input1 || !input2 || !group ||
        class_count < 1 || anchors.size() != 18 ||
        labels.size() != static_cast<size_t>(class_count) ||
        zero_points.size() < 3 || quant_scales.size() < 3 ||
        scale_width <= 0.0f || scale_height <= 0.0f)
    {
        return -1;
    }

    group->count = 0;
    std::vector<float> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;
    TensorType *inputs[3] = {input0, input1, input2};
    const int strides[3] = {8, 16, 32};

    int valid_count = 0;
    for (int head = 0; head < 3; ++head)
    {
        valid_count += processHead(
            inputs[head], anchors.data() + head * 6,
            model_height / strides[head], model_width / strides[head],
            strides[head], boxes, scores, class_ids,
            confidence_threshold, zero_points[head], quant_scales[head],
            class_count, labels);
    }
    if (valid_count == 0)
        return 0;

    std::vector<int> order(valid_count);
    for (int i = 0; i < valid_count; ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(),
              [&scores](int first, int second) {
                  return scores[first] > scores[second];
              });

    const std::set<int> classes(class_ids.begin(), class_ids.end());
    for (const int class_id : classes)
        nms(valid_count, boxes, class_ids, order, class_id, nms_threshold);

    int result_count = 0;
    for (int i = 0; i < valid_count && result_count < OBJ_NUMB_MAX_SIZE; ++i)
    {
        const int index = order[i];
        if (index == -1)
            continue;

        const float x1 = boxes[index * 4];
        const float y1 = boxes[index * 4 + 1];
        const float x2 = x1 + boxes[index * 4 + 2];
        const float y2 = y1 + boxes[index * 4 + 3];
        detect_result_t &result = group->results[result_count];
        result.box.left = clamp(x1, 0, model_width) / scale_width;
        result.box.top = clamp(y1, 0, model_height) / scale_height;
        result.box.right = clamp(x2, 0, model_width) / scale_width;
        result.box.bottom = clamp(y2, 0, model_height) / scale_height;
        result.prop = scores[index];

        const int class_id = class_ids[index];
        const std::string label =
            class_id >= 0 && class_id < static_cast<int>(labels.size())
                ? labels[class_id]
                : "class_" + std::to_string(class_id);
        std::snprintf(result.name, OBJ_NAME_MAX_SIZE, "%s", label.c_str());
        ++result_count;
    }
    group->count = result_count;
    return 0;
}
} // namespace

int post_process(int8_t *input0, int8_t *input1, int8_t *input2,
                 int model_in_h, int model_in_w,
                 float conf_threshold, float nms_threshold,
                 float scale_w, float scale_h,
                 std::vector<int32_t> &qnt_zps,
                 std::vector<float> &qnt_scales,
                 detect_result_group_t *group, int class_num,
                 const std::vector<int> &anchors,
                 const std::vector<std::string> &labels)
{
    return postProcessImpl(
        input0, input1, input2, model_in_h, model_in_w,
        conf_threshold, nms_threshold, scale_w, scale_h,
        qnt_zps, qnt_scales, group, class_num, anchors, labels);
}

int post_process_float(float *input0, float *input1, float *input2,
                       int model_in_h, int model_in_w,
                       float conf_threshold, float nms_threshold,
                       float scale_w, float scale_h,
                       detect_result_group_t *group, int class_num,
                       const std::vector<int> &anchors,
                       const std::vector<std::string> &labels)
{
    const std::vector<int32_t> zero_points(3, 0);
    const std::vector<float> scales(3, 1.0f);
    return postProcessImpl(
        input0, input1, input2, model_in_h, model_in_w,
        conf_threshold, nms_threshold, scale_w, scale_h,
        zero_points, scales, group, class_num, anchors, labels);
}
