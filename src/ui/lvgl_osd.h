#ifndef SRC_UI_LVGL_OSD
#define SRC_UI_LVGL_OSD

// LVGL bound straight to drm_display's dumb framebuffer: each flush is one
// memcpy of the changed rectangle, in the overlay plane's own pixel format
// (RGB565 on this panel). LVGL renders into a small partial buffer in cached
// RAM; the caller owns presentation (drm_display::uiShow() when tick()
// reports a change).

#ifdef USE_LVGL

#include <cstdint>

class LvglOsd
{
public:
    LvglOsd() = default;
    ~LvglOsd();

    // DRM backend: flushes go straight into drm_display's dumb framebuffer
    // (format from drm_display::uiFormat()). The caller shows the plane with
    // drm_display::uiShow() when tick() reports a change. Size comes from the
    // caller (the DRM panel), never a compile-time constant.
    bool beginDrm(int width, int height);
    void end();
    bool active() const { return _disp != nullptr; }

    // Feed pointer state (evdev touch). Coordinates are in display pixels.
    void pointer(int x, int y, bool pressed);

    // Feed a 3-way rotary encoder: `steps` is the rotation since the last
    // call (negative = left), `pressed` is the push button.
    void encoder(int steps, bool pressed);

    // Runs the LVGL timers and reports whether anything was flushed since the
    // last tick(). When it returns false the screen is unchanged and the
    // caller can skip presenting entirely.
    bool tick();

    // Mark the whole screen dirty and repaint it NOW (synchronous refresh),
    // so a backend transition (video -> home) shows fresh content in the
    // same iteration instead of a stale frame.
    void invalidate();

private:
    // LVGL types are kept out of this header so including it (e.g. from
    // application.cpp) does not drag in lvgl.h; the .cpp casts them back.
    friend struct LvglOsdCallbacks;

    bool beginCommon(int width, int height, int bpp);

    void *_disp = nullptr;
    void *_indev = nullptr;
    void *_encoder = nullptr;
    uint8_t *_buffer = nullptr;
    int _encSteps = 0;
    bool _encPressed = false;
    int _width = 0;
    int _height = 0;
    int _bpp = 4;
    int _px = 0, _py = 0;
    bool _flushed = true; // output changed since the last tick()
    bool _pressed = false;
};

#endif /* USE_LVGL */
#endif /* SRC_UI_LVGL_OSD */
