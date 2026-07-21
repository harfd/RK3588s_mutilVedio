/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include "v4l2_camera.h"

#include "dma_alloc.hpp"
#include "im2d.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace
{
int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;
    do
    {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

void logErrno(const char *operation, const std::string &device_path)
{
    std::cerr << operation << " failed for " << device_path
              << ": " << std::strerror(errno) << std::endl;
}
} // namespace

V4L2Camera::~V4L2Camera()
{
    close();
}

int V4L2Camera::open(const std::string &device_path, int requested_width,
                     int requested_height, int requested_fps,
                     bool use_nv21, bool auto_white_balance)
{
    close();
    device_path_ = device_path;
    requested_width_ = requested_width > 0
                           ? requested_width
                           : kDefaultRequestedWidth;
    requested_height_ = requested_height > 0
                            ? requested_height
                            : kDefaultRequestedHeight;
    requested_fps_ = requested_fps;
    use_nv21_ = use_nv21;
    auto_white_balance_ = auto_white_balance;
    white_balance_gains_ = cv::Vec3f(1.0f, 1.0f, 1.0f);

    camera_fd_ = ::open(device_path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (camera_fd_ < 0)
    {
        logErrno("open camera", device_path_);
        return -errno;
    }

    int ret = configureDevice();
    if (ret < 0)
    {
        close();
        return ret;
    }

    ret = allocateBuffers();
    if (ret < 0)
    {
        close();
        return ret;
    }

    for (uint32_t i = 0; i < capture_buffers_.size(); ++i)
    {
        ret = queueBuffer(i);
        if (ret < 0)
        {
            close();
            return ret;
        }
    }

    enum v4l2_buf_type type = static_cast<enum v4l2_buf_type>(buffer_type_);
    if (xioctl(camera_fd_, VIDIOC_STREAMON, &type) < 0)
    {
        ret = -errno;
        logErrno("VIDIOC_STREAMON", device_path_);
        close();
        return ret;
    }

    streaming_ = true;
    std::cout << "V4L2 DMA-BUF camera opened: " << device_path_
              << ", source=" << source_width_ << "x" << source_height_
              << ", stride=" << source_stride_
              << ", size=" << source_size_
              << ", output=" << kOutputWidth << "x" << kOutputHeight
              << ", chroma=" << (use_nv21_ ? "VU(NV21)" : "UV(NV12)")
              << ", range=" << (full_range_ ? "full" : "limited")
              << ", auto_wb=" << (auto_white_balance_ ? "on" : "off")
              << std::endl;
    return 0;
}

int V4L2Camera::configureDevice()
{
    struct v4l2_capability capability = {};
    if (xioctl(camera_fd_, VIDIOC_QUERYCAP, &capability) < 0)
    {
        logErrno("VIDIOC_QUERYCAP", device_path_);
        return -errno;
    }

    uint32_t capabilities = capability.capabilities;
    if (capabilities & V4L2_CAP_DEVICE_CAPS)
        capabilities = capability.device_caps;

    if (!(capabilities & V4L2_CAP_STREAMING))
    {
        std::cerr << device_path_ << " does not support V4L2 streaming" << std::endl;
        return -ENOTSUP;
    }

    if (capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    {
        is_multiplanar_ = true;
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    }
    else if (capabilities & V4L2_CAP_VIDEO_CAPTURE)
    {
        is_multiplanar_ = false;
        buffer_type_ = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    }
    else
    {
        std::cerr << device_path_ << " is not a V4L2 capture device" << std::endl;
        return -ENOTSUP;
    }

    struct v4l2_format format = {};
    format.type = buffer_type_;

    if (is_multiplanar_)
    {
        format.fmt.pix_mp.width = requested_width_;
        format.fmt.pix_mp.height = requested_height_;
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    }
    else
    {
        format.fmt.pix.width = requested_width_;
        format.fmt.pix.height = requested_height_;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix.field = V4L2_FIELD_ANY;
    }

    if (xioctl(camera_fd_, VIDIOC_S_FMT, &format) < 0)
    {
        logErrno("VIDIOC_S_FMT(NV12)", device_path_);
        return -errno;
    }

    if (requested_fps_ > 0)
    {
        struct v4l2_streamparm stream_params = {};
        stream_params.type = buffer_type_;
        stream_params.parm.capture.timeperframe.numerator = 1;
        stream_params.parm.capture.timeperframe.denominator = requested_fps_;
        if (xioctl(camera_fd_, VIDIOC_S_PARM, &stream_params) < 0)
        {
            std::cerr << "VIDIOC_S_PARM(" << requested_fps_
                      << " fps) is not supported by " << device_path_
                      << "; continuing with the driver frame rate" << std::endl;
        }
    }

    uint32_t pixel_format;
    uint32_t quantization;
    if (is_multiplanar_)
    {
        pixel_format = format.fmt.pix_mp.pixelformat;
        quantization = format.fmt.pix_mp.quantization;
        plane_count_ = format.fmt.pix_mp.num_planes;
        source_width_ = static_cast<int>(format.fmt.pix_mp.width);
        source_height_ = static_cast<int>(format.fmt.pix_mp.height);

        if (plane_count_ != 1)
        {
            std::cerr << "NV12 camera returned " << plane_count_
                      << " memory planes; this loader requires one contiguous NV12 plane"
                      << std::endl;
            return -ENOTSUP;
        }

        source_stride_ = static_cast<int>(format.fmt.pix_mp.plane_fmt[0].bytesperline);
        source_size_ = format.fmt.pix_mp.plane_fmt[0].sizeimage;
    }
    else
    {
        pixel_format = format.fmt.pix.pixelformat;
        quantization = format.fmt.pix.quantization;
        plane_count_ = 1;
        source_width_ = static_cast<int>(format.fmt.pix.width);
        source_height_ = static_cast<int>(format.fmt.pix.height);
        source_stride_ = static_cast<int>(format.fmt.pix.bytesperline);
        source_size_ = format.fmt.pix.sizeimage;
    }

    if (pixel_format != V4L2_PIX_FMT_NV12)
    {
        std::cerr << device_path_ << " did not accept NV12 format" << std::endl;
        return -ENOTSUP;
    }

    full_range_ = quantization == V4L2_QUANTIZATION_FULL_RANGE;

    if (source_stride_ <= 0)
        source_stride_ = source_width_;

    const size_t minimum_size = static_cast<size_t>(source_stride_) *
                                static_cast<size_t>(source_height_) * 3 / 2;
    if (source_size_ < minimum_size)
    {
        std::cerr << "Invalid NV12 sizeimage: " << source_size_
                  << ", expected at least " << minimum_size << std::endl;
        return -EINVAL;
    }

    return 0;
}

int V4L2Camera::allocateBuffers()
{
    struct v4l2_requestbuffers request = {};
    request.count = kCaptureBufferCount;
    request.type = buffer_type_;
    request.memory = V4L2_MEMORY_DMABUF;

    if (xioctl(camera_fd_, VIDIOC_REQBUFS, &request) < 0)
    {
        logErrno("VIDIOC_REQBUFS(DMABUF)", device_path_);
        return -errno;
    }
    if (request.count < 2)
    {
        std::cerr << "V4L2 driver returned too few DMA-BUF slots: "
                  << request.count << std::endl;
        return -ENOMEM;
    }

    capture_buffers_.resize(request.count);
    for (DmaBuffer &buffer : capture_buffers_)
    {
        buffer.size = source_size_;
        int ret = dma_buf_alloc(DMA_HEAP_PATH, buffer.size, &buffer.fd, &buffer.va);
        if (ret < 0)
        {
            std::cerr << "Failed to allocate camera DMA-BUF from "
                      << DMA_HEAP_PATH << std::endl;
            return ret;
        }
        buffer.rga_handle = importbuffer_fd(
            buffer.fd, static_cast<int>(buffer.size));
        if (buffer.rga_handle == 0)
        {
            std::cerr << "Failed to import camera DMA-BUF into RGA" << std::endl;
            return -EIO;
        }
    }

    scaled_nv12_.size = static_cast<size_t>(kOutputWidth) * kOutputHeight * 3 / 2;
    int ret = dma_buf_alloc(DMA_HEAP_PATH, scaled_nv12_.size,
                            &scaled_nv12_.fd, &scaled_nv12_.va);
    if (ret < 0)
        return ret;
    scaled_nv12_.rga_handle = importbuffer_fd(
        scaled_nv12_.fd, static_cast<int>(scaled_nv12_.size));
    if (scaled_nv12_.rga_handle == 0)
    {
        std::cerr << "Failed to import scaled NV12 DMA-BUF into RGA" << std::endl;
        return -EIO;
    }

    return 0;
}

int V4L2Camera::queueBuffer(uint32_t index)
{
    if (index >= capture_buffers_.size())
        return -EINVAL;

    struct v4l2_buffer buffer = {};
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    buffer.type = buffer_type_;
    buffer.memory = V4L2_MEMORY_DMABUF;
    buffer.index = index;

    if (is_multiplanar_)
    {
        buffer.m.planes = planes;
        buffer.length = plane_count_;
        planes[0].m.fd = capture_buffers_[index].fd;
        planes[0].length = static_cast<uint32_t>(capture_buffers_[index].size);
    }
    else
    {
        buffer.m.fd = capture_buffers_[index].fd;
        buffer.length = static_cast<uint32_t>(capture_buffers_[index].size);
    }

    if (xioctl(camera_fd_, VIDIOC_QBUF, &buffer) < 0)
    {
        logErrno("VIDIOC_QBUF(DMABUF)", device_path_);
        return -errno;
    }
    return 0;
}

bool V4L2Camera::captureFrame(Mbuffer &output, const std::atomic<bool> &stop_flag)
{
    if (!isOpen())
        return false;

    struct pollfd poll_fd = {};
    poll_fd.fd = camera_fd_;
    poll_fd.events = POLLIN | POLLPRI;

    int poll_ret;
    do
    {
        poll_ret = poll(&poll_fd, 1, 1000);
    } while (poll_ret < 0 && errno == EINTR && !stop_flag.load());

    if (stop_flag.load())
        return true;
    if (poll_ret == 0)
    {
        ++poll_timeout_count_;
        if (poll_timeout_count_ % 5 == 0)
        {
            std::cerr << "V4L2 camera produced no frame for "
                      << poll_timeout_count_ << " seconds: "
                      << device_path_ << std::endl;
        }
        return true;
    }
    if (poll_ret < 0)
    {
        logErrno("poll camera", device_path_);
        return false;
    }
    if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        std::cerr << "Camera poll error, revents=0x" << std::hex
                  << poll_fd.revents << std::dec << std::endl;
        return false;
    }

    struct v4l2_buffer buffer = {};
    struct v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    buffer.type = buffer_type_;
    buffer.memory = V4L2_MEMORY_DMABUF;
    if (is_multiplanar_)
    {
        buffer.m.planes = planes;
        buffer.length = plane_count_;
    }

    if (xioctl(camera_fd_, VIDIOC_DQBUF, &buffer) < 0)
    {
        if (errno == EAGAIN)
            return true;
        logErrno("VIDIOC_DQBUF(DMABUF)", device_path_);
        return false;
    }
    poll_timeout_count_ = 0;

    if (buffer.index >= capture_buffers_.size())
    {
        std::cerr << "V4L2 returned invalid buffer index " << buffer.index << std::endl;
        return false;
    }

    const bool processed = processBuffer(buffer.index, output);
    const int queue_ret = queueBuffer(buffer.index);
    if (processed)
    {
        ++captured_frame_count_;
        if (captured_frame_count_ == 1 || captured_frame_count_ % 100 == 0)
        {
            std::cout << "V4L2 camera frame " << captured_frame_count_
                      << " processed by RGA, output="
                      << kOutputWidth << "x" << kOutputHeight << std::endl;
        }
    }
    if (queue_ret != 0)
        return false;

    // RGA 瞬时繁忙只丢弃当前帧，不重启整个 V4L2 设备。
    return true;
}

bool V4L2Camera::processBuffer(uint32_t index, Mbuffer &output)
{
    const int yuv_format = use_nv21_
                               ? RK_FORMAT_YCrCb_420_SP
                               : RK_FORMAT_YCbCr_420_SP;
    rga_buffer_t source = wrapbuffer_handle_t(
        capture_buffers_[index].rga_handle,
        source_width_, source_height_,
        source_stride_, source_height_,
        yuv_format);
    rga_buffer_t scaled = wrapbuffer_handle_t(
        scaled_nv12_.rga_handle,
        kOutputWidth, kOutputHeight,
        kOutputWidth, kOutputHeight,
        yuv_format);

    IM_STATUS status = imresize(source, scaled);
    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA camera YUV resize failed: "
                  << imStrError_t(status) << std::endl;
        return false;
    }

    if (!output.dma_pool)
    {
        std::lock_guard<std::mutex> lock(output.mtx);
        if (!output.dma_pool)
            output.dma_pool.reset(new DmaImagePool());
    }

    const size_t bgr_size =
        static_cast<size_t>(kOutputWidth) * kOutputHeight * 3;
    if (!output.dma_pool->configure(
            4, kOutputWidth, kOutputHeight,
            kOutputWidth, kOutputHeight,
            RK_FORMAT_BGR_888, bgr_size))
        return false;

    auto output_frame = output.dma_pool->acquire();
    if (!output_frame)
    {
        static uint64_t dropped_frames = 0;
        ++dropped_frames;
        if (dropped_frames == 1 || dropped_frames % 100 == 0)
            std::cerr << "Camera output DMA pool is busy; dropped "
                      << dropped_frames << " frame(s)" << std::endl;
        return false;
    }

    rga_buffer_t bgr = wrapbuffer_handle_t(
        output_frame->rgaHandle(),
        kOutputWidth, kOutputHeight,
        output_frame->widthStride(), output_frame->heightStride(),
        RK_FORMAT_BGR_888);
    status = imcvtcolor(scaled, bgr,
                        yuv_format, RK_FORMAT_BGR_888,
                        full_range_ ? IM_YUV_TO_RGB_BT601_FULL
                                    : IM_YUV_TO_RGB_BT601_LIMIT);
    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA camera YUV to BGR failed: "
                  << imStrError_t(status) << std::endl;
        return false;
    }

    if (auto_white_balance_)
    {
        if (!output_frame->syncForCpu())
            return false;
        cv::Mat output_view = output_frame->bgrView();
        applyAutoWhiteBalance(output_view);
    }
    // 生产者在发布前结束 CPU 访问，保证后续 RGA/RKNN 直接读取到最新数据。
    if (!output_frame->syncForDevice())
        return false;

    {
        std::lock_guard<std::mutex> lock(output.mtx);
        output.dma_frame = output_frame;
        ++output.sequence;
    }
    return true;
}

void V4L2Camera::applyAutoWhiteBalance(cv::Mat &image)
{
    cv::Mat sample;
    cv::resize(image, sample, cv::Size(160, 120), 0.0, 0.0,
               cv::INTER_AREA);
    const cv::Scalar means = cv::mean(sample);
    const double gray = (means[0] + means[1] + means[2]) / 3.0;
    if (gray < 1.0)
        return;

    cv::Vec3f target_gains;
    for (int channel = 0; channel < 3; ++channel)
    {
        const float gain = static_cast<float>(
            gray / std::max(means[channel], 1.0));
        target_gains[channel] = std::max(0.70f, std::min(1.40f, gain));
    }

    const float smoothing = captured_frame_count_ < 10 ? 0.25f : 0.05f;
    white_balance_gains_ =
        white_balance_gains_ * (1.0f - smoothing) +
        target_gains * smoothing;

    if (captured_frame_count_ == 0 ||
        (captured_frame_count_ + 1) % 100 == 0)
    {
        std::cout << "Camera auto-WB gains B/G/R="
                  << white_balance_gains_[0] << "/"
                  << white_balance_gains_[1] << "/"
                  << white_balance_gains_[2] << std::endl;
    }

    const cv::Matx33f correction(
        white_balance_gains_[0], 0.0f, 0.0f,
        0.0f, white_balance_gains_[1], 0.0f,
        0.0f, 0.0f, white_balance_gains_[2]);
    cv::transform(image, image, correction);
}

void V4L2Camera::releaseBuffer(DmaBuffer &buffer)
{
    if (buffer.rga_handle > 0)
    {
        releasebuffer_handle(buffer.rga_handle);
        buffer.rga_handle = 0;
    }
    if (buffer.fd >= 0)
        dma_buf_free(buffer.size, &buffer.fd, buffer.va);
    buffer.va = nullptr;
    buffer.size = 0;
}

void V4L2Camera::close()
{
    if (camera_fd_ >= 0 && streaming_)
    {
        enum v4l2_buf_type type = static_cast<enum v4l2_buf_type>(buffer_type_);
        if (xioctl(camera_fd_, VIDIOC_STREAMOFF, &type) < 0)
            logErrno("VIDIOC_STREAMOFF", device_path_);
        streaming_ = false;
    }

    if (camera_fd_ >= 0 && buffer_type_ != 0)
    {
        struct v4l2_requestbuffers request = {};
        request.count = 0;
        request.type = buffer_type_;
        request.memory = V4L2_MEMORY_DMABUF;
        xioctl(camera_fd_, VIDIOC_REQBUFS, &request);
    }

    for (DmaBuffer &buffer : capture_buffers_)
        releaseBuffer(buffer);
    capture_buffers_.clear();
    releaseBuffer(scaled_nv12_);

    if (camera_fd_ >= 0)
    {
        ::close(camera_fd_);
        camera_fd_ = -1;
    }

    buffer_type_ = 0;
    plane_count_ = 1;
    is_multiplanar_ = false;
    source_width_ = 0;
    source_height_ = 0;
    source_stride_ = 0;
    source_size_ = 0;
    captured_frame_count_ = 0;
    poll_timeout_count_ = 0;
}
