/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include <iostream>
#include <thread>
#include <string>
#include <opencv2/opencv.hpp>
#include "push.h"

static bool running = true;

// 从 MP4 文件读帧并送入编码/推流
void decode_from_mp4(const std::string &mp4_path)
{
    cv::VideoCapture cap(mp4_path);
    if (!cap.isOpened())
    {
        std::cerr << "Failed to open video file: " << mp4_path << std::endl;
        return;
    }
    std::cout << "Decoding from MP4: " << mp4_path << std::endl;

    cv::Mat frame;
    while (cap.read(frame) && running)
    {
        if (frame.empty())
            continue;
        set_Mat(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::cout << "Decode thread finished." << std::endl;
}

int main()
{
    pushToGB28181();
    std::cout << "init" << std::endl;

    // 使用 MP4 文件路径，可按需修改
    std::string mp4_path = "/home/cat/project2/test.mp4";
    std::thread decode_thread(decode_from_mp4, mp4_path);
    decode_thread.join();
    return 0;
}
