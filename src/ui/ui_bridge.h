#ifndef SRC_UI_UI_BRIDGE
#define SRC_UI_UI_BRIDGE

// The contract between the built-in screens and the application: a small,
// stable set of names (protocol, status, resume/restart/backgrounded state)
// that the UI toolkit of the day renders however it likes. The screens can
// be restyled or relaid out without touching the application, as long as
// they keep using these names.

#if defined(USE_LVGL) || defined(USE_SLINT)

#include <cstdint>

// Plain-C surface shared by both UI toolkits (and once by the EEZ screens):
// integer -> int32_t, string -> const char *.
extern "C"
{
    int32_t get_var_protocol();
    void set_var_protocol(int32_t value);
    const char *get_var_status();
    void set_var_status(const char *value);
}

namespace ui_bridge
{
// Protocol ids as referenced by the screens.
enum Protocol
{
    PROTOCOL_AA_USB = 1,
    PROTOCOL_AA_WIRELESS = 2,
    PROTOCOL_CARPLAY_WIRED = 3,    // wired CarPlay over USB (config-6 / carkit)
    PROTOCOL_CARPLAY_WIRELESS = 4, // wireless CarPlay (Wi-Fi AP + Bluetooth)
};

// Status line shown on the picker (connection state, phone name...).
void setStatus(const char *status);

// Set when a change has been persisted and needs a restart to take effect;
// the loops exit so the init script (S99carplay) respawns us.
bool restartRequested();

// A connected-but-backgrounded session is waiting behind the UI, so the
// picker can offer a way back into it.
void setBackgroundedSession(bool waiting);
bool backgroundedSession();
// Set by the "Resume" row; the app consumes it and asks the phone to project.
void requestResume();
bool takeResumeRequest();

// A change is staged and needs a restart. Nothing restarts under the driver:
// the settings header says so and an explicit "Restart now" appears.
void setRestartNeeded();
bool restartNeeded();

// Short label for the current source, for the home header and the Source row.
const char *protocolName();
void requestRestart();
} // namespace ui_bridge

#endif /* USE_LVGL || USE_SLINT */
#endif /* SRC_UI_UI_BRIDGE */
