/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef _PUSH_H
#define _PUSH_H
#include <opencv2/opencv.hpp>

int pushToGB28181();
void stopGB28181();

int set_Mat(cv::Mat &img);
void notify_decode_finished();
#endif
