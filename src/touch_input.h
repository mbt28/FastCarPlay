#ifndef SRC_TOUCH_INPUT
#define SRC_TOUCH_INPUT

// Touch input for the DRM/headless paths: reads the touchscreen (e.g. GT911)
// directly from its Linux evdev device and feeds touch into the protocol
// messages, or into the on-device UI while a screen is up. Gated on __linux__
// (evdev is Linux-only) and independent of the decoder.

#ifdef __linux__

#include <atomic>
#include <functional>
#include <thread>

#include "struct/multitouch.h"

class IConnection;

class TouchInput
{
public:
    explicit TouchInput(IConnection &conn);
    ~TouchInput();

    // On the DRM path the on-device UI is fed from
    // here: the same calibrated evdev reader, routed to the UI instead of the
    // phone while a screen is up. The loop toggles routeToUi() each frame; the
    // sink receives the primary finger as normalized 0..1 coordinates.
    // Set the sink once before enabling routing (see the release/acquire on
    // _uiActive, which publishes the sink to the touch thread).
    void setUiSink(std::function<void(float, float, bool)> sink) { _uiSink = std::move(sink); }
    void routeToUi(bool active) { _uiActive.store(active, std::memory_order_release); }

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
    std::atomic<bool> _uiActive{false};
    std::function<void(float, float, bool)> _uiSink;
    std::thread _thread;
    int _fd = -1;
    int _xmin = 0, _xmax = 1, _ymin = 0, _ymax = 1;
    bool _mt = false; // multitouch (protocol B, ABS_MT_*) vs single-touch (ABS_X/Y)
    Contact _slots[MUTLITOUCH_MAX_TOUCH];

    // Route-transition state (touch thread only). A contact belongs to the
    // route where it started: flipping routeToUi() mid-contact sends the old
    // side a terminal release/up and swallows the rest of the contact, so the
    // new side never sees a press it has no context for.
    bool _lastToUi = false;
    bool _suppressContact = false;
    bool _uiDown = false;
    float _uiLastX = 0, _uiLastY = 0;
};

#endif /* __linux__ */
#endif /* SRC_TOUCH_INPUT */
