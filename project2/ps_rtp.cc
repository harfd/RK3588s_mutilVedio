/*
 * Copyright (c) 2025-04-01 HeXiaotian
 * Non-commercial use only. Redistribution, resale,
 * and derivative works are prohibited.
 */

#include "ps_rtp.h"

/*** *@remark:	讲传入的数据按地位一个一个的压入数据

*@param :  buffer	[in]  压入数据的buffer

*		   count	[in]  需要压入数据占的位数

*		   bits 	[in]  压入的数值

*/
void bits_write(bits_buffer_s *buffer, int count, uint64_t bits)
{
    while (count > 0)
    {
        count--;
        // 查看当前比特位是多少
        // 如果是1 就压入1

        if ((bits >> count) & 0x01)
        {
            buffer->p_data[buffer->i_data] |= buffer->i_mask; // 将1压入，原始数据是00000...
        }
        else
        {
            // 压入0
            buffer->p_data[buffer->i_data] &= ~(buffer->i_mask);
        }
        // 更新掩码喝字节索引
        buffer->i_mask >>= 1;
        if (buffer->i_mask == 0) // 掩码移到最后一位了
        {
            buffer->i_data++;
            buffer->i_mask = 0x80; // 重置为下一个字节的MSB
        }
    }
}
// ps头的封装
/*** *@remark:	 ps头的封装,里面的具体数据的填写已经占位，可以参考标准

*@param :	pData  [in] 填充ps头数据的地址

*			s64Src [in] 时间戳

*@return:	0 success, others failed

*/
int gb28181_make_ps_header(char *pData, unsigned long long s64Scr)
{
    bits_buffer_s bitsBuffer;
    unsigned long long lScrExt = (s64Scr) % 100;
    bitsBuffer.i_size = PS_HDR_LEN;
    bitsBuffer.i_data = 0;    // 初试的数据
    bitsBuffer.i_mask = 0x80; // 从高位开始写入，掩码
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PS_HDR_LEN);
    // 开始写入
    // 起始位
    bits_write(&bitsBuffer, 32, 0x000001BA);
    // 标志符
    bits_write(&bitsBuffer, 2, 1);
    bits_write(&bitsBuffer, 3, (s64Scr >> 30) & 0x07); // src的高三位
    bits_write(&bitsBuffer, 1, 1);
    /*
    32-30位 | 29-15位 | 14-0位  | 扩展位
    3bits | 15bits | 15bits | 9bits
    */
    bits_write(&bitsBuffer, 15, (s64Scr >> 15) & 0x7FFF); // src的中间的15位
    bits_write(&bitsBuffer, 1, 1);                        /*marker bit*/
    bits_write(&bitsBuffer, 15, s64Scr & 0x7fff);         // 低十五位
    bits_write(&bitsBuffer, 1, 1);                        /*marker bit*/
    bits_write(&bitsBuffer, 9, lScrExt & 0x01ff);
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 22, (255) & 0x3fffff); /*该字段通常为固定值 255（GB28181 默认）*/
    bits_write(&bitsBuffer, 2, 3);                 /*marker bits '11'*/
    bits_write(&bitsBuffer, 5, 0x1f);              /*reserved(reserved for future use)*/

    bits_write(&bitsBuffer, 3, 0);
}
// 系统头
int gb28181_make_sys_header(char *pData)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = SYS_HDR_LEN;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, SYS_HDR_LEN);
    bits_write(&bitsBuffer, 32, 0x000001BB);
    // 为什么是减去6，减去起始的字节 4  还需要减去本身占的两个字节2
    bits_write(&bitsBuffer, 16, SYS_HDR_LEN - 6); /*header_length 表示次字节后面的长度，后面的相关头也是次意思*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 22, 50000); /*rate_bound*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 6, 1);                                                         // audio bound  同时处理音频流的最大数目
    bits_write(&bitsBuffer, 1, 0);                                                         // fixed_flag置位1表示固定比特率操作，置位0则为可变比特率操作
    bits_write(&bitsBuffer, 1, 1);                                                         // cpcs 启用循环播放和 SCR 时钟同步
    bits_write(&bitsBuffer, 1, 1);                                                         //   音频解码必须跟随系统时钟
    bits_write(&bitsBuffer, 1, 1);                                                         // 视频解码必须跟随系统时钟
    bits_write(&bitsBuffer, 1, 1);                                                         /*marker_bit*/
    bits_write(&bitsBuffer, 5, 1);                                                         /*视频流数量上限字段*/
    bits_write(&bitsBuffer, 1, 0); /*设为 0 表示不启用 MPEG-1 之外的特性，确保最大兼容性*/ /*若使用 H.265 等新编码，需设为 1*/
    bits_write(&bitsBuffer, 7, 0x7F);                                                      /*reserver*/
    /*audio stream bound*/
    bits_write(&bitsBuffer, 8, 0xC0); /*标识 音频流 stream_id*/
    bits_write(&bitsBuffer, 2, 3);    /*marker_bit */
    // 前导 stream_id 指示音频流，则 P-STD_buffer_bound_scale 必有‘ 0’值
    bits_write(&bitsBuffer, 1, 0); /*PSTD_buffer_bound_scale*/
    // STD_buffer_bound_scale 有‘0’值，那么 P-STD_buffer_size_bound 以 128 字节为单位度量该缓冲器尺寸限制。
    bits_write(&bitsBuffer, 13, 512); /*PSTD_buffer_size_bound*/

    /*video stream bound*/
    bits_write(&bitsBuffer, 8, 0xE0); /*stream_id*/
                                      // 前导 stream_id 指示视频流，则 P-STD_buffer_bound_scale 必有‘ 1’值
    bits_write(&bitsBuffer, 2, 3);    /*marker_bit */
    bits_write(&bitsBuffer, 1, 1);    /*PSTD_buffer_bound_scale*/
    // /若 P-STD_buffer_bound_scale 有‘ 1’值，那么 P-STD_buffer_size_bound 以 1024 字节为单位度量该缓冲器尺寸限制。
    bits_write(&bitsBuffer, 13, 2048); /*PSTD_buffer_size_bound*/
    return 0;
}
/*** *@remark:	 psm头的封装,里面的具体数据的填写已经占位，可以参考标准

*@param :	pData  [in] 填充ps头数据的地址

*@return:	0 success, others failed

*/
int gb28181_make_psm_header(char *pData)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = PSM_HDR_LEN;
    bitsBuffer.i_data = 0;

    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PSM_HDR_LEN);
    bits_write(&bitsBuffer, 24, 0x000001);
    bits_write(&bitsBuffer, 8, 0xBC); // map_stream_id 节目映射流的标准ID
    bits_write(&bitsBuffer, 16, 18);  /*program stream map length*/
    bits_write(&bitsBuffer, 1, 1);    // current_next_indicator 置‘ 1’时指示发送的节目流映射为当前有效
    bits_write(&bitsBuffer, 2, 3);    // reserve
    // current_next_indicator 置为‘ 1’时， program_stream_map_version 应是当前有效的节目流映射的版本。
    // current_next_indicator 设置为‘ 0’时， program_stream_map_version 应是下一个有效的节目流映射的版本。
    bits_write(&bitsBuffer, 5, 0);    // program_stream_map_version
    bits_write(&bitsBuffer, 7, 0x7F); /*reserved */
    bits_write(&bitsBuffer, 1, 1);    /*marker bit */
    // 指示紧随此字段的描述符的总长
    bits_write(&bitsBuffer, 16, 0); /*programe stream info length节目描述信息（Program Stream Info） 的长度*/
    // stream_id + stream_type + 保留位
    // 2路 × 4字节 = 8字节 → 故 elementary_stream_map_length=8。
    bits_write(&bitsBuffer, 16, 8); /*elementary stream map length  is  */

    /*audio*/
    bits_write(&bitsBuffer, 8, 0x90); /*stream_type*/
    bits_write(&bitsBuffer, 8, 0xC0); /*elementary_stream_id*/
    bits_write(&bitsBuffer, 16, 0);   /*elementary_stream_info_length is*/
    /*video*/
    bits_write(&bitsBuffer, 8, 0x1B);                          /*stream_type  h264*/
    bits_write(&bitsBuffer, 8, 0xE0); /*elementary_stream_id*/ // 代表视频流
    bits_write(&bitsBuffer, 16, 0);                            /*elementary_stream_info_length */

    // CRC 校验 crc (2e b9 0f 3d)
    bits_write(&bitsBuffer, 8, 0x45); // CRC 位24~31（最高字节）
    bits_write(&bitsBuffer, 8, 0xBD); // CRC 位16~23
    bits_write(&bitsBuffer, 8, 0xDC); // CRC 位8~15
    bits_write(&bitsBuffer, 8, 0xF4); // CRC 位0~7（最低字节）
    return 0;
}
/*** *@remark:	 pes头的封装

*@param :	pData	   [in] 填充ps头数据的地址

*			stream_id  [in] 码流类型

*			paylaod_len[in] 负载长度

*			pts 	   [in] 时间戳

*			dts 	   [in]

*@return:	0 success, others failed

*/
// 解码时间戳 DTS 指示解码器何时对该帧进行解码。
// PTS显示时间戳
int gb28181_make_pes_header(char *pData, int stream_id, int payload_len, unsigned long long pts, unsigned long long dts)
{
    bits_buffer_s bitsBuffer;
    bitsBuffer.i_size = PES_HDR_LEN;
    bitsBuffer.i_data = 0;
    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, PES_HDR_LEN);
    bits_write(&bitsBuffer, 24, 0x000001); /*start code*/
    bits_write(&bitsBuffer, 8, (stream_id));
    bits_write(&bitsBuffer, 16, (payload_len) + 13); /*packet_len*/
    bits_write(&bitsBuffer, 2, 2);                   /*'10' */
    // 表示PES分组有效负载的加密模式，不加密时建议置0，否则置01，10和11保留。
    bits_write(&bitsBuffer, 2, 0); /*scrambling_control*/
    // 指示PES分组负载的优先级。1高优先级，0低优先级。非参考帧（包括B帧和，E帧，P帧）应置0，其余帧置1。
    bits_write(&bitsBuffer, 1, 0); /*priority*/
    // 置１表明此PES分组头部之后是data_stream_alignment_descriptor所定义的访问单元数据类型。
    // 若为‘０’，则没有定义是否对准。建议当是输入单元的第一个包时置1，其余置0。
    bits_write(&bitsBuffer, 1, 0); /*data_alignment_indicator*/
    //	置‘１’，表明相应PES分组的有效负载中的数据是有版权的，建议置0。
    bits_write(&bitsBuffer, 1, 0); /*copyright*/
    bits_write(&bitsBuffer, 1, 0); /*original_or_copy,建议置1*/

    bits_write(&bitsBuffer, 1, 1); /*PTS_flag*/
    bits_write(&bitsBuffer, 1, 1); /*DTS_flag*/
    // 置‘１’表示ESCR的base和extension字段都在PES分组首部出现；建议置0。
    bits_write(&bitsBuffer, 1, 0); /*ESCR_flag*/
    // 置‘１’表示PES首部有ES_rate字段；建议置0。
    bits_write(&bitsBuffer, 1, 0); /*ES_rate_flag*/
    bits_write(&bitsBuffer, 1, 0); /*DSM_trick_mode_flag*/

    bits_write(&bitsBuffer, 1, 0); /*additional_copy_info_flag*/

    bits_write(&bitsBuffer, 1, 0); /*PES_CRC_flag*/

    bits_write(&bitsBuffer, 1, 0); /*PES_extension_flag*/

    bits_write(&bitsBuffer, 8, 10); /*header_data_length*/
    /*PTS,DTS*/
    // 0011 表示 PTS
    bits_write(&bitsBuffer, 4, 3);                    /*'0011'*/
    bits_write(&bitsBuffer, 3, ((pts) >> 30) & 0x07); /*PTS[32..30]*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 15, ((pts) >> 15) & 0x7FFF); /*PTS[29..15]*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 15, (pts) & 0x7FFF); /*PTS[14..0]*/
    bits_write(&bitsBuffer, 1, 1);
    // 0001 表示 DTS
    bits_write(&bitsBuffer, 4, 1);                    /*'0001'*/
    bits_write(&bitsBuffer, 3, ((dts) >> 30) & 0x07); /*DTS[32..30]*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 15, ((dts) >> 15) & 0x7FFF); /*DTS[29..15]*/
    bits_write(&bitsBuffer, 1, 1);
    bits_write(&bitsBuffer, 15, (dts) & 0x7FFF); /*DTS[14..0]*/
    bits_write(&bitsBuffer, 1, 1);
    return 0;
}
// 检测给定的缓冲区中是否有h264的起始码
/*
    起始码（Start Code）用于分隔 NALU
    常见形式为 00 00 01（3 字节）和 00 00 00 01（4 字节）。
*/
int findStartCode(char *buf, int zeros_in_startcode)
{
    int info = 1; // 假设是起始码
    int i;
    for (i = 0; i < zeros_in_startcode; i++)

        if (buf[i] != 0) // 前n个字节必须是0
            info = 0;
    if (buf[i] != 1) // 最后一个字节必须是1
        info = 0;

    return info; // 返回1 就是起始码
}

/**
 * Reads the next NAL unit from the input file.
 *
 * @param inpf 文件
 * @param buf 存储NAL单元
 * @return 返回NAL单元的实际长度 取出起始符
 */

// rtp头
/**
 * 打包rtp头.
 *
 * @param pData 指向rtp头的缓冲区
 * @param marker_flag rtp中的marker位
 * @param cseq RTP 序列号
 * @param curpts RTP 时间戳
 * @param ssrc RTP 同步源标识
 * @return Length of the NAL unit (excluding start code), or 0 if EOF reached
 */
/*
    rtp报文
0                   1                   2                   3
0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| V |P|X|  CC   |M|     PT      |       Sequence Number         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Timestamp                           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Synchronization Source (SSRC) identifier            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
int gb28181_make_rtp_header(char *pData, int marker_flag, unsigned short cseq, long long curpts, unsigned int ssrc)
{
    bits_buffer_s bitsBuffer;
    if (pData == NULL)

        return -1;
    bitsBuffer.i_size = RTP_HDR_LEN; // rtp头部长度位12字节
    bitsBuffer.i_data = 0;

    bitsBuffer.i_mask = 0x80;
    bitsBuffer.p_data = (unsigned char *)(pData);
    memset(bitsBuffer.p_data, 0, RTP_HDR_LEN);
    bits_write(&bitsBuffer, 2, RTP_VERSION);   // rtp的版本号
    bits_write(&bitsBuffer, 1, 0);             // 填充位，一般位0
    bits_write(&bitsBuffer, 1, 0);             // 扩展位
    bits_write(&bitsBuffer, 4, 0);             // CSRC 计数
    bits_write(&bitsBuffer, 1, (marker_flag)); // 表示当前 RTP 数据包是否是该帧的最后一个包 M ，对于视频，标记一帧的结束
    bits_write(&bitsBuffer, 7, 96);            // PT payloadType 负载类型  这边是h264  所以对应96
    bits_write(&bitsBuffer, 16, (cseq));       // Sequence Number 序列号
    bits_write(&bitsBuffer, 32, (curpts));     // 时间戳 表示当前 RTP 包的时间戳。//同步音视频播放时间
    bits_write(&bitsBuffer, 32, (ssrc));       // 标识 RTP 流
    return 0;
}

/**
 * 打包rtp头.
 *
 * @param pData H.264 码流数据，pes部分
 * @param nFrameLen H.264 码流数据长度
 * @param pPacker 存储 RTP 打包相关信息
 * @param stream_type  流类型（0：视频，1：音频）
 * @return
 */

/*
   视频关键帧的封装 RTP + PS header + PS system header + PS system Map + PES header +h264 data
   视频非关键帧的封装 RTP +PS header + PES header + h264 data
   音频帧的封装: RTP + PES header + G711
    [PS头][系统头][PSM][PES(视频)][PES(音频)][PES(视频)]...
*/
int gb28181_streampackageForH264(char *pData, int nFrameLen, Data_Info_s *pPacker, int stream_type, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    int is_keyframe = 0;
    char szTempPacketHead[256]; // 临时存放头部数据
    int nSizePos = 0;
    int nSize = 0; // 当前分片的有效载荷（H.264数据）长度
    char *pBuff = NULL;
    memset(szTempPacketHead, 0, 256);
    // 1、ps head
    // PS头标志着一个数据包的开始
    gb28181_make_ps_header(szTempPacketHead + nSizePos, pPacker->s64CurPts);
    nSizePos += PS_HDR_LEN; // 将后移动ps头各位置，位系统头腾位置
    if (nFrameLen >= 4)
    {
        uint8_t *nal_start = (uint8_t *)pData;
        if (nal_start[0] == 0x00 && nal_start[1] == 0x00 &&
            (nal_start[2] == 0x01 || (nal_start[2] == 0x00 && nal_start[3] == 0x01)))
        {
            uint8_t nal_type = nal_start[2] == 0x01 ? nal_start[3] & 0x1f : nal_start[4] & 0x1f;
            if (nal_type == 0x05)
            {
                is_keyframe = 1;
            }
        }
    }
    if (is_keyframe == 1)
    {
        gb28181_make_sys_header(szTempPacketHead + nSizePos);
        nSizePos += SYS_HDR_LEN;
        gb28181_make_psm_header(szTempPacketHead + nSizePos);
        nSizePos += PSM_HDR_LEN;
        // 加上rtp头发送
    }
    if (gb28181_send_rtp_pack(szTempPacketHead, nSizePos, 0, pPacker, __cp, __hostshort, udp_socket) != 0)
        return -1;
    /*
    PES包
    [PES头][Payload数据]
    */
    pBuff = pData - PES_HDR_LEN; // 避免使用memcpy  零拷贝
    while (nFrameLen > 0)
    {
        // pes包的长度不超过65535
        if (nFrameLen > PS_PES_PAYLOAD_SIZE) // 如果超过最大限度
        {
            nSize = PS_PES_PAYLOAD_SIZE; // 超过最大负载，按分片大小切割
        }
        else
        {
            nSize = nFrameLen;
        }
        // 添加pes头 ，0XC0音频流 0xE0视频流（默认）
        gb28181_make_pes_header(pBuff, stream_type ? 0xC0 : 0xE0, nSize, pPacker->s64CurPts, pPacker->s64CurPts);
        // 添加rtp头发送数据
        // nSize + PES_HDR_LEN h264数据和pes头数据
        if (gb28181_send_rtp_pack(pBuff, nSize + PES_HDR_LEN, ((nSize == nFrameLen) ? 1 : 0), pPacker, __cp, __hostshort, udp_socket))
        {
            printf("gb28181_send_pack failed!\n");
            return -1;
        }
        // 分片后每次发送的数据移动指针操作

        nFrameLen -= nSize;
        pBuff += nSize;
    }
    return 0;
}
int gb28181_streampackageForH264_first(std::vector<H264Frame> data, struct Data_Info_s *pPacker,
                                       int stream_type, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    // 1. 参数校验
    if (data.size() < 3 || !pPacker || !__cp)
    {
        return -1; // 错误码：参数无效
    }

    // 2. 计算各部分大小
    const int total_header_size = PS_HDR_LEN + SYS_HDR_LEN + PSM_HDR_LEN;
    const int pes_header_size = PES_HDR_LEN;
    const int max_payload = RTP_MAX_PACKET_BUFF - RTP_HDR_LEN - total_header_size;

    // 3. 分配内存（使用智能指针避免泄漏）
    std::vector<char> packet_buffer(RTP_MAX_PACKET_BUFF, 0);
    char *pBuff = packet_buffer.data();

    // 4. 构建PS头
    gb28181_make_ps_header(pBuff, pPacker->s64CurPts);
    gb28181_make_sys_header(pBuff + PS_HDR_LEN);
    gb28181_make_psm_header(pBuff + PS_HDR_LEN + SYS_HDR_LEN);

    // 5. 处理SPS/PPS/IDR数据
    int offset = total_header_size;

    // SPS
    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                static_cast<int>(data[0].data.size()),
                                pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2; // PES头生成失败
    }
    offset += pes_header_size;
    memcpy(pBuff + offset,
           data[0].data.data(),
           data[0].data.size());
    offset += static_cast<int>(data[0].data.size());

    // PPS
    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                static_cast<int>(data[1].data.size()),
                                pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2;
    }
    offset += pes_header_size;
    memcpy(pBuff + offset,
           data[1].data.data(),
           data[1].data.size());
    offset += static_cast<int>(data[1].data.size());

    // IDR帧第一部分
    int remaining_idr_size = max_payload - (offset - total_header_size) - pes_header_size;
    if (remaining_idr_size <= 0 ||
        remaining_idr_size > static_cast<int>(data[2].data.size()))
    {
        return -3; // 缓冲区空间不足
    }

    if (gb28181_make_pes_header(pBuff + offset, stream_type ? 0xC0 : 0xE0,
                                remaining_idr_size, pPacker->s64CurPts, pPacker->s64CurPts))
    {
        return -2;
    }
    offset += pes_header_size;
    memcpy(pBuff + offset,
           data[2].data.data(),
           remaining_idr_size);
    offset += remaining_idr_size;

    // 6. 发送第一个RTP包
    if (gb28181_send_rtp_pack(pBuff, offset, 0, pPacker, __cp, __hostshort, udp_socket) < 0)
    {
        return -4; // 发送失败
    }
    printf("send first frame\n");
    // 7. 处理剩余IDR数据
    int remaining_data = static_cast<int>(data[2].data.size()) - remaining_idr_size;
    // 未发送的起始位置
    char *pRemainingData = reinterpret_cast<char *>(data[2].data.data()) + remaining_idr_size;

    while (remaining_data > 0)
    {
        int chunk_size = 0;
        if (remaining_data > PS_PES_PAYLOAD_SIZE)
        {
            chunk_size = PS_PES_PAYLOAD_SIZE;
        }
        else
        {
            // 一把发送
            chunk_size = remaining_data;
        }

        if (gb28181_send_rtp_pack(pRemainingData, chunk_size, ((chunk_size == remaining_data) ? 1 : 0), pPacker, __cp, __hostshort, udp_socket) < 0)
        {
            return -4;
        }

        pRemainingData += chunk_size;
        remaining_data -= chunk_size;
    }

    return 0; // 成功
}

// 发送数据包
/**
 * 打包rtp头.
 *
 * @param buff 要发送的数据
 * @param size 发送数据的长度
 * @return
 */
#include <unistd.h> // 引入 usleep() 用于延迟
#include <poll.h>
int SendDataBuff(char *buff, int size, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    // 1. 绑定源端口（固定为 50086）
    // printf("Debug: Socket fd=%d\n", udp_socket); // 调试

    // 2. 设置目标地址
    struct sockaddr_in addr_serv;
    memset(&addr_serv, 0, sizeof(addr_serv));
    addr_serv.sin_family = AF_INET;
    addr_serv.sin_addr.s_addr = inet_addr(__cp); // 目标 IP（如 "103.85.84.253"）
    addr_serv.sin_port = htons(__hostshort);     // 目标端口（如 16053）
    socklen_t len = sizeof(addr_serv);
    struct pollfd fds;
    fds.fd = udp_socket;
    fds.events = POLLOUT; // 监听套接字是否可写

    // 3. 发送数据（重试 3 次）
    int max_retries = 3;
    for (int attempt = 1; attempt <= max_retries; attempt++)
    {
        // 使用 poll 等待套接字可写
        int poll_result = poll(&fds, 1, 100); // 阻塞100ms
        if (poll_result > 0)                  // poll 返回 > 0 说明套接字可写
        {
            int ret = sendto(udp_socket, buff, size, 0, (struct sockaddr *)&addr_serv, len);
            if (ret > 0)
            {
                // printf("SUCCESS Sent %d/%d bytes (Src Port: 50086, Dst Port: %d)\n",
                //        ret, size, __hostshort);
                return ret;
            }
            else
            {
                perror("[ERROR] sendto failed");
                printf("send fail:%d\n", ret);
            }
        }
        else if (poll_result == 0)
        {
            // poll 返回 0 表示超时（没有事件发生）
            printf("[INFO] Timeout, retrying...\n");
        }
        else
        {
            // poll 出现错误
            perror("[ERROR] poll failed");
            return -1;
        }

        printf("[INFO] Retrying %d/%d...\n", attempt, max_retries);
    }

    printf("[ERROR] Send failed after %d retries\n", max_retries);
    return -1;
}

/**
 * 发送rtp包
 *
 * @param databuff 指向要发送的数据的缓冲区的指针。databuff不包括rtp头部数据，只有载荷部分
 * @param nDataLen 数据的长度 包括数据负载的大小（即不包括 RTP 头部）
 * @param mark_flag  用来指示当前的 RTP 包是否为一个数据流的最后一个包
 * @param pPacker 指向 Data_Info_s 类型结构体的指针
 * @return
 */
int gb28181_send_rtp_pack(char *databuff, int nDataLen, int mark_flag, Data_Info_s *pPacker, const char *__cp, uint16_t __hostshort, int udp_socket)
{
    // 保存原始 PS 数据
    FILE *fp = fopen("output.ps", "ab");
    if (fp)
    {
        fwrite(databuff, 1, nDataLen, fp);
        fclose(fp);
    }

    // rtp头部信息不能和载荷一起处理，需要分开
    int nRes = 0;
    int nPlayLoadLen = 0;       // 每次发送的数据大小,不包括rtp头部，每个数据包的有效载荷，也就是ps流的大小
    int nSendSize = 0;          // 实际发送的字节数
    char szRtpHdr[RTP_HDR_LEN]; // 存放rtp头部信息
    memset(szRtpHdr, 0, RTP_HDR_LEN);
    // 所有RTP有效数据（NALU）的长度不能超过1460字节，否则就需要分片发送
    if (nDataLen + RTP_HDR_LEN <= RTP_MAX_PACKET_BUFF)
    {

        // 发送数据，一帧
        // 每发送一个新的rtp包，序列号都应该递增，这样接收方就可以根据序列号的顺序来恢复包的顺序，进行正确的解码和播放
        // pPacker->s64CurPts 接收端正确同步音视频的播放 时间戳
        // 同步源标识符
        gb28181_make_rtp_header(szRtpHdr, ((mark_flag == 1) ? 1 : 0), ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
        // 拷贝rtp头部信息到包中
        memcpy(pPacker->szBuff, szRtpHdr, RTP_HDR_LEN);
        // 拷贝载荷到包中
        memcpy(pPacker->szBuff + RTP_HDR_LEN, databuff, nDataLen);
        // 发送完整的一帧
        nRes = SendDataBuff(pPacker->szBuff, nDataLen + RTP_HDR_LEN, __cp, __hostshort, udp_socket);
        if (nRes != (RTP_HDR_LEN + nDataLen))
        {
            printf(" udp send error2 !\n");
            return -1;
        }
    }
    else
    {
        // 分片处理
        nPlayLoadLen = RTP_MAX_PACKET_BUFF - RTP_HDR_LEN;
        // 先存入rtp头部信息
        gb28181_make_rtp_header(pPacker->szBuff, 0, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
        // 拷贝rtp载荷的数据
        memcpy(pPacker->szBuff + RTP_HDR_LEN, databuff, nPlayLoadLen);
        nRes = SendDataBuff(pPacker->szBuff, RTP_HDR_LEN + nPlayLoadLen, __cp, __hostshort, udp_socket);
        if (nRes != (RTP_HDR_LEN + nPlayLoadLen))
        {
            printf(" udp send error1 !\n");
            return -1;
        }
        // 更新剩下的数据长度
        nDataLen -= nPlayLoadLen;
        databuff += nPlayLoadLen; // 前nPlayLoadLen个数据已经发送，需要将指针往后指
        // 还需要为rtp头部信息存储
        databuff -= RTP_HDR_LEN;
        while (nDataLen > 0)
        {
            // 检查当前剩余的数据长度 (nDataLen) 是否小于或等于每个 RTP 包所能承载的最大有效载荷大小
            if (nDataLen <= nPlayLoadLen)
            {
                // 一帧数据发送完，置mark标志位
                gb28181_make_rtp_header(databuff, mark_flag, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
                nSendSize = nDataLen; // 实际发送的长度
            }
            else
            {
                // 没有发送完整
                gb28181_make_rtp_header(databuff, 0, ++pPacker->u16CSeq, pPacker->s64CurPts, pPacker->u32Ssrc);
                nSendSize = nPlayLoadLen;
            }
            nRes = SendDataBuff(databuff, RTP_HDR_LEN + nSendSize, __cp, __hostshort, udp_socket);
            if (nRes != (RTP_HDR_LEN + nSendSize))
            {
                printf(" udp send error3 !\n");
                return -1;
            }
            nDataLen -= nSendSize; // 更新剩下的数据的大小
            databuff += nSendSize; // 前nSendSize个数据已经发送，需要将指针往后指
        }
    }
    return 0;
}
