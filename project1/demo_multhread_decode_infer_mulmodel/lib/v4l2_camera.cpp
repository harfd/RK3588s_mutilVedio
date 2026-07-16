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

int V4L2Camera::open(const std::string &device_path)
{
    close();
    device_path_ = device_path;

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
        format.fmt.pix_mp.width = kRequestedWidth;
        format.fmt.pix_mp.height = kRequestedHeight;
        format.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix_mp.field = V4L2_FIELD_ANY;
    }
    else
    {
        format.fmt.pix.width = kRequestedWidth;
        format.fmt.pix.height = kRequestedHeight;
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        format.fmt.pix.field = V4L2_FIELD_ANY;
    }

    if (xioctl(camera_fd_, VIDIOC_S_FMT, &format) < 0)
    {
        logErrno("VIDIOC_S_FMT(NV12)", device_path_);
        return -errno;
    }

    uint32_t pixel_format;
    if (is_multiplanar_)
    {
        pixel_format = format.fmt.pix_mp.pixelformat;
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
    }

    scaled_nv12_.size = static_cast<size_t>(kOutputWidth) * kOutputHeight * 3 / 2;
    int ret = dma_buf_alloc(DMA_HEAP_PATH, scaled_nv12_.size,
                            &scaled_nv12_.fd, &scaled_nv12_.va);
    if (ret < 0)
        return ret;

    output_bgr_.size = static_cast<size_t>(kOutputWidth) * kOutputHeight * 3;
    ret = dma_buf_alloc(DMA_HEAP_PATH, output_bgr_.size,
                        &output_bgr_.fd, &output_bgr_.va);
    if (ret < 0)
        return ret;

    output_bgr_view_ = cv::Mat(kOutputHeight, kOutputWidth, CV_8UC3, output_bgr_.va);
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
        return true;
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

    if (buffer.index >= capture_buffers_.size())
    {
        std::cerr << "V4L2 returned invalid buffer index " << buffer.index << std::endl;
        return false;
    }

    const bool processed = processBuffer(buffer.index, output);
    const int queue_ret = queueBuffer(buffer.index);
    return processed && queue_ret == 0;
}

bool V4L2Camera::processBuffer(uint32_t index, Mbuffer &output)
{
    rga_buffer_t source = wrapbuffer_fd_t(
        capture_buffers_[index].fd,
        source_width_, source_height_,
        source_stride_, source_height_,
        RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t scaled = wrapbuffer_fd_t(
        scaled_nv12_.fd,
        kOutputWidth, kOutputHeight,
        kOutputWidth, kOutputHeight,
        RK_FORMAT_YCbCr_420_SP);

    IM_STATUS status = imresize(source, scaled);
    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA NV12 resize failed: " << imStrError(status) << std::endl;
        return false;
    }

    std::lock_guard<std::mutex> lock(output.mtx);
    if (output_cpu_access_active_)
    {
        if (dma_sync_cpu_to_device(output_bgr_.fd) < 0)
            logErrno("DMA_BUF_SYNC_END", device_path_);
        output_cpu_access_active_ = false;
    }

    rga_buffer_t bgr = wrapbuffer_fd_t(
        output_bgr_.fd,
        kOutputWidth, kOutputHeight,
        kOutputWidth, kOutputHeight,
        RK_FORMAT_BGR_888);
    status = imcvtcolor(scaled, bgr,
                        RK_FORMAT_YCbCr_420_SP, RK_FORMAT_BGR_888,
                        IM_COLOR_SPACE_DEFAULT);
    if (status != IM_STATUS_SUCCESS)
    {
        std::cerr << "RGA NV12 to BGR failed: " << imStrError(status) << std::endl;
        return false;
    }

    if (dma_sync_device_to_cpu(output_bgr_.fd) < 0)
    {
        logErrno("DMA_BUF_SYNC_START", device_path_);
        return false;
    }
    output_cpu_access_active_ = true;
    output.img = output_bgr_view_;
    return true;
}

void V4L2Camera::releaseBuffer(DmaBuffer &buffer)
{
    if (buffer.fd >= 0)
        dma_buf_free(buffer.size, &buffer.fd, buffer.va);
    buffer.va = nullptr;
    buffer.size = 0;
}

void V4L2Camera::close()
{
    if (output_cpu_access_active_ && output_bgr_.fd >= 0)
    {
        dma_sync_cpu_to_device(output_bgr_.fd);
        output_cpu_access_active_ = false;
    }

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
    releaseBuffer(output_bgr_);
    output_bgr_view_.release();

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
}
