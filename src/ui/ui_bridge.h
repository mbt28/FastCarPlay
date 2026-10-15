#ifndef SRC_UI_UI_BRIDGE
#define SRC_UI_UI_BRIDGE

// The contract between the built-in screens and the application: a small,
// stable set of names (protocol, status, resume/restart/backgrounded state)
// that the UI toolkit of the day renders however it likes. The screens can
// be restyled or relaid out without touching the application, as long as
// they keep using these names.

#ifdef USE_SLINT

#include <cstdint>
#include <string>

// Plain-C variable surface the screens are wired to:
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
// the loops exit and main re-execs the binary (S99carplay does not respawn).
bool restartRequested();

// A connected-but-backgrounded session is waiting behind the UI, so the
// picker can offer a way back into it.
void setBackgroundedSession(bool waiting);
bool backgroundedSession();
// Set by the "Resume" row; the app consumes it and asks the phone to project.
void requestResume();
bool takeResumeRequest();
// Set when the Night setting changes; the app tells a running session.
void nightModeChanged();
bool takeNightModeChange();

// How the phone reaches us right now -- a session over USB or over Wi-Fi --
// for the home screen's icons (Bluetooth comes from the Bluetooth manager).
void setPhoneLink(bool usb, bool wifi);
bool phoneLinkUsb();
bool phoneLinkWifi();

// Settings > Video > Test camera: the screens ask for the camera and the
// loop runs it; a camera that cannot start (or stops) says why, and the
// screens go back to Video showing it.
void setCameraTest(bool on);
bool cameraTest();
void cameraFailed(const std::string &why);
bool takeCameraFailure(std::string &why);

// Settings > System: reboot or shut down the head unit. The loops exit and
// main runs the system's command (reboot-command / shutdown-command).
enum class Power
{
    None,
    Reboot,
    Shutdown,
};
void requestPower(Power action);
Power powerRequested();

// A change is staged and needs a restart. Nothing restarts under the driver:
// the settings header says so and an explicit "Restart now" appears.
void setRestartNeeded();
bool restartNeeded();

// Short label for the current source, for the home header and the Source row.
const char *protocolName();
void requestRestart();
} // namespace ui_bridge

#endif /* USE_SLINT */
#endif /* SRC_UI_UI_BRIDGE */
