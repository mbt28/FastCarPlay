#ifndef SRC_UI_SLINT_OSD
#define SRC_UI_SLINT_OSD

// The Slint UI bound straight to drm_display's dumb framebuffer: the
// software renderer paints into a persistent full-size RGB565 buffer and the
// dirty rows are copied into the overlay plane (uiWriteRect). The caller owns
// presentation (drm_display::uiShow() when tick() reports a change).
//
// Slint is single-threaded: pointer() and key() may be called from the input
// threads (they only record state); everything else must stay on the loop
// thread.

#ifdef USE_SLINT

#include <atomic>
#include <cstdint>
#include <mutex>

#include "osd_key.h"

class SlintOsd
{
public:
    SlintOsd() = default;
    ~SlintOsd();

    // DRM backend: requires the overlay plane to be RGB565 (uiBpp() == 2) --
    // anything else fails loudly rather than rendering garbage. Size comes
    // from the caller (the DRM panel), never a compile-time constant.
    // One begin per process: Slint's platform cannot be torn down.
    bool beginDrm(int width, int height);
    void end();
    bool active() const { return _active; }

    // Feed pointer state (evdev touch). Coordinates are in display pixels.
    // Thread-safe; the next tick() dispatches on the loop thread.
    void pointer(int x, int y, bool pressed);

    // Feed a keyboard event: navigation key, the typed ASCII character (0 if
    // none), and the evdev value (1 press, 2 repeat, 0 release). Thread-safe;
    // queued in order and applied by the next tick().
    void key(OsdKey nav, char ch, int value);

    // The UI was just hidden (video took the screen): queued keys are stale,
    // and what only runs while a screen is visible (discovery) stops.
    void hidden();

    // Pumps app state into the UI, dispatches input, runs timers/animations
    // and renders what changed. Returns true when the overlay buffer changed
    // and the caller should uiShow(). overVideo: only a window of the plane
    // is up (see overVideo()) -- a whole-frame push is left for the screens.
    bool tick(bool overVideo = false);

    // What to show over live video right now -- the volume bar, for a
    // moment after a volume change (also one not tick()ed in yet) -- and
    // where: the overlay plane's window. Cheap enough to ask every loop turn.
    bool overVideo(int &x, int &y, int &w, int &h);

    // Push the whole frame to the plane on the next tick (backend transition:
    // the renderer's buffer always holds the complete current frame).
    void invalidate();

private:
    struct Impl; // Slint types stay out of this header (C++20 only in the .cpp)
    Impl *_impl = nullptr;
    bool _active = false;

    std::atomic<int> _px{0}, _py{0};
    std::atomic<bool> _pressed{false};
    std::atomic<bool> _pressSeen{false}; // tap shorter than one tick

    // Discrete events, so a FIFO rather than the pointer's last-state atomics:
    // several keys can land within one tick. Lives here, not in Impl, so a
    // key() racing end() never touches freed memory.
    struct KeyEvent
    {
        OsdKey nav;
        char ch;
        int8_t value;
    };
    static constexpr int KEY_QUEUE = 32;
    std::mutex _keyMutex;
    KeyEvent _keys[KEY_QUEUE];
    int _keyCount = 0;
};

#endif /* USE_SLINT */
#endif /* SRC_UI_SLINT_OSD */
