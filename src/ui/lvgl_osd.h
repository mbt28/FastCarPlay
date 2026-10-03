#ifndef SRC_UI_LVGL_OSD
#define SRC_UI_LVGL_OSD

// LVGL with two presentation backends:
//   - renderer = sdl  (desktop / Pi dev): bound to the window's SDL_Renderer;
//     each flush lands in a streaming texture which blit() copies over.
//   - renderer = drm  (F1C200s): bound straight to drm_display's dumb
//     framebuffer -- each flush is one memcpy of the changed rectangle, in
//     the overlay plane's own pixel format (RGB565 on this panel). No SDL
//     texture, no composition surface, no intermediate copies.
// LVGL renders into a small partial buffer in cached RAM either way; the
// caller owns presentation (SDL_RenderPresent / drm_display::uiShow()).

#ifdef USE_LVGL

#include <SDL2/SDL.h>

class LvglOsd
{
public:
    LvglOsd() = default;
    ~LvglOsd();

    // Creates the LVGL display/input at the given size and builds the
    // screens. Size comes from the caller (the DRM panel, or the SDL
    // window), never a compile-time constant.
    bool begin(SDL_Renderer *renderer, int width, int height);
#ifdef USE_CEDRUS
    // DRM backend: flushes go straight into drm_display's dumb framebuffer
    // (format from drm_display::uiFormat()). The caller shows the plane with
    // drm_display::uiShow() when tick() reports a change.
    bool beginDrm(int width, int height);
#endif
    void end();
    bool active() const { return _disp != nullptr; }

    // Feed pointer state (SDL mouse/touch on the desktop path, evdev/GT911 on
    // the device path). Coordinates are in display pixels.
    void pointer(int x, int y, bool pressed);

    // Feed a 3-way rotary encoder: `steps` is the rotation since the last
    // call (negative = left), `pressed` is the push button. Head units often
    // have only this, so every screen must be fully operable through it.
    void encoder(int steps, bool pressed);

    // Runs the flow + LVGL timers and copies the result to the renderer.
    void render();

    // Split form for callers that pace themselves: tick() runs the LVGL
    // timers and reports whether anything was flushed since the last blit()
    // (SDL) / tick() (DRM). When it returns false the screen is unchanged
    // and the caller can skip presenting entirely.
    bool tick();
    void blit();

    // Mark the whole screen dirty and repaint it NOW (synchronous refresh),
    // so a backend transition (video -> home) shows fresh content in the
    // same iteration instead of a stale frame.
    void invalidate();

    // Like blit(), but copies only the union of the rectangles LVGL flushed
    // since the last blit and returns it (empty rect if nothing changed).
    // SDL backend only; the DRM backend writes dirty rects directly.
    SDL_Rect blitDirty();

private:
    // LVGL types are kept out of this header so including it (e.g. from
    // application.cpp) does not drag in lvgl.h; the .cpp casts them back.
    friend struct LvglOsdCallbacks;

    bool beginCommon(int width, int height, int bpp);

    SDL_Renderer *_renderer = nullptr;
    SDL_Texture *_texture = nullptr;
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
    bool _drm = false;    // flushes go to drm_display, not an SDL texture
    bool _flushed = true; // output changed since the last blit()/tick()
    SDL_Rect _dirty{0, 0, 0, 0}; // union of flushed areas since the last blit()
    bool _pressed = false;
    bool _generated = false; // EEZ screens vs the built-in picker
};

#endif /* USE_LVGL */
#endif /* SRC_UI_LVGL_OSD */
