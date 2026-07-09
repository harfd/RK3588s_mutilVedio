/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef _GB28181_H
#define _GB28181_H

#include <iostream>
#include "eXosip2/eXosip.h"
#include <netinet/in.h>
#include <thread>
#include <vector>
#include <sstream>
#include <map>
#include "xml_utils.h"
#include <map>
#include <string>
#include <vector>
using std::map;
using std::string;
using std::vector;

struct MediaStream
{
    string mediaType; // 媒体类型(video/audio)
    string port;      // 端口号
    string protocol;  // 传输协议
    string trans;
    vector<string> payloadTypes; // 负载类型列表
    map<string, string> rtpmaps; // 编解码映射表
    vector<string> core;
};
struct connectionInfo
{
    string ip;
    string networkType;
    string addressType;
};
struct SessionInfo
{
    string username;
    string sessionID;
    string sessionVersion;
    string networkType;
    string addressType;
    string connectionAddress;
};

struct SDPInfo
{
    string version;
    SessionInfo origin;
    string sessionName;
    connectionInfo connectionAddress;
    string timing;
    MediaStream mediaStreams;
    string ssrc;
};
struct GB28Info
{
    std::string serveName;
    std::string serveIp;
    std::string servePort;
    std::string deviceName;
    std::string deviceIp;
    std::string devicePort;
    std::string servePassword;
    std::string pushPort;
};
class GB28181Connect
{
public:
    explicit GB28181Connect(const GB28Info &gbinfo); // 必须显示调用构造函数 ，加上explicit
    // void register_thread(int second, eXosip_t *ctx, eXosip_event_t *event);
    int send_register(eXosip_t *ctx, GB28Info info);
    void respond_catalog_by_message(eXosip_t *ctx, int sn, const GB28Info &info);
    void respond_deviceinfo_by_message(eXosip_t *ctx, int sn, const GB28Info &info);
    void respond_deviceControl_by_message(eXosip_t *ctx, int sn, const GB28Info &info);
    void respond_MobilePosition_by_subscribe(eXosip_t *ctx, int sn, const GB28Info &info);

    void send_channel_list(eXosip_t *ctx, int sn, GB28Info info);
    int handleRegister(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);
    int handleNews(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);
    int handleInvite(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo, SDPInfo &sdp_info);
    int handleSubcribe(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo);

    GB28Info info;
    SDPInfo sdp_info;
};
void register_thread(int second, eXosip_t *ctx, eXosip_event_t *event, int rID);
#endif //
