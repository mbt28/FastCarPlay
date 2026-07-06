#include "touch_input.h"

#ifdef __linux__

#include <linux/input.h>
// linux/input.h defines BTN_LEFT/RIGHT/BACK as evdev codes that collide with
// the Carlinkit button ids in protocol_const.h. We only use BTN_TOUCH here, so
// drop the evdev aliases before the protocol headers define their versions.
#undef BTN_LEFT
#undef BTN_RIGHT
#undef BTN_BACK
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cstdio>
#include <string>

#include "protocol/iconnection.h"
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

TouchInput::TouchInput(IConnection &conn)
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

    // Multitouch (protocol B) if the device reports per-contact MT axes,
    // otherwise fall back to single-touch ABS_X/ABS_Y + BTN_TOUCH.
    _mt = hasAbsAxis(_fd, ABS_MT_POSITION_X);

    // Range for normalization -> [0,1] (the protocol multiplies by 10000).
    int ax = _mt ? ABS_MT_POSITION_X : ABS_X;
    int ay = _mt ? ABS_MT_POSITION_Y : ABS_Y;
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

void TouchInput::emit()
{
    // Before the projection is up, keep slot history in sync so the first real
    // frame doesn't replay a stale "down" for a finger already on the glass.
    bool connected = _conn.state() == PROTOCOL_STATUS_CONNECTED;

    Multitouch touches;
    for (int i = 0; i < MUTLITOUCH_MAX_TOUCH; i++)
    {
        Contact &c = _slots[i];
        if (!c.active && !c.wasActive)
            continue; // idle slot

        // active & !wasActive = just pressed; active & wasActive = held/moved;
        // !active & wasActive = just released.
        int action = c.active ? (c.wasActive ? MT_ACTION_MOVE : MT_ACTION_DOWN) : MT_ACTION_UP;

        float nx = (float)(c.x - _xmin) / (_xmax - _xmin);
        float ny = (float)(c.y - _ymin) / (_ymax - _ymin);
        if (Settings::touchSwapXY) { float t = nx; nx = ny; ny = t; }
        if (Settings::touchInvertX) nx = 1.0f - nx;
        if (Settings::touchInvertY) ny = 1.0f - ny;
        if (nx < 0) nx = 0; else if (nx > 1) nx = 1;
        if (ny < 0) ny = 0; else if (ny > 1) ny = 1;

        // Pointer id = the (stable) slot index, not the evdev tracking id which
        // changes on every touch-down.
        touches.add(nx, ny, action, i);

        c.wasActive = c.active;
        if (!c.active)
            c.id = -1; // slot freed
    }

    if (connected && touches.size() > 0)
        _conn.send(Message::MultiTouch(touches));
}

void TouchInput::loop()
{
    int cur = 0;        // current MT slot (ABS_MT_SLOT)
    bool dirty = false; // something changed since the last SYN_REPORT

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
                if (_mt)
                {
                    switch (e.code)
                    {
                    case ABS_MT_SLOT:
                        cur = e.value;
                        break;
                    case ABS_MT_TRACKING_ID:
                        if (cur >= 0 && cur < MUTLITOUCH_MAX_TOUCH)
                        {
                            if (e.value < 0)
                                _slots[cur].active = false; // finger lifted
                            else
                            {
                                _slots[cur].id = e.value;
                                _slots[cur].active = true; // finger down
                            }
                            dirty = true;
                        }
                        break;
                    case ABS_MT_POSITION_X:
                        if (cur >= 0 && cur < MUTLITOUCH_MAX_TOUCH) { _slots[cur].x = e.value; dirty = true; }
                        break;
                    case ABS_MT_POSITION_Y:
                        if (cur >= 0 && cur < MUTLITOUCH_MAX_TOUCH) { _slots[cur].y = e.value; dirty = true; }
                        break;
                    }
                }
                else // single-touch device: everything is finger 0
                {
                    if (e.code == ABS_X) { _slots[0].x = e.value; dirty = true; }
                    else if (e.code == ABS_Y) { _slots[0].y = e.value; dirty = true; }
                }
            }
            else if (e.type == EV_KEY && e.code == BTN_TOUCH)
            {
                if (!_mt) // single-touch press/release
                {
                    _slots[0].active = (e.value != 0);
                    _slots[0].id = 0;
                    dirty = true;
                }
            }
            else if (e.type == EV_SYN && e.code == SYN_REPORT)
            {
                if (dirty)
                    emit();
                dirty = false;
            }
        }
    }
}

#endif /* __linux__ */
