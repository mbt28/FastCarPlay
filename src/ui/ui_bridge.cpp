#include "ui_bridge.h"

#ifdef USE_LVGL

#include <atomic>
#include <string>

#include <lvgl/lvgl.h>

#include "common/logger.h"
#include "settings.h"

namespace
{
std::atomic<bool> g_restart{false};
std::atomic<bool> g_backgrounded{false};
std::atomic<bool> g_resume{false};
std::atomic<bool> g_restartNeeded{false};
std::string g_status = "";

// Protocol id <-> the "protocol" setting string. One table, so the picker,
// the setting and any future source all stay in step.
struct ProtocolEntry
{
    int id;
    const char *name;  // settings value
    const char *label; // short label -- the driver is scanning, not reading
};

const ProtocolEntry PROTOCOLS[] = {
    {ui_bridge::PROTOCOL_CARLINKIT, "carlinkit", "Dongle"},
    {ui_bridge::PROTOCOL_AA_USB, "aa-usb", "AA Wired"},
    {ui_bridge::PROTOCOL_AA_WIRELESS, "aa-wireless", "AA Wireless"},
    {ui_bridge::PROTOCOL_CARPLAY_WIRED, "carplay-wired", "CarPlay Wired"},
    {ui_bridge::PROTOCOL_CARPLAY_WIRELESS, "carplay-wireless", "CarPlay Wireless"},
};

// Persist the choice and ask for a restart. Writes to
// $HOME/.fastcarplay/usersettings.txt -- never the shipped preset, which an
// image update would overwrite.
void selectProtocol(int id)
{
    for (const ProtocolEntry &entry : PROTOCOLS)
    {
        if (entry.id != id)
            continue;

        if (Settings::protocol.value == entry.name)
        {
            log_v("Source already %s", entry.name);
            return;
        }
        if (!Settings::setUser("protocol", entry.name))
        {
            ui_bridge::setStatus("Could not save");
            return;
        }

        // Staged, not applied: a live session must never be dropped by a
        // stray tap. The user restarts when they are ready.
        log_i("Source -> %s (restart to apply)", entry.name);
        g_restartNeeded = true;
        return;
    }
    log_e("Unknown protocol id %d", id);
}
} // namespace

namespace ui_bridge
{
void setStatus(const char *status)
{
    g_status = status != nullptr ? status : "";
}

bool restartRequested()
{
    return g_restart;
}

void setBackgroundedSession(bool waiting)
{
    g_backgrounded = waiting;
}

bool backgroundedSession()
{
    return g_backgrounded;
}

void requestResume()
{
    g_resume = true;
}

bool takeResumeRequest()
{
    return g_resume.exchange(false);
}

void setRestartNeeded()
{
    g_restartNeeded = true;
}

bool restartNeeded()
{
    return g_restartNeeded;
}

const char *protocolName()
{
    for (const ProtocolEntry &entry : PROTOCOLS)
        if (Settings::protocol.value == entry.name)
            return entry.label;
    return "None";
}

void requestRestart()
{
    setStatus("Restarting...");
    g_restart = true;
}
} // namespace ui_bridge

// ---------------------------------------------------------------------------
// Native variables. Signatures must match what EEZ generates into vars.h:
// integer -> int32_t, boolean -> bool, string -> const char *.
// ---------------------------------------------------------------------------

extern "C" int32_t get_var_protocol()
{
    for (const ProtocolEntry &entry : PROTOCOLS)
        if (Settings::protocol.value == entry.name)
            return entry.id;
    return ui_bridge::PROTOCOL_CARLINKIT;
}

extern "C" void set_var_protocol(int32_t value)
{
    selectProtocol((int)value);
}

extern "C" const char *get_var_status()
{
    return g_status.c_str();
}

extern "C" void set_var_status(const char *value)
{
    ui_bridge::setStatus(value);
}

// ---------------------------------------------------------------------------
// Native actions. ActionExecFunc is void(lv_event_t *), so each source gets
// its own action rather than one taking an id.
// ---------------------------------------------------------------------------

extern "C" void action_use_aa_usb(lv_event_t *e)
{
    (void)e;
    selectProtocol(ui_bridge::PROTOCOL_AA_USB);
}

extern "C" void action_use_aa_wireless(lv_event_t *e)
{
    (void)e;
    selectProtocol(ui_bridge::PROTOCOL_AA_WIRELESS);
}

extern "C" void action_use_dongle(lv_event_t *e)
{
    (void)e;
    selectProtocol(ui_bridge::PROTOCOL_CARLINKIT);
}

#endif /* USE_LVGL */
