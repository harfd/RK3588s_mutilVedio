/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include "gb28181.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>

std::mutex eXosip_mutex;
// 解析sdp
int parseSDP(const std::string &sdp, SDPInfo &info)
{
    std::istringstream iss(sdp); // 根据空格来划分
    std::string line;
    MediaStream currentMedia;
    bool inMediaBlock = false;
    while (getline(iss, line))
    {
        if (line.empty())
            continue;
        size_t pos = line.find('='); // 找“=”
        if (pos == std::string::npos)
            continue;
        std::string key = line.substr(0, pos);    // 将字符串起始位置到=之前的值都给key
        std::string value = line.substr(pos + 1); // 将=后面的值给value，到子字符串结束
        if (key == "v")
        {
            info.version = value;
        }
        else if (key == "o")
        {
            std::string tmp = value;
            std::istringstream is(tmp); // 根据空格来划分
            // is >> o >> session.username >> session.sessionID >> session.sessionVersion >> session.networkType >> session.addressType >> session.connectionAddress;
            if (!(is >> info.origin.username >> info.origin.sessionID >> info.origin.sessionVersion >> info.origin.networkType >> info.origin.addressType >> info.origin.connectionAddress))
            {
                std::cout << "orin解析错误" << std::endl;
                return -3;
            }
        }
        else if (key == "s")
        {
            info.sessionName = value;
        }
        else if (key == "c")
        {
            std::string tmp = value;
            std::cout << "tmp:" << tmp << std::endl;
            // 提取IP:
            std::istringstream is(tmp); // 根据空格来划分
            if (!(is >> info.connectionAddress.networkType >> info.connectionAddress.addressType >> info.connectionAddress.ip))
            {
                std::cout << "connectionAddress解析错误" << std::endl;
                return -1;
            }
        }
        else if (key == "t")
        {
            info.timing = value;
        }
        else if (key == "m")
        {
            std::istringstream mStream(value);
            if (!(mStream >> info.mediaStreams.mediaType >> info.mediaStreams.port >> info.mediaStreams.protocol))
            {
                std::cout << "解析m失败" << std::endl;
                return -2;
            }
            std::string payload;
            // 读取剩下的
            while (mStream >> payload)
            {
                info.mediaStreams.payloadTypes.push_back(payload);
            }
        }
        else if (key == "a")
        {
            if (value.find("rtpmap:") == 0)
            {
                // 格式: rtpmap:<payload> <codec>/<clockrate>
                size_t colonPos = value.find(':');
                size_t spacePos = value.find(' ');
                size_t slashPos = value.find('/');

                if (spacePos != std::string::npos && slashPos != std::string::npos)
                {
                    std::string payloadType = value.substr(colonPos + 1, spacePos - colonPos - 1);
                    std::string codec = value.substr(spacePos + 1, slashPos - spacePos - 1);
                    std::string clockRate = value.substr(slashPos + 1);
                    info.mediaStreams.rtpmaps[payloadType] = value.substr(spacePos + 1);
                }
            }
            else if (value == "recvonly" || value == "sendonly")
            {
                info.mediaStreams.trans = value; // 将方向信息添加到协议中
            }
        }
        else if (key == "y")
        {
            info.ssrc = value;
        }
    }

    return 0;
}
GB28181Connect::GB28181Connect(const GB28Info &gbinfo) : info(gbinfo)
{
    std::cout << "服务器ID:" << this->info.serveName << std::endl;
    std::cout << "服务器ip:" << this->info.serveIp << std::endl;
    std::cout << "服务器密码:" << this->info.servePassword << std::endl;
    std::cout << "服务器端口:" << this->info.servePort << std::endl;
    std::cout << "设备ID:" << this->info.deviceIp << std::endl;
    std::cout << "设备ip" << this->info.deviceIp << std::endl;
    std::cout << "设备端口" << this->info.devicePort << std::endl;
}

int GB28181Connect::handleRegister(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    if (event->response != NULL)
    {
        int status_code = event->response->status_code;
        printf("收到响应状态码: %d\n", status_code);
        if (status_code == 401)
        {
            printf("注册失败，收到 401 Unauthorized 错误。\n");
            // 此时可以进行重试或其他处理
            printf("event->rid: %d\n", event->rid);
            osip_www_authenticate_t *www_authenticate_header = nullptr;
            int ret = 0;
            ret = osip_message_get_www_authenticate(event->response, 0, &www_authenticate_header);
            if (ret != 0 || www_authenticate_header == nullptr || www_authenticate_header->realm == NULL)
            {
                printf("获取返回信息失败\n");
                return -1;
            }
            // 2. 添加认证信息

            eXosip_lock(ctx);
            /*
                16161616用户id需要与from头中的id相同
                密码是平台的密码
            */
            ret = eXosip_add_authentication_info(ctx, gbinfo.deviceName.c_str(), gbinfo.deviceName.c_str(), gbinfo.servePassword.c_str(), "MD5", www_authenticate_header->realm);
            eXosip_unlock(ctx);
            if (ret != 0)
            {
                printf("认证失败\n");
            }
            else
            {
                printf("认证成功\n");
            }
            // 重新注册
        }
        else
        {
            printf("注册失败，状态码: %d\n", status_code);
        }
    }
    return 0;
}
int i = 0;
int GB28181Connect::handleInvite(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo, SDPInfo &sdp_info)
{

    printf("receive invite%d\n", i);

    int status_code = event->response->status_code;
    printf("收到来电后的状态码：%d\n", status_code);
    // 获取SDP
    osip_body_t *body = nullptr;
    if (event->request)
    {
        osip_message_get_body(event->request, 0, &body);
        if (body && body->body)
        {
            std::cout << "send body" << std::endl;
            std::cout << "body:" << body->body << std::endl;
            std::string sdp_content(body->body);
            std::cout << "原始SDP内容:\n"
                      << sdp_content << std::endl;
            parseSDP(sdp_content, sdp_info);
            //  3. 构造并发送200 OK响应
            osip_message_t *answer = nullptr;
            std::stringstream ss;
            ss << "v=0\r\n";
            ss << "o=" << sdp_info.origin.username << " 0 0 IN IP4 " << "192.168.1.198" << "\r\n";
            ss << "s=Play\r\n";
            ss << "u=" << gbinfo.deviceName << ":255\r\n"; // 补全u字段
            ss << "c=IN IP4 " << "192.168.1.198" << "\r\n";
            ss << "t=0 0\r\n";
            ss << "m=video " << gbinfo.pushPort << " RTP/AVP 96\r\n";
            ss << "a=rtpmap:96 PS/90000\r\n";
            ss << "a=sendonly\r\n";
            ss << "y=" << sdp_info.ssrc << "\r\n";
            std::cout << "\n构造的SDP应答内容:\n"
                      << ss.str() << std::endl;
            gbinfo.pushPort = sdp_info.mediaStreams.port;
            std::string sdp_output_str = ss.str();

            osip_message_t *message = event->request;

            printf("eXosip_call_build_answer\n");
            int status = eXosip_call_build_answer(ctx, event->tid, 200, &message);

            if (status != 0)
            {
                std::cout << "eXosip_call_build_sdp_answer fail" << std::endl;
                return -1;
            }

            osip_message_set_content_type(message, "APPLICATION/SDP");
            std::cout << "osip_message_set_content_type" << std::endl;
            osip_message_set_body(message, sdp_output_str.c_str(), sdp_output_str.size());
            std::cout << "osip_message_set_body" << std::endl;
            {
                std::lock_guard<std::mutex> lock(eXosip_mutex);
                int ret = eXosip_call_send_answer(ctx, event->tid, 200, message);
                if (ret != 0)
                {
                    printf("send sdp failed\n");
                }
            }

                std::cout << "eXosip_call_send_answer" << std::endl;
            return -1;
        }
    }
}
void GB28181Connect::respond_MobilePosition_by_subscribe(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[1024];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "<CmdType>MobilePosition</CmdType>\n"
             "<SN>%d</SN>\n"
             "<DeviceID>%s</DeviceID>\n"
             "<Result>OK</Result>\n"
             "</Response>",
             sn, info.deviceName.c_str());
    osip_message_t *notify = nullptr;
    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    int ret = eXosip_message_build_request(ctx, &notify, "MESSAGE", to.c_str(), from.c_str(), NULL);
    if (ret != 0)
    {
        printf("build notify fail\n");
        return;
    }
    osip_message_set_content_type(notify, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(notify, xml, strlen(xml));

    if (eXosip_message_send_request(ctx, notify) < 0)
    {
        printf("Send MobilePosition MESSAGE failed.\n");
    }
    else
    {
        printf("MobilePosition MESSAGE response sent successfully.\n");
    }
}
int GB28181Connect::handleSubcribe(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    printf("有订阅消息\n");

    if (event->request)
    {
        osip_body_t *body;
        int ret = osip_message_get_body(event->request, 0, &body);
        printf("osip_message_get_body 返回值: %d\n", ret); // 查看返回值
        // 需要解析body，这是穿过来的一个信息
        if (ret == 0 && body && body->body)
        {
            // 获取类型
            osip_content_type_t *content_type = osip_message_get_content_type(event->request);
            if (content_type && content_type->type && content_type->subtype)
            {
                std::string type(content_type->type);
                std::string subtype(content_type->subtype);
                std::string type_lower = type;
                std::string subtype_lower = subtype;
                std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                std::transform(subtype_lower.begin(), subtype_lower.end(), subtype_lower.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (type_lower == "application" && subtype_lower == "manscdp+xml")
                {
                    std::string cmdType = extractCmdType(body->body);
                    if (cmdType == "MobilePosition")
                    {
                        osip_message_t *answer;
                        eXosip_insubscription_build_answer(ctx, event->tid, 200, &answer);
                        {
                            std::lock_guard<std::mutex> lock(eXosip_mutex);
                            eXosip_insubscription_send_answer(ctx, event->tid, 200, answer);
                            std::string sn = extractSN(body->body);
                            int num = std::stoi(sn);
                            respond_MobilePosition_by_subscribe(ctx, num, info);
                        }
                    }
                    else
                    {
                        osip_message_t *answer;
                        eXosip_insubscription_build_answer(ctx, event->tid, 200, &answer);
                        {
                            std::lock_guard<std::mutex> lock(eXosip_mutex);
                            eXosip_insubscription_send_answer(ctx, event->tid, 200, answer);
                            printf("send successfully\n");
                            std::string sn = extractSN(body->body);
                            int num = std::stoi(sn);
                            send_channel_list(ctx, num, gbinfo);
                        }
                    }
                }
            }
        }
        else
        {
            printf(" 没有消息体内容\n");
        }
    }
}
void GB28181Connect::respond_catalog_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[2048];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "  <CmdType>Catalog</CmdType>\n"
             "  <SN>%d</SN>\n"
             "  <DeviceID>%s</DeviceID>\n"
             "  <SumNum>1</SumNum>\n"
             "  <DeviceList>\n"
             "    <Item>\n"
             "      <DeviceID>%s</DeviceID>\n"
             "      <Name>test5</Name>\n"
             "      <Manufacturer>厂商名称</Manufacturer>\n"
             "      <Model>型号</Model>\n"
             "      <Owner>Owner</Owner>\n"
             "      <Parental>0</Parental>\n"
             "      <ParentID>%s</ParentID>\n"
             "      <SafetyWay>0</SafetyWay>\n"
             "      <RegisterWay>1</RegisterWay>\n"
             "      <Secrecy>0</Secrecy>\n"
             "      <Status>ON</Status>\n"
             "      <IPAddress>%s</IPAddress>\n"
             "      <Port>%d</Port>\n"
             "    </Item>\n"
             "  </DeviceList>\n"
             "</Response>",
             sn,
             info.deviceName.c_str(),
             info.deviceName.c_str(),
             info.deviceName.c_str(),
             info.serveIp.c_str(),
             atoi(info.servePort.c_str()));
    // 构造响应 MESSAGE
    osip_message_t *msg = nullptr;

    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;

    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("Build MESSAGE failed.\n");
        return;
    }

    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(msg, xml, strlen(xml));

    if (eXosip_message_send_request(ctx, msg) < 0)
    {
        printf("Send Catalog MESSAGE failed.\n");
    }
    else
    {
        printf("Catalog MESSAGE response sent successfully.\n");
    }
}
void GB28181Connect::respond_deviceinfo_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[1024];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\" encoding=\"GB2312\"?>\n"
             "<Response>\n"
             "  <CmdType>DeviceInfo</CmdType>\n"
             "  <SN>%d</SN>\n"
             "  <DeviceID>%s</DeviceID>\n"
             "  <Result>OK</Result>\n"
             "  <DeviceName>GB28181-Device</DeviceName>\n"
             "  <Manufacturer>Manufacturer</Manufacturer>\n"
             "  <Model>Model</Model>\n"
             "  <Firmware>1.0</Firmware>\n"
             "</Response>",
             sn, info.deviceName.c_str());

    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    osip_message_t *msg = nullptr;
    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("DeviceInfo: Build MESSAGE failed.\n");
        return;
    }
    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(msg, xml, strlen(xml));
    if (eXosip_message_send_request(ctx, msg) < 0)
        printf("DeviceInfo: Send MESSAGE failed.\n");
    else
        printf("DeviceInfo response sent (SN=%d).\n", sn);
}
void GB28181Connect::respond_deviceControl_by_message(eXosip_t *ctx, int sn, const GB28Info &info)
{
    char xml[1024];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\"?>\n"
             "<Response>\n"
             "<CmdType>DeviceControl</CmdType>\n"
             "<SN>%d</SN>\n"
             "<DeviceID>%s</DeviceID>\n"
             "<Result>OK</Result>\n"
             "</Response>",
             sn, info.deviceName.c_str());
    osip_message_t *msg = nullptr;
    std::string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    std::string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    if (eXosip_message_build_request(ctx, &msg, "MESSAGE", to.c_str(), from.c_str(), nullptr) != 0)
    {
        printf("Build MESSAGE failed.\n");
        return;
    }
    osip_message_set_content_type(msg, "Application/MANSCDP+xml; charset=GB2312");
    osip_message_set_body(msg, xml, strlen(xml));

    if (eXosip_message_send_request(ctx, msg) < 0)
    {
        printf("Send DeviceControl MESSAGE failed.\n");
    }
    else
    {
        printf("DeviceControl MESSAGE response sent successfully.\n");
    }
}

int GB28181Connect::handleNews(eXosip_t *ctx, eXosip_event_t *event, GB28Info gbinfo)
{
    if (event->request)
    {
        if (event->type == 23)
        {
            osip_body_t *body;
            // 获取平台发送的消息
            int ret = osip_message_get_body(event->request, 0, &body);
            if (ret == 0 && body && body->body)
            {
                printf("Message body:\n%s\n", body->body);
                osip_content_type_t *content_type = osip_message_get_content_type(event->request);
                if (content_type && content_type->type && content_type->subtype)
                {
                    std::string type(content_type->type);
                    std::string subtype(content_type->subtype);
                    std::string type_lower = type;
                    std::string subtype_lower = subtype;
                    std::transform(type_lower.begin(), type_lower.end(), type_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    std::transform(subtype_lower.begin(), subtype_lower.end(), subtype_lower.begin(),
                                   [](unsigned char c) { return std::tolower(c); });
                    if (type_lower == "application" && subtype_lower == "manscdp+xml")
                    {
                        std::string cmdType = extractCmdType(body->body);
                        if (cmdType == "DeviceInfo")
                        {
                            printf("DeviceInfo query received\n");
                            std::string sn_str = extractSN(body->body);
                            if (!sn_str.empty())
                            {
                                osip_message_t *answer = nullptr;
                                eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                                if (answer)
                                {
                                    {
                                        std::lock_guard<std::mutex> lock(eXosip_mutex);
                                        eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    }
                                    int sn_num = std::stoi(sn_str);
                                    respond_deviceinfo_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                        else if (cmdType == "Catalog")
                        {
                            printf("catalog query message\n");
                            std::string sn = extractSN(body->body);
                            osip_message_t *answer = nullptr;
                            eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                            if (answer)
                            {
                                {
                                    std::lock_guard<std::mutex> lock(eXosip_mutex);
                                    eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    int sn_num = std::stoi(sn);
                                    respond_catalog_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                        else if (cmdType == "DeviceControl")
                        {
                            printf("DeviceControl query message\n");
                            std::string sn = extractSN(body->body);
                            osip_message_t *answer = nullptr;
                            eXosip_message_build_answer(ctx, event->tid, 200, &answer);
                            if (answer)
                            {
                                {
                                    std::lock_guard<std::mutex> lock(eXosip_mutex);
                                    eXosip_message_send_answer(ctx, event->tid, 200, answer);
                                    int sn_num = std::stoi(sn);
                                    respond_deviceControl_by_message(ctx, sn_num, gbinfo);
                                }
                            }
                        }
                    }

                    // 1. 提取SN（如果是订阅消息）
                    std::string sn = extractSN(body->body);
                    if (!sn.empty())
                    {
                        printf("Extracted SN: %s\n", sn.c_str());

                        // 2. 构建并发送200 OK响应（使用message的API，不是call的API）
                    }
                }
                else
                {
                    printf("No message body or parse failed\n");
                }
            }
        }
    }
}

int GB28181Connect::send_register(eXosip_t *ctx, GB28Info info)
{

    osip_message_t *reg = NULL;
    char *contact = NULL;
    int registration_id;

    // 构建 From、To 和 Contact 头
    string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    printf("from:%s\n", from.c_str());
    // 创建 REGISTER 请求
    // 想服务器注册
    // from：客户端地址，proxy服务器地址，contact和from相同
    registration_id = eXosip_register_build_initial_register(ctx, from.c_str(), to.c_str(), contact, 60, &reg);
    if (registration_id < 0)
    {
        std::cerr << "[SIP] 构建 REGISTER 请求失败！\n";
        return -1;
    }
    printf("registration_id:%d\n", registration_id);

    // 发送 REGISTER 请求
    {
        std::lock_guard<std::mutex> lock(eXosip_mutex);
        if (eXosip_register_send_register(ctx, registration_id, reg) != 0)
        {
            std::cerr << "[SIP] 发送 REGISTER 请求失败！\n";
            return -1;
        }
    }

    std::cout << "[SIP] REGISTER 请求已发送。\n";
    return registration_id;
}
void GB28181Connect::send_channel_list(eXosip_t *ctx, int sn, GB28Info info)
{
    char tmp[2048]; // 增大缓冲区防止溢出
    const char *xml =
        "<?xml version=\"1.0\"?>\n"
        "<Response>\n"
        "  <CmdType>Catalog</CmdType>\n"
        "  <SN>%d</SN>\n"
        "  <DeviceID>%lld</DeviceID>\n"
        "  <SumNum>1</SumNum>\n" // 放在 DeviceList 之前
        "  <DeviceList>\n"
        "    <Item>\n"
        "      <DeviceID>%lld</DeviceID>\n"
        "      <Name>通道1</Name>\n"
        "      <Manufacturer>厂商名称</Manufacturer>\n"
        "      <Model>型号</Model>\n"
        "      <Owner>Owner</Owner>\n"
        "      <Parental>0</Parental>\n"
        "      <ParentID>%lld</ParentID>\n"
        "      <SafetyWay>0</SafetyWay>\n"
        "      <RegisterWay>1</RegisterWay>\n"
        "      <Secrecy>0</Secrecy>\n"
        "      <Status>ON</Status>\n"
        "      <IPAddress>%s</IPAddress>\n"
        "      <Port>%d</Port>\n"
        "    </Item>\n"
        "  </DeviceList>\n"
        "</Response>";
    char formatted_xml[2048];
    snprintf(formatted_xml, sizeof(formatted_xml), xml, sn, atoll(info.deviceName.c_str()), atoll(info.deviceName.c_str()), atoll(info.deviceName.c_str()), info.serveIp.c_str(), atoi(info.serveIp.c_str())); // 使用tid作为SN

    osip_message_t *notify = NULL;
    string to = "sip:" + info.serveName + "@" + info.serveIp + ":" + info.servePort;
    string from = "sip:" + info.deviceName + "@" + info.deviceIp + ":" + info.devicePort;
    const char *eventvalue = "Catalog";
    const char *msgtype = "Application/MANSCDP+xml; charset=GB2312";
    int ret = eXosip_message_build_request(ctx, &notify, "NOTIFY", to.c_str(), from.c_str(), NULL);
    if (ret != 0)
    {
        printf("build notify fail\n");
        return;
    }
    osip_message_set_header(notify, "Event", eventvalue);
    osip_message_set_header(notify, "Subscription-State", "active");
    if (notify != NULL)
    {
        strncpy(tmp, formatted_xml, sizeof(tmp) - 1);
        // char gb2312_xml[2048];
        tmp[sizeof(tmp) - 1] = '\0';
        // utf8_to_gb2312(tmp, gb2312_xml, sizeof(gb2312_xml));
        osip_message_set_body(notify, tmp, strlen(tmp));
        osip_message_set_content_type(notify, msgtype);
    }

    ret = eXosip_message_send_request(ctx, notify);
    if (ret < 0)
    {
        printf("send notify fail\n");
    }
    else
    {
        printf("Catalog NOTIFY sent successfully\n");
    }
}

void register_thread(int second, eXosip_t *ctx, eXosip_event_t *event, int rID)
{
    while (1)
    {
        std::this_thread::sleep_for(std::chrono::seconds(second));
        int ret = 0;
        osip_message_t *register_message = NULL;
        eXosip_lock(ctx);
        // 构建注册
        ret = eXosip_register_build_register(ctx, rID, 60, &register_message);
        eXosip_unlock(ctx);
        if (ret != 0)
        {
            printf("eXosip_register_build_register fail:%d\n", ret);
            // 等待上一个注册的操作完成
            std::this_thread::sleep_for(std::chrono::seconds(5)); // 错误时稍等重试
            continue;
        }
        else
        {
            printf("eXosip_register_build_register success:%d\n", ret);
        }
        // eXosip_lock(ctx);
        //  发送注册消息
        {
            std::lock_guard<std::mutex> lock(eXosip_mutex);
            ret = eXosip_register_send_register(ctx, event->rid, register_message);
            // eXosip_unlock(ctx);
            if (ret != 0)
            {
                printf("eXosip_register_send_register fail%d\n", ret); // 释放消息内存
                // std::this_thread::sleep_for(std::chrono::seconds(5)); // 错误时稍等重试
            }
            else
            {
                printf("re REGISTER message sent successfully\n");
            }
        }
    }
}