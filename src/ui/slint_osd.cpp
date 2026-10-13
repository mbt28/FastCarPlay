#include "slint_osd.h"

#ifdef USE_SLINT

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include <slint-platform.h>

#include "app_window.h" // generated from src/ui/slint/app.slint

#include "common/logger.h"
#include "drm_display.h"
#include "protocol/wifi_ap.h"
#include "settings.h"
#include "ui_bridge.h"

namespace
{
// WPA2 fixes the passphrase at 8-63 characters and the SSID at 1-32; hostapd
// refuses to start outside those, so they are enforced before saving.
struct Field
{
    const char *key;   // settings key
    const char *label; // editor title
    int minLength;
    int maxLength; // bytes, as hostapd counts them
};
const Field FIELDS[] = {
    {"wifi-ssid", "Name", 1, 32},
    {"wifi-passphrase", "Password", 8, 63},
    {"bluetooth-name", "Bluetooth", 1, 32},
};
constexpr int FIELD_COUNT = (int)(sizeof(FIELDS) / sizeof(FIELDS[0]));

std::string fieldValue(int id)
{
    switch (id)
    {
    case 0:
        return Settings::wifiSsid.value;
    case 1:
        return Settings::wifiPass.value;
    case 2:
        return Settings::btName.value;
    default:
        return "";
    }
}

// Drop the last character, not the last byte (an existing value may hold
// multi-byte UTF-8 even though the OSK itself only emits ASCII).
void utf8PopBack(std::string &s)
{
    while (!s.empty() && (s.back() & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

void persist(const char *key, const char *value, bool needsRestart)
{
    if (!Settings::setUser(key, value))
    {
        log_e("Could not save %s", key);
        return;
    }
    if (needsRestart)
        ui_bridge::setRestartNeeded();
}

const char *nightModeName(int mode)
{
    switch (mode)
    {
    case 0:
        return "Day";
    case 1:
        return "Night";
    default:
        return "Auto";
    }
}

// ── slint platform glue ──────────────────────────────────────────────────
class OsdWindow : public slint::platform::WindowAdapter
{
public:
    OsdWindow(uint32_t w, uint32_t h)
        : _renderer(slint::platform::SoftwareRenderer::RepaintBufferType::ReusedBuffer),
          _size({w, h})
    {
    }

    slint::PhysicalSize size() override { return _size; }
    slint::platform::AbstractRenderer &renderer() override { return _renderer; }
    void request_redraw() override { dirty = true; }

    slint::platform::SoftwareRenderer _renderer;
    slint::PhysicalSize _size;
    bool dirty = true;
};

class OsdPlatform : public slint::platform::Platform
{
public:
    OsdPlatform(uint32_t w, uint32_t h) : _w(w), _h(h) {}

    std::unique_ptr<slint::platform::WindowAdapter> create_window_adapter() override
    {
        auto win = std::make_unique<OsdWindow>(_w, _h);
        window = win.get();
        return win;
    }

    OsdWindow *window = nullptr;
    uint32_t _w, _h;
};

// set_platform() is once per process; remember what it got.
OsdPlatform *g_platform = nullptr;
} // namespace

struct SlintOsd::Impl
{
    slint::ComponentHandle<AppWindow> ui;
    OsdWindow *win = nullptr;
    std::unique_ptr<slint::platform::Rgb565Pixel[]> buffer;
    int width = 0;
    int height = 0;
    bool fullPush = true; // first frame pushes everything
    bool lastDown = false;

    // Editor state: the buffer is owned here, the UI only displays it.
    int editing = -1;
    std::string editBuffer;

    // Last pushed property values, so an unchanged tick sets nothing and the
    // renderer stays idle (every overlay commit costs real time on the F1C).
    std::string pStatus{"\x01"}, pSource, pNight, pFps, pSsid, pPass, pBt;
    int pProtocol = -1;
    bool pWaiting = false, pRestartNeeded = false;

    Impl(int w, int h, slint::ComponentHandle<AppWindow> handle)
        : ui(std::move(handle)), width(w), height(h)
    {
    }

    void setEditing(int id)
    {
        editing = id;
        const auto &bridge = ui->global<Bridge>();
        bridge.set_editing(id);
        if (id >= 0)
        {
            editBuffer = fieldValue(id);
            bridge.set_editor_label(FIELDS[id].label);
            bridge.set_editor_text(slint::SharedString(editBuffer));
            bridge.set_editor_error("");
        }
    }

    void saveField()
    {
        if (editing < 0 || editing >= FIELD_COUNT)
            return;
        const Field &field = FIELDS[editing];
        const int length = (int)editBuffer.size();
        if (length < field.minLength || length > field.maxLength)
        {
            char message[64];
            snprintf(message, sizeof(message), "Needs %d-%d characters", field.minLength,
                     field.maxLength);
            ui->global<Bridge>().set_editor_error(message);
            return; // keep the editor open rather than saving something unusable
        }

        if (editing == 0 || editing == 1)
        {
            // The AP belongs to the system: written into its config (not
            // usersettings.txt) and the AP restarts. No app restart needed --
            // the backends read the effective config when a session starts.
            const std::string ssid = (editing == 0) ? editBuffer : Settings::wifiSsid.value;
            const std::string pass = (editing == 1) ? editBuffer : Settings::wifiPass.value;
            if (wifi_ap::configure(ssid, pass))
            {
                // Re-read so the rows show what hostapd actually advertises.
                const wifi_ap::Params ap = wifi_ap::read();
                if (!ap.ssid.empty())
                {
                    Settings::wifiSsid.value = ap.ssid;
                    Settings::wifiPass.value = ap.passphrase;
                }
            }
            else
            {
                log_e("Could not update the access point");
            }
        }
        else
        {
            persist(field.key, editBuffer.c_str(), true); // BT name is set at start-up
        }
        setEditing(-1);
    }

    // App state -> Bridge properties, on change only.
    void pump()
    {
        const auto &bridge = ui->global<Bridge>();

        const std::string status = get_var_status();
        if (status != pStatus)
        {
            bridge.set_status(slint::SharedString(status));
            pStatus = status;
        }
        const std::string source = ui_bridge::protocolName();
        if (source != pSource)
        {
            bridge.set_source_label(slint::SharedString(source));
            pSource = source;
        }
        const bool waiting = ui_bridge::backgroundedSession();
        if (waiting != pWaiting)
        {
            bridge.set_session_waiting(waiting);
            pWaiting = waiting;
        }
        const bool restartNeeded = ui_bridge::restartNeeded();
        if (restartNeeded != pRestartNeeded)
        {
            bridge.set_restart_needed(restartNeeded);
            pRestartNeeded = restartNeeded;
        }
        const int protocol = get_var_protocol();
        if (protocol != pProtocol)
        {
            bridge.set_protocol(protocol);
            pProtocol = protocol;
        }
        const std::string night = nightModeName(Settings::nightMode);
        if (night != pNight)
        {
            bridge.set_night_label(slint::SharedString(night));
            pNight = night;
        }
        char fps[16];
        snprintf(fps, sizeof(fps), "%d fps", (int)Settings::aaFps);
        if (pFps != fps)
        {
            bridge.set_fps_label(slint::SharedString(fps));
            pFps = fps;
        }
        if (Settings::wifiSsid.value != pSsid)
        {
            pSsid = Settings::wifiSsid.value;
            bridge.set_wifi_ssid(slint::SharedString(pSsid));
        }
        if (Settings::wifiPass.value != pPass)
        {
            pPass = Settings::wifiPass.value;
            bridge.set_wifi_pass(slint::SharedString(pPass));
        }
        if (Settings::btName.value != pBt)
        {
            pBt = Settings::btName.value;
            bridge.set_bt_name(slint::SharedString(pBt));
        }
    }
};

SlintOsd::~SlintOsd()
{
    end();
}

bool SlintOsd::beginDrm(int width, int height)
{
    if (_impl != nullptr)
        return true;
    if (width <= 0 || height <= 0)
        return false;
    if (!drm_display::uiBegin())
        return false;
    if (drm_display::uiBpp() != 2)
    {
        // The .slint styling and this renderer target RGB565; running on a
        // surprise format would render garbage, so refuse instead.
        log_e("Slint UI: overlay is %d bpp, RGB565 required", drm_display::uiBpp() * 8);
        return false;
    }
    if (g_platform != nullptr)
    {
        log_e("Slint UI: platform already initialized (one begin per process)");
        return false;
    }

    auto platform = std::make_unique<OsdPlatform>((uint32_t)width, (uint32_t)height);
    g_platform = platform.get();
    slint::platform::set_platform(std::move(platform));

    auto ui = AppWindow::create();
    _impl = new Impl(width, height, ui);
    _impl->buffer.reset(new slint::platform::Rgb565Pixel[(size_t)width * height]);

    // UI -> app wiring. Callbacks run inside tick() on the loop thread.
    const auto &bridge = ui->global<Bridge>();
    bridge.set_editing(-1);
    bridge.on_select_protocol([](int id) { set_var_protocol(id); });
    bridge.on_resume([] { ui_bridge::requestResume(); });
    bridge.on_restart_now([] { ui_bridge::requestRestart(); });
    bridge.on_cycle_night([] {
        // 0 day -> 1 night -> 2 auto -> 0. Only reaches the phone in the
        // sensor batch at connect, so it needs a restart to take effect.
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%d", (int)((Settings::nightMode + 1) % 3));
        persist("night-mode", buffer, true);
    });
    bridge.on_cycle_fps([] {
        persist("aa-video-fps", Settings::aaFps >= 60 ? "30" : "60", true);
    });
    Impl *impl = _impl;
    bridge.on_edit_field([impl](int id) {
        if (id >= 0 && id < FIELD_COUNT)
            impl->setEditing(id);
    });
    bridge.on_osk_key([impl](slint::SharedString key) {
        if (impl->editing < 0)
            return;
        const std::string k(key.data(), key.size());
        if ((int)(impl->editBuffer.size() + k.size()) > FIELDS[impl->editing].maxLength)
            return;
        impl->editBuffer += k;
        impl->ui->global<Bridge>().set_editor_text(slint::SharedString(impl->editBuffer));
    });
    bridge.on_osk_backspace([impl] {
        if (impl->editing < 0)
            return;
        utf8PopBack(impl->editBuffer);
        impl->ui->global<Bridge>().set_editor_text(slint::SharedString(impl->editBuffer));
    });
    bridge.on_osk_ok([impl] { impl->saveField(); });
    bridge.on_osk_cancel([impl] { impl->setEditing(-1); });

    ui->show();
    _impl->win = g_platform->window;
    if (_impl->win == nullptr)
    {
        log_e("Slint UI: no window adapter");
        end();
        return false;
    }

    _active = true;
    log_v("Slint UI: started %dx%d (RGB565, %zu KB frame)", width, height,
          ((size_t)width * height * 2) / 1024);
    return true;
}

void SlintOsd::end()
{
    if (_impl != nullptr)
    {
        _impl->ui->hide();
        delete _impl;
        _impl = nullptr;
    }
    _active = false;
    // The slint platform itself stays: it cannot be unset within a process.
}

void SlintOsd::pointer(int x, int y, bool pressed)
{
    _px = x;
    _py = y;
    if (pressed)
        _pressSeen = true;
    _pressed = pressed;
}

bool SlintOsd::tick()
{
    if (_impl == nullptr || _impl->win == nullptr)
        return false;
    Impl &impl = *_impl;

    // Touch -> pointer events, on this thread as Slint requires. A tap that
    // fit entirely between two ticks still delivers a press + release pair.
    {
        const bool down = _pressed;
        const bool seen = _pressSeen.exchange(false);
        const slint::LogicalPosition pos({(float)_px.load(), (float)_py.load()});
        auto &window = impl.win->window();
        if (down && !impl.lastDown)
            window.dispatch_pointer_press_event(pos, slint::PointerEventButton::Left);
        else if (!down && impl.lastDown)
            window.dispatch_pointer_release_event(pos, slint::PointerEventButton::Left);
        else if (down)
            window.dispatch_pointer_move_event(pos);
        else if (seen)
        {
            window.dispatch_pointer_press_event(pos, slint::PointerEventButton::Left);
            window.dispatch_pointer_release_event(pos, slint::PointerEventButton::Left);
        }
        impl.lastDown = down;
    }

    impl.pump();
    slint::platform::update_timers_and_animations();

    const bool fullPush = impl.fullPush;
    if (!impl.win->dirty && !impl.win->window().has_active_animations() && !fullPush)
        return false;
    impl.win->dirty = false;
    impl.fullPush = false;

    // ReusedBuffer: the buffer always holds the complete current frame, the
    // renderer only repaints what changed and reports where.
    auto region = impl.win->_renderer.render(
        std::span<slint::platform::Rgb565Pixel>(impl.buffer.get(),
                                                (size_t)impl.width * impl.height),
        (size_t)impl.width);

    int y0 = 0, y1 = impl.height;
    if (!fullPush)
    {
        const auto origin = region.bounding_box_origin();
        const auto size = region.bounding_box_size();
        if (size.width == 0 || size.height == 0)
            return false; // nothing actually changed
        y0 = origin.y;
        y1 = origin.y + (int)size.height;
    }
    drm_display::uiWriteRect(0, y0, impl.width, y1 - y0,
                             (const uint8_t *)(impl.buffer.get() + (size_t)y0 * impl.width),
                             impl.width * 2);
    return true;
}

void SlintOsd::invalidate()
{
    if (_impl == nullptr)
        return;
    _impl->fullPush = true;
    if (_impl->win != nullptr)
        _impl->win->dirty = true;
}

#endif /* USE_SLINT */
