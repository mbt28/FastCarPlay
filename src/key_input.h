#ifndef SRC_KEY_INPUT
#define SRC_KEY_INPUT

// Bluetooth keyboards and remotes as app input. Reads every evdev node that a
// Bluetooth HID device creates (bus BUS_BLUETOOTH, via BlueZ's uhid), grabbed
// exclusively so no other kernel handler sees the keys (Magic SysRq would
// otherwise let Alt+SysRq+B on a paired keyboard reboot the head unit).
// Hotplug without udev: inotify on /dev/input.
//
// Keys go to the phone as the app's CMD_CONTROL BTN_* codes (the same path as
// the serial test reader), or -- while the on-screen UI is up -- to the UI as
// OsdKey navigation plus typed characters. Like TouchInput, a key belongs to
// the route it was pressed on, so its release (and a held select) never
// straddles a route change.

#ifdef __linux__

#include <atomic>
#include <functional>
#include <thread>

#include "ui/osd_key.h"

class IConnection;

class KeyInput
{
public:
    explicit KeyInput(IConnection &conn);
    ~KeyInput();

    // UI sink, called on the reader thread: navigation key (None when the key
    // only types), the typed ASCII character after Shift/CapsLock (0 if none),
    // and the evdev value (1 press, 2 repeat, 0 release). Set it once, before
    // routeToUi(true) is first called.
    void setUiSink(std::function<void(OsdKey key, char ch, int value)> sink);
    // The loop flips this exactly where touch routing flips.
    void routeToUi(bool active);

private:
    struct Impl;
    Impl *_impl;
};

#endif /* __linux__ */
#endif /* SRC_KEY_INPUT */
