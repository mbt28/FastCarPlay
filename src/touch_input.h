#ifndef SRC_TOUCH_INPUT
#define SRC_TOUCH_INPUT

// Touch input for the DRM/headless render paths (renderer = drm/none). Those
// paths have no SDL video and therefore no SDL_FINGER events, so we read the
// touchscreen (e.g. GT911) directly from its Linux evdev device and feed touch
// into the same protocol the SDL path uses. Gated on __linux__ (evdev is
// Linux-only), independent of the decoder; the app only instantiates it on the
// non-SDL loops, so which renderer is selected decides when it runs.

#ifdef __linux__

#include <atomic>
#include <thread>

#include "struct/multitouch.h"

class IConnection;

class TouchInput
{
public:
    explicit TouchInput(IConnection &conn);
    ~TouchInput();

private:
    bool openDevice();
    void loop();
    void emit(); // build a Multitouch from the current slots and send it

    // One tracked finger. `active` = currently touching, `wasActive` = touching
    // at the previous frame -> the transition gives the down/move/up phase.
    struct Contact
    {
        int id = -1; // evdev MT tracking id (-1 = free slot)
        int x = 0, y = 0;
        bool active = false;
        bool wasActive = false;
    };

    IConnection &_conn;
    std::atomic<bool> _active;
    std::thread _thread;
    int _fd = -1;
    int _xmin = 0, _xmax = 1, _ymin = 0, _ymax = 1;
    bool _mt = false; // multitouch (protocol B, ABS_MT_*) vs single-touch (ABS_X/Y)
    Contact _slots[MUTLITOUCH_MAX_TOUCH];
};

#endif /* __linux__ */
#endif /* SRC_TOUCH_INPUT */
