#include "mpp_decoder.h"
#include <cstring>
#include <iostream>
#include <unistd.h>

MppDecoder::MppDecoder()
{
    mpp_ctx = NULL;
    mpp_mpi = NULL;
    packet = NULL;
    frame = NULL;
    callback = nullptr;
    userdata = nullptr;
    fps = -1;
    id = 0;
    last_frame_time_ms = 0;
    mpp_type = MPP_VIDEO_CodingAVC;
    need_split = 1;
}

MppDecoder::~MppDecoder()
{
    if (loop_data.packet)
    {
        mpp_packet_deinit(&loop_data.packet);
        loop_data.packet = NULL;
    }
    if (frame)
    {
        mpp_frame_deinit(&frame);
        frame = NULL;
    }
    if (mpp_ctx)
    {
        mpp_destroy(mpp_ctx);
        mpp_ctx = NULL;
    }
    if (loop_data.frm_grp)
    {
        mpp_buffer_group_put(loop_data.frm_grp);
        loop_data.frm_grp = NULL;
    }
}

int MppDecoder::Init(int video_type, int fps, void *userdata, int id)
{
    this->id = id;
    MPP_RET ret = MPP_OK;
    this->userdata = userdata;
    this->fps = fps;
    this->last_frame_time_ms = 0;
    
    if (video_type == 264)
    {
        mpp_type = MPP_VIDEO_CodingAVC;
    }
    else if (video_type == 265)
    {
        mpp_type = MPP_VIDEO_CodingHEVC;
    }
    else
    {
        std::cerr << "Unsupported video_type: " << video_type << std::endl;
        return -1;
    }

    memset(&loop_data, 0, sizeof(loop_data));
    
    MppDecCfg cfg = NULL;
    ret = mpp_create(&mpp_ctx, &mpp_mpi);
    if (MPP_OK != ret)
    {
        std::cerr << "mpp_create failed" << std::endl;
        return -1;
    }
    
    ret = mpp_init(mpp_ctx, MPP_CTX_DEC, mpp_type);
    if (ret)
    {
        std::cerr << "mpp_init failed" << std::endl;
        return -1;
    }
    
    mpp_dec_cfg_init(&cfg);
    
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_GET_CFG, cfg);
    if (ret)
    {
        std::cerr << "failed to get decoder cfg ret " << ret << std::endl;
        return -1;
    }
    
    ret = mpp_dec_cfg_set_u32(cfg, "base:split_parse", need_split);
    if (ret)
    {
        std::cerr << "failed to set split_parse ret " << ret << std::endl;
        return -1;
    }
    
    ret = mpp_mpi->control(mpp_ctx, MPP_DEC_SET_CFG, cfg);
    if (ret)
    {
        std::cerr << "failed to set cfg ret " << ret << std::endl;
        return -1;
    }
    
    mpp_dec_cfg_deinit(cfg);
    
    loop_data.ctx = mpp_ctx;
    loop_data.mpi = mpp_mpi;
    loop_data.eos = 0;
    loop_data.frame = NULL;
    
    return 0;
}

int MppDecoder::Reset()
{
    if (mpp_mpi != NULL)
    {
        mpp_mpi->reset(mpp_ctx);
    }
    return 0;
}

int MppDecoder::SetCallback(MppDecoderFrameCallback callback)
{
    this->callback = callback;
    return 0;
}

int MppDecoder::Decode(uint8_t *pkt_data, int pkt_size, int pkt_eos)
{
    MpiDecLoopData *data = &loop_data;
    RK_U32 pkt_done = 0;
    MPP_RET ret = MPP_OK;
    MppCtx ctx = data->ctx;
    MppApi *mpi = data->mpi;
    int got_frames = 0;
    
    if (packet == NULL)
    {
        ret = mpp_packet_init(&packet, NULL, 0);
    }
    
    mpp_packet_set_data(packet, pkt_data);
    mpp_packet_set_size(packet, pkt_size);
    mpp_packet_set_pos(packet, pkt_data);
    mpp_packet_set_length(packet, pkt_size);
    
    if (pkt_eos)
        mpp_packet_set_eos(packet);
    
    do
    {
        RK_S32 times = 5;
        
        if (!pkt_done)
        {
            ret = mpi->decode_put_packet(ctx, packet);
            if (MPP_OK == ret)
                pkt_done = 1;
        }
        
        do
        {
            RK_S32 get_frm = 0;
            RK_U32 frm_eos = 0;
            
        try_again:
            ret = mpi->decode_get_frame(ctx, &frame);
            
            if (MPP_ERR_TIMEOUT == ret)
            {
                if (times > 0)
                {
                    times--;
                    usleep(2000);
                    goto try_again;
                }
            }
            
            if (MPP_OK != ret)
            {
                break;
            }
            
            if (frame)
            {
                RK_U32 buf_size = mpp_frame_get_buf_size(frame);
                
                if (mpp_frame_get_info_change(frame))
                {
                    if (NULL == data->frm_grp)
                    {
                        ret = mpp_buffer_group_get_internal(&data->frm_grp, MPP_BUFFER_TYPE_DRM);
                        if (ret)
                        {
                            std::cerr << "get mpp buffer group failed ret " << ret << std::endl;
                            break;
                        }
                        
                        ret = mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, data->frm_grp);
                        if (ret)
                        {
                            std::cerr << "set buffer group failed ret " << ret << std::endl;
                            break;
                        }
                    }
                    else
                    {
                        ret = mpp_buffer_group_clear(data->frm_grp);
                        if (ret)
                        {
                            std::cerr << "clear buffer group failed ret " << ret << std::endl;
                            break;
                        }
                    }
                    
                    ret = mpp_buffer_group_limit_config(data->frm_grp, buf_size, 24);
                    if (ret)
                    {
                        std::cerr << "limit buffer group failed ret " << ret << std::endl;
                        break;
                    }
                    
                    ret = mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);
                    if (ret)
                    {
                        std::cerr << "info change ready failed ret " << ret << std::endl;
                        break;
                    }
                }
                else
                {
                    RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
                    RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
                    RK_U32 hor_width = mpp_frame_get_width(frame);
                    RK_U32 ver_height = mpp_frame_get_height(frame);
                    
                    got_frames++;
                    
                    if (callback != nullptr)
                    {
                        MppFrameFormat format = mpp_frame_get_fmt(frame);
                        char *data_vir = (char *)mpp_buffer_get_ptr(mpp_frame_get_buffer(frame));
                        callback(this->userdata, hor_stride, ver_stride, hor_width, ver_height, format, 0, data_vir, this->id);
                    }
                }
                
                frm_eos = mpp_frame_get_eos(frame);
                ret = mpp_frame_deinit(&frame);
                frame = NULL;
                get_frm = 1;
            }
            
            if (pkt_eos && pkt_done && !frm_eos)
            {
                usleep(1 * 1000);
                continue;
            }
            
            if (frm_eos)
            {
                break;
            }
            
            if (get_frm)
                continue;
                
            break;
        } while (1);

        if (pkt_done)
            break;
            
        usleep(3 * 1000);
    } while (1);
    
    mpp_packet_deinit(&packet);
    return got_frames > 0;
}