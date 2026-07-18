#include <string>
#include <iostream>
#include <memory>
#include <cerrno>
#include <cstring>
#include <unistd.h>

#include "common/functions.h"
#include "common/logger.h"

#include "application.h"
#include "settings.h"

#ifdef USE_LVGL
#include "ui/ui_bridge.h"
#endif

static const char *title = "Fast Car Play v0.9";

static char **savedArgv = nullptr;

void start()
{
    set_log_level(Settings::loglevel);
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
    execv("/proc/self/exe", savedArgv);
    // Only reached if exec failed; the caller then exits normally and a
    // supervisor (S99carplay) can still bring us back.
    std::cerr << "[Main] Restart failed > " << strerror(errno) << std::endl;
}

int main(int argc, char **argv)
{
    savedArgv = argv;
    std::cout << title << std::endl;
    if (argc > 2)
    {
        std::cerr << "  Usage: " << argv[0] << " [settings_file]" << std::endl;
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
