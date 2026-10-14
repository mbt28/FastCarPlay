#include "key_input.h"

#ifdef __linux__

#include <linux/input.h>
// linux/input.h defines BTN_LEFT/RIGHT/BACK as evdev codes that collide with
// the app button ids in protocol_const.h; drop them before those are defined.
#undef BTN_LEFT
#undef BTN_RIGHT
#undef BTN_BACK
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "audio_volume.h"
#include "common/clock.h"
#include "common/logger.h"
#include "common/threading.h"
#include "protocol/iconnection.h"
#include "protocol/message.h"
#include "protocol/protocol_const.h"

namespace
{
constexpr int LONG_BITS = 8 * sizeof(long);

// Repeats are synthesised here instead of forwarding the kernel's (250 ms,
// then every 33 ms): at 30 events/s a held arrow would flood the phone's
// write queue, which Android Auto shares with video acknowledgements.
constexpr uint32_t REPEAT_DELAY_MS = 400;
constexpr uint32_t REPEAT_PHONE_MS = 150;
constexpr uint32_t REPEAT_UI_MS = 120;
constexpr uint32_t REPEAT_VOLUME_MS = 150;
// A held volume key repeats this long at most (0 to 100 % takes ~3 s); then
// it needs a fresh press.
constexpr uint32_t VOLUME_REPEAT_FOR_MS = 4000;
// A wheel's turns reach the phone one detent per report, as CarPlay and
// Android Auto expect, but no faster than this: a fast spin must not flood
// that same write queue. A spin ahead of the pace keeps at most
// MAX_TURNS_PENDING steps; the rest of it is dropped.
constexpr uint32_t TURN_PHONE_MS = 30;
constexpr int MAX_TURNS_PENDING = 4;

// devtmpfs creates the node before it is always openable.
constexpr uint32_t OPEN_RETRY_MS = 100;
constexpr uint32_t OPEN_RETRY_FOR_MS = 2000;

constexpr uint32_t DROP_LOG_MS = 10000;

constexpr const char *INPUT_DIR = "/dev/input";

bool testBit(const unsigned long *bits, int bit)
{
    return (bits[bit / LONG_BITS] >> (bit % LONG_BITS)) & 1UL;
}

// Gamepad buttons and d-pads act as the keyboard keys they stand for.
int normalize(int code)
{
    switch (code)
    {
    case BTN_DPAD_UP:
        return KEY_UP;
    case BTN_DPAD_DOWN:
        return KEY_DOWN;
    case BTN_DPAD_LEFT:
        return KEY_LEFT;
    case BTN_DPAD_RIGHT:
        return KEY_RIGHT;
    case BTN_SOUTH:
        return KEY_ENTER;
    case BTN_EAST:
        return KEY_ESC;
    case BTN_START:
    case BTN_MODE:
        return KEY_HOMEPAGE;
    case BTN_MIDDLE: // a wheel's click: the rotary knob's press
        return KEY_ENTER;
    default:
        return code;
    }
}

// evdev key -> the app's CMD_CONTROL button (0: nothing for the phone).
// Never pass evdev codes through: they overlap the BTN ids numerically
// (KEY_MUTE == BTN_UP, KEY_RIGHT == BTN_BACK, ...). Typing keys -- letters,
// digits, Space, Backspace -- are never shortcuts: in a phone's search field
// they would select or go back instead of typing.
int phoneButton(int code)
{
    switch (code)
    {
    case KEY_UP:
        return BTN_UP;
    case KEY_DOWN:
        return BTN_DOWN;
    case KEY_LEFT:
        return BTN_LEFT;
    case KEY_RIGHT:
        return BTN_RIGHT;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_OK:
    case KEY_SELECT:
        return BTN_SELECT_DOWN;
    case KEY_ESC:
    case KEY_BACK:
        return BTN_BACK;
    case KEY_HOME:
    case KEY_HOMEPAGE:
        return BTN_HOME;
    case KEY_PLAYPAUSE:
        return BTN_203;
    case KEY_PLAY:
    case KEY_PLAYCD:
        return BTN_PLAY;
    case KEY_PAUSE:
    case KEY_PAUSECD:
        return BTN_PAUSE;
    case KEY_NEXTSONG:
        return BTN_NEXT_TRACK;
    case KEY_PREVIOUSSONG:
        return BTN_PREVIOUS_TRACK;
    case KEY_VOICECOMMAND:
    case KEY_SEARCH:
    case KEY_ASSISTANT:
        return BTN_SIRI;
    default:
        return 0;
    }
}

// Volume keys set the head unit's own volume: +1 up, -1 down, 2 mute.
int volumeKey(int code)
{
    switch (code)
    {
    case KEY_VOLUMEUP:
        return 1;
    case KEY_VOLUMEDOWN:
        return -1;
    case KEY_MUTE:
        return 2;
    default:
        return 0;
    }
}

// Media and voice keys always reach the phone, even while a screen is up.
bool phoneOnly(int code)
{
    switch (code)
    {
    case KEY_PLAYPAUSE:
    case KEY_PLAY:
    case KEY_PLAYCD:
    case KEY_PAUSE:
    case KEY_PAUSECD:
    case KEY_NEXTSONG:
    case KEY_PREVIOUSSONG:
    case KEY_VOICECOMMAND:
    case KEY_SEARCH:
    case KEY_ASSISTANT:
        return true;
    default:
        return false;
    }
}

OsdKey uiKey(int code)
{
    switch (code)
    {
    case KEY_UP:
        return OsdKey::Up;
    case KEY_DOWN:
        return OsdKey::Down;
    case KEY_LEFT:
        return OsdKey::Left;
    case KEY_RIGHT:
        return OsdKey::Right;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_OK:
    case KEY_SELECT:
    case KEY_SPACE:
        return OsdKey::Ok;
    case KEY_ESC:
    case KEY_BACK:
        return OsdKey::Back;
    case KEY_BACKSPACE:
        return OsdKey::Erase;
    case KEY_HOME:
    case KEY_HOMEPAGE:
        return OsdKey::Home;
    default:
        return OsdKey::None;
    }
}

// US layout, indexed by evdev code KEY_ESC (1) .. KEY_SPACE (57).
constexpr char US_PLAIN[] = "\0\0"
                            "1234567890-="
                            "\0\0"
                            "qwertyuiop[]"
                            "\0\0"
                            "asdfghjkl;'`"
                            "\0"
                            "\\zxcvbnm,./"
                            "\0*\0 ";
constexpr char US_SHIFT[] = "\0\0"
                            "!@#$%^&*()_+"
                            "\0\0"
                            "QWERTYUIOP{}"
                            "\0\0"
                            "ASDFGHJKL:\"~"
                            "\0"
                            "|ZXCVBNM<>?"
                            "\0*\0 ";
static_assert(sizeof(US_PLAIN) == 59 && sizeof(US_SHIFT) == 59, "evdev 0..57 + NUL");

char typedChar(int code, bool shift, bool caps)
{
    if (code < 0 || code > KEY_SPACE)
        return 0;
    const char plain = US_PLAIN[code];
    if (plain >= 'a' && plain <= 'z')
        return (shift != caps) ? (char)(plain - 'a' + 'A') : plain;
    return shift ? US_SHIFT[code] : plain;
}

// Does the device report at least one key this reader acts on?
bool hasUsefulKeys(int fd)
{
    unsigned long keys[(KEY_CNT + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
        return false;
    for (int code = 1; code < KEY_CNT; code++)
        if (testBit(keys, code) && (phoneButton(normalize(code)) != 0 ||
                                    uiKey(normalize(code)) != OsdKey::None ||
                                    typedChar(code, false, false) != 0 || volumeKey(code) != 0))
            return true;
    return false;
}

bool hasHat(int fd)
{
    unsigned long abs[(ABS_CNT + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs) < 0)
        return false;
    return testBit(abs, ABS_HAT0X) || testBit(abs, ABS_HAT0Y);
}

// A scroll wheel: used as a rotary knob. The kernel reports REL_WHEEL in
// detents, high-resolution wheels included. Dials (REL_DIAL) are not read:
// their counts are not detents (a Surface Dial makes 3600 a turn).
bool hasWheel(int fd)
{
    unsigned long rel[(REL_CNT + LONG_BITS - 1) / LONG_BITS] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) < 0)
        return false;
    return testBit(rel, REL_WHEEL);
}

bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}
} // namespace

struct KeyInput::Impl
{
    explicit Impl(IConnection &c) : conn(c) {}

    struct Device
    {
        int fd = -1;
        std::string node;
        bool shiftL = false, shiftR = false, caps = false;
        uint8_t otherMods = 0; // Ctrl / Alt / Meta held: no typed characters
        int hatX = 0, hatY = 0;
        bool dropped = false; // SYN_DROPPED seen: skipping to the next SYN_REPORT
        bool dropLogged = false;
        uint32_t dropLogMs = 0; // a flooding device logs once per DROP_LOG_MS
    };

    // A pressed key, remembered with the route it was pressed on: its
    // release (and its repeats) go to that same route.
    struct Held
    {
        int fd;
        int code;
        bool toUi;
        OsdKey nav;
        char ch;
        int button;         // phone route: the BTN sent on press
        bool selectToPhone; // BTN_SELECT_DOWN went out: needs its UP
        uint32_t nextRepeat;
        uint32_t period;    // 0: no repeat
        int8_t volume = 0;  // a held volume key: +1 / -1, on no route
        uint32_t until = 0; // a volume repeat stops by itself here
    };

    struct Retry
    {
        std::string name;
        uint32_t next;
        uint32_t until;
    };

    IConnection &conn;
    std::function<void(OsdKey, char, int)> sink;
    std::atomic<bool> toUi{false};
    std::atomic<bool> running{false};
    int wakeFd = -1;
    int inotifyFd = -1;
    int devWd = -1;   // /dev: for /dev/input coming back
    int inputWd = -1; // /dev/input: nodes coming and going
    std::thread thread;

    // Reader-thread state only.
    std::vector<Device> devices;
    std::vector<Held> held;
    std::vector<Retry> retries;
    bool routedToUi = false;
    int turnsPending = 0;  // wheel steps owed to the phone: > 0 next, < 0 previous
    uint32_t lastTurn = 0; // when the last one went

    // False when the button did not go out (no session, or a full queue).
    bool sendPhone(int button)
    {
        return conn.state() == PROTOCOL_STATUS_CONNECTED && conn.send(Message::Control(button));
    }

    // Terminal release for held keys (all, or one device's): a select held on
    // the phone is let go there, a key held on a screen gets its release.
    // Their own later releases then find nothing and are swallowed. A held
    // volume key belongs to no route: a route change (keepVolume) leaves it.
    void releaseHeld(int fd, bool keepVolume = false)
    {
        for (size_t i = held.size(); i-- > 0;)
        {
            const Held &h = held[i];
            if ((fd >= 0 && h.fd != fd) || (keepVolume && h.volume))
                continue;
            if (h.toUi && sink)
                sink(h.nav, h.ch, 0);
            else if (h.selectToPhone)
                sendPhone(BTN_SELECT_UP);
            held.erase(held.begin() + (long)i);
        }
    }

    void checkRoute()
    {
        const bool now = toUi.load(std::memory_order_acquire) && sink;
        if (now == routedToUi)
            return;
        routedToUi = now;
        releaseHeld(-1, true);
        turnsPending = 0;
    }

    void press(Device &dev, int code)
    {
        const uint32_t now = mono_ms();
        if (phoneOnly(code))
        {
            sendPhone(phoneButton(code));
            return;
        }
        // A second press without a release in between: the old entry must
        // not outlive this one (and keep repeating).
        release(dev, code);
        if (const int v = volumeKey(code))
        {
            // The head unit's volume, whatever is on screen; up/down repeat.
            // Any volume key first stops a repeat still running -- from a
            // key whose release got lost (link loss, a remote that drops
            // key-ups), which would otherwise drive the volume to its end.
            for (size_t i = held.size(); i-- > 0;)
                if (held[i].volume)
                    held.erase(held.begin() + (long)i);
            if (v == 2)
            {
                AudioVolume::instance().toggleMute();
                return;
            }
            AudioVolume::instance().step(v);
            Held h{dev.fd, code, false, OsdKey::None, 0, 0, false, now + REPEAT_DELAY_MS,
                   REPEAT_VOLUME_MS};
            h.volume = (int8_t)v;
            h.until = now + VOLUME_REPEAT_FOR_MS;
            held.push_back(h);
            return;
        }
        Held h{dev.fd, code, false, OsdKey::None, 0, 0, false, 0, 0};
        if (routedToUi)
        {
            const bool shift = dev.shiftL || dev.shiftR;
            h.toUi = true;
            h.nav = uiKey(code);
            h.ch = dev.otherMods ? 0 : typedChar(code, shift, dev.caps);
            if (h.nav == OsdKey::None && h.ch == 0)
                return;
            sink(h.nav, h.ch, 1);
            // Arrows repeat (the screens ignore a repeated Left = Back; the
            // editor's keyboard ring wants them), and so does Backspace.
            if (h.nav == OsdKey::Up || h.nav == OsdKey::Down || h.nav == OsdKey::Left ||
                h.nav == OsdKey::Right || h.nav == OsdKey::Erase)
                h.period = REPEAT_UI_MS;
        }
        else
        {
            h.button = phoneButton(code);
            if (h.button == 0)
                return;
            // One state read for both: an UP is owed exactly when the DOWN went out.
            const bool sent = sendPhone(h.button);
            h.selectToPhone = h.button == BTN_SELECT_DOWN && sent;
            if (h.button == BTN_UP || h.button == BTN_DOWN || h.button == BTN_LEFT ||
                h.button == BTN_RIGHT)
                h.period = REPEAT_PHONE_MS;
        }
        if (h.period)
            h.nextRepeat = now + REPEAT_DELAY_MS;
        held.push_back(h);
    }

    void release(Device &dev, int code)
    {
        for (size_t i = 0; i < held.size(); i++)
        {
            const Held &h = held[i];
            if (h.fd != dev.fd || h.code != code)
                continue;
            if (h.toUi && sink)
                sink(h.nav, h.ch, 0);
            else if (h.selectToPhone)
                sendPhone(BTN_SELECT_UP);
            held.erase(held.begin() + (long)i);
            return;
        }
    }

    void onKey(Device &dev, int code, int value)
    {
        switch (code)
        {
        case KEY_LEFTSHIFT:
            dev.shiftL = value != 0;
            return;
        case KEY_RIGHTSHIFT:
            dev.shiftR = value != 0;
            return;
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
        {
            const uint8_t bit = (uint8_t)(1u << (code == KEY_LEFTCTRL    ? 0
                                                 : code == KEY_RIGHTCTRL ? 1
                                                 : code == KEY_LEFTALT   ? 2
                                                 : code == KEY_RIGHTALT  ? 3
                                                 : code == KEY_LEFTMETA  ? 4
                                                                         : 5));
            if (value)
                dev.otherMods |= bit;
            else
                dev.otherMods &= (uint8_t)~bit;
            return;
        }
        case KEY_CAPSLOCK:
            if (value == 1)
                dev.caps = !dev.caps;
            return;
        default:
            break;
        }
        if (value == 2)
            return; // kernel autorepeat: ours is throttled (see REPEAT_*)
        code = normalize(code);
        if (value)
            press(dev, code);
        else
            release(dev, code);
    }

    // Hat switches (most gamepad d-pads) as arrow key presses/releases.
    void onHat(Device &dev, int axis, int value)
    {
        int &cur = axis == ABS_HAT0X ? dev.hatX : dev.hatY;
        const int dir = value < 0 ? -1 : value > 0 ? 1 : 0;
        if (dir == cur)
            return;
        const int negative = axis == ABS_HAT0X ? KEY_LEFT : KEY_UP;
        const int positive = axis == ABS_HAT0X ? KEY_RIGHT : KEY_DOWN;
        if (cur != 0)
            release(dev, cur < 0 ? negative : positive);
        cur = dir;
        if (dir != 0)
            press(dev, dir < 0 ? negative : positive);
    }

    // A wheel as a rotary knob, one step per detent. It counts away from the
    // user as positive: "previous", like scrolling up.
    void onRotate(int value)
    {
        const int steps = std::clamp(-value, -8, 8); // > 0: next; a fast spin, in one event
        if (routedToUi)
        {
            for (int i = 0; i < std::abs(steps); i++)
                sink(steps > 0 ? OsdKey::Next : OsdKey::Prev, 0, 1);
            return;
        }
        // A turn the other way drops what is still owed.
        if (turnsPending != 0 && (steps > 0) != (turnsPending > 0))
            turnsPending = 0;
        turnsPending = std::clamp(turnsPending + steps, -MAX_TURNS_PENDING, MAX_TURNS_PENDING);
        sendTurn(mono_ms());
    }

    // One owed wheel step to the phone, once the pace allows it.
    void sendTurn(uint32_t now)
    {
        if (turnsPending == 0 || now - lastTurn < TURN_PHONE_MS)
            return;
        sendPhone(turnsPending > 0 ? BTN_ROTATE_NEXT : BTN_ROTATE_PREV);
        turnsPending += turnsPending > 0 ? -1 : 1;
        lastTurn = now;
    }

    // Returns false when the node could not be opened (yet): worth a retry.
    bool tryOpen(const std::string &name)
    {
        const std::string node = std::string(INPUT_DIR) + "/" + name;
        for (const Device &d : devices)
            if (d.node == node)
                return true;
        const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            return errno == ENOENT; // already gone: nothing to retry
        // Only Bluetooth HID devices: the touchscreen (I2C) also reports keys.
        struct input_id id;
        memset(&id, 0, sizeof(id));
        if (ioctl(fd, EVIOCGID, &id) < 0 || id.bustype != BUS_BLUETOOTH ||
            !(hasUsefulKeys(fd) || hasHat(fd) || hasWheel(fd)))
        {
            ::close(fd);
            return true;
        }
        // Exclusive: no other handler sees these keys. Without it, Magic
        // SysRq (enabled on the image) would let Alt+SysRq+B on a paired
        // keyboard reboot the head unit.
        if (ioctl(fd, EVIOCGRAB, 1) < 0)
            log_w("key-input: %s: no exclusive grab (%s)", node.c_str(), strerror(errno));
        char devName[96] = "?";
        ioctl(fd, EVIOCGNAME(sizeof(devName)), devName);
        devName[sizeof(devName) - 1] = 0;
        Device d;
        d.fd = fd;
        d.node = node;
        devices.push_back(d);
        log_i("key-input: + %s '%s'", node.c_str(), devName);
        return true;
    }

    void openOrRetry(const std::string &name)
    {
        if (tryOpen(name))
            return;
        for (Retry &r : retries)
            if (r.name == name)
                return;
        const uint32_t now = mono_ms();
        retries.push_back({name, now + OPEN_RETRY_MS, now + OPEN_RETRY_FOR_MS});
    }

    void dropDevice(size_t index)
    {
        Device &d = devices[index];
        releaseHeld(d.fd);
        log_i("key-input: - %s", d.node.c_str());
        ::close(d.fd);
        devices.erase(devices.begin() + (long)index);
    }

    void scanExisting()
    {
        DIR *dir = opendir(INPUT_DIR);
        if (!dir)
            return;
        while (struct dirent *e = readdir(dir))
            if (strncmp(e->d_name, "event", 5) == 0)
                openOrRetry(e->d_name);
        closedir(dir);
    }

    // (Re)arms the /dev/input watch; false while the directory is missing.
    bool watchInputDir()
    {
        inputWd = inotify_add_watch(inotifyFd, INPUT_DIR, IN_CREATE | IN_ATTRIB | IN_DELETE);
        return inputWd >= 0;
    }

    void onInotify()
    {
        alignas(struct inotify_event) char buf[1024];
        for (;;)
        {
            const ssize_t n = read(inotifyFd, buf, sizeof(buf));
            if (n <= 0)
                return;
            for (ssize_t off = 0; off < n;)
            {
                const struct inotify_event *e = (const struct inotify_event *)(buf + off);
                off += (ssize_t)(sizeof(struct inotify_event) + e->len);
                if (e->wd == devWd)
                {
                    // devtmpfs removes /dev/input with its last node and makes
                    // a new one for the next: watch that, and catch up on any
                    // node created before the watch was in place.
                    if ((e->mask & IN_CREATE) && e->len && !strcmp(e->name, "input") &&
                        watchInputDir())
                        scanExisting();
                    continue;
                }
                if (e->mask & IN_IGNORED)
                {
                    if (e->wd == inputWd)
                        inputWd = -1; // the directory is gone
                    continue;
                }
                if (e->len == 0 || strncmp(e->name, "event", 5) != 0)
                    continue;
                if (e->mask & IN_DELETE)
                {
                    const std::string node = std::string(INPUT_DIR) + "/" + e->name;
                    for (size_t i = 0; i < devices.size(); i++)
                        if (devices[i].node == node)
                        {
                            dropDevice(i);
                            break;
                        }
                }
                else
                    openOrRetry(e->name);
            }
        }
    }

    // After SYN_DROPPED (the kernel's buffer overflowed while this thread
    // was not scheduled): a release may be among the lost events. Let go of
    // everything the device held and re-read its modifiers; a key still down
    // counts again from its next press.
    void resync(Device &dev)
    {
        releaseHeld(dev.fd);
        unsigned long keys[(KEY_MAX + LONG_BITS) / LONG_BITS];
        memset(keys, 0, sizeof(keys));
        if (ioctl(dev.fd, EVIOCGKEY(sizeof(keys)), keys) >= 0)
        {
            dev.shiftL = testBit(keys, KEY_LEFTSHIFT);
            dev.shiftR = testBit(keys, KEY_RIGHTSHIFT);
            dev.otherMods = 0;
            const int mods[] = {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT,
                                KEY_RIGHTALT, KEY_LEFTMETA,  KEY_RIGHTMETA};
            for (int i = 0; i < 6; i++)
                if (testBit(keys, mods[i]))
                    dev.otherMods |= (uint8_t)(1u << i);
        }
        dev.hatX = dev.hatY = 0;
        const uint32_t now = mono_ms();
        if (!dev.dropLogged || now - dev.dropLogMs >= DROP_LOG_MS)
        {
            log_w("key-input: %s: events dropped -- released held keys", dev.node.c_str());
            dev.dropLogged = true;
            dev.dropLogMs = now;
        }
    }

    // Drains one device; false when it is gone (hang-up, ENODEV).
    bool readDevice(Device &dev)
    {
        struct input_event ev[32];
        for (;;)
        {
            const ssize_t n = read(dev.fd, ev, sizeof(ev));
            if (n < 0)
                return errno == EAGAIN || errno == EINTR;
            if (n == 0)
                return false;
            for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++)
            {
                const struct input_event &e = ev[i];
                if (e.type == EV_SYN)
                {
                    if (e.code == SYN_DROPPED)
                        dev.dropped = true;
                    else if (e.code == SYN_REPORT && dev.dropped)
                    {
                        dev.dropped = false;
                        resync(dev);
                    }
                }
                else if (dev.dropped)
                    continue; // rest of a cut-short packet: ignored until SYN_REPORT
                else if (e.type == EV_KEY)
                    onKey(dev, e.code, e.value);
                else if (e.type == EV_ABS && (e.code == ABS_HAT0X || e.code == ABS_HAT0Y))
                    onHat(dev, e.code, e.value);
                else if (e.type == EV_REL && e.code == REL_WHEEL)
                    onRotate(e.value);
            }
        }
    }

    // Repeats, owed wheel turns and open retries that are due; returns the poll timeout until
    // the next one (-1: nothing pending, sleep until an event).
    int runTimers()
    {
        const uint32_t now = mono_ms();
        for (Held &h : held)
        {
            if (!h.period || !due(now, h.nextRepeat))
                continue;
            if (h.volume && due(now, h.until))
            {
                h.period = 0; // held too long: wait for a fresh press
                continue;
            }
            if (h.volume)
                AudioVolume::instance().step(h.volume);
            else if (h.toUi)
                sink(h.nav, h.ch, 2);
            else
                sendPhone(h.button);
            h.nextRepeat = now + h.period;
        }
        sendTurn(now);
        for (size_t i = retries.size(); i-- > 0;)
        {
            Retry &r = retries[i];
            if (!due(now, r.next))
                continue;
            if (tryOpen(r.name) || due(now, r.until))
                retries.erase(retries.begin() + (long)i);
            else
                r.next = now + OPEN_RETRY_MS;
        }

        int timeout = -1;
        auto consider = [&](uint32_t deadline) {
            const int32_t left = (int32_t)(deadline - now);
            const int ms = left < 0 ? 0 : left;
            if (timeout < 0 || ms < timeout)
                timeout = ms;
        };
        for (const Held &h : held)
            if (h.period)
                consider(h.nextRepeat);
        for (const Retry &r : retries)
            consider(r.next);
        if (turnsPending != 0)
            consider(lastTurn + TURN_PHONE_MS);
        return timeout;
    }

    void loop()
    {
        setThreadName("key-input");
        scanExisting();
        std::vector<struct pollfd> fds;
        while (running.load(std::memory_order_acquire))
        {
            checkRoute();
            const int timeout = runTimers();

            fds.clear();
            fds.push_back({wakeFd, POLLIN, 0});
            if (inotifyFd >= 0)
                fds.push_back({inotifyFd, POLLIN, 0});
            const size_t first = fds.size();
            for (const Device &d : devices)
                fds.push_back({d.fd, POLLIN, 0});

            if (poll(fds.data(), fds.size(), timeout) < 0)
            {
                if (errno == EINTR)
                    continue;
                log_e("key-input: poll failed (%s)", strerror(errno));
                break;
            }
            if (fds[0].revents & POLLIN)
            {
                uint64_t v;
                (void)!read(wakeFd, &v, sizeof(v));
            }
            if (!running.load(std::memory_order_acquire))
                break;
            checkRoute(); // before any key: a press belongs to the current route
            // Devices before inotify, back to front: only a device's own drop
            // can shift `devices` while its fds slots are still being matched.
            for (size_t i = fds.size(); i-- > first;)
            {
                const short re = fds[i].revents;
                if (!re)
                    continue;
                const size_t index = i - first;
                if ((re & (POLLHUP | POLLERR | POLLNVAL)) || !readDevice(devices[index]))
                    dropDevice(index);
            }
            if (inotifyFd >= 0 && (fds[1].revents & POLLIN))
                onInotify();
        }
        releaseHeld(-1);
        for (Device &d : devices)
            ::close(d.fd);
        devices.clear();
    }
};

KeyInput::KeyInput(IConnection &conn) : _impl(new Impl(conn))
{
    _impl->wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (_impl->wakeFd < 0)
    {
        log_e("key-input: eventfd failed (%s) -- no keyboard input", strerror(errno));
        return;
    }
    _impl->inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (_impl->inotifyFd >= 0)
    {
        // /dev as well: /dev/input may not exist yet, or come and go.
        _impl->devWd = inotify_add_watch(_impl->inotifyFd, "/dev", IN_CREATE | IN_ONLYDIR);
        if (!_impl->watchInputDir() && _impl->devWd < 0)
        {
            log_w("key-input: cannot watch %s (%s) -- keyboards connected later are missed",
                  INPUT_DIR, strerror(errno));
            ::close(_impl->inotifyFd);
            _impl->inotifyFd = -1;
        }
    }
    _impl->running.store(true, std::memory_order_release);
    Impl *impl = _impl;
    _impl->thread = std::thread([impl] { impl->loop(); });
}

KeyInput::~KeyInput()
{
    _impl->running.store(false, std::memory_order_release);
    if (_impl->wakeFd >= 0)
    {
        const uint64_t one = 1;
        (void)!write(_impl->wakeFd, &one, sizeof(one));
    }
    if (_impl->thread.joinable())
        _impl->thread.join();
    if (_impl->inotifyFd >= 0)
        ::close(_impl->inotifyFd);
    if (_impl->wakeFd >= 0)
        ::close(_impl->wakeFd);
    delete _impl;
}

void KeyInput::setUiSink(std::function<void(OsdKey key, char ch, int value)> sink)
{
    _impl->sink = std::move(sink);
}

void KeyInput::routeToUi(bool active)
{
    // Called every loop tick; only a change wakes the reader, which then
    // releases what was held on the old route.
    if (_impl->toUi.exchange(active, std::memory_order_acq_rel) != active && _impl->wakeFd >= 0)
    {
        const uint64_t one = 1;
        (void)!write(_impl->wakeFd, &one, sizeof(one));
    }
}

#endif /* __linux__ */
