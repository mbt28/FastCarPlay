#include <string>
#include <iostream>
#include <memory>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "common/functions.h"
#include "common/logger.h"

#include "application.h"
#include "autogen/version.h"
#include "protocol/wifi_ap.h"
#include "settings.h"

#ifdef USE_LVGL
#include "ui/ui_bridge.h"
#endif

static const char *title = "Fast Car Play v0.9";

static char **savedArgv = nullptr;

void start()
{
    set_log_level(Settings::loglevel);

    // The access point belongs to the system, so its real configuration wins
    // over anything a preset says. Seeding the mirrors here -- before print()
    // and before any backend starts -- means the UI, the logs and what we hand
    // the phone all come from the same place hostapd was started from.
    {
        const wifi_ap::Params ap = wifi_ap::read();
        if (!ap.ssid.empty())
        {
            Settings::wifiIface.value = ap.iface;
            Settings::wifiSsid.value = ap.ssid;
            Settings::wifiPass.value = ap.passphrase;
            if (ap.channel > 0)
                Settings::wifiChannel.value = ap.channel;
            if (!ap.ip.empty())
                Settings::apIp.value = ap.ip;
            log_i("wifi: system AP '%s' on %s (%s, ch %d)%s", ap.ssid.c_str(),
                  ap.iface.c_str(), ap.ip.empty() ? "no address" : ap.ip.c_str(),
                  ap.channel, ap.valid() ? "" : " -- not up");
        }
    }

    Settings::print();

    Application app;
    app.start(title);
}

// Re-exec ourselves so a source/setting change takes effect. Done here, after
// start() has returned and Application's destructor has released SDL, USB and
// the DRM master -- exec does not unwind the stack, so tearing down first is
// the difference between a clean restart and leaking the display.
// Self-contained: no init script or supervisor needed, so it behaves the same
// on a dev box as it does on the head unit.
static void restartSelf()
{
    if (savedArgv == nullptr)
        return;

    std::cout << "Restarting to apply settings" << std::endl;
    // The device init (S99carplay) launches us once and does NOT respawn, so a
    // failed exec = shutdown. Try hard: the resolved absolute path first (some
    // environments won't exec the /proc/self/exe magic symlink), then the
    // symlink, then the invoked name via PATH.
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0)
    {
        exe[n] = '\0';
        execv(exe, savedArgv);
        std::cerr << "[Main] execv " << exe << " failed > " << strerror(errno) << std::endl;
    }
    execv("/proc/self/exe", savedArgv);
    std::cerr << "[Main] execv /proc/self/exe failed > " << strerror(errno) << std::endl;
    execvp(savedArgv[0], savedArgv);
    std::cerr << "[Main] execvp " << savedArgv[0] << " failed > " << strerror(errno) << std::endl;
}

int main(int argc, char **argv)
{
    savedArgv = argv;

    // Answer "what is actually running?" without starting anything. After a
    // partly-applied update this is not the same as what /etc claims, which is
    // exactly when the question gets asked.
    if (argc == 2 && (std::string(argv[1]) == "--version" || std::string(argv[1]) == "-v"))
    {
        std::cout << title << "\nversion " << FCP_VERSION << "\nbuild " << FCP_BUILD << std::endl;
        return 0;
    }

    std::cout << title << std::endl;
    if (argc > 2)
    {
        std::cerr << "  Usage: " << argv[0] << " [settings_file | --version]" << std::endl;
        return 0;
    }
    try
    {
        if (argc == 2 && !Settings::load(argv[1]))
            return 1;

        // User overrides last, so anything changed from the on-device UI wins
        // over the shipped preset (which an image update may rewrite).
        Settings::loadUser();

        start();

#ifdef USE_LVGL
        // The UI persisted the change before asking to restart, so the new
        // process reads it back from usersettings.txt on the way up.
        // Not in the UI harness: it exists to iterate on screens, and with a
        // scripted click it would re-select and restart forever.
        if (ui_bridge::restartRequested() && !Settings::lvglTest)
            restartSelf();
#endif
    }
    catch (const std::exception &e)
    {
        std::cerr << "[Main] Error > " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
