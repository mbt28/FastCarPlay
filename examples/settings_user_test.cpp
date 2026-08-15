// Round-trip test for the user-settings override layer.
//
// The shipped presets (settings_cedrus_aa.txt etc.) are installed from this
// repo by the buildroot package, so an image update rewrites them. Anything
// the on-device UI changes must therefore land in
// $HOME/.fastcarplay/usersettings.txt and be applied *after* the preset.
// This checks that contract, plus that the writer preserves unrelated keys
// and rejects unknown ones.
//
//   make settings_user_test && ../out/settings_user_test

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "settings.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main()
{
    // Sandbox: userPath() is derived from HOME, so point it at a temp dir.
    char tmpl[] = "/tmp/fcp-usersettings-XXXXXX";
    const char *home = mkdtemp(tmpl);
    if (home == nullptr)
    {
        perror("mkdtemp");
        return 1;
    }
    setenv("HOME", home, 1);

    const std::string path = Settings::userPath();
    printf("user settings path: %s\n\n", path.c_str());
    check(path == std::string(home) + "/.fastcarplay/usersettings.txt",
          "userPath() is $HOME/.fastcarplay/usersettings.txt");

    printf("\nmissing file is not an error:\n");
    check(!Settings::loadUser(), "loadUser() returns false when absent");

    printf("\nwrite + apply:\n");
    Settings::protocol.value = "carlinkit";
    check(Settings::setUser("protocol", "aa-usb"), "setUser(protocol, aa-usb)");
    check(Settings::protocol.value == "aa-usb", "applied to the live setting");
    check(!slurp(path).empty(), "file written");

    printf("\nunknown keys are rejected (never reach the file):\n");
    check(!Settings::setUser("not-a-real-setting", "1"), "setUser() returns false");
    check(slurp(path).find("not-a-real-setting") == std::string::npos,
          "key absent from the file");

    printf("\nunrelated overrides survive a later write:\n");
    check(Settings::setUser("night-mode", "2"), "setUser(night-mode, 2)");
    check(Settings::setUser("protocol", "aa-wireless"), "setUser(protocol, aa-wireless)");
    const std::string body = slurp(path);
    check(body.find("night-mode") != std::string::npos, "night-mode still present");
    check(body.find("aa-wireless") != std::string::npos, "protocol updated");
    check(body.find("aa-usb") == std::string::npos, "old value replaced, not appended");

    printf("\noverride wins over the preset:\n");
    Settings::protocol.value = "carlinkit"; // as if a preset had just loaded
    check(Settings::loadUser(), "loadUser() returns true");
    check(Settings::protocol.value == "aa-wireless", "override applied over preset");

    // A value that cannot be parsed used to be reported as saved and written to
    // the file anyway, so it failed to parse again at every subsequent boot and
    // the setting silently never took effect.
    printf("\nunparseable values are rejected (never reach the file):\n");
    Settings::aaFps.value = 30;
    check(!Settings::setUser("aa-video-fps", "sixty"), "setUser() returns false");
    check(Settings::aaFps.value == 30, "live value left untouched");
    check(slurp(path).find("sixty") == std::string::npos, "value absent from the file");
    check(!Settings::setUser("cursor", "yes"), "bad bool rejected");
    check(!Settings::setUser("aa-video-fps", "60fps"), "trailing junk rejected");
    check(!Settings::setUser("aspect-correction", "1.0x"), "bad float rejected");
    check(Settings::setUser("aspect-correction", "1.25"), "good float accepted");

    printf("\nthe saved file still parses after a rejected write:\n");
    Settings::aaFps.value = 30;
    check(Settings::loadUser(), "loadUser() returns true");
    check(Settings::aaFps.value == 30, "aa-video-fps unchanged by the rejected write");

    // load() has always accepted an alias; setUser() used to compare against
    // name only, so a key readable from a preset was not writable from the UI.
    printf("\naliases are writable, and fold onto the canonical name:\n");
    Settings::aaResolution.value = 1;
    check(Settings::setUser("android-resolution", "2"), "setUser(alias) accepted");
    check(Settings::aaResolution.value == 2, "applied to the live setting");
    const std::string aliased = slurp(path);
    check(aliased.find("aa-resolution = 2") != std::string::npos, "stored canonically");
    check(aliased.find("android-resolution") == std::string::npos,
          "alias spelling not left behind as a duplicate");

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
