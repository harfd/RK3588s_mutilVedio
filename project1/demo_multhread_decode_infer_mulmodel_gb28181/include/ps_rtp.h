/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#ifndef PROJECT2_PS_RTP_H
#define PROJECT2_PS_RTP_H

#include <stdint.h>
#include <string>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "decode.h"

#define PS_HDR_LEN 14
#define SYS_HDR_LEN 18
#define PSM_HDR_LEN 24
#define PES_HDR_LEN 19
#define RTP_HDR_LEN 12
#define RTP_VERSION 2
#define RTP_MAX_PACKET_BUFF 1400
#define PS_PES_PAYLOAD_SIZE 65522

struct bits_buffer_s
{
    unsigned char *p_data;
    unsigned char i_mask;
    int i_size;
    int i_data;
};

struct Data_Info_s
{
    uint64_t s64CurPts;
    int IFrame;
    uint16_t u16CSeq;
    uint32_t u32Ssrc;
    char szBuff[RTP_MAX_PACKET_BUFF];
};

int gb28181_make_rtp_header(char *pData, int marker_flag, unsigned short cseq, long long curpts, unsigned int ssrc);
int gb28181_make_pes_header(char *pData, int stream_id, int payload_len, unsigned long long pts, unsigned long long dts);
int gb28181_make_psm_header(char *pData);
int gb28181_make_sys_header(char *pData);
int gb28181_send_rtp_pack(char *databuff, int nDataLen, int mark_flag, Data_Info_s *pPacker, const char *dest_ip, uint16_t dest_port, int udp_socket);
int gb28181_make_ps_header(char *pData, unsigned long long s64Scr);
int SendDataBuff(char *buff, int size, const char *dest_ip, uint16_t dest_port, int udp_socket);
int findStartCode(char *buf, int zeros_in_startcode);
int gb28181_streampackageForH264(char *pData, int nFrameLen, Data_Info_s *pPacker, int stream_type, const char *dest_ip, uint16_t dest_port, int udp_socket);
int gb28181_streampackageForH264_first(std::vector<H264Frame> data, Data_Info_s *pPacker, int stream_type, const char *dest_ip, uint16_t dest_port, int udp_socket);

#endif /* PROJECT2_PS_RTP_H */
