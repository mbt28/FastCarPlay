#include "touch_input.h"

#ifdef USE_CEDAR

#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cstdio>
#include <string>

#include "protocol/connection.h"
#include "protocol/protocol_const.h"
#include "protocol/message.h"
#include "settings.h"
#include "common/logger.h"

namespace {
constexpr int LONG_BITS = 8 * sizeof(long);

// Does this evdev device report the given EV_ABS axis?
bool hasAbsAxis(int fd, int code)
{
    unsigned long bits[(ABS_CNT + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits) < 0)
        return false;
    return (bits[code / LONG_BITS] >> (code % LONG_BITS)) & 1UL;
}
} // namespace

TouchInput::TouchInput(Connection &conn)
    : _conn(conn), _active(false)
{
    if (!openDevice())
    {
        fprintf(stderr, "[Touch] no touchscreen found (set 'touch-device' to override)\n");
        return;
    }
    _active = true;
    _thread = std::thread(&TouchInput::loop, this);
}

TouchInput::~TouchInput()
{
    _active = false;
    if (_thread.joinable())
        _thread.join();
    if (_fd >= 0)
        close(_fd);
}

bool TouchInput::openDevice()
{
    std::string path = Settings::touchDevice.value;
    if (!path.empty())
    {
        _fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    }
    else
    {
        // Auto-detect: first evdev node that exposes an absolute-position axis.
        for (int i = 0; i < 32 && _fd < 0; i++)
        {
            char p[32];
            snprintf(p, sizeof(p), "/dev/input/event%d", i);
            int fd = open(p, O_RDONLY | O_NONBLOCK);
            if (fd < 0)
                continue;
            if (hasAbsAxis(fd, ABS_MT_POSITION_X) || hasAbsAxis(fd, ABS_X))
            {
                _fd = fd;
                path = p;
            }
            else
                close(fd);
        }
    }
    if (_fd < 0)
        return false;

    // Range for normalization -> [0,1] (the protocol multiplies by 10000).
    int ax = hasAbsAxis(_fd, ABS_MT_POSITION_X) ? ABS_MT_POSITION_X : ABS_X;
    int ay = hasAbsAxis(_fd, ABS_MT_POSITION_Y) ? ABS_MT_POSITION_Y : ABS_Y;
    struct input_absinfo ai;
    if (ioctl(_fd, EVIOCGABS(ax), &ai) == 0) { _xmin = ai.minimum; _xmax = ai.maximum; }
    if (ioctl(_fd, EVIOCGABS(ay), &ai) == 0) { _ymin = ai.minimum; _ymax = ai.maximum; }
    if (_xmax <= _xmin) _xmax = _xmin + 1;
    if (_ymax <= _ymin) _ymax = _ymin + 1;

    char name[128] = "?";
    ioctl(_fd, EVIOCGNAME(sizeof(name)), name);
    fprintf(stderr, "[Touch] %s '%s'  X[%d..%d] Y[%d..%d]\n",
            path.c_str(), name, _xmin, _xmax, _ymin, _ymax);
    return true;
}

void TouchInput::loop()
{
    int slot = 0;                 // active MT slot (track finger 0 only)
    int rawx = 0, rawy = 0;
    int sentx = -1, senty = -1;   // last coordinate sent (avoid redundant moves)
    bool down = false, wasDown = false;
    bool havePos = false;

    while (_active)
    {
        struct pollfd pfd{_fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) // 200ms so the dtor can stop us promptly
            continue;

        struct input_event ev[64];
        ssize_t n = read(_fd, ev, sizeof(ev));
        if (n < (ssize_t)sizeof(ev[0]))
            continue;

        for (size_t i = 0; i < n / sizeof(ev[0]); i++)
        {
            const struct input_event &e = ev[i];
            if (e.type == EV_ABS)
            {
                switch (e.code)
                {
                case ABS_MT_SLOT:       slot = e.value; break;
                case ABS_MT_POSITION_X: if (slot == 0) { rawx = e.value; havePos = true; } break;
                case ABS_MT_POSITION_Y: if (slot == 0) { rawy = e.value; havePos = true; } break;
                case ABS_X:             rawx = e.value; havePos = true; break;
                case ABS_Y:             rawy = e.value; havePos = true; break;
                }
            }
            else if (e.type == EV_KEY && e.code == BTN_TOUCH)
            {
                down = (e.value != 0);
            }
            else if (e.type == EV_SYN && e.code == SYN_REPORT)
            {
                if (!havePos || _conn.state() != PROTOCOL_STATUS_CONNECTED)
                {
                    wasDown = down;
                    continue;
                }
                float nx = (float)(rawx - _xmin) / (_xmax - _xmin);
                float ny = (float)(rawy - _ymin) / (_ymax - _ymin);
                if (Settings::touchSwapXY) { float t = nx; nx = ny; ny = t; }
                if (Settings::touchInvertX) nx = 1.0f - nx;
                if (Settings::touchInvertY) ny = 1.0f - ny;
                if (nx < 0) nx = 0; else if (nx > 1) nx = 1;
                if (ny < 0) ny = 0; else if (ny > 1) ny = 1;

                if (down && !wasDown)
                    _conn.send(Message::Click(nx, ny, true));
                else if (down && wasDown && (rawx != sentx || rawy != senty))
                    _conn.send(Message::Move(nx, ny));
                else if (!down && wasDown)
                    _conn.send(Message::Click(nx, ny, false));

                sentx = rawx; senty = rawy;
                wasDown = down;
            }
        }
    }
}

#endif /* USE_CEDAR */
