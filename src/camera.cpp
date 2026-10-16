#include "camera.h"

#if defined(USE_CEDRUS) && defined(__linux__)

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <drm_fourcc.h>

#include "common/logger.h"
#include "common/threading.h"
#include "drm_display.h"
#include "settings.h"

namespace
{
// Capture buffers: one on screen, the driver needs two queued (the TV decoder
// arms the next address a frame ahead), one to spare. Three still work.
constexpr unsigned BUFFERS = 4;
constexpr unsigned MIN_BUFFERS = 3;

// A display that keeps refusing frames is not coming back.
constexpr int MAX_REFUSED = 10;

int xioctl(int fd, unsigned long request, void *arg)
{
    int r;
    do
        r = ioctl(fd, request, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

bool isCapture(int fd)
{
    v4l2_capability cap{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
        return false;
    const uint32_t caps =
        (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) &&
           !(caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE));
}

// The configured device, or else the first capture device (decoders are
// memory-to-memory and skipped). -1 when there is none.
int openDevice(std::string *error)
{
    const std::string &dev = Settings::cameraDevice.value;
    if (!dev.empty())
    {
        const int fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
        {
            if (error)
                *error = errno == ENOENT ? "No camera at " + dev : "Cannot open " + dev;
            return -1;
        }
        if (isCapture(fd))
            return fd;
        ::close(fd);
        if (error)
            *error = dev + " is not a camera";
        return -1;
    }
    for (int i = 0; i < 16; i++)
    {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        const int fd = ::open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        if (isCapture(fd))
            return fd;
        ::close(fd);
    }
    if (error)
        *error = "No camera found";
    return -1;
}

// A control as 0-100 % of its range; -1 when the device has none.
int levelOf(int fd, uint32_t id)
{
    v4l2_queryctrl q{};
    q.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED) ||
        q.maximum <= q.minimum)
        return -1;
    v4l2_control c{};
    c.id = id;
    if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0)
        return -1;
    return (int)(((int64_t)(c.value - q.minimum) * 100 + (q.maximum - q.minimum) / 2) /
                 (q.maximum - q.minimum));
}

void setLevel(int fd, uint32_t id, int level, const char *name)
{
    if (level < 0)
        return; // the device's own value
    v4l2_queryctrl q{};
    q.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0 || q.maximum <= q.minimum)
        return;
    v4l2_control c{};
    c.id = id;
    c.value = q.minimum + (int)(((int64_t)(q.maximum - q.minimum) * (level > 100 ? 100 : level) + 50) / 100);
    if (xioctl(fd, VIDIOC_S_CTRL, &c) < 0)
        log_w("camera: %s = %d: %s", name, c.value, strerror(errno));
}

// The video standard: the setting, or what the input carries. A detection
// can match several (625 lines: PAL and PAL-Nc); plain PAL / NTSC win.
void setStandard(int fd)
{
    const std::string &want = Settings::cameraStandard.value;
    v4l2_std_id std = 0;
    if (want == "pal")
        std = V4L2_STD_PAL;
    else if (want == "ntsc")
        std = V4L2_STD_NTSC_M;
    else
    {
        // The decoder only locks a few hundred ms after it is powered up.
        for (int i = 0; i < 8 && std == 0; i++)
        {
            v4l2_std_id seen = 0;
            if (xioctl(fd, VIDIOC_QUERYSTD, &seen) == 0 && seen != V4L2_STD_UNKNOWN)
                std = seen;
            else
                usleep(100 * 1000);
        }
        if (std == 0)
            return; // no signal yet: keep the current one
        if (std & V4L2_STD_NTSC_M)
            std = V4L2_STD_NTSC_M;
        else if (std & V4L2_STD_PAL)
            std = V4L2_STD_PAL;
    }
    if (xioctl(fd, VIDIOC_S_STD, &std) < 0)
        log_w("camera: set standard: %s", strerror(errno));
}

// A boolean control by name (driver-private ones have no fixed id), or 0.
uint32_t findControl(int fd, const char *name)
{
    v4l2_queryctrl q{};
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0)
    {
        if (strcmp((const char *)q.name, name) == 0)
            return q.id;
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    return 0;
}
} // namespace

Camera::Info Camera::probe()
{
    Info info;
    const int fd = openDevice(nullptr);
    if (fd < 0)
        return info;
    info.present = true;
    for (uint32_t i = 0; i < 8; i++)
    {
        v4l2_input in{};
        in.index = i;
        if (xioctl(fd, VIDIOC_ENUMINPUT, &in) < 0)
            break;
        info.inputs.emplace_back((const char *)in.name);
    }
    info.brightness = levelOf(fd, V4L2_CID_BRIGHTNESS);
    info.contrast = levelOf(fd, V4L2_CID_CONTRAST);
    info.saturation = levelOf(fd, V4L2_CID_SATURATION);
    ::close(fd);
    return info;
}

bool Camera::queue(int index)
{
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)index;
    return xioctl(_fd, VIDIOC_QBUF, &b) == 0;
}

bool Camera::start()
{
    if (_fd >= 0)
        return true;
    _error.clear();
    _failed = false;
    _fd = openDevice(&_error);
    if (_fd < 0)
        return false;

    auto give_up = [this](const std::string &why) {
        _error = why;
        log_w("camera: %s", why.c_str());
        stop();
        return false;
    };

    // Input, then standard, then the levels (a standard change resets some,
    // e.g. saturation on the TV decoder), then the format the standard gives.
    int input = Settings::cameraInput;
    if (xioctl(_fd, VIDIOC_S_INPUT, &input) < 0 && errno != ENOTTY)
        log_w("camera: input %d: %s", input, strerror(errno));
    v4l2_input in{};
    in.index = (uint32_t)input;
    if (xioctl(_fd, VIDIOC_ENUMINPUT, &in) == 0 && (in.capabilities & V4L2_IN_CAP_STD))
        setStandard(_fd);
    setLevel(_fd, V4L2_CID_BRIGHTNESS, Settings::cameraBrightness, "brightness");
    setLevel(_fd, V4L2_CID_CONTRAST, Settings::cameraContrast, "contrast");
    setLevel(_fd, V4L2_CID_SATURATION, Settings::cameraSaturation, "saturation");

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(_fd, VIDIOC_G_FMT, &fmt) < 0)
        return give_up("Camera format unknown");
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(_fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12)
        return give_up("Camera has no NV12");
    _width = (int)fmt.fmt.pix.width;
    _height = (int)fmt.fmt.pix.height;
    _pitch = (int)fmt.fmt.pix.bytesperline;

    v4l2_requestbuffers req{};
    req.count = BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < MIN_BUFFERS)
        return give_up("Not enough video memory for the camera");
    for (uint32_t i = 0; i < req.count; i++)
    {
        v4l2_exportbuffer eb{};
        eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        eb.index = i;
        eb.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(_fd, VIDIOC_EXPBUF, &eb) < 0)
            return give_up("Camera buffers cannot be shared");
        _dmabufs.push_back(eb.fd);
        if (!queue((int)i))
            return give_up("Camera refused a buffer");
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(_fd, VIDIOC_STREAMON, &type) < 0)
    {
        // The TV decoder's 3D comb filter (off by default) takes 2 MiB more
        // while streaming, from the memory the phone's video decoder also
        // uses: without it the picture is a little softer, but there.
        const int err = errno;
        const uint32_t comb = err == ENOMEM ? findControl(_fd, "Comb Filter 3D") : 0;
        v4l2_control off{};
        off.id = comb;
        if (comb == 0 || xioctl(_fd, VIDIOC_S_CTRL, &off) < 0 ||
            xioctl(_fd, VIDIOC_STREAMON, &type) < 0)
            return give_up(err == ENOMEM ? "Not enough video memory for the camera"
                                         : std::string("Camera does not start: ") + strerror(err));
        log_i("camera: comb filter off -- not enough memory for it");
    }

    drm_display::holdVideo(true);
    _streaming = true;
    log_i("camera: %dx%d NV12, %u buffers, input %d", _width, _height, req.count, input);
    _run = true;
    _thread = std::thread(&Camera::loop, this);
    return true;
}

void Camera::fail(const std::string &why)
{
    _error = why;
    log_w("camera: %s", why.c_str());
    _failed = true;
}

void Camera::loop()
{
    setThreadName("camera");
    int shown = -1; // on scanout until the next one replaces it
    int refused = 0;
    while (_run.load())
    {
        pollfd p{_fd, POLLIN, 0};
        const int r = poll(&p, 1, 200);
        if (r < 0 && errno != EINTR)
        {
            fail("Camera stopped");
            break;
        }
        if (r <= 0)
            continue;
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        if (xioctl(_fd, VIDIOC_DQBUF, &b) < 0)
        {
            if (errno == EAGAIN)
                continue;
            fail("Camera stopped");
            break;
        }
        const int fd = _dmabufs[b.index];
        const int fds[2] = {fd, fd};
        const uint32_t pitches[2] = {(uint32_t)_pitch, (uint32_t)_pitch};
        const uint32_t offsets[2] = {0, (uint32_t)(_pitch * _height)};
        const drm_display::Show s = drm_display::showCamera(
            DRM_FORMAT_NV12, _width, _height, 2, fds, pitches, offsets, DRM_FORMAT_MOD_LINEAR);
        if (s == drm_display::Show::Shown)
        {
            if (shown >= 0)
                queue(shown);
            shown = (int)b.index;
            refused = 0;
            continue;
        }
        // A flip still pending: this frame is skipped.
        queue((int)b.index);
        if (s == drm_display::Show::Failed && ++refused >= MAX_REFUSED)
        {
            fail("The display cannot show this camera");
            break;
        }
    }
}

void Camera::stop()
{
    if (_fd < 0)
        return;
    _run = false;
    if (_thread.joinable())
        _thread.join();
    if (_streaming)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(_fd, VIDIOC_STREAMOFF, &type);
        // A buffer may still be on scanout: the display lets go of them first.
        drm_display::flushVideo();
        drm_display::holdVideo(false);
        _streaming = false;
    }
    for (int fd : _dmabufs)
        ::close(fd);
    _dmabufs.clear();
    v4l2_requestbuffers req{};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(_fd, VIDIOC_REQBUFS, &req);
    ::close(_fd);
    _fd = -1;
}

#endif
