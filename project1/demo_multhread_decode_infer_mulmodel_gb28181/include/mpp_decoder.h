#ifndef _MPP_DECODER_H_
#define _MPP_DECODER_H_

#include <stdint.h>
#include <functional>
#include <mutex>
#include <cstring>
#include <rockchip/mpp_frame.h>
#include <rockchip/rk_mpi.h>

typedef std::function<void(void *userdata, int width_stride, int height_stride, int width, int height, int format, int fd, void *data, int id)> MppDecoderFrameCallback;

typedef struct
{
    MppCtx ctx;
    MppApi *mpi;
    RK_U32 eos;
    MppBufferGroup frm_grp;
    MppBufferGroup pkt_grp;
    MppPacket packet;
    MppFrame frame;
    size_t max_usage;
} MpiDecLoopData;

class MppDecoder
{
public:
    MppCtx mpp_ctx = NULL;
    MppApi *mpp_mpi = NULL;
    MppDecoder();
    ~MppDecoder();

    int Init(int video_type, int fps, void *userdata, int id);
    int Reset();
    int SetCallback(MppDecoderFrameCallback callback);
    int Decode(uint8_t *pkt_data, int pkt_size, int pkt_eos);

private:
    MppParam mpp_param1 = NULL;
    RK_U32 need_split = 1;
    RK_U32 width_mpp;
    RK_U32 height_mpp;
    MppCodingType mpp_type;
    size_t packet_size = 2400 * 1300 * 3 / 2;
    MpiDecLoopData loop_data;
    MppPacket packet = NULL;
    MppFrame frame = NULL;
    MppDecoderFrameCallback callback;
    int fps = -1;
    unsigned long last_frame_time_ms = 0;
    void *userdata = NULL;
    int id = 0;
    std::mutex mtx;
};

#endif