
/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include "ps_rtp.h"
#include "gb28181.h"
#include "decode.h"
#include "push.h"
#include "config.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

std::atomic<bool> work_shut(false);
std::atomic<bool> is_check(true);
std::atomic<bool> first_frame_flg(true);
std::atomic<bool> gb_stop(false);

std::mutex is_check_mtx;
std::condition_variable cond_check;

static std::thread g_sip_thread;
FrameQueue<H264Frame> h264data(50);

void work(SDPInfo &sdp_info, int udp_socket, Data_Info_s &packer)
{
    int ul = 1;
    int ret = ioctl(udp_socket, FIONBIO, &ul); // 设置为非阻塞模式
    bool is_first = true;
    if (ret == -1)
    {
        printf("设置非阻塞失败!");
        return;
    }
    std::cout << "sdp_info.ssrc" << sdp_info.ssrc << std::endl;
    printf("u32Ssrc=%d\n", packer.u32Ssrc);
    H264Frame data;
    int count = 0;
    std::unique_ptr<char[]> bufdata(new char[1024 * 1024]);
    char *buf = bufdata.get();
    printf("h264data work:%d\n", h264data.size());
    while (true)
    {
        if (work_shut)
        {
            printf("work_shut\n");
            break;
        }
        // printf("before wait_and_pop=%d\n", h264data.size());
        //  阻塞在这里了。
        if (!h264data.wait_and_pop(data))
        {
            printf("have no data\n");
            continue;
        }
        // printf("after wait_and_pop\n");
        //  printf("time:%lld\n", packer.s64CurPts);

        if (is_first)
        {
            printf("first frame1\n");
            if (first_frame_flg == true)
            {
                printf("first_frame_flg is true\n");
            }
            else
            {
                printf("first_frame_flg is false\n");
            }
            auto nalus = splitNalus(reinterpret_cast<const char *>(data.data.data()),
                                    static_cast<int>(data.data.size()));
            printf("first frame size:%d\n", nalus.size());
            int ret = gb28181_streampackageForH264_first(nalus, &packer, 0, sdp_info.connectionAddress.ip.c_str(), static_cast<uint16_t>(std::stoul(sdp_info.mediaStreams.port)), udp_socket);
            if (ret < 0)
            {
                printf("first send fail:%d\n", ret);
                return;
            }
            packer.s64CurPts += 3000;
            usleep(1000000 / 30);
            is_first = false;
        }
        else
        {

            memcpy(buf + PES_HDR_LEN, data.data.data(), data.data.size());
            gb28181_streampackageForH264(buf + PES_HDR_LEN,
                                         static_cast<int>(data.data.size()),
                                         &packer,
                                         0,
                                         sdp_info.connectionAddress.ip.c_str(),
                                         static_cast<uint16_t>(std::stoul(sdp_info.mediaStreams.port)),
                                         udp_socket);
            packer.s64CurPts += 3000; // 90000/fps
            usleep(1000000 / 30);     // 帧率 30fps
        }
    }
    close(udp_socket);
    return;
}

// 存十帧
FrameQueue<cv::Mat> matData(10);
int set_Mat(cv::Mat &img)
{
    if (img.empty())
        return -1;
    // 深拷贝 Mat，避免局部变量销毁后数据指针失效
    // 使用 push(const T&) 重载，确保 Mat 被正确拷贝到队列
    cv::Mat img_copy = img.clone();
    if (!matData.push(img_copy))
    {
        printf("set mat failed\n");
        return -1;
    }
    return 0;
}
void notify_decode_finished()
{
    matData.shutdown();
}
void encode()
{
    Mat data;
    Mat frame_yuv;
    Mat resized_data;
    while (matData.wait_and_pop(data)) // 从线程安全队列中获取一帧图像
    {
        if (work_shut)
        {
            break;
        }
        // 调整大小
        // 因为mpp编码器设置的是720p的帧率

        if (data.empty() || data.rows <= 0 || data.cols <= 0)
        {
            printf("is empty\n");
            continue;
        }
        try
        {
            // 修改编码分辨率的时候需要修改（使用临时 Mat 避免 in-place resize 问题）
            cv::resize(data, resized_data, cv::Size(1280, 720));

            // 创建 YUV 图像空间
            if (frame_yuv.empty() || frame_yuv.cols != resized_data.cols || frame_yuv.rows != resized_data.rows * 3 / 2)
            {
                frame_yuv.create(resized_data.rows * 3 / 2, resized_data.cols, CV_8UC1);
            }

            // 转为 YUV420
            cv::cvtColor(resized_data, frame_yuv, cv::COLOR_BGR2YUV_I420);
        }
        catch (const cv::Exception &e)
        {
            printf("OpenCV error, skip frame: %s\n", e.what());
            continue;
        }

        // 编码输出结构（先用临时缓冲区接收，再放入 H264Frame）
        const size_t buf_size = frame_yuv.total() * frame_yuv.elemSize();
        std::vector<std::uint8_t> buf(buf_size);
        char *encode_buf = reinterpret_cast<char *>(buf.data());
        size_t encode_len = buf_size;

        // 编码函数
        YuvtoH264(resized_data.cols, resized_data.rows, frame_yuv, encode_buf, encode_len);
        buf.resize(encode_len);

        H264Frame h264_frame;
        h264_frame.data = std::move(buf);

        // 推入编码数据队列
        h264data.push(h264_frame);
    }
    h264data.shutdown();
    printf("encode exit\n");
    return;
}
void checkQueueThread()
{
    while (!gb_stop.load())
    {
        // 程序起来就需要检查
        std::unique_lock<std::mutex> lock(is_check_mtx); // 获取锁
        cond_check.wait(lock, []()
                        { return is_check.load(); });
        if (matData.size() == 10)
        {
            std::cout << "need to clear queue" << std::endl;
            matData.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); // 控制检查的效率
    }
}
void SipThread(GB28181Connect gbinvite, SDPInfo sdp_info, GB28Info gbinfo)
{
    int registerID;
    std::atomic<bool> first_register_thread(true);
    std::thread encode_thread;
    std::thread work_thread;
    std::thread check_thread(checkQueueThread);
    Data_Info_s packer;
    int udp_socket = -1;
    eXosip_t *ctx; // eXosip 句柄
    int ret = 0;
    // 初始化 eXosip
    ctx = eXosip_malloc();
    if (ctx == NULL)
    {
        fprintf(stderr, "Could not initialize eXosip2.\n");
        return;
    }

    if (eXosip_init(ctx) != 0)
    {
        std::cerr << "[SIP] 初始化 eXosip 失败！\n";
        return;
    }
    //  设置本地监听地址和端口
    ret = eXosip_listen_addr(ctx, IPPROTO_UDP, NULL, atoi(gbinfo.devicePort.c_str()), AF_INET, 0);
    if (ret != 0)
    {
        fprintf(stderr, "Could not set listening address.\n");
        eXosip_quit(ctx);
        return;
    }

    printf("eXosip2 初始化成功\n");

    // 发送 REGISTER 请求
    registerID = gbinvite.send_register(ctx, gbinfo);

    // 事件循环
    while (!gb_stop.load())
    {
        // eXosip_lock(ctx);
        eXosip_event_t *event;
        event = eXosip_event_wait(ctx, 0, 40); // 侦听是否有消息到来
        eXosip_lock(ctx);
        eXosip_automatic_action(ctx);
        eXosip_unlock(ctx);
        if (event == NULL)
        {
            continue;
        }
        printf("receive news: %d\n", event->type);

        // 处理事件
        switch (event->type)
        {
        case EXOSIP_REGISTRATION_SUCCESS:
        {
            printf("返回码:%d\n", event->response->status_code);
            printf("register success。\n");
            if (first_register_thread.load())
            {
                std::thread t(register_thread, 30, ctx, event, registerID);
                t.detach(); // 线程分离
                first_register_thread.store(false);
            }
        }
        break;
        case EXOSIP_REGISTRATION_FAILURE:
        {
            gbinvite.handleRegister(ctx, event, gbinfo);
        }

        break; // 确保在处理完事件后跳出 switch 语句

        case EXOSIP_MESSAGE_NEW:
        {
            printf("Received SIP message\n");
            gbinvite.handleNews(ctx, event, gbinfo);
            break;
        }
        case EXOSIP_CALL_ACK:
        {
            printf("start ask\n");
            is_check.store(false);
            h264data.reset(50);
            printf("start com\n");
            udp_socket = socket(AF_INET, SOCK_DGRAM, 0);

            if (udp_socket < 0)
            {
                perror("socket creation failed");
                return;
            }
            struct sockaddr_in addr_local;
            memset(&addr_local, 0, sizeof(addr_local));
            addr_local.sin_family = AF_INET;
            addr_local.sin_port = htons(50099);             // 固定源端口
            addr_local.sin_addr.s_addr = htonl(INADDR_ANY); // 绑定所有本地 IP
            if (udp_socket < 0)
            {
                printf("socket has been close\n");
            }
            if (bind(udp_socket, (struct sockaddr *)&addr_local, sizeof(addr_local)) < 0)
            {
                perror("bind failed");
                close(udp_socket);
                return;
            }
            first_frame_flg.store(true);
            packer.s64CurPts = 0;
            packer.u32Ssrc = atoi(sdp_info.ssrc.c_str());
            packer.u16CSeq = 0;
            printf("receive answer\n");
            work_shut = false;
            encode_thread = std::thread(encode);
            printf("start work thred\n");
            // 推流线程

            work_thread = std::thread(work, std::ref(sdp_info), udp_socket, std::ref(packer));
            break;
        }
        case EXOSIP_IN_SUBSCRIPTION_NEW:
        {
            gbinvite.handleSubcribe(ctx, event, gbinfo);
            break;
        }
        case EXOSIP_CALL_CLOSED:
        {
            printf("close\n");
            is_check.store(true);
            work_shut = true;
            cond_check.notify_one();
            if (udp_socket >= 0)
            {
                close(udp_socket);
                udp_socket = -1;
            }

            if (encode_thread.joinable())
            {
                encode_thread.join();
                printf("encode_thread stopped\n");
            }

            if (work_thread.joinable())
            {
                work_thread.join();
                printf("work_thread stopped\n");
            }

            // printf("close size=%d\n", h264data.size());

            break;
        }
        case EXOSIP_CALL_INVITE:

            gbinvite.handleInvite(ctx, event, gbinfo, sdp_info);
            break;
        }
        eXosip_event_free(event);
    }
    work_shut = true;
    cond_check.notify_all();
    if (encode_thread.joinable())
    {
        encode_thread.join();
    }
    if (work_thread.joinable())
    {
        work_thread.join();
    }
    gb_stop.store(true);
    cond_check.notify_all();
    if (check_thread.joinable())
    {
        check_thread.join();
    }
    // 退出 eXosip
    eXosip_quit(ctx);
    free(ctx);
}
int pushToGB28181()
{
    const GBConfig &cfg = get_gb_config();

    GB28Info gbinfo;
    gbinfo.devicePort = cfg.devicePort;
    gbinfo.deviceIp = cfg.deviceIp;
    gbinfo.deviceName = cfg.deviceName;
    gbinfo.servePort = cfg.servePort;
    gbinfo.serveIp = cfg.serveIp;
    gbinfo.serveName = cfg.serveName;
    gbinfo.servePassword = cfg.servePassword;
    gbinfo.pushPort = cfg.pushPort;

    SDPInfo sdp_info;
    GB28181Connect gbinvite(gbinfo);

    gb_stop.store(false);
    work_shut.store(false);
    is_check.store(true);

    try
    {
        printf("aaa\n");
        g_sip_thread = std::thread(SipThread, gbinvite, sdp_info, gbinfo);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Exception caught while creating SIP thread: " << e.what() << std::endl;
        return -1;
    }
}

void stopGB28181()
{
    gb_stop.store(true);
    work_shut.store(true);
    cond_check.notify_all();
    if (g_sip_thread.joinable())
    {
        g_sip_thread.join();
    }
}
