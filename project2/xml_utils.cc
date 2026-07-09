/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include "xml_utils.h"
#include <iconv.h>
#include <cstring>
#include <regex>

int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize)
{
    iconv_t cd = iconv_open("GB2312", "UTF-8");
    if (cd == (iconv_t)-1)
        return -1;
    char *in = (char *)inbuf;
    char *out = outbuf;
    size_t inbytesleft = strlen(inbuf);
    size_t outbytesleft = outbufsize - 1;
    memset(outbuf, 0, outbufsize);
    if (iconv(cd, &in, &inbytesleft, &out, &outbytesleft) == (size_t)-1)
    {
        iconv_close(cd);
        return -1;
    }
    iconv_close(cd);
    return 0;
}

std::string extractSN(const std::string &xml)
{
    std::regex sn_regex("<SN>(\\d+)</SN>");
    std::smatch match;
    if (std::regex_search(xml, match, sn_regex))
        return match[1].str();
    return "";
}

std::string extractCmdType(const char *xmlBody)
{
    std::regex pattern("<CmdType>(.*?)</CmdType>");
    std::cmatch matches;
    if (std::regex_search(xmlBody, matches, pattern) && matches.size() > 1)
        return matches[1].str();
    return "";
}
