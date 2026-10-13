#ifndef SRC_UI_SLINT_OSD
#define SRC_UI_SLINT_OSD

// The Slint UI bound straight to drm_display's dumb framebuffer: the
// software renderer paints into a persistent full-size RGB565 buffer and the
// dirty rows are copied into the overlay plane (uiWriteRect). The caller owns
// presentation (drm_display::uiShow() when tick() reports a change).
//
// Slint is single-threaded: pointer() may be called from the touch thread
// (it only records state); everything else must stay on the loop thread.

#ifdef USE_SLINT

#include <atomic>

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

    // Pumps app state into the UI, dispatches input, runs timers/animations
    // and renders what changed. Returns true when the overlay buffer changed
    // and the caller should uiShow().
    bool tick();

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
};

#endif /* USE_SLINT */
#endif /* SRC_UI_SLINT_OSD */
