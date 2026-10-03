#include "lvgl_osd.h"

#ifdef USE_LVGL

#include <cstdlib>

#include <lvgl/lvgl.h>

#include "common/logger.h"
#include "settings.h"
#include "ui_screens.h"

#ifdef USE_CEDRUS
#include "drm_display.h"
#include <drm_fourcc.h>
#endif

#ifdef USE_EEZ
// EEZ Studio generated entry points (ui_init/ui_tick). Not wrapped in
// extern "C": ui.h guards its own declarations and pulls in eez-flow.h,
// which is C++ (templates, overloads, namespaces).
#include "ui.h"
#endif

// Partial render: LVGL draws into this fraction of the screen at a time and
// flushes each rectangle. A tenth of the panel keeps the buffer small and
// lets LVGL redraw only what changed, which matters on the 408 MHz arm926
// where a full-frame repaint would be pure waste.
#define LVGL_BUFFER_LINES_DIV 10

// Bridges LVGL's C callbacks back onto the instance stored in user_data.
struct LvglOsdCallbacks
{
    static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
    {
        LvglOsd *self = (LvglOsd *)lv_display_get_user_data(disp);
        if (self != nullptr)
        {
            const int w = area->x2 - area->x1 + 1;
            const int h = area->y2 - area->y1 + 1;
#ifdef USE_CEDRUS
            if (self->_drm)
            {
                drm_display::uiWriteRect(area->x1, area->y1, w, h, px, w * self->_bpp);
                self->_flushed = true;
            }
            else
#endif
            if (self->_texture != nullptr)
            {
                SDL_Rect rect{area->x1, area->y1, w, h};
                SDL_UpdateTexture(self->_texture, &rect, px, rect.w * self->_bpp);
                self->_flushed = true;
                if (self->_dirty.w == 0 || self->_dirty.h == 0)
                    self->_dirty = rect;
                else
                    SDL_UnionRect(&self->_dirty, &rect, &self->_dirty);
            }
        }
        lv_display_flush_ready(disp);
    }

    static void read(lv_indev_t *indev, lv_indev_data_t *data)
    {
        LvglOsd *self = (LvglOsd *)lv_indev_get_user_data(indev);
        if (self == nullptr)
            return;

        data->point.x = self->_px;
        data->point.y = self->_py;
        data->state = self->_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }

    static void readEncoder(lv_indev_t *indev, lv_indev_data_t *data)
    {
        LvglOsd *self = (LvglOsd *)lv_indev_get_user_data(indev);
        if (self == nullptr)
            return;

        // enc_diff is consumed by LVGL, so hand over the accumulated steps
        // and reset: detents that arrive between reads are never dropped.
        data->enc_diff = (int16_t)self->_encSteps;
        self->_encSteps = 0;
        data->state = self->_encPressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }
};

LvglOsd::~LvglOsd()
{
    end();
}

// The display/indev/screen setup both backends share. `bpp` is the bytes per
// pixel of the output the flush callback writes to.
bool LvglOsd::beginCommon(int width, int height, int bpp)
{
    _width = width;
    _height = height;
    _bpp = bpp;

    int lines = _height / LVGL_BUFFER_LINES_DIV;
    if (lines < 1)
        lines = 1;
    size_t bufferSize = (size_t)_width * lines * bpp;
    _buffer = (uint8_t *)malloc(bufferSize);
    if (_buffer == nullptr)
    {
        log_e("LVGL: buffer alloc failed (%zu bytes)", bufferSize);
        end();
        return false;
    }

    lv_init();
    lv_tick_set_cb(SDL_GetTicks);

    lv_display_t *disp = lv_display_create(_width, _height);
    if (disp == nullptr)
    {
        log_e("LVGL: display create failed");
        end();
        return false;
    }
    _disp = disp;

    lv_display_set_color_format(disp, _bpp == 2 ? LV_COLOR_FORMAT_RGB565
                                                : LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_buffers(disp, _buffer, nullptr, bufferSize,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, LvglOsdCallbacks::flush);
    lv_display_set_user_data(disp, this);

    lv_indev_t *indev = lv_indev_create();
    if (indev != nullptr)
    {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, LvglOsdCallbacks::read);
        lv_indev_set_user_data(indev, this);
        lv_indev_set_display(indev, disp);
    }
    _indev = indev;

    lv_indev_t *encoder = lv_indev_create();
    if (encoder != nullptr)
    {
        lv_indev_set_type(encoder, LV_INDEV_TYPE_ENCODER);
        lv_indev_set_read_cb(encoder, LvglOsdCallbacks::readEncoder);
        lv_indev_set_user_data(encoder, this);
        lv_indev_set_display(encoder, disp);
    }
    _encoder = encoder;

    _generated = Settings::lvglScreen.value == "generated";
#ifdef USE_EEZ
    if (_generated)
        ui_init(); // EEZ Studio screens (and the flow)
    else
        ui_screens::begin(_width, _height); // built-in screens
#else
    if (_generated)
        log_w("LVGL: lvgl-screen=generated needs a USE_EEZ build, using the picker");
    _generated = false;
    ui_screens::begin(_width, _height); // built-in screens
#endif

    // The screens own the focus group, so it only exists once they are built.
    if (encoder != nullptr && ui_screens::group() != nullptr)
        lv_indev_set_group(encoder, ui_screens::group());

    log_v("LVGL: started %dx%d (%s, %d bpp, partial buffer %zu bytes)", _width, _height,
          _generated ? "generated" : "picker", _bpp, bufferSize);
    return true;
}

bool LvglOsd::begin(SDL_Renderer *renderer, int width, int height)
{
    if (_disp != nullptr)
        return true;

    if (renderer == nullptr || width <= 0 || height <= 0)
    {
        log_e("LVGL: invalid renderer or size %dx%d", width, height);
        return false;
    }

    _renderer = renderer;
    _drm = false;

    _texture = SDL_CreateTexture(_renderer, SDL_PIXELFORMAT_ARGB8888,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    if (_texture == nullptr)
    {
        log_e("LVGL: texture failed > %s", SDL_GetError());
        return false;
    }
    SDL_SetTextureBlendMode(_texture, SDL_BLENDMODE_NONE); // opaque screens: plain copy, not a per-pixel blend

    return beginCommon(width, height, 4);
}

#ifdef USE_CEDRUS
bool LvglOsd::beginDrm(int width, int height)
{
    if (_disp != nullptr)
        return true;
    if (width <= 0 || height <= 0)
        return false;
    if (!drm_display::uiBegin())
        return false;

    _drm = true;
    _renderer = nullptr;
    _texture = nullptr;
    const int bpp = drm_display::uiBpp();
    if (bpp != 2 && bpp != 4)
    {
        log_e("LVGL: unsupported overlay bpp %d", bpp);
        return false;
    }
    return beginCommon(width, height, bpp);
}
#endif

void LvglOsd::end()
{
    if (_disp != nullptr)
    {
        lv_deinit();
        _disp = nullptr;
        _indev = nullptr;
        _encoder = nullptr;
    }
    if (_texture != nullptr)
    {
        SDL_DestroyTexture(_texture);
        _texture = nullptr;
    }
    if (_buffer != nullptr)
    {
        free(_buffer);
        _buffer = nullptr;
    }
    _renderer = nullptr;
    _drm = false;
}

void LvglOsd::pointer(int x, int y, bool pressed)
{
    _px = x;
    _py = y;
    _pressed = pressed;
}

void LvglOsd::encoder(int steps, bool pressed)
{
    _encSteps += steps; // accumulate; the read callback drains it
    _encPressed = pressed;
}

void LvglOsd::render()
{
    tick();
    blit();
}

bool LvglOsd::tick()
{
    if (_disp == nullptr)
        return false;

#ifdef USE_EEZ
    if (_generated)
        ui_tick(); // flow tick + screen tick (EEZ generated)
    else
        ui_screens::tick();
#else
    ui_screens::tick();
#endif
    lv_timer_handler();
    if (_drm)
    {
        // DRM backend: the flushes already landed in the dumb buffer.
        bool changed = _flushed;
        _flushed = false;
        return changed;
    }
    return _flushed;
}

void LvglOsd::invalidate()
{
    if (_disp == nullptr)
        return;
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now((lv_display_t *)_disp);
}

void LvglOsd::blit()
{
    if (_disp == nullptr || _drm)
        return;
    SDL_RenderCopy(_renderer, _texture, nullptr, nullptr);
    // SDL batches render commands; the DRM path reads the renderer's target
    // surface directly, so execute them now (otherwise the present copies a
    // surface that is one update behind and the UI looks a frame laggy).
    SDL_RenderFlush(_renderer);
    _flushed = false;
    _dirty = SDL_Rect{0, 0, 0, 0};
}

SDL_Rect LvglOsd::blitDirty()
{
    SDL_Rect r = _dirty;
    if (_disp == nullptr || _drm || r.w <= 0 || r.h <= 0)
        return SDL_Rect{0, 0, 0, 0};
    SDL_RenderCopy(_renderer, _texture, &r, &r);
    SDL_RenderFlush(_renderer); // see blit()
    _flushed = false;
    _dirty = SDL_Rect{0, 0, 0, 0};
    return r;
}

#endif /* USE_LVGL */
