#include "slint_osd.h"

#ifdef USE_SLINT

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <slint-platform.h>

#include "app_window.h" // generated from src/ui/slint/app.slint

#include "common/clock.h"
#include "common/logger.h"
#include "drm_display.h"
#include "protocol/wifi_ap.h"
#include "audio_volume.h"
#include "camera.h"
#include "settings.h"
#include "ui_bridge.h"

#ifdef USE_BLUETOOTH
#include "bluetooth/bt_manager.h"
#endif

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
    {"bluetooth-name", "BT name", 1, 32},
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

// The camera view's "tap to close" stays up this long.
constexpr uint32_t CAMERA_HINT_MS = 3000;

// The camera's standards as the setting spells them, and as shown.
const char *const CAMERA_STANDARDS[] = {"auto", "pal", "ntsc"};
const char *const CAMERA_STANDARD_LABELS[] = {"Auto", "PAL", "NTSC"};

int cameraStandardIndex()
{
    for (int i = 0; i < 3; i++)
        if (Settings::cameraStandard.value == CAMERA_STANDARDS[i])
            return i;
    return 0;
}

// Keyboard focus is only re-seeded for someone actually using a keyboard.
constexpr uint32_t KEY_RECENT_MS = 10000;

// The volume bar stays up this long after the last change.
constexpr uint32_t VOLUME_BAR_MS = 2000;

bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

#ifdef USE_BLUETOOTH
BtRow toRow(const bt::DeviceRow &d)
{
    BtRow r;
    r.id = slint::SharedString(d.id);
    r.name = slint::SharedString(d.name);
    r.detail = slint::SharedString(d.detail);
    r.connected = d.connected;
    return r;
}

// The ring is on a row of this list (ring-row "<prefix><id>") that has gone.
bool ringRowGone(std::string_view ring, std::string_view prefix,
                 const std::vector<bt::DeviceRow> &rows)
{
    if (ring.substr(0, prefix.size()) != prefix)
        return false;
    const std::string_view id = ring.substr(prefix.size());
    return std::none_of(rows.begin(), rows.end(), [&](const bt::DeviceRow &d) { return d.id == id; });
}

// Bring a list model in line with the manager's rows, by id: changed rows are
// updated in place, vanished ones erased, new ones appended. Never a reset or
// a new model: that destroys every row instance (keyboard focus is lost and
// the whole list repaints). Both sides keep first-seen order, so appending
// keeps them aligned.
void syncRows(slint::VectorModel<BtRow> &model, const std::vector<bt::DeviceRow> &rows)
{
    for (size_t i = model.row_count(); i-- > 0;)
    {
        const auto row = model.row_data(i);
        const std::string_view id = row ? std::string_view(row->id) : std::string_view();
        const bool keep = std::any_of(rows.begin(), rows.end(),
                                      [&](const bt::DeviceRow &d) { return d.id == id; });
        if (!keep)
            model.erase(i);
    }
    for (const bt::DeviceRow &d : rows)
    {
        const BtRow want = toRow(d);
        size_t i = 0;
        while (i < model.row_count() && model.row_data(i)->id != want.id)
            i++;
        if (i == model.row_count())
            model.push_back(want);
        else if (!(*model.row_data(i) == want))
            model.set_row_data(i, want);
    }
}
#endif

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
    bool volumeSeen = false;
    uint32_t pVolumeGen = 0;
    bool volumeBar = false; // the bar is up (in the scene)
    uint32_t volumeBarUntil = 0;
    bool pLinkUsb = false, pLinkWifi = false;
    int pProtocol = -1;
    bool pWaiting = false, pRestartNeeded = false;

    // Keyboard. Switching a screen or closing a dialog destroys the focused
    // row, so focus is re-seeded on the first row -- but only for someone
    // who used a key recently (touch never shows a ring).
    uint32_t lastKeyMs = 0;
    bool keyUsed = false;
    bool focusSeed = false;
    int pScreen = -1;
    bool pModal = false;
    bool eraseEdits = false; // the held Backspace was pressed in the editor
    // The row with the ring went away while its screen stayed: next tick,
    // once it is really gone, the ring goes back on the first row.
    bool ringLost = false;
    bool switched = false; // this pump saw a screen switch

    void ringGone(const Bridge &bridge)
    {
        bridge.set_ring_row("");
        if (!switched)
            ringLost = true;
    }

    // The camera (Settings > Video): what the device offers, probed when the
    // Video screen opens, and the hint shown over the camera view.
#ifdef USE_CEDRUS
    Camera::Info camera;
#endif
    bool cameraDirty = true;
    bool cameraHint = false;
    uint32_t cameraHintUntil = 0;

    // A picture level: the saved one, or the device's own until changed.
    static int cameraLevel(const Setting<int> &saved, int device)
    {
        return saved.value >= 0 ? saved.value : device;
    }

    void pumpCamera(const Bridge &bridge)
    {
        if (!cameraDirty)
            return;
        cameraDirty = false;
#ifdef USE_CEDRUS
        auto label = [](int level) {
            char text[8];
            snprintf(text, sizeof(text), "%d%%", level);
            return level < 0 ? slint::SharedString() : slint::SharedString(text);
        };
        bridge.set_camera_available(camera.present);
        const int input = Settings::cameraInput;
        bridge.set_camera_input_label(
            camera.inputs.size() < 2 ? slint::SharedString()
            : input >= 0 && input < (int)camera.inputs.size()
                ? slint::SharedString(camera.inputs[(size_t)input])
                : slint::SharedString("Input " + std::to_string(input)));
        bridge.set_camera_standard_label(CAMERA_STANDARD_LABELS[cameraStandardIndex()]);
        bridge.set_camera_brightness_label(
            label(camera.brightness < 0 ? -1 : cameraLevel(Settings::cameraBrightness, camera.brightness)));
        bridge.set_camera_contrast_label(
            label(camera.contrast < 0 ? -1 : cameraLevel(Settings::cameraContrast, camera.contrast)));
        bridge.set_camera_saturation_label(
            label(camera.saturation < 0 ? -1 : cameraLevel(Settings::cameraSaturation, camera.saturation)));
#else
        (void)bridge;
#endif
    }

    // The dialog shows Bluetooth's prompt -- pairing has a deadline, so it
    // wins -- or else our own question from the System screen.
    struct DialogText
    {
        int kind = 0; // 0 none, 1 a code (Cancel only), 2 yes/no
        std::string title, code, hint, accept;
    };
    DialogText btDialog;      // the Bluetooth manager's, as last seen
    int systemAsk = 0;        // the System screen asks: 0 none, 1 reboot, 2 shut down
    bool dialogDirty = true;  // either changed: pumpDialog looks again
    bool dialogLocal = false; // the dialog shown is our own question
    int pPromptKind = 0;
    std::string pPromptTitle, pPromptCode, pPromptHint, pPromptAccept;

    void answerSystem(bool yes)
    {
        const int asked = systemAsk;
        systemAsk = 0;
        dialogDirty = true;
        if (yes && asked != 0)
            ui_bridge::requestPower(asked == 1 ? ui_bridge::Power::Reboot
                                               : ui_bridge::Power::Shutdown);
    }

#ifdef USE_BLUETOOTH
    // Set once and never replaced (see syncRows).
    std::shared_ptr<slint::VectorModel<BtRow>> btPaired =
        std::make_shared<slint::VectorModel<BtRow>>();
    std::shared_ptr<slint::VectorModel<BtRow>> btFound =
        std::make_shared<slint::VectorModel<BtRow>>();
    bool btSeen = false; // first snapshot pushed
    uint32_t pBtGen = 0;
    std::string btCurrentId; // the device screen's device (by id)
    BtRow pBtCurrent;
    std::string pBtSummary, pBtStatus;
    bool pBtScanning = false, pBtScanAllowed = true, pLinkBt = false;
#endif

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
            ui->global<Osk>().invoke_reset(); // letters, no ring
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

    // Tab/Backtab/Return as Slint key events (they reach the focused Row).
    void dispatchKey(std::u8string_view key, bool repeat)
    {
        auto &window = win->window();
        if (repeat)
            window.dispatch_key_press_repeat_event(key);
        else
            window.dispatch_key_press_event(key);
        window.dispatch_key_release_event(key);
    }

    // One keyboard event, on the loop thread. Dialogs take keys first. On the
    // list screens Up/Down move Slint's focus -- injected as Backtab/Tab, so
    // the built-in traversal (conditional rows, lists) and scroll-into-view do
    // the work -- and Enter activates the focused row. Back and Home never
    // depend on focus: a recreated screen has none.
    void applyKey(OsdKey nav, char ch, int value)
    {
        if (value == 0)
            return; // releases carry nothing: no modifier keys are sent to Slint
        const bool repeat = value == 2;
        lastKeyMs = mono_ms();
        keyUsed = true;
        const auto &bridge = ui->global<Bridge>();
        // A held Backspace keeps deleting only if it was pressed in the
        // editor: the press that cancels a dialog over it must not then eat
        // the text beneath.
        if (nav == OsdKey::Erase && !repeat)
            eraseEdits = bridge.get_prompt_kind() == 0 && editing >= 0;

        if (nav == OsdKey::Home)
        {
            if (repeat)
                return;
            if (bridge.get_prompt_kind() != 0)
                bridge.invoke_prompt_cancel();
            if (editing >= 0)
                setEditing(-1);
            bridge.set_screen(0);
            return;
        }
        if (const int kind = bridge.get_prompt_kind(); kind != 0)
        {
            if (repeat)
                return;
            // Buttons left to right: [accept] Cancel (a code dialog has only
            // Cancel). The arrows show and move a ring over them; Enter
            // presses the ringed button, or accepts when there is no ring.
            // Back always cancels. Our own questions are a reboot or a shut
            // down: there the ring starts on Cancel, and a press with no
            // ring only shows it -- a double press must not take the head
            // unit down.
            const int first = kind == 2 ? 0 : 1;
            const int start = dialogLocal ? 1 : first;
            const int focus = bridge.get_prompt_focus();
            switch (nav)
            {
            case OsdKey::Left:
            case OsdKey::Up:
                bridge.set_prompt_focus(focus < 0 ? start : std::max(first, focus - 1));
                break;
            case OsdKey::Right:
            case OsdKey::Down:
                bridge.set_prompt_focus(focus < 0 ? start : std::min(1, focus + 1));
                break;
            case OsdKey::Next: // a knob goes round
                bridge.set_prompt_focus(focus < 0 ? start : focus >= 1 ? first : focus + 1);
                break;
            case OsdKey::Prev:
                bridge.set_prompt_focus(focus < 0 ? start : focus <= first ? 1 : focus - 1);
                break;
            case OsdKey::Ok:
                if (focus < 0 && dialogLocal)
                    bridge.set_prompt_focus(1);
                else if (focus == 1)
                    bridge.invoke_prompt_cancel();
                else if (kind == 2)
                    bridge.invoke_prompt_accept();
                break;
            case OsdKey::Back:
            case OsdKey::Erase:
                bridge.invoke_prompt_cancel();
                break;
            default:
                break;
            }
            return;
        }
        // The camera view: any key closes it.
        if (bridge.get_screen() == 9)
        {
            if (!repeat)
                bridge.set_screen(8);
            return;
        }
        if (editing >= 0)
        {
            // Arrows move the on-screen keyboard's ring (how a remote types)
            // and Enter presses the ringed key. Typing on a keyboard hides
            // the ring again, so its Enter saves.
            const auto &osk = ui->global<Osk>();
            switch (nav)
            {
            case OsdKey::Up:
                osk.invoke_move(0, -1);
                return;
            case OsdKey::Down:
                osk.invoke_move(0, 1);
                return;
            case OsdKey::Left:
                osk.invoke_move(-1, 0);
                return;
            case OsdKey::Right:
                osk.invoke_move(1, 0);
                return;
            case OsdKey::Next: // a knob walks every key in reading order
                osk.invoke_step(1);
                return;
            case OsdKey::Prev:
                osk.invoke_step(-1);
                return;
            default:
                break;
            }
            // Through the same callbacks as the on-screen keyboard, so the
            // length limits and validation stay in one place.
            if (nav == OsdKey::Erase)
            {
                osk.set_row(-1);
                if (!repeat || eraseEdits)
                    bridge.invoke_osk_backspace(); // held Backspace keeps deleting
            }
            else if (repeat)
                return;
            else if (nav == OsdKey::Ok && ch != ' ')
            {
                if (osk.get_row() >= 0)
                    osk.invoke_activate();
                else
                    bridge.invoke_osk_ok();
            }
            else if (nav == OsdKey::Back)
                bridge.invoke_osk_cancel();
            else if (ch >= 0x20 && ch < 0x7F)
            {
                osk.set_row(-1);
                const char text[2] = {ch, 0};
                bridge.invoke_osk_key(slint::SharedString(text));
            }
            return;
        }
        // The volume row's -/+ sit side by side: there Left/Right move
        // between them (Left on "-" is still Back). A screen switched by an
        // earlier key of this tick has not reported its focus yet.
        const int stepper = bridge.get_screen() == pScreen ? bridge.get_stepper_focus() : -1;
        if ((nav == OsdKey::Right && stepper == 0) || (nav == OsdKey::Left && stepper == 1))
        {
            dispatchKey(nav == OsdKey::Right ? slint::platform::key_codes::Tab
                                             : slint::platform::key_codes::Backtab,
                        repeat);
            return;
        }
        switch (nav)
        {
        case OsdKey::Down:
        case OsdKey::Next: // a knob: the same focus chain, wrapping at its ends
            dispatchKey(slint::platform::key_codes::Tab, repeat);
            break;
        case OsdKey::Up:
        case OsdKey::Prev:
            dispatchKey(slint::platform::key_codes::Backtab, repeat);
            break;
        case OsdKey::Ok:
            if (!repeat)
                dispatchKey(slint::platform::key_codes::Return, false);
            break;
        case OsdKey::Back:
        case OsdKey::Erase:
        case OsdKey::Left: // remotes have no Back key; Left is the convention
            // Back from the home screen returns to a paused session, the
            // same as its Resume row.
            if (!repeat && !ui->invoke_back() && ui_bridge::backgroundedSession())
                ui_bridge::requestResume();
            break;
        default: // Right elsewhere, and typing outside the editor: nothing to act on
            break;
        }
    }

    // App state -> Bridge properties, on change only.
    void pump()
    {
        const auto &bridge = ui->global<Bridge>();
        switched = false;

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
            if (!waiting && bridge.get_ring_row() == "resume")
                ringGone(bridge);
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
        // Volume keys change it from another thread: one atomic load per tick.
        // Every change after the first look brings the volume bar up.
        const AudioVolume &volume = AudioVolume::instance();
        const uint32_t volumeGen = volume.generation();
        if (!volumeSeen || volumeGen != pVolumeGen)
        {
            const bool change = volumeSeen;
            volumeSeen = true;
            pVolumeGen = volumeGen;
            char label[16];
            if (volume.muted())
                snprintf(label, sizeof(label), "Muted");
            else
                snprintf(label, sizeof(label), "%d%%", volume.level());
            bridge.set_volume_label(slint::SharedString(label));
            bridge.set_volume_level(volume.level());
            bridge.set_volume_muted(volume.muted());
            if (change)
            {
                volumeBarUntil = mono_ms() + VOLUME_BAR_MS;
                if (!volumeBar)
                {
                    volumeBar = true;
                    bridge.set_volume_bar(true);
                }
            }
        }
        if (volumeBar && due(mono_ms(), volumeBarUntil))
        {
            volumeBar = false;
            bridge.set_volume_bar(false);
        }
        // How the phone is connected: the Home screen's icons.
        const bool linkUsb = ui_bridge::phoneLinkUsb();
        if (linkUsb != pLinkUsb)
        {
            pLinkUsb = linkUsb;
            bridge.set_link_usb(linkUsb);
        }
        const bool linkWifi = ui_bridge::phoneLinkWifi();
        if (linkWifi != pLinkWifi)
        {
            pLinkWifi = linkWifi;
            bridge.set_link_wifi(linkWifi);
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

        // A camera that could not start (or stopped): back to Video, saying why.
        if (std::string why; ui_bridge::takeCameraFailure(why))
        {
            bridge.set_camera_error(slint::SharedString(why));
            if (bridge.get_screen() == 9)
                bridge.set_screen(8);
        }
        if (cameraHint && due(mono_ms(), cameraHintUntil))
        {
            cameraHint = false;
            bridge.set_camera_hint(false);
        }

        const int screen = bridge.get_screen();
        const bool modal = bridge.get_prompt_kind() != 0 || editing >= 0;
        if (screen != pScreen)
        {
            // The loop runs the camera while its view is up.
            ui_bridge::setCameraTest(screen == 9);
            if (screen == 9)
            {
                bridge.set_camera_error("");
                cameraHint = true;
                cameraHintUntil = mono_ms() + CAMERA_HINT_MS;
                bridge.set_camera_hint(true);
            }
#ifdef USE_CEDRUS
            else if (screen == 8 && pScreen != 9)
            {
                camera = Camera::probe(); // opens the device for a moment
                cameraDirty = true;
            }
#endif
        }
        pumpCamera(bridge);
        if (screen != pScreen || modal != pModal)
        {
            focusSeed = true;
            switched = true;
            ringLost = false; // the switch re-seeds the ring anyway
            // A screen that went away does not report losing focus.
            bridge.set_stepper_focus(-1);
            bridge.set_ring_row("");
#ifdef USE_BLUETOOTH
            const bool btNow = screen == 4 || screen == 5;
            const bool btBefore = pScreen == 4 || pScreen == 5;
            if (btNow != btBefore)
                bt::Manager::instance().screenVisible(btNow);
#endif
            pScreen = screen;
            pModal = modal;
        }
#ifdef USE_BLUETOOTH
        pumpBluetooth(bridge, screen);
#endif
        pumpDialog(bridge);
    }

#ifdef USE_BLUETOOTH
    // Bluetooth state -> Bridge, gated on the manager's generation: an idle
    // screen pays one atomic load per tick.
    void pumpBluetooth(const Bridge &bridge, int screen)
    {
        auto &manager = bt::Manager::instance();
        const uint32_t generation = manager.generation();
        if (btSeen && generation == pBtGen)
            return;
        btSeen = true;
        pBtGen = generation;
        const bt::Snapshot s = manager.snapshot();

        syncRows(*btPaired, s.paired);
        syncRows(*btFound, s.found);
        const slint::SharedString ring = bridge.get_ring_row();
        if (ringRowGone(ring, "paired:", s.paired) || ringRowGone(ring, "found:", s.found))
            ringGone(bridge);

        BtRow current;
        bool present = false;
        for (const bt::DeviceRow &d : s.paired)
            if (d.id == btCurrentId)
            {
                current = toRow(d);
                present = true;
            }
        if (!(current == pBtCurrent))
        {
            bridge.set_bt_current(current);
            pBtCurrent = current;
        }
        // The device screen's device was forgotten: back to the list.
        if (screen == 5 && !present)
            bridge.set_screen(4);

        if (s.summary != pBtSummary)
        {
            pBtSummary = s.summary;
            bridge.set_bt_summary(slint::SharedString(pBtSummary));
        }
        if (s.status != pBtStatus)
        {
            pBtStatus = s.status;
            bridge.set_bt_status(slint::SharedString(pBtStatus));
        }
        if (s.scanning != pBtScanning)
        {
            pBtScanning = s.scanning;
            bridge.set_bt_scanning(pBtScanning);
        }
        if (s.scanAllowed != pBtScanAllowed)
        {
            pBtScanAllowed = s.scanAllowed;
            bridge.set_bt_scan_allowed(pBtScanAllowed);
        }
        if (s.phoneConnected != pLinkBt)
        {
            pLinkBt = s.phoneConnected;
            bridge.set_link_bt(pLinkBt);
        }
        btDialog.kind = (int)s.prompt.kind;
        btDialog.title = s.prompt.title;
        btDialog.code = s.prompt.code;
        btDialog.hint = s.prompt.hint;
        btDialog.accept = s.prompt.acceptLabel;
        dialogDirty = true;
    }
#endif

    // Bluetooth's prompt, or else our own question -> Bridge, on change.
    void pumpDialog(const Bridge &bridge)
    {
        if (!dialogDirty)
            return;
        dialogDirty = false;
        DialogText d = btDialog;
        dialogLocal = d.kind == 0 && systemAsk != 0;
        if (dialogLocal)
        {
            const bool reboot = systemAsk == 1;
            d.kind = 2;
            d.title = reboot ? "Reboot the head unit?" : "Shut down the head unit?";
            d.hint = reboot ? "FastCarPlay closes, then the head unit starts again."
                            : "FastCarPlay closes and the head unit stops. It starts again "
                              "when its power is switched off and on.";
            d.accept = reboot ? "Reboot" : "Shut down";
        }
        // Another dialog (or none): it starts without a keyboard ring.
        if (d.kind != pPromptKind || d.title != pPromptTitle)
            bridge.set_prompt_focus(-1);
        if (d.title != pPromptTitle)
        {
            pPromptTitle = d.title;
            bridge.set_prompt_title(slint::SharedString(pPromptTitle));
        }
        if (d.code != pPromptCode)
        {
            pPromptCode = d.code;
            bridge.set_prompt_code(slint::SharedString(pPromptCode));
        }
        if (d.hint != pPromptHint)
        {
            pPromptHint = d.hint;
            bridge.set_prompt_hint(slint::SharedString(pPromptHint));
        }
        if (d.accept != pPromptAccept)
        {
            pPromptAccept = d.accept;
            bridge.set_prompt_accept_label(slint::SharedString(pPromptAccept));
        }
        // Kind last: the dialog appears with its text already in place.
        if (d.kind != pPromptKind)
        {
            pPromptKind = d.kind;
            bridge.set_prompt_kind(d.kind);
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
        // 0 day -> 1 night -> 2 auto -> 0; a running session switches at once.
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%d", (int)((Settings::nightMode + 1) % 3));
        persist("night-mode", buffer, false);
        ui_bridge::nightModeChanged();
    });
    bridge.on_cycle_fps([] {
        persist("aa-video-fps", Settings::aaFps >= 60 ? "30" : "60", true);
    });
    // Applies at once (no restart) and saves itself a moment later.
    bridge.on_volume_step([](int direction) { AudioVolume::instance().step(direction); });
    bridge.on_volume_mute([] { AudioVolume::instance().toggleMute(); });
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
    bridge.on_camera_cycle_input([impl] {
#ifdef USE_CEDRUS
        const int n = (int)impl->camera.inputs.size();
        if (n > 1)
            persist("camera-input", std::to_string((Settings::cameraInput + 1) % n).c_str(), false);
#endif
        impl->cameraDirty = true;
    });
    bridge.on_camera_cycle_standard([impl] {
        persist("camera-standard", CAMERA_STANDARDS[(cameraStandardIndex() + 1) % 3], false);
        impl->cameraDirty = true;
    });
    bridge.on_camera_step([impl](int which, int direction) {
#ifdef USE_CEDRUS
        static const char *const keys[] = {"camera-brightness", "camera-contrast",
                                           "camera-saturation"};
        const Setting<int> *const saved[] = {&Settings::cameraBrightness,
                                             &Settings::cameraContrast,
                                             &Settings::cameraSaturation};
        const int device[] = {impl->camera.brightness, impl->camera.contrast,
                              impl->camera.saturation};
        if (which < 0 || which > 2 || device[which] < 0)
            return;
        const int level =
            std::clamp(Impl::cameraLevel(*saved[which], device[which]) + 5 * direction, 0, 100);
        persist(keys[which], std::to_string(level).c_str(), false);
#else
        (void)which;
        (void)direction;
#endif
        impl->cameraDirty = true;
    });
    bridge.on_system_ask([impl](int what) {
        impl->systemAsk = what;
        impl->dialogDirty = true;
    });
    // The dialog answers whoever asked: our own question, or Bluetooth.
    bridge.on_prompt_accept([impl] {
        if (impl->dialogLocal)
        {
            impl->answerSystem(true);
            return;
        }
#ifdef USE_BLUETOOTH
        bt::Manager::instance().promptAccept();
#endif
    });
    bridge.on_prompt_cancel([impl] {
        if (impl->dialogLocal)
        {
            impl->answerSystem(false);
            return;
        }
#ifdef USE_BLUETOOTH
        bt::Manager::instance().promptCancel();
#endif
    });

#ifdef USE_BLUETOOTH
    // The manager queues every request for its own thread; nothing here blocks.
    bridge.set_bt_available(true);
    bridge.set_bt_paired(impl->btPaired);
    bridge.set_bt_found(impl->btFound);
    bridge.on_bt_scan([](bool on) { bt::Manager::instance().scan(on); });
    bridge.on_bt_pair([](slint::SharedString id) {
        bt::Manager::instance().pair(std::string(std::string_view(id)));
    });
    bridge.on_bt_open([impl](slint::SharedString id) {
        impl->btCurrentId = std::string(std::string_view(id));
        impl->btSeen = false; // re-pump bt-current now
    });
    bridge.on_bt_connect([](slint::SharedString id, bool on) {
        bt::Manager::instance().connect(std::string(std::string_view(id)), on);
    });
    bridge.on_bt_forget([](slint::SharedString id) {
        bt::Manager::instance().forget(std::string(std::string_view(id)));
    });
#endif

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
    // Order matters against tick(): position first, then _pressed, then the
    // catch-up flag. tick() drains _pressSeen before it reads _pressed, so a
    // press can never be seen by the catch-up branch while _pressed still
    // reads false (which would dispatch the tap twice).
    _px = x;
    _py = y;
    _pressed = pressed;
    if (pressed)
        _pressSeen = true;
}

bool SlintOsd::overVideo(int &x, int &y, int &w, int &h)
{
    if (_impl == nullptr || _impl->win == nullptr)
        return false;
    const Impl &impl = *_impl;
    // A change the next tick() has not pumped yet brings the bar up too. The
    // camera view's hint sits in the same place.
    const bool pending =
        impl.volumeSeen && AudioVolume::instance().generation() != impl.pVolumeGen;
    if (!pending && !(impl.volumeBar && !due(mono_ms(), impl.volumeBarUntil)) &&
        !(impl.cameraHint && !due(mono_ms(), impl.cameraHintUntil)))
        return false;
    const auto &bar = impl.ui->global<VolumeBar>();
    x = (int)bar.get_x();
    y = (int)bar.get_y();
    w = (int)bar.get_width();
    h = (int)bar.get_height();
    return true;
}

bool SlintOsd::tick(bool overVideo)
{
    if (_impl == nullptr || _impl->win == nullptr)
        return false;
    Impl &impl = *_impl;

    // The ring's row was removed last tick and is destroyed by now: from
    // nothing focused, Tab lands on the first row.
    if (impl.ringLost)
    {
        impl.ringLost = false;
        impl.dispatchKey(slint::platform::key_codes::Tab, false);
    }

    // Touch -> pointer events, on this thread as Slint requires. A tap that
    // fit entirely between two ticks still delivers a press + release pair.
    // Drain the catch-up flag BEFORE reading _pressed (pointer() writes them
    // in the opposite order), so seen && !down can only mean a completed tap.
    {
        const bool seen = _pressSeen.exchange(false);
        const bool down = _pressed;
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

    // Keys before pump(), so a screen a key switches to is pumped and laid
    // out in this same tick.
    {
        KeyEvent keys[KEY_QUEUE];
        int count;
        {
            std::lock_guard<std::mutex> lock(_keyMutex);
            count = _keyCount;
            std::copy(_keys, _keys + count, keys);
            _keyCount = 0;
        }
        for (int i = 0; i < count; i++)
            impl.applyKey(keys[i].nav, keys[i].ch, keys[i].value);
    }

    impl.pump();
    if (impl.focusSeed)
    {
        impl.focusSeed = false;
        // Tab from nothing focused lands on the first row (Back): the ring is
        // where a keyboard user expects it after every switch.
        if (impl.keyUsed && mono_ms() - impl.lastKeyMs < KEY_RECENT_MS)
            impl.dispatchKey(slint::platform::key_codes::Tab, false);
    }
    slint::platform::update_timers_and_animations();

    // Over video only the bar's window is up: a pending whole-frame push
    // waits for the screens (copying 750 KB here would be wasted work).
    const bool fullPush = impl.fullPush && !overVideo;
    if (!impl.win->dirty && !impl.win->window().has_active_animations() && !fullPush)
        return false;
    impl.win->dirty = false;
    if (fullPush)
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

void SlintOsd::key(OsdKey nav, char ch, int value)
{
    std::lock_guard<std::mutex> lock(_keyMutex);
    // A full queue drops the newest: the UI is not ticking (or not keeping
    // up), and replaying a burst later would only surprise.
    if (_keyCount < KEY_QUEUE)
        _keys[_keyCount++] = {nav, ch, (int8_t)value};
}

void SlintOsd::hidden()
{
    {
        std::lock_guard<std::mutex> lock(_keyMutex);
        _keyCount = 0;
    }
    if (_impl == nullptr)
        return;
#ifdef USE_BLUETOOTH
    // Discovery stops with the screens. An open dialog stays: it is still
    // there to finish when the UI comes back, and BlueZ's own timeouts end
    // it otherwise.
    if (_impl->pScreen == 4 || _impl->pScreen == 5)
        bt::Manager::instance().screenVisible(false);
#endif
    // Our own reboot / shut down question does not wait behind the video:
    // the screens come back without it.
    if (_impl->systemAsk != 0)
        _impl->answerSystem(false);
    // pScreen stays: the screen -- and its focused row -- are still there
    // when the UI comes back, so there is nothing to re-seed.
}

#endif /* USE_SLINT */
