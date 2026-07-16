/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef PROJECT2_XML_UTILS_H
#define PROJECT2_XML_UTILS_H

#include <string>

int utf8_to_gb2312(const char *inbuf, char *outbuf, size_t outbufsize);
std::string extractSN(const std::string &xml);
std::string extractCmdType(const char *xmlBody);

#endif /* PROJECT2_XML_UTILS_H */
