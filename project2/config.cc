/*
 * GB28181 配置集中管理
 */

#include "config.h"

#include <fstream>
#include <sstream>
#include <iostream>

namespace
{
void trim(std::string &s)
{
    const char *ws = " \t\r\n";
    auto start = s.find_first_not_of(ws);
    auto end = s.find_last_not_of(ws);
    if (start == std::string::npos)
    {
        s.clear();
        return;
    }
    s = s.substr(start, end - start + 1);
}

void load_from_file(GBConfig &cfg)
{
    std::ifstream ifs("gb28181.conf");
    if (!ifs.is_open())
    {
        std::cout << "[Config] gb28181.conf not found, using built-in defaults." << std::endl;
        return;
    }

    std::cout << "[Config] Loading gb28181.conf" << std::endl;

    std::string line;
    while (std::getline(ifs, line))
    {
        trim(line);
        if (line.empty() || line[0] == '#')
            continue;

        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        trim(key);
        trim(value);

        if (key == "serveIp")
            cfg.serveIp = value;
        else if (key == "servePort")
            cfg.servePort = value;
        else if (key == "serveName")
            cfg.serveName = value;
        else if (key == "servePassword")
            cfg.servePassword = value;
        else if (key == "deviceIp")
            cfg.deviceIp = value;
        else if (key == "devicePort")
            cfg.devicePort = value;
        else if (key == "deviceName")
            cfg.deviceName = value;
        else if (key == "pushPort")
            cfg.pushPort = value;
    }
}
} // namespace

const GBConfig &get_gb_config()
{
    static GBConfig cfg;
    static bool initialized = false;

    if (!initialized)
    {
        
        cfg.devicePort = "15060";
        cfg.deviceIp = "192.168.137.70";
        cfg.deviceName = "32000000001320104389";

        cfg.servePort = "5060";
        cfg.serveIp = "192.168.137.1";
        cfg.serveName = "34020000002000000001";
        cfg.servePassword = "123456";

        cfg.pushPort = "7000";

        load_from_file(cfg);
        initialized = true;
    }

    return cfg;
}

