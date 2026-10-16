#include "camera.h"

#if defined(USE_CEDRUS) && defined(__linux__)

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include "common/clock.h"
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

// How often the signal (and, on Auto, the standard) is looked at.
constexpr uint32_t POLL_MS = 1000;
// A standard or input change waits this long for the next press, so cycling
// through them restarts the stream once.
constexpr uint32_t RESTART_DELAY_MS = 300;
// Changes are saved this long after the last one (NAND writes).
constexpr uint32_t SAVE_DELAY_MS = 2000;
// The screens stay up at most this long for the first frame (the decoder
// locking onto the signal), then give way to whatever the plane has.
constexpr uint32_t FIRST_FRAME_MS = 1000;

// The per-input profiles: camera-0 .. camera-3.
Setting<std::string> *const PROFILES[4] = {&Settings::cameraProfile0, &Settings::cameraProfile1,
                                           &Settings::cameraProfile2, &Settings::cameraProfile3};

const char *const STANDARDS[] = {"auto", "pal", "ntsc"};
const char *const STANDARD_LABELS[] = {"Auto", "PAL", "NTSC"};

int standardIndex(const std::string &s)
{
    for (int i = 0; i < 3; i++)
        if (s == STANDARDS[i])
            return i;
    return 0;
}

// The standard a profile asks for; 0 = Auto (whatever the input carries).
v4l2_std_id wanted(const std::string &s)
{
    return s == "pal" ? V4L2_STD_PAL : s == "ntsc" ? V4L2_STD_NTSC_M : 0;
}

// A detection can match several (625 lines: PAL and PAL-Nc); plain PAL /
// NTSC win.
v4l2_std_id plain(v4l2_std_id std)
{
    if (std & V4L2_STD_NTSC_M)
        return V4L2_STD_NTSC_M;
    if (std & V4L2_STD_PAL)
        return V4L2_STD_PAL;
    return std;
}

bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

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

// A control by name (driver-private ones have no fixed id), or 0.
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

// What the column offers, in its order: standard controls by id, the TV
// decoder's private ones by name. Nothing else is ever touched -- some of
// the decoder's controls make noise or misbehave (burst gates, comb filter).
struct Wanted
{
    uint32_t id;
    const char *name; // the name to look for when the id is not there
    const char *key;
    const char *label;
};
const Wanted WANTED[] = {
    {V4L2_CID_BRIGHTNESS, nullptr, "brightness", "Brightness"},
    {V4L2_CID_CONTRAST, nullptr, "contrast", "Contrast"},
    {V4L2_CID_SATURATION, nullptr, "saturation", "Saturation"},
    {V4L2_CID_HUE, nullptr, "hue", "Hue"},
    {V4L2_CID_SHARPNESS, nullptr, "sharpness", "Sharpness"},
    {V4L2_CID_CHROMA_GAIN, "Chroma Gain", "chroma-gain", "Chroma gain"},
    {0, "Chroma Enhance", "chroma-enhance", "Chroma enh."},
};

// "standard=pal,brightness=40,..."; anything unknown or broken is skipped.
void parseProfile(const std::string &text, std::string &standard,
                  std::vector<std::pair<std::string, int>> &values)
{
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t end = text.find(',', pos);
        if (end == std::string::npos)
            end = text.size();
        const std::string item = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = item.substr(0, eq), value = item.substr(eq + 1);
        if (key == "standard")
        {
            standard = STANDARDS[standardIndex(value)];
            continue;
        }
        char *rest = nullptr;
        const long v = strtol(value.c_str(), &rest, 10);
        if (!value.empty() && rest && *rest == 0)
            values.emplace_back(key, (int)v);
    }
}
} // namespace

int Camera::Profile::get(const char *key, int fallback) const
{
    for (const auto &kv : values)
        if (kv.first == key)
            return kv.second;
    return fallback;
}

void Camera::Profile::set(const char *key, int value)
{
    for (auto &kv : values)
        if (kv.first == key)
        {
            kv.second = value;
            return;
        }
    values.emplace_back(key, value);
}

Camera &Camera::instance()
{
    static Camera camera;
    return camera;
}

bool Camera::present()
{
    const int fd = openDevice(nullptr);
    if (fd < 0)
        return false;
    ::close(fd);
    return true;
}

void Camera::scanControls()
{
    _controls.clear();
    for (const Wanted &w : WANTED)
    {
        v4l2_queryctrl q{};
        q.id = w.id;
        if ((w.id == 0 || xioctl(_fd, VIDIOC_QUERYCTRL, &q) < 0) && w.name)
        {
            q = {};
            q.id = findControl(_fd, w.name);
            if (q.id == 0 || xioctl(_fd, VIDIOC_QUERYCTRL, &q) < 0)
                continue;
        }
        else if (w.id == 0)
            continue;
        if ((q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) ||
            q.maximum <= q.minimum)
            continue;
        Control c{q.id, w.key, w.label, q.minimum, q.maximum, q.step > 0 ? q.step : 1,
                  q.default_value, q.default_value, q.type == V4L2_CTRL_TYPE_MENU, {}};
        if (c.menu)
            for (int i = q.minimum; i <= q.maximum; i++)
            {
                v4l2_querymenu m{};
                m.id = q.id;
                m.index = (uint32_t)i;
                if (xioctl(_fd, VIDIOC_QUERYMENU, &m) == 0)
                    c.items.emplace_back(i, (const char *)m.name);
            }
        if (c.menu && c.items.empty())
            continue;
        _controls.push_back(std::move(c));
    }
}

// Input, standard (std 0 = Auto: detect, blocking -- only on the first open,
// when the screen is black anyway), then the levels: a standard change
// resets some of them on the TV decoder. Buffers must be freed: S_STD is
// refused while any exist.
void Camera::configure(v4l2_std_id std)
{
    int input = _input;
    if (xioctl(_fd, VIDIOC_S_INPUT, &input) < 0 && errno != ENOTTY)
        log_w("camera: input %d: %s", input, strerror(errno));
    v4l2_input in{};
    in.index = (uint32_t)_input;
    _hasStd = xioctl(_fd, VIDIOC_ENUMINPUT, &in) == 0 && (in.capabilities & V4L2_IN_CAP_STD);
    if (_hasStd)
    {
        // The decoder only locks a few hundred ms after it is powered up.
        for (int i = 0; i < 8 && std == 0; i++)
        {
            v4l2_std_id seen = 0;
            if (xioctl(_fd, VIDIOC_QUERYSTD, &seen) == 0 && seen != V4L2_STD_UNKNOWN)
                std = plain(seen);
            else
                usleep(100 * 1000);
        }
        if (std != 0 && xioctl(_fd, VIDIOC_S_STD, &std) < 0)
            log_w("camera: set standard: %s", strerror(errno));
        xioctl(_fd, VIDIOC_G_STD, &_std);
    }
    applyProfile();
}

// Every control to the current input's value, or to the control's default
// (read again: it can follow the standard). Never the device's current
// value -- that would carry one input's tuning over to another.
void Camera::applyProfile()
{
    for (Control &c : _controls)
    {
        v4l2_queryctrl q{};
        q.id = c.id;
        if (xioctl(_fd, VIDIOC_QUERYCTRL, &q) == 0)
            c.def = q.default_value;
        v4l2_control ctrl{};
        ctrl.id = c.id;
        ctrl.value = std::clamp(profile().get(c.key, c.def), c.min, c.max);
        if (xioctl(_fd, VIDIOC_S_CTRL, &ctrl) < 0)
            log_w("camera: %s = %d: %s", c.key, ctrl.value, strerror(errno));
        ctrl.value = c.def;
        xioctl(_fd, VIDIOC_G_CTRL, &ctrl);
        c.value = ctrl.value;
    }
}

bool Camera::queue(int index)
{
    v4l2_buffer b{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)index;
    return xioctl(_fd, VIDIOC_QBUF, &b) == 0;
}

// The format the standard gives, buffers exported for the display, stream on,
// and the capture thread. False with _error set.
bool Camera::streamOn()
{
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(_fd, VIDIOC_G_FMT, &fmt) < 0)
    {
        _error = "Camera format unknown";
        return false;
    }
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    if (xioctl(_fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12)
    {
        _error = "Camera has no NV12";
        return false;
    }
    _width = (int)fmt.fmt.pix.width;
    _height = (int)fmt.fmt.pix.height;
    _pitch = (int)fmt.fmt.pix.bytesperline;

    v4l2_requestbuffers req{};
    req.count = BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(_fd, VIDIOC_REQBUFS, &req) < 0 || req.count < MIN_BUFFERS)
    {
        _error = "Not enough video memory for the camera";
        _noMemory = true;
        return false;
    }
    for (uint32_t i = 0; i < req.count; i++)
    {
        v4l2_exportbuffer eb{};
        eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        eb.index = i;
        eb.flags = O_RDWR | O_CLOEXEC;
        if (xioctl(_fd, VIDIOC_EXPBUF, &eb) < 0)
        {
            _error = "Camera buffers cannot be shared";
            return false;
        }
        _dmabufs.push_back(eb.fd);
        if (!queue((int)i))
        {
            _error = "Camera refused a buffer";
            return false;
        }
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
        {
            _error = err == ENOMEM ? "Not enough video memory for the camera"
                                   : std::string("Camera does not start: ") + strerror(err);
            _noMemory = err == ENOMEM;
            return false;
        }
        log_i("camera: comb filter off -- not enough memory for it");
    }

    if (!_held)
    {
        drm_display::holdVideo(true);
        _held = true;
    }
    _streaming = true;
    log_i("camera: %dx%d NV12, %u buffers, input %d", _width, _height, req.count, _input);
    _run = true;
    _thread = std::thread(&Camera::loop, this);
    return true;
}

// The stream and its buffers; with release, also the device and the plane.
// Without, the device stays open -- closing it powers the decoder down, and
// it would have to lock again.
void Camera::streamOff(bool release)
{
    _run = false;
    if (_thread.joinable())
        _thread.join();
    if (_streaming)
    {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(_fd, VIDIOC_STREAMOFF, &type);
        // A buffer may still be on scanout: the display lets go of them first.
        drm_display::flushVideo();
        _streaming = false;
    }
    for (int fd : _dmabufs)
        ::close(fd);
    _dmabufs.clear();
    v4l2_requestbuffers req{};
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    xioctl(_fd, VIDIOC_REQBUFS, &req);
    if (!release)
        return;
    if (_held)
    {
        drm_display::holdVideo(false);
        _held = false;
    }
    ::close(_fd);
    _fd = -1;
}

bool Camera::start()
{
    if (_fd >= 0)
        return true;
    _error.clear();
    _failed = false;
    _noMemory = false;
    _firstShown = false;
    _startMs = mono_ms();
    _fd = openDevice(&_error);
    if (_fd < 0)
        return false;

    _inputs.clear();
    for (uint32_t i = 0; i < 8; i++)
    {
        v4l2_input in{};
        in.index = i;
        if (xioctl(_fd, VIDIOC_ENUMINPUT, &in) < 0)
            break;
        _inputs.emplace_back((const char *)in.name);
    }
    if (_inputs.size() > 4)
        _inputs.resize(4); // one saved profile each
    for (int i = 0; i < 4; i++)
    {
        _profiles[i] = Profile();
        parseProfile(PROFILES[i]->value, _profiles[i].standard, _profiles[i].values);
    }
    _input = std::clamp((int)Settings::cameraInput, 0, std::max(0, (int)_inputs.size() - 1));
    _restartAt = 0;
    _noSignal = false;
    _unsaved = false;
    scanControls();
    configure(wanted(profile().standard));
    if (!streamOn())
    {
        const std::string why = _error;
        log_w("camera: %s", why.c_str());
        stop();
        _error = why;
        return false;
    }
    _pollAt = mono_ms() + POLL_MS;
    changed(false);
    return true;
}

bool Camera::pictureReady() const
{
    return _firstShown.load() || mono_ms() - _startMs >= FIRST_FRAME_MS;
}

void Camera::restart()
{
    _noMemory = false;
    streamOff(false);
    configure(_restartStd);
    if (!streamOn())
        fail(_error);
    changed(false);
}

void Camera::stop()
{
    if (_fd < 0)
        return;
    if (_unsaved)
        save();
    streamOff(true);
    _levels.clear();
    _generation++;
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
            _firstShown = true;
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

void Camera::tick()
{
    if (_fd < 0 || _failed)
        return;
    const uint32_t now = mono_ms();
    if (_restartAt != 0 && due(now, _restartAt))
    {
        _restartAt = 0;
        restart();
        _pollAt = now + POLL_MS;
    }
    if (due(now, _pollAt))
    {
        _pollAt = now + POLL_MS;
        v4l2_input in{};
        in.index = (uint32_t)_input;
        if (xioctl(_fd, VIDIOC_ENUMINPUT, &in) == 0)
        {
            const bool none = (in.status & V4L2_IN_ST_NO_SIGNAL) != 0;
            if (none != _noSignal)
            {
                _noSignal = none;
                changed(false);
            }
        }
        // Auto follows the input: a reversing camera often powers up only
        // in reverse, after the view is open.
        v4l2_std_id seen = 0;
        if (_hasStd && wanted(profile().standard) == 0 && !_noSignal && _restartAt == 0 &&
            xioctl(_fd, VIDIOC_QUERYSTD, &seen) == 0 && seen != V4L2_STD_UNKNOWN &&
            !(plain(seen) & _std))
        {
            _restartStd = plain(seen);
            _restartAt = now;
        }
    }
    if (_unsaved && due(now, _saveAt))
        save();
}

const char *Camera::standardLabel() const
{
    return STANDARD_LABELS[standardIndex(profile().standard)];
}

void Camera::selectInput(int input)
{
    if (_fd < 0 || input < 0 || input >= (int)_inputs.size() || input == _input)
        return;
    const uint32_t now = mono_ms();
    _input = input;
    int which = _input;
    xioctl(_fd, VIDIOC_S_INPUT, &which);
    v4l2_input in{};
    in.index = (uint32_t)_input;
    _hasStd = xioctl(_fd, VIDIOC_ENUMINPUT, &in) == 0 && (in.capabilities & V4L2_IN_CAP_STD);
    const v4l2_std_id want = wanted(profile().standard);
    if (_hasStd && want != 0 && want != _std)
    {
        _restartStd = want;
        _restartAt = now + RESTART_DELAY_MS;
    }
    else
    {
        applyProfile();
        _pollAt = now + 400; // Auto: look at the new input soon
    }
    _noSignal = false;
    changed(true);
}

void Camera::cycleStandard(int direction)
{
    if (_fd < 0 || !_hasStd || direction == 0)
        return;
    const uint32_t now = mono_ms();
    profile().standard =
        STANDARDS[(standardIndex(profile().standard) + (direction > 0 ? 1 : 2)) % 3];
    const v4l2_std_id want = wanted(profile().standard);
    if (want != 0 && want != _std)
    {
        _restartStd = want;
        _restartAt = now + RESTART_DELAY_MS;
    }
    else
    {
        _restartAt = 0;
        _pollAt = now; // Auto: check the input now
    }
    changed(true);
}

void Camera::adjustLevel(int level, int direction)
{
    if (_fd < 0 || level < 0 || level >= (int)_controls.size() || direction == 0)
        return;
    const int step = direction > 0 ? 1 : -1;
    Control &c = _controls[(size_t)level];
    int value = c.value;
    if (c.menu)
    {
        size_t i = 0;
        while (i < c.items.size() && c.items[i].first != c.value)
            i++;
        const int at = std::clamp((int)i + step, 0, (int)c.items.size() - 1);
        value = c.items[(size_t)at].first;
    }
    else
    {
        // 5 % of the range, in whole steps of the control.
        const int stride = std::max(c.step, ((c.max - c.min) / 20 / c.step) * c.step);
        value = std::clamp(c.value + step * stride, c.min, c.max);
    }
    v4l2_control ctrl{};
    ctrl.id = c.id;
    ctrl.value = value;
    if (xioctl(_fd, VIDIOC_S_CTRL, &ctrl) < 0)
        log_w("camera: %s = %d: %s", c.key, value, strerror(errno));
    ctrl.value = value;
    xioctl(_fd, VIDIOC_G_CTRL, &ctrl);
    c.value = ctrl.value;
    profile().set(c.key, c.value);
    changed(true);
}

void Camera::buildLevels()
{
    _levels.clear();
    for (const Control &c : _controls)
    {
        std::string value;
        if (c.menu)
        {
            for (const auto &item : c.items)
                if (item.first == c.value)
                    value = item.second;
        }
        else
        {
            char text[16];
            if (c.min < 0) // centred (hue): an offset either way
                snprintf(text, sizeof(text), "%+d%%",
                         (int)((int64_t)c.value * 100 / std::max(-c.min, c.max)));
            else
                snprintf(text, sizeof(text), "%d%%",
                         (int)(((int64_t)(c.value - c.min) * 100 + (c.max - c.min) / 2) /
                               (c.max - c.min)));
            value = text;
        }
        _levels.push_back({c.label, value});
    }
}

void Camera::changed(bool save)
{
    buildLevels();
    _generation++;
    if (save)
    {
        _unsaved = true;
        _saveAt = mono_ms() + SAVE_DELAY_MS;
    }
}

void Camera::save()
{
    _unsaved = false;
    for (int i = 0; i < 4; i++)
    {
        const Profile &p = _profiles[i];
        std::string text;
        if (p.standard != "auto" || !p.values.empty())
        {
            text = "standard=" + p.standard;
            for (const auto &kv : p.values)
                text += "," + kv.first + "=" + std::to_string(kv.second);
        }
        if (text != PROFILES[i]->value)
            Settings::setUser(PROFILES[i]->name, text);
    }
    if (_input != Settings::cameraInput)
        Settings::setUser("camera-input", std::to_string(_input));
}

#endif
