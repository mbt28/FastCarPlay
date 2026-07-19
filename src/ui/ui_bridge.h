#ifndef SRC_UI_UI_BRIDGE
#define SRC_UI_UI_BRIDGE

// The contract between the EEZ Studio designed screens and the application.
//
// Screens reference a small, stable set of names:
//   native variables  protocol (int), status (string)   -- app -> UI
//   native actions    use_aa_usb / use_aa_wireless / use_dongle  -- UI -> app
//
// EEZ generates the declarations into src/ui/generated/vars.h and wires them
// into native_vars[] / actions[]; this file is the only place they are
// implemented. Anyone can restyle or relayout the screens without touching
// C++, as long as they keep using these names.

#ifdef USE_LVGL

#include <cstdint>

#include <lvgl/lvgl.h>

// The generated vars.h/actions.h declare these too; keeping one copy here
// means the built-in picker and the EEZ screens cannot drift apart. Types
// must match what EEZ emits: integer -> int32_t, string -> const char *.
extern "C"
{
    int32_t get_var_protocol();
    void set_var_protocol(int32_t value);
    const char *get_var_status();
    void set_var_status(const char *value);

    void action_use_aa_usb(lv_event_t *e);
    void action_use_aa_wireless(lv_event_t *e);
    void action_use_dongle(lv_event_t *e);
}

namespace ui_bridge
{
// Protocol ids as referenced by the designed screens. 3/4 are reserved for
// CarPlay so adding it later does not renumber the existing entries.
enum Protocol
{
    PROTOCOL_CARLINKIT = 0,
    PROTOCOL_AA_USB = 1,
    PROTOCOL_AA_WIRELESS = 2,
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

#endif /* USE_LVGL */
#endif /* SRC_UI_UI_BRIDGE */
