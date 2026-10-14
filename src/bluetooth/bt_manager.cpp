#include "bt_manager.h"

#ifdef USE_BLUETOOTH

#include <dbus/dbus.h>
#include <poll.h>
#include <strings.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "common/clock.h"
#include "common/logger.h"
#include "common/threading.h"

namespace bt
{
namespace
{
constexpr const char *BLUEZ = "org.bluez";
constexpr const char *AGENT_PATH = "/fcp/agent";
// Phones keep numeric comparison (MITM, which e.g. the Galaxy S21 insists
// on); keyboards get passkey entry with the code shown here. KeyboardDisplay
// would add LE cases where the head unit has to type a passkey.
constexpr const char *AGENT_CAPABILITY = "DisplayYesNo";

constexpr const char *IFACE_DBUS = "org.freedesktop.DBus";
constexpr const char *IFACE_OM = "org.freedesktop.DBus.ObjectManager";
constexpr const char *IFACE_PROPS = "org.freedesktop.DBus.Properties";
constexpr const char *IFACE_ADAPTER = "org.bluez.Adapter1";
constexpr const char *IFACE_DEVICE = "org.bluez.Device1";
constexpr const char *IFACE_INPUT = "org.bluez.Input1";
constexpr const char *IFACE_BATTERY = "org.bluez.Battery1";
constexpr const char *IFACE_AGENT = "org.bluez.Agent1";
constexpr const char *IFACE_AGENT_MANAGER = "org.bluez.AgentManager1";

constexpr const char *ERR_REJECTED = "org.bluez.Error.Rejected";

constexpr const char *UUID_HID = "00001124-0000-1000-8000-00805f9b34fb";
constexpr const char *UUID_HOG = "00001812-0000-1000-8000-00805f9b34fb";

constexpr uint32_t SCAN_MS = 30000;
constexpr int CALL_MS = 25000;
constexpr int PAIR_CALL_MS = 120000; // includes the user typing the code
constexpr int CONNECT_CALL_MS = 30000;
// After Pair() succeeds the device stays the pairing target this long: a
// classic keyboard opens its HID channels right away, before Trusted lands.
constexpr uint32_t TARGET_GRACE_MS = 10000;
// Classic keyboards usually connect HID themselves after pairing; one
// Connect() after this delay covers the ones that do not.
constexpr uint32_t CONNECT_AFTER_PAIR_MS = 3000;
constexpr uint32_t STATUS_SHORT_MS = 5000;
constexpr uint32_t STATUS_LONG_MS = 8000;
constexpr size_t NAME_CHARS = 28;

// The system bus (dbus-daemon's default address). On the image it runs only
// while Bluetooth is on -- `bt on` starts it, `bt off` kills it -- and a
// wired-mode app starts before it, so the manager waits for the socket and
// reconnects whenever it comes back.
constexpr const char *RUN_DIR = "/var/run";
constexpr const char *BUS_DIR = "/var/run/dbus";
constexpr const char *BUS_SOCKET_NAME = "system_bus_socket";
constexpr const char *BUS_SOCKET = "/var/run/dbus/system_bus_socket";
// The socket appears at bind(), a moment before the daemon takes
// connections: a few quick retries while it is there, then back to waiting.
constexpr uint32_t BUS_RETRY_MS = 500;
constexpr int BUS_RETRIES = 6;

bool due(uint32_t now, uint32_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

struct Device
{
    std::string address, alias, icon;
    uint32_t cls = 0;
    uint16_t appearance = 0;
    int16_t rssi = 0;
    bool hasClass = false, hasAppearance = false, hasRssi = false;
    bool hidUuid = false; // HID (0x1124) or HID over GATT (0x1812) listed
    bool paired = false, bonded = false, trusted = false, connected = false;
    bool hasInput = false; // org.bluez.Input1: a probed classic HID device
    int battery = -1;      // org.bluez.Battery1 percentage (LE, while connected)
    uint32_t order = 0;    // first sighting: lists never re-sort
    uint32_t seenScan = 0; // number of the last discovery that reported it
};

enum class Kind : uint8_t
{
    Keyboard,
    Mouse,
    Gamepad,
    Remote,
    Input,
};

bool isHid(const Device &d)
{
    if (d.hasClass)
    {
        const uint32_t major = (d.cls >> 8) & 0x1f;
        if (major == 0x05) // Peripheral
            return true;
        // A phone or computer that also offers a HID role stays a phone.
        if (major == 0x01 || major == 0x02)
            return false;
    }
    if (d.icon.compare(0, 6, "input-") == 0)
        return true;
    if (d.hasAppearance && ((d.appearance >= 0x03C0 && d.appearance <= 0x03FF) ||
                            (d.appearance >= 0x0180 && d.appearance <= 0x01BF)))
        return true;
    return d.hidUuid || d.hasInput;
}

Kind kindOf(const Device &d)
{
    if (d.hasClass && ((d.cls >> 8) & 0x1f) == 0x05)
    {
        const uint32_t minor = (d.cls >> 2) & 0x3f;
        if (minor & 0x10) // keyboard, or keyboard + pointing combo
            return Kind::Keyboard;
        if (minor & 0x20)
            return Kind::Mouse;
        switch (minor & 0x0f)
        {
        case 1: // joystick
        case 2: // gamepad
            return Kind::Gamepad;
        case 3:
            return Kind::Remote;
        default:
            break;
        }
    }
    if (d.icon == "input-keyboard")
        return Kind::Keyboard;
    if (d.icon == "input-mouse")
        return Kind::Mouse;
    if (d.icon == "input-gaming")
        return Kind::Gamepad;
    if (d.hasAppearance)
    {
        if (d.appearance == 0x03C1)
            return Kind::Keyboard;
        if (d.appearance == 0x03C2)
            return Kind::Mouse;
        if (d.appearance == 0x03C3 || d.appearance == 0x03C4)
            return Kind::Gamepad;
        if (d.appearance >= 0x0180 && d.appearance <= 0x01BF)
            return Kind::Remote;
    }
    return Kind::Input;
}

const char *kindText(Kind k)
{
    switch (k)
    {
    case Kind::Keyboard:
        return "Keyboard";
    case Kind::Mouse:
        return "Mouse";
    case Kind::Gamepad:
        return "Gamepad";
    case Kind::Remote:
        return "Remote";
    default:
        return "Input device";
    }
}

// Printable ASCII only (the UI embeds no other glyphs), runs of white space
// collapsed, at most NAME_CHARS characters.
std::string displayName(const Device &d)
{
    const std::string &raw = d.alias.empty() ? d.address : d.alias;
    std::string out;
    bool gap = false;
    for (unsigned char c : raw)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            gap = !out.empty();
            continue;
        }
        if (c < 0x20 || c > 0x7e)
            continue;
        if (gap)
        {
            if (out.size() + 1 >= NAME_CHARS)
                break;
            out += ' ';
            gap = false;
        }
        if (out.size() >= NAME_CHARS)
            break;
        out += (char)c;
    }
    return out.empty() ? d.address : out;
}

// "123456" -> "1 2 3 4 5 6": easier to read off a car screen.
std::string spaced(const std::string &code)
{
    std::string out;
    for (char c : code)
    {
        if (!out.empty())
            out += ' ';
        out += c;
    }
    return out;
}

std::string sixDigits(uint32_t value)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%06u", (unsigned)(value % 1000000));
    return buf;
}

std::string randomPin()
{
    uint32_t v = 0;
    if (getrandom(&v, sizeof(v), 0) != (ssize_t)sizeof(v))
        v = mono_ms() * 2654435761u;
    return sixDigits(v);
}

bool isError(DBusMessage *reply)
{
    return !reply || dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR;
}

std::string errorName(DBusMessage *reply)
{
    if (!reply)
        return "no reply";
    const char *name = dbus_message_get_error_name(reply);
    return name ? name : "";
}

std::string errorText(DBusMessage *reply)
{
    std::string text = errorName(reply);
    const char *detail = nullptr;
    if (reply && dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &detail, DBUS_TYPE_INVALID) &&
        detail && *detail)
        text += std::string(" (") + detail + ")";
    return text;
}

// --- a{sv} / a{sa{sv}} walking ------------------------------------------------

template <typename F>
void forEachProp(DBusMessageIter *array, F fn)
{
    if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter entries;
    dbus_message_iter_recurse(array, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry, value;
        dbus_message_iter_recurse(&entries, &entry);
        const char *key = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
        {
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT)
            {
                dbus_message_iter_recurse(&entry, &value);
                fn(key, &value);
            }
        }
        dbus_message_iter_next(&entries);
    }
}

// a{sa{sv}}: fn(interface, &propertiesArray)
template <typename F>
void forEachInterface(DBusMessageIter *array, F fn)
{
    if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter entries;
    dbus_message_iter_recurse(array, &entries);
    while (dbus_message_iter_get_arg_type(&entries) == DBUS_TYPE_DICT_ENTRY)
    {
        DBusMessageIter entry;
        dbus_message_iter_recurse(&entries, &entry);
        const char *iface = nullptr;
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
        {
            dbus_message_iter_get_basic(&entry, &iface);
            dbus_message_iter_next(&entry);
            fn(iface, &entry);
        }
        dbus_message_iter_next(&entries);
    }
}

template <typename F>
void forEachString(DBusMessageIter *array, F fn)
{
    if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
        return;
    DBusMessageIter items;
    dbus_message_iter_recurse(array, &items);
    while (dbus_message_iter_get_arg_type(&items) == DBUS_TYPE_STRING)
    {
        const char *s = nullptr;
        dbus_message_iter_get_basic(&items, &s);
        fn(s);
        dbus_message_iter_next(&items);
    }
}

bool getBool(DBusMessageIter *v, bool &out)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_BOOLEAN)
        return false;
    dbus_bool_t b = FALSE;
    dbus_message_iter_get_basic(v, &b);
    out = b;
    return true;
}

bool getString(DBusMessageIter *v, std::string &out)
{
    if (dbus_message_iter_get_arg_type(v) != DBUS_TYPE_STRING)
        return false;
    const char *s = nullptr;
    dbus_message_iter_get_basic(v, &s);
    out = s ? s : "";
    return true;
}

bool samePrompt(const Prompt &a, const Prompt &b)
{
    return a.kind == b.kind && a.title == b.title && a.code == b.code && a.hint == b.hint &&
           a.acceptLabel == b.acceptLabel;
}

bool sameSnapshot(const Snapshot &a, const Snapshot &b)
{
    return a.paired == b.paired && a.found == b.found && a.scanning == b.scanning &&
           a.scanAllowed == b.scanAllowed && a.phoneConnected == b.phoneConnected &&
           a.status == b.status && a.summary == b.summary && samePrompt(a.prompt, b.prompt);
}
} // namespace

struct Manager::Impl
{
    explicit Impl(Manager &m) : owner(m) {}

    using Reply = std::function<void(DBusMessage *reply)>; // nullptr: never sent
    struct PendingCtx
    {
        Impl *self;
        Reply fn;
    };
    struct TimeoutSlot
    {
        DBusTimeout *timeout;
        uint32_t deadline;
    };
    // Who the open dialog belongs to.
    enum class PromptFor : uint8_t
    {
        None,
        Code,    // a code the device must type (no reply owed)
        Confirm, // a deferred RequestConfirmation (`deferred` holds it)
        Forget,  // the user's own Forget, confirmed first
    };

    Manager &owner;
    int wakeFd = -1;
    int inotifyFd = -1; // the system bus socket coming (back)
    std::thread thread;
    std::atomic<bool> running{false};
    bool phonePairing = false;

    std::mutex cmdMutex;
    std::vector<std::function<void()>> commands;
    std::atomic<int8_t> sessionSent{-1}; // last setSessionActive() value queued

    mutable std::mutex snapMutex;
    Snapshot snap; // written only by the manager thread, under snapMutex

    // --- manager-thread state -----------------------------------------------
    DBusConnection *conn = nullptr; // null while there is no system bus
    std::vector<DBusWatch *> watches;
    std::vector<TimeoutSlot> timeouts;
    std::vector<DBusPendingCall *> pending;
    bool busLost = false;
    int runWd = -1, busWd = -1; // inotify: RUN_DIR (for BUS_DIR), BUS_DIR (for the socket)
    bool busTry = true;         // connect at the next loop turn
    bool busRetryArmed = false;
    uint32_t busRetryAt = 0;
    int busRetries = 0;
    bool busWaitLogged = false;

    std::string bluezOwner; // unique bus name of bluetoothd; empty: not running
    bool cacheReady = false;
    bool agentRegistered = false;
    std::string adapterPath;
    bool powered = false, pairable = false, discoverable = false;
    bool policyApplied = false;
    std::map<std::string, Device> devices;
    uint32_t orderSeq = 0;

    // Discovery: what the user asked for (what the UI shows) and where BlueZ
    // is. One Start/StopDiscovery at a time: BlueZ answers a stop that
    // overtakes a pending start (and the reverse) with InProgress and keeps
    // its own state, so a change of mind waits for the reply (syncScan()).
    enum class ScanState : uint8_t
    {
        Off,
        Starting,
        On,
        Stopping,
    };
    bool scanWanted = false;
    ScanState scanState = ScanState::Off;
    uint32_t scanNumber = 0;
    uint32_t scanUntil = 0;
    bool sessionActive = false;

    // The device the user is pairing from the UI: the only HID device the
    // agent says yes to (see isTarget()).
    std::string target;
    bool pairPending = false;
    uint32_t pairSeq = 0; // tags each Pair() so a reply that lost its pairing is ignored
    bool userCancelled = false;
    uint32_t targetUntil = 0;
    std::string connectAfterPair;
    uint32_t connectAfterPairAt = 0;
    std::string connecting; // Connect() in flight, for the header

    PromptFor promptFor = PromptFor::None;
    std::string promptPath;
    Prompt prompt;
    DBusMessage *deferred = nullptr; // the RequestConfirmation behind a Confirm

    std::string transient; // header status with an expiry
    uint32_t transientUntil = 0;

    // --- libdbus main-loop glue ---------------------------------------------

    static dbus_bool_t addWatch(DBusWatch *w, void *data)
    {
        static_cast<Impl *>(data)->watches.push_back(w);
        return TRUE;
    }
    static void removeWatch(DBusWatch *w, void *data)
    {
        auto &v = static_cast<Impl *>(data)->watches;
        v.erase(std::remove(v.begin(), v.end(), w), v.end());
    }
    static void toggleWatch(DBusWatch *, void *) {} // enabled state is read per poll
    static dbus_bool_t addTimeout(DBusTimeout *t, void *data)
    {
        static_cast<Impl *>(data)->timeouts.push_back(
            {t, mono_ms() + (uint32_t)dbus_timeout_get_interval(t)});
        return TRUE;
    }
    static void removeTimeout(DBusTimeout *t, void *data)
    {
        auto &v = static_cast<Impl *>(data)->timeouts;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [t](const TimeoutSlot &s) { return s.timeout == t; }),
                v.end());
    }
    static void toggleTimeout(DBusTimeout *t, void *data)
    {
        for (TimeoutSlot &s : static_cast<Impl *>(data)->timeouts)
            if (s.timeout == t)
                s.deadline = mono_ms() + (uint32_t)dbus_timeout_get_interval(t);
    }

    void wake()
    {
        const uint64_t one = 1;
        if (wakeFd >= 0)
            (void)!write(wakeFd, &one, sizeof(one));
    }

    void queue(std::function<void()> fn)
    {
        if (!running.load(std::memory_order_acquire))
            return; // no thread to run it: never started, or stopping
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            commands.push_back(std::move(fn));
        }
        wake();
    }

    // --- outgoing calls -----------------------------------------------------

    static void onPending(DBusPendingCall *pc, void *data)
    {
        PendingCtx *ctx = static_cast<PendingCtx *>(data);
        Impl *self = ctx->self;
        Reply fn = std::move(ctx->fn);
        DBusMessage *reply = dbus_pending_call_steal_reply(pc);
        self->pending.erase(std::remove(self->pending.begin(), self->pending.end(), pc),
                            self->pending.end());
        dbus_pending_call_unref(pc); // frees ctx
        if (fn)
            fn(reply);
        if (reply)
            dbus_message_unref(reply);
    }

    // Asynchronous: the reply (or an error, or nullptr when the call could not
    // even be sent) reaches `fn` from a later dispatch on this thread.
    void call(DBusMessage *msg, int timeoutMs, Reply fn)
    {
        DBusPendingCall *pc = nullptr;
        if (msg)
        {
            dbus_message_set_auto_start(msg, FALSE);
            if (!dbus_connection_send_with_reply(conn, msg, &pc, timeoutMs))
                pc = nullptr;
            dbus_message_unref(msg);
        }
        if (!pc)
        {
            if (fn)
                fn(nullptr);
            return;
        }
        PendingCtx *ctx = new PendingCtx{this, std::move(fn)};
        if (!dbus_pending_call_set_notify(pc, &Impl::onPending, ctx,
                                          [](void *p) { delete static_cast<PendingCtx *>(p); }))
        {
            delete ctx;
            dbus_pending_call_cancel(pc);
            dbus_pending_call_unref(pc);
            return;
        }
        pending.push_back(pc);
    }

    static DBusMessage *bluez(const std::string &path, const char *iface, const char *member)
    {
        return dbus_message_new_method_call(BLUEZ, path.c_str(), iface, member);
    }

    // Logs a failed fire-and-forget call.
    static Reply logFailure(const char *what)
    {
        return [what](DBusMessage *reply) {
            if (isError(reply))
                log_w("bt-mgr: %s failed > %s", what, errorText(reply).c_str());
        };
    }

    void setProp(const std::string &path, const char *iface, const char *name, bool value,
                 Reply fn)
    {
        DBusMessage *msg = bluez(path, IFACE_PROPS, "Set");
        if (msg)
        {
            const dbus_bool_t b = value ? TRUE : FALSE;
            DBusMessageIter it, var;
            dbus_message_iter_init_append(msg, &it);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
            dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING,
                                             &var);
            dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &b);
            dbus_message_iter_close_container(&it, &var);
        }
        call(msg, CALL_MS, std::move(fn));
    }

    void deviceCall(const std::string &path, const char *member, int timeoutMs, Reply fn)
    {
        call(bluez(path, IFACE_DEVICE, member), timeoutMs, std::move(fn));
    }

    void adapterCall(const char *member, const char *devicePath, Reply fn)
    {
        DBusMessage *msg = bluez(adapterPath, IFACE_ADAPTER, member);
        if (msg && devicePath)
            dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &devicePath, DBUS_TYPE_INVALID);
        call(msg, CALL_MS, std::move(fn));
    }

    // --- replies to BlueZ's agent calls ---------------------------------------

    void replyEmpty(DBusMessage *msg)
    {
        if (dbus_message_get_no_reply(msg))
            return;
        if (DBusMessage *r = dbus_message_new_method_return(msg))
        {
            dbus_connection_send(conn, r, nullptr);
            dbus_message_unref(r);
        }
    }

    void replyError(DBusMessage *msg, const char *name, const char *text)
    {
        if (dbus_message_get_no_reply(msg))
            return;
        if (DBusMessage *r = dbus_message_new_error(msg, name, text))
        {
            dbus_connection_send(conn, r, nullptr);
            dbus_message_unref(r);
        }
    }

    void replyString(DBusMessage *msg, const std::string &value)
    {
        if (dbus_message_get_no_reply(msg))
            return;
        if (DBusMessage *r = dbus_message_new_method_return(msg))
        {
            const char *s = value.c_str();
            dbus_message_append_args(r, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID);
            dbus_connection_send(conn, r, nullptr);
            dbus_message_unref(r);
        }
    }

    // --- device cache ---------------------------------------------------------

    Device *find(const std::string &path)
    {
        auto it = devices.find(path);
        return it == devices.end() ? nullptr : &it->second;
    }
    const Device *find(const std::string &path) const
    {
        auto it = devices.find(path);
        return it == devices.end() ? nullptr : &it->second;
    }

    Device &device(const std::string &path)
    {
        auto it = devices.find(path);
        if (it != devices.end())
            return it->second;
        Device &d = devices[path];
        d.order = ++orderSeq;
        return d;
    }

    std::string nameOf(const std::string &path) const
    {
        const Device *d = find(path);
        return d ? displayName(*d) : std::string("device");
    }

    // After the user's Cancel the device is no longer welcome: a request
    // still on its way is refused instead of opening a new dialog.
    bool isTarget(const std::string &path) const
    {
        return !target.empty() && path == target && !userCancelled &&
               (pairPending || !due(mono_ms(), targetUntil));
    }

    bool hidPath(const std::string &path)
    {
        const Device *d = find(path);
        return d && isHid(*d);
    }

    // A phone -- any device that is not an input device -- is connected.
    bool phoneConnected() const
    {
        for (const auto &kv : devices)
            if (kv.second.connected && !isHid(kv.second))
                return true;
        return false;
    }

    // In a wireless mode a phone connected over Bluetooth is its session --
    // or the handoff before it -- on the radio that discovery would share.
    bool phoneLinked() const { return phonePairing && phoneConnected(); }

    bool scanBlocked() const { return sessionActive || phoneLinked(); }

    void applyDevice(const std::string &path, Device &d, DBusMessageIter *props)
    {
        forEachProp(props, [&](const char *key, DBusMessageIter *v) {
            const int type = dbus_message_iter_get_arg_type(v);
            if (!strcmp(key, "Address"))
                getString(v, d.address);
            else if (!strcmp(key, "Alias"))
                getString(v, d.alias);
            else if (!strcmp(key, "Icon"))
                getString(v, d.icon);
            else if (!strcmp(key, "Class") && type == DBUS_TYPE_UINT32)
            {
                dbus_uint32_t c = 0;
                dbus_message_iter_get_basic(v, &c);
                d.cls = c;
                d.hasClass = true;
            }
            else if (!strcmp(key, "Appearance") && type == DBUS_TYPE_UINT16)
            {
                dbus_uint16_t a = 0;
                dbus_message_iter_get_basic(v, &a);
                d.appearance = a;
                d.hasAppearance = true;
            }
            else if (!strcmp(key, "RSSI") && type == DBUS_TYPE_INT16)
            {
                dbus_int16_t r = 0;
                dbus_message_iter_get_basic(v, &r);
                d.rssi = r;
                d.hasRssi = true;
                if (scanWanted)
                    d.seenScan = scanNumber;
            }
            else if (!strcmp(key, "UUIDs"))
            {
                d.hidUuid = false;
                forEachString(v, [&](const char *u) {
                    if (u && (!strcasecmp(u, UUID_HID) || !strcasecmp(u, UUID_HOG)))
                        d.hidUuid = true;
                });
            }
            else if (!strcmp(key, "Paired"))
            {
                const bool was = d.paired;
                getBool(v, d.paired);
                if (d.paired && !was)
                    onKeyStored(path);
            }
            else if (!strcmp(key, "Bonded"))
            {
                // Bonded comes with the stored key; Paired (and the Pair()
                // reply) only after the service search that follows.
                const bool was = d.bonded;
                getBool(v, d.bonded);
                if (d.bonded && !was)
                    onKeyStored(path);
            }
            else if (!strcmp(key, "Trusted"))
                getBool(v, d.trusted);
            else if (!strcmp(key, "Connected"))
            {
                const bool was = d.connected;
                getBool(v, d.connected);
                if (d.connected && !was && scanWanted && phonePairing && !isHid(d))
                {
                    log_i("bt-mgr: a phone connected -- search stopped");
                    stopScan();
                }
            }
        });
    }

    static void invalidateDevice(Device &d, DBusMessageIter *names)
    {
        forEachString(names, [&](const char *n) {
            if (!strcmp(n, "RSSI"))
                d.hasRssi = false;
            else if (!strcmp(n, "Icon"))
                d.icon.clear();
            else if (!strcmp(n, "Class"))
                d.hasClass = false;
            else if (!strcmp(n, "Appearance"))
                d.hasAppearance = false;
        });
    }

    void applyAdapter(const std::string &path, DBusMessageIter *props)
    {
        if (adapterPath.empty() || path == "/org/bluez/hci0")
        {
            if (adapterPath != path)
                policyApplied = false;
            adapterPath = path;
        }
        if (path != adapterPath)
            return;
        forEachProp(props, [&](const char *key, DBusMessageIter *v) {
            if (!strcmp(key, "Powered"))
                getBool(v, powered);
            else if (!strcmp(key, "Pairable"))
                getBool(v, pairable);
            else if (!strcmp(key, "Discoverable"))
                getBool(v, discoverable);
        });
    }

    void applyBattery(Device &d, DBusMessageIter *props)
    {
        forEachProp(props, [&](const char *key, DBusMessageIter *v) {
            if (!strcmp(key, "Percentage") && dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BYTE)
            {
                unsigned char p = 0;
                dbus_message_iter_get_basic(v, &p);
                d.battery = p;
            }
        });
    }

    void interfaceAdded(const std::string &path, const char *iface, DBusMessageIter *props)
    {
        if (!strcmp(iface, IFACE_DEVICE))
            applyDevice(path, device(path), props);
        else if (!strcmp(iface, IFACE_INPUT))
            device(path).hasInput = true;
        else if (!strcmp(iface, IFACE_BATTERY))
            applyBattery(device(path), props);
        else if (!strcmp(iface, IFACE_ADAPTER))
            applyAdapter(path, props);
    }

    void interfaceRemoved(const std::string &path, const char *iface)
    {
        if (!strcmp(iface, IFACE_DEVICE))
        {
            devices.erase(path);
            if (connectAfterPair == path)
                connectAfterPair.clear();
            if (promptPath == path && promptFor != PromptFor::None)
                closePrompt(false);
            if (path == target)
            {
                // Removing a device cancels its service search without
                // answering the Pair() waiting on it.
                if (pairPending)
                    setStatus(userCancelled ? "Pairing cancelled" : "Pairing failed", STATUS_LONG_MS);
                abandonPairing();
            }
        }
        else if (!strcmp(iface, IFACE_INPUT))
        {
            if (Device *d = find(path))
                d->hasInput = false;
        }
        else if (!strcmp(iface, IFACE_BATTERY))
        {
            if (Device *d = find(path))
                d->battery = -1;
        }
        else if (!strcmp(iface, IFACE_ADAPTER) && path == adapterPath)
        {
            log_w("bt-mgr: adapter %s gone", path.c_str());
            adapterPath.clear();
            scanWanted = false;
            scanState = ScanState::Off;
        }
    }

    // --- adapter policy & agent registration ----------------------------------

    void applyPolicy()
    {
        if (adapterPath.empty() || policyApplied)
            return;
        policyApplied = true;
        // Never power down or reset: a connected keyboard survives everything.
        if (!powered)
            setProp(adapterPath, IFACE_ADAPTER, "Powered", true, logFailure("Powered=true"));
        // Without Pairable even our own Pair() would not store a key.
        if (!pairable)
            setProp(adapterPath, IFACE_ADAPTER, "Pairable", true, logFailure("Pairable=true"));
        // Wired modes: nobody needs to find the head unit; bonded keyboards
        // still reconnect (BlueZ keeps them on the page-scan accept list).
        if (!phonePairing && discoverable)
            setProp(adapterPath, IFACE_ADAPTER, "Discoverable", false,
                    logFailure("Discoverable=false"));
    }

    void registerAgent()
    {
        DBusMessage *msg = bluez("/org/bluez", IFACE_AGENT_MANAGER, "RegisterAgent");
        if (msg)
        {
            const char *path = AGENT_PATH;
            const char *cap = AGENT_CAPABILITY;
            dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_STRING, &cap,
                                     DBUS_TYPE_INVALID);
        }
        call(msg, CALL_MS, [this](DBusMessage *reply) {
            if (isError(reply) && errorName(reply) != "org.bluez.Error.AlreadyExists")
            {
                log_e("bt-mgr: RegisterAgent failed > %s -- no pairing possible",
                      errorText(reply).c_str());
                return;
            }
            DBusMessage *msg = bluez("/org/bluez", IFACE_AGENT_MANAGER, "RequestDefaultAgent");
            if (msg)
            {
                const char *path = AGENT_PATH;
                dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
            }
            call(msg, CALL_MS, [this](DBusMessage *reply) {
                if (isError(reply))
                {
                    log_e("bt-mgr: RequestDefaultAgent failed > %s", errorText(reply).c_str());
                    return;
                }
                agentRegistered = true;
                log_i("bt-mgr: pairing agent registered (%s, default)", AGENT_CAPABILITY);
            });
        });
    }

    void onManagedObjects(DBusMessage *reply)
    {
        if (isError(reply))
        {
            log_e("bt-mgr: GetManagedObjects failed > %s", errorText(reply).c_str());
            return;
        }
        // A fresh view: keep only list order and scan sightings of known paths.
        std::map<std::string, Device> old;
        old.swap(devices);
        DBusMessageIter it, objects;
        if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
        {
            dbus_message_iter_recurse(&it, &objects);
            while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY)
            {
                DBusMessageIter entry;
                dbus_message_iter_recurse(&objects, &entry);
                const char *path = nullptr;
                if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_OBJECT_PATH)
                {
                    dbus_message_iter_get_basic(&entry, &path);
                    dbus_message_iter_next(&entry);
                    const std::string p = path;
                    forEachInterface(&entry, [&](const char *iface, DBusMessageIter *props) {
                        interfaceAdded(p, iface, props);
                    });
                }
                dbus_message_iter_next(&objects);
            }
        }
        for (auto &kv : devices)
        {
            auto prev = old.find(kv.first);
            if (prev != old.end())
            {
                kv.second.order = prev->second.order;
                kv.second.seenScan = prev->second.seenScan;
            }
        }
        cacheReady = true;
        if (adapterPath.empty())
            log_w("bt-mgr: no Bluetooth adapter");
        else
            log_i("bt-mgr: adapter %s, %zu known device(s)", adapterPath.c_str(), devices.size());
        applyPolicy();
        if (!agentRegistered)
            registerAgent();
    }

    void bluezUp(const std::string &owner)
    {
        if (owner == bluezOwner)
            return;
        bluezDown();
        bluezOwner = owner;
        log_i("bt-mgr: bluetoothd is up (%s)", owner.c_str());
        call(bluez("/", IFACE_OM, "GetManagedObjects"), CALL_MS,
             [this, owner](DBusMessage *reply) {
                 if (owner == bluezOwner)
                     onManagedObjects(reply);
             });
    }

    void bluezDown()
    {
        if (bluezOwner.empty())
            return;
        log_w("bt-mgr: bluetoothd went away");
        resetBluez();
    }

    // Forgets everything bluetoothd told us (it is gone, or the bus is).
    void resetBluez()
    {
        bluezOwner.clear();
        cacheReady = false;
        agentRegistered = false;
        adapterPath.clear();
        policyApplied = false;
        devices.clear();
        scanWanted = false;
        scanState = ScanState::Off;
        abandonPairing();
        connectAfterPair.clear();
        connecting.clear();
        closePrompt(false);
    }

    // --- the dialog -----------------------------------------------------------

    void openPrompt(PromptFor kind, const std::string &path, Prompt p)
    {
        closePrompt(true); // a newer request replaces an older dialog
        promptFor = kind;
        promptPath = path;
        prompt = std::move(p);
    }

    // rejectDeferred: answer a still-pending RequestConfirmation with "no"
    // (false when BlueZ already gave up on it, e.g. after Cancel()).
    void closePrompt(bool rejectDeferred)
    {
        if (deferred)
        {
            if (rejectDeferred)
                replyError(deferred, ERR_REJECTED, "Replaced");
            dbus_message_unref(deferred);
            deferred = nullptr;
        }
        promptFor = PromptFor::None;
        promptPath.clear();
        prompt = Prompt{};
    }

    void showCode(const std::string &path, const std::string &code, int entered)
    {
        Prompt p;
        p.kind = PromptKind::ShowCode;
        p.title = "Pair " + nameOf(path);
        p.code = spaced(code);
        p.hint = "Type the code on the keyboard, then press Enter";
        if (entered > 0)
            p.hint += " (" + std::to_string(entered) + " of " + std::to_string(code.size()) +
                      " typed)";
        if (promptFor == PromptFor::Code && promptPath == path)
            prompt = std::move(p); // progress update: same dialog
        else
            openPrompt(PromptFor::Code, path, std::move(p));
    }

    // The key is stored: from here on CancelPairing would not cancel but
    // silently unpair, so the code dialog (and its Cancel) goes away.
    void onKeyStored(const std::string &path)
    {
        if (promptFor == PromptFor::Code && promptPath == path)
            closePrompt(false);
    }

    // Ends the pairing without its Pair() reply; a late reply is ignored.
    void abandonPairing()
    {
        pairPending = false;
        pairSeq++;
        target.clear();
    }

    void setStatus(std::string text, uint32_t ms)
    {
        transient = std::move(text);
        transientUntil = mono_ms() + ms;
    }

    // --- agent ----------------------------------------------------------------

    static DBusHandlerResult agentMessage(DBusConnection *, DBusMessage *msg, void *data)
    {
        return static_cast<Impl *>(data)->onAgent(msg);
    }

    // Wired modes ran no agent before this manager, so BlueZ refused every
    // pairing and untrusted service request from phones there; keep it that
    // way (an earlier wireless bond must not bring up e.g. HFP, which would
    // take the phone's call audio to a head unit that renders none).
    void rejectWired(DBusMessage *msg, const char *dev, const char *what)
    {
        log_i("bt-mgr: refused %s from %s (no phone uses Bluetooth in this mode)", what, dev);
        replyError(msg, ERR_REJECTED, "Not in this mode");
    }

    DBusHandlerResult onAgent(DBusMessage *msg)
    {
        if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL ||
            !dbus_message_has_interface(msg, IFACE_AGENT))
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        const char *member = dbus_message_get_member(msg);
        const char *sender = dbus_message_get_sender(msg);
        if (!member || !sender || bluezOwner != sender)
        {
            replyError(msg, ERR_REJECTED, "Not from bluetoothd");
            return DBUS_HANDLER_RESULT_HANDLED;
        }

        const char *dev = nullptr;
        if (!strcmp(member, "Release"))
        {
            log_w("bt-mgr: agent released by bluetoothd");
            agentRegistered = false;
            closePrompt(false);
            replyEmpty(msg);
        }
        else if (!strcmp(member, "Cancel"))
        {
            log_i("bt-mgr: agent request cancelled");
            if (promptFor == PromptFor::Confirm || promptFor == PromptFor::Code)
                closePrompt(false);
            replyEmpty(msg);
        }
        else if (!strcmp(member, "RequestConfirmation"))
        {
            dbus_uint32_t passkey = 0;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32,
                                       &passkey, DBUS_TYPE_INVALID))
                replyError(msg, ERR_REJECTED, "Bad arguments");
            else if (isTarget(dev))
            {
                // A device with a display: the user compares the codes.
                Prompt p;
                p.kind = PromptKind::Confirm;
                p.title = "Pair " + nameOf(dev);
                p.code = spaced(sixDigits(passkey));
                p.hint = "Pair if " + nameOf(dev) + " shows the same code.";
                p.acceptLabel = "Pair";
                openPrompt(PromptFor::Confirm, dev, std::move(p));
                deferred = dbus_message_ref(msg);
            }
            else if (hidPath(dev))
            {
                log_w("bt-mgr: rejected pairing from input device %s (not started here)", dev);
                replyError(msg, ERR_REJECTED, "Pair it from the head unit");
            }
            else if (!phonePairing)
                rejectWired(msg, dev, "pairing");
            else
                replyEmpty(msg); // phones: numeric comparison, accepted as always
        }
        else if (!strcmp(member, "RequestAuthorization"))
        {
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID))
                replyError(msg, ERR_REJECTED, "Bad arguments");
            else if (isTarget(dev))
                replyEmpty(msg);
            else if (hidPath(dev))
            {
                log_w("bt-mgr: rejected just-works pairing from input device %s", dev);
                replyError(msg, ERR_REJECTED, "Pair it from the head unit");
            }
            else if (!phonePairing)
                rejectWired(msg, dev, "pairing");
            else
                replyEmpty(msg);
        }
        else if (!strcmp(member, "AuthorizeService"))
        {
            const char *uuid = nullptr;
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING,
                                       &uuid, DBUS_TYPE_INVALID))
                replyError(msg, ERR_REJECTED, "Bad arguments");
            else if (isTarget(dev))
                replyEmpty(msg);
            else if (!strcasecmp(uuid, UUID_HID) || !strcasecmp(uuid, UUID_HOG))
            {
                // Trusted devices never get here; this is an unknown keyboard.
                log_w("bt-mgr: rejected HID service from untrusted device %s", dev);
                replyError(msg, ERR_REJECTED, "Not trusted");
            }
            else if (!phonePairing)
                rejectWired(msg, dev, uuid);
            else
                replyEmpty(msg); // HFP, iAP2, ... for phones: as always
        }
        else if (!strcmp(member, "DisplayPasskey"))
        {
            dbus_uint32_t passkey = 0;
            dbus_uint16_t entered = 0;
            if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_UINT32,
                                      &passkey, DBUS_TYPE_UINT16, &entered, DBUS_TYPE_INVALID))
            {
                if (isTarget(dev))
                    showCode(dev, sixDigits(passkey), entered);
                else if (hidPath(dev))
                {
                    // Cannot be refused by a reply: end it by dropping the link.
                    log_w("bt-mgr: input device %s tried to pair -- disconnecting", dev);
                    deviceCall(dev, "Disconnect", CALL_MS, logFailure("Disconnect"));
                }
            }
            replyEmpty(msg);
        }
        else if (!strcmp(member, "DisplayPinCode"))
        {
            const char *pin = nullptr;
            // Must be answered at once: bluetoothd sends the PIN either way.
            if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_STRING,
                                      &pin, DBUS_TYPE_INVALID) &&
                isTarget(dev))
                showCode(dev, pin, 0);
            replyEmpty(msg);
        }
        else if (!strcmp(member, "RequestPinCode"))
        {
            // Legacy (pre-2.1) devices. A keyboard types the PIN we show; a
            // remote or game pad uses the conventional fixed one.
            if (!dbus_message_get_args(msg, nullptr, DBUS_TYPE_OBJECT_PATH, &dev, DBUS_TYPE_INVALID))
                replyError(msg, ERR_REJECTED, "Bad arguments");
            else if (!isTarget(dev))
            {
                log_w("bt-mgr: rejected PIN pairing from %s (not started here)", dev);
                replyError(msg, ERR_REJECTED, "Pair it from the head unit");
            }
            else
            {
                const Device *d = find(dev);
                if (d && kindOf(*d) == Kind::Keyboard)
                {
                    const std::string pin = randomPin();
                    showCode(dev, pin, 0);
                    replyString(msg, pin);
                }
                else
                    replyString(msg, "0000");
            }
        }
        else if (!strcmp(member, "RequestPasskey"))
        {
            // Unreachable with DisplayYesNo; never type on the head unit.
            replyError(msg, ERR_REJECTED, "Not supported");
        }
        else
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    // --- signals --------------------------------------------------------------

    static DBusHandlerResult filter(DBusConnection *, DBusMessage *msg, void *data)
    {
        static_cast<Impl *>(data)->onSignal(msg);
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    void onSignal(DBusMessage *msg)
    {
        if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_SIGNAL)
            return;
        if (dbus_message_is_signal(msg, DBUS_INTERFACE_LOCAL, "Disconnected"))
        {
            log_w("bt-mgr: system bus gone -- waiting for it to come back");
            busLost = true;
            bluezDown();
            return;
        }
        if (dbus_message_is_signal(msg, IFACE_DBUS, "NameOwnerChanged"))
        {
            const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
            if (dbus_message_get_args(msg, nullptr, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING,
                                      &oldOwner, DBUS_TYPE_STRING, &newOwner, DBUS_TYPE_INVALID) &&
                !strcmp(name, BLUEZ))
            {
                if (*newOwner)
                    bluezUp(newOwner);
                else
                    bluezDown();
            }
            return;
        }
        const char *sender = dbus_message_get_sender(msg);
        if (!sender || bluezOwner.empty() || bluezOwner != sender)
            return;

        DBusMessageIter it;
        if (dbus_message_is_signal(msg, IFACE_OM, "InterfacesAdded"))
        {
            const char *path = nullptr;
            if (!dbus_message_iter_init(msg, &it) ||
                dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH)
                return;
            dbus_message_iter_get_basic(&it, &path);
            dbus_message_iter_next(&it);
            const std::string p = path;
            const bool hadAdapter = !adapterPath.empty();
            forEachInterface(&it, [&](const char *iface, DBusMessageIter *props) {
                interfaceAdded(p, iface, props);
            });
            if (!hadAdapter && !adapterPath.empty() && cacheReady)
            {
                log_i("bt-mgr: adapter %s appeared", adapterPath.c_str());
                applyPolicy();
            }
        }
        else if (dbus_message_is_signal(msg, IFACE_OM, "InterfacesRemoved"))
        {
            const char *path = nullptr;
            if (!dbus_message_iter_init(msg, &it) ||
                dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH)
                return;
            dbus_message_iter_get_basic(&it, &path);
            dbus_message_iter_next(&it);
            const std::string p = path;
            forEachString(&it, [&](const char *iface) { interfaceRemoved(p, iface); });
        }
        else if (dbus_message_is_signal(msg, IFACE_PROPS, "PropertiesChanged"))
        {
            const char *iface = nullptr;
            const char *path = dbus_message_get_path(msg);
            if (!path || !dbus_message_iter_init(msg, &it) ||
                dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)
                return;
            dbus_message_iter_get_basic(&it, &iface);
            dbus_message_iter_next(&it);
            const std::string p = path;
            if (!strcmp(iface, IFACE_DEVICE))
            {
                Device *d = find(p);
                if (!d)
                    return; // not announced yet (InterfacesAdded comes first)
                applyDevice(p, *d, &it);
                dbus_message_iter_next(&it);
                invalidateDevice(*d, &it);
            }
            else if (!strcmp(iface, IFACE_BATTERY))
            {
                if (Device *d = find(p))
                    applyBattery(*d, &it);
            }
            else if (!strcmp(iface, IFACE_ADAPTER))
                applyAdapter(p, &it);
        }
    }

    // --- UI commands (run on the manager thread) ------------------------------

    bool ready() const { return cacheReady && !adapterPath.empty(); }

    void startScan()
    {
        if (scanBlocked())
        {
            setStatus("Not during a session", STATUS_SHORT_MS);
            return;
        }
        if (!ready())
        {
            setStatus("Bluetooth is off", STATUS_SHORT_MS);
            return;
        }
        scanUntil = mono_ms() + SCAN_MS;
        if (scanWanted)
            return;
        scanWanted = true;
        scanNumber++;
        // Devices already reported by a discovery still running elsewhere.
        for (auto &kv : devices)
            if (kv.second.hasRssi)
                kv.second.seenScan = scanNumber;
        log_i("bt-mgr: searching for input devices");
        syncScan();
    }

    void stopScan()
    {
        if (!scanWanted)
            return;
        scanWanted = false;
        syncScan();
    }

    // Moves BlueZ towards scanWanted; every reply calls this again, so a
    // change of mind while a call is out is applied right after it.
    void syncScan()
    {
        if (adapterPath.empty())
            return;
        const std::string owner = bluezOwner;
        if (scanWanted && scanState == ScanState::Off)
        {
            scanState = ScanState::Starting;
            // No discovery filter: classic keyboards rarely list the HID UUID
            // in their inquiry data, and any filter also reports
            // non-discoverable LE advertisers.
            adapterCall("StartDiscovery", nullptr, [this, owner](DBusMessage *reply) {
                if (owner != bluezOwner)
                    return; // bluetoothd restarted meanwhile
                // InProgress: this connection's discovery is already running.
                if (!isError(reply) || errorName(reply) == "org.bluez.Error.InProgress")
                    scanState = ScanState::On;
                else
                {
                    log_w("bt-mgr: StartDiscovery failed > %s", errorText(reply).c_str());
                    scanState = ScanState::Off;
                    if (scanWanted)
                    {
                        scanWanted = false;
                        setStatus("Could not search", STATUS_SHORT_MS);
                    }
                }
                syncScan();
            });
        }
        else if (!scanWanted && scanState == ScanState::On)
        {
            scanState = ScanState::Stopping;
            adapterCall("StopDiscovery", nullptr, [this, owner](DBusMessage *reply) {
                if (owner != bluezOwner)
                    return;
                // Failed ("No discovery started") and NotReady: not running either.
                const std::string err = isError(reply) ? errorName(reply) : "";
                if (!err.empty() && err != "org.bluez.Error.Failed" &&
                    err != "org.bluez.Error.NotReady")
                    log_w("bt-mgr: StopDiscovery failed > %s", errorText(reply).c_str());
                scanState = ScanState::Off;
                syncScan();
            });
        }
    }

    void doPair(const std::string &path)
    {
        Device *d = find(path);
        if (!d || !ready())
            return;
        if (pairPending)
        {
            setStatus("Busy - wait for the current pairing", STATUS_SHORT_MS);
            return;
        }
        // Quiet radio; the kernel would cancel the inquiry for the page anyway.
        stopScan();
        target = path;
        pairPending = true;
        userCancelled = false;
        const uint32_t seq = ++pairSeq;
        const std::string name = displayName(*d);
        log_i("bt-mgr: pairing %s (%s)", path.c_str(), name.c_str());
        deviceCall(path, "Pair", PAIR_CALL_MS, [this, seq, path, name](DBusMessage *reply) {
            if (seq == pairSeq && pairPending)
                onPairReply(path, name, reply);
        });
    }

    void onPairReply(const std::string &path, const std::string &name, DBusMessage *reply)
    {
        pairPending = false;
        const std::string err = isError(reply) ? errorName(reply) : "";
        // A stored key is a finished pairing, even when the service search
        // behind Pair() never answered.
        const Device *d = find(path);
        const bool ok = err.empty() || err == "org.bluez.Error.AlreadyExists" ||
                        (d && (d->bonded || d->paired));
        if (userCancelled)
        {
            target.clear();
            if (promptPath == path && promptFor != PromptFor::None)
                closePrompt(true);
            if (ok)
            {
                // Cancel lost the race against the key exchange (and its
                // CancelPairing may already have dropped the key in the
                // controller): remove the half-made bond.
                log_i("bt-mgr: pairing %s cancelled after the key exchange -- removing it",
                      name.c_str());
                adapterCall("RemoveDevice", path.c_str(), logFailure("RemoveDevice"));
            }
            setStatus("Pairing cancelled", STATUS_LONG_MS);
            return;
        }
        if (ok)
        {
            log_i("bt-mgr: paired %s", name.c_str());
            targetUntil = mono_ms() + TARGET_GRACE_MS;
            // Trusted: classic reconnects skip authorization, and LE keeps
            // auto-connect across a Disconnect().
            setProp(path, IFACE_DEVICE, "Trusted", true, logFailure("Trusted=true"));
            if (promptPath == path && promptFor != PromptFor::None)
                closePrompt(true);
            connectAfterPair = path;
            connectAfterPairAt = mono_ms() + CONNECT_AFTER_PAIR_MS;
            setStatus("Paired " + name, STATUS_SHORT_MS);
            return;
        }
        log_w("bt-mgr: pairing %s failed > %s", name.c_str(), errorText(reply).c_str());
        target.clear();
        if (promptPath == path && promptFor != PromptFor::None)
            closePrompt(true);
        std::string why;
        if (err == "org.bluez.Error.AuthenticationFailed")
            why = " - wrong code";
        else if (err == "org.bluez.Error.AuthenticationRejected")
            why = " - rejected";
        else if (err == "org.bluez.Error.AuthenticationTimeout" ||
                 err == "org.freedesktop.DBus.Error.NoReply")
            why = " - timed out";
        else if (err == "org.bluez.Error.ConnectionAttemptFailed")
            why = " - not reachable";
        else if (err == "org.bluez.Error.InProgress")
            why = " - busy";
        setStatus("Pairing failed" + why, STATUS_LONG_MS);
    }

    void doConnect(const std::string &path, bool on)
    {
        if (!find(path) || !ready())
            return;
        // A Disconnect would cancel the service search behind Pair(), which
        // then never answers.
        if (pairPending && path == target)
        {
            setStatus("Busy - wait for the current pairing", STATUS_SHORT_MS);
            return;
        }
        if (!on)
        {
            deviceCall(path, "Disconnect", CALL_MS, logFailure("Disconnect"));
            return;
        }
        connecting = path;
        deviceCall(path, "Connect", CONNECT_CALL_MS, [this, path](DBusMessage *reply) {
            if (connecting == path)
                connecting.clear();
            if (isError(reply))
            {
                log_w("bt-mgr: connect %s failed > %s", path.c_str(), errorText(reply).c_str());
                // Most keyboards only connect themselves, from a key press.
                setStatus("Could not connect - press a key on the device", STATUS_LONG_MS);
            }
        });
    }

    void doForget(const std::string &path)
    {
        const Device *d = find(path);
        if (!d)
            return;
        Prompt p;
        p.kind = PromptKind::Confirm;
        p.title = "Forget " + displayName(*d) + "?";
        p.hint = "To use it again, pair it again from Add device.";
        p.acceptLabel = "Forget";
        openPrompt(PromptFor::Forget, path, std::move(p));
    }

    void doPromptAccept()
    {
        const std::string path = promptPath;
        switch (promptFor)
        {
        case PromptFor::Confirm:
            if (deferred)
            {
                replyEmpty(deferred);
                dbus_message_unref(deferred);
                deferred = nullptr;
            }
            closePrompt(false);
            break;
        case PromptFor::Forget:
        {
            closePrompt(false);
            if (!ready())
                break;
            const std::string name = nameOf(path);
            if (target == path)
                abandonPairing(); // RemoveDevice leaves a pending Pair() unanswered
            if (connectAfterPair == path)
                connectAfterPair.clear();
            log_i("bt-mgr: forgetting %s (%s)", path.c_str(), name.c_str());
            // Disconnects, unpairs, deletes the stored key; InterfacesRemoved
            // then drops it from the list.
            adapterCall("RemoveDevice", path.c_str(), [this, name](DBusMessage *reply) {
                if (isError(reply))
                {
                    log_w("bt-mgr: RemoveDevice failed > %s", errorText(reply).c_str());
                    setStatus("Could not forget " + name, STATUS_LONG_MS);
                }
                else
                    setStatus("Forgot " + name, STATUS_SHORT_MS);
            });
            break;
        }
        default:
            break;
        }
    }

    void doPromptCancel()
    {
        switch (promptFor)
        {
        case PromptFor::Code:
            if (pairPending && promptPath == target)
            {
                // onPairReply() settles it: an error, or -- when the key got
                // stored first -- a bond that it removes again. CancelPairing
                // only before that: once the key is stored it no longer
                // cancels but silently unpairs in the controller.
                userCancelled = true;
                const Device *d = find(target);
                if (!d || (!d->bonded && !d->paired))
                    deviceCall(target, "CancelPairing", CALL_MS, logFailure("CancelPairing"));
            }
            closePrompt(false);
            break;
        case PromptFor::Confirm:
            if (deferred)
                userCancelled = true;
            closePrompt(true); // "no" to the pending confirmation
            break;
        case PromptFor::Forget:
            closePrompt(false);
            break;
        default:
            break;
        }
    }

    // --- loop -----------------------------------------------------------------

    void drainCommands()
    {
        std::vector<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> lock(cmdMutex);
            todo.swap(commands);
        }
        for (auto &fn : todo)
            fn();
    }

    void dispatchAll()
    {
        while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
            ;
    }

    void runDeadlines()
    {
        const uint32_t now = mono_ms();
        if (scanWanted && due(now, scanUntil))
        {
            log_i("bt-mgr: search finished");
            stopScan();
        }
        if (!connectAfterPair.empty() && due(now, connectAfterPairAt))
        {
            const std::string path = connectAfterPair;
            connectAfterPair.clear();
            // Harmless when HID is already up (classic keyboards usually
            // connect themselves); LE devices just re-arm auto-connect.
            deviceCall(path, "Connect", CONNECT_CALL_MS, [path](DBusMessage *reply) {
                if (isError(reply))
                    log_i("bt-mgr: connect after pairing %s > %s", path.c_str(),
                          errorText(reply).c_str());
            });
        }
        if (!transient.empty() && due(now, transientUntil))
            transient.clear();
        if (!target.empty() && !pairPending && due(now, targetUntil))
            target.clear();
    }

    int nextTimeout() const
    {
        const uint32_t now = mono_ms();
        int timeout = -1;
        auto consider = [&](uint32_t deadline) {
            const int32_t left = (int32_t)(deadline - now);
            const int ms = left < 0 ? 0 : left;
            if (timeout < 0 || ms < timeout)
                timeout = ms;
        };
        if (scanWanted)
            consider(scanUntil);
        if (!connectAfterPair.empty())
            consider(connectAfterPairAt);
        if (!transient.empty())
            consider(transientUntil);
        if (!target.empty() && !pairPending)
            consider(targetUntil);
        if (!conn && busRetryArmed)
            consider(busRetryAt);
        for (const TimeoutSlot &s : timeouts)
            if (dbus_timeout_get_enabled(s.timeout))
                consider(s.deadline);
        return timeout;
    }

    Snapshot build() const
    {
        Snapshot s;
        std::vector<std::pair<uint32_t, const std::pair<const std::string, Device> *>> order;
        const std::string prefix = adapterPath + "/";
        if (ready())
            for (const auto &kv : devices)
                if (kv.first.compare(0, prefix.size(), prefix) == 0 && isHid(kv.second))
                    order.push_back({kv.second.order, &kv});
        std::sort(order.begin(), order.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        int connected = 0;
        for (const auto &o : order)
        {
            const std::string &path = o.second->first;
            const Device &d = o.second->second;
            DeviceRow row;
            row.id = path;
            row.name = displayName(d);
            if (d.paired || d.bonded)
            {
                row.connected = d.connected;
                row.detail = d.connected ? "Connected" : "Not connected";
                if (d.battery >= 0)
                    row.detail += "  " + std::to_string(d.battery) + "%";
                connected += d.connected;
                s.paired.push_back(std::move(row));
            }
            else if (scanNumber && d.seenScan == scanNumber)
            {
                row.detail = kindText(kindOf(d));
                if (d.hasRssi)
                    row.detail += d.rssi >= -60 ? "  strong" : d.rssi >= -75 ? "  good" : "  weak";
                s.found.push_back(std::move(row));
            }
        }

        s.scanning = scanWanted;
        s.scanAllowed = !scanBlocked();
        s.phoneConnected = ready() && phoneConnected();
        if (!transient.empty())
            s.status = transient;
        else if (pairPending)
            s.status = "Pairing " + nameOf(target) + "...";
        else if (!connecting.empty())
            s.status = "Connecting " + nameOf(connecting) + "...";

        if (!ready())
            s.summary = "Off";
        else if (connected)
            s.summary = std::to_string(connected) + " connected";
        else if (!s.paired.empty())
            s.summary = std::to_string(s.paired.size()) + " paired";
        else
            s.summary = "None";
        s.prompt = prompt;
        return s;
    }

    void publish()
    {
        Snapshot s = build();
        if (sameSnapshot(s, snap)) // only this thread writes `snap`
            return;
        {
            std::lock_guard<std::mutex> lock(snapMutex);
            snap = std::move(s);
        }
        owner._generation.fetch_add(1, std::memory_order_acq_rel);
    }

    void handleTimeouts()
    {
        const uint32_t now = mono_ms();
        std::vector<DBusTimeout *> fire;
        for (const TimeoutSlot &s : timeouts)
            if (dbus_timeout_get_enabled(s.timeout) && due(now, s.deadline))
                fire.push_back(s.timeout);
        for (DBusTimeout *t : fire)
        {
            auto it = std::find_if(timeouts.begin(), timeouts.end(),
                                   [t](const TimeoutSlot &s) { return s.timeout == t; });
            if (it == timeouts.end())
                continue; // removed by an earlier handler
            it->deadline = now + (uint32_t)dbus_timeout_get_interval(t);
            dbus_timeout_handle(t);
        }
    }

    // Is bluetoothd running now? Later starts and stops arrive as
    // NameOwnerChanged (matched in start()).
    void queryBluez()
    {
        DBusMessage *msg = dbus_message_new_method_call(IFACE_DBUS, "/org/freedesktop/DBus",
                                                        IFACE_DBUS, "GetNameOwner");
        if (msg)
        {
            const char *name = BLUEZ;
            dbus_message_append_args(msg, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
        }
        call(msg, CALL_MS, [this](DBusMessage *reply) {
            const char *owner = nullptr;
            if (!isError(reply) &&
                dbus_message_get_args(reply, nullptr, DBUS_TYPE_STRING, &owner, DBUS_TYPE_INVALID) &&
                owner && *owner)
                bluezUp(owner);
            else if (bluezOwner.empty())
                log_w("bt-mgr: bluetoothd is not running -- waiting for it");
        });
    }

    // --- the system bus coming and going ---------------------------------------

    void watchBusDir()
    {
        busWd = inotify_add_watch(inotifyFd, BUS_DIR, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
        if (busWd >= 0)
            busTry = true; // the socket may be in it already
    }

    void onBusInotify()
    {
        alignas(struct inotify_event) char buf[512];
        for (;;)
        {
            const ssize_t n = read(inotifyFd, buf, sizeof(buf));
            if (n <= 0)
                return;
            for (ssize_t off = 0; off < n;)
            {
                const struct inotify_event *e = (const struct inotify_event *)(buf + off);
                off += (ssize_t)(sizeof(struct inotify_event) + e->len);
                if (e->mask & IN_IGNORED)
                {
                    if (e->wd == busWd)
                        busWd = -1; // BUS_DIR removed; RUN_DIR's watch sees it return
                    continue;
                }
                if (e->len == 0)
                    continue;
                if (e->wd == runWd && !strcmp(e->name, "dbus"))
                    watchBusDir();
                else if (e->wd == busWd && !strcmp(e->name, BUS_SOCKET_NAME))
                {
                    busTry = true;
                    busRetries = 0;
                }
            }
        }
    }

    bool connectBus()
    {
        busTry = false;
        busRetryArmed = false;
        DBusError err;
        dbus_error_init(&err);
        // Private: a `bt off` (dbus-daemon gone) must not exit the app, and one
        // connection = one agent registration that this object fully controls.
        DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
        if (!c)
        {
            if (access(BUS_SOCKET, F_OK) == 0 && busRetries < BUS_RETRIES)
            {
                busRetries++;
                busRetryArmed = true;
                busRetryAt = mono_ms() + BUS_RETRY_MS;
            }
            if (!busWaitLogged)
                log_i("bt-mgr: no system bus yet (%s) -- waiting for it",
                      dbus_error_is_set(&err) ? err.message : "?");
            busWaitLogged = true;
            dbus_error_free(&err);
            return false;
        }
        if (busWaitLogged)
            log_i("bt-mgr: system bus is up");
        busWaitLogged = false;
        busRetries = 0;
        conn = c;
        dbus_connection_set_exit_on_disconnect(conn, FALSE);
        dbus_connection_set_watch_functions(conn, &Impl::addWatch, &Impl::removeWatch,
                                            &Impl::toggleWatch, this, nullptr);
        dbus_connection_set_timeout_functions(conn, &Impl::addTimeout, &Impl::removeTimeout,
                                              &Impl::toggleTimeout, this, nullptr);
        static const DBusObjectPathVTable agentVtable = {nullptr, &Impl::agentMessage, nullptr,
                                                         nullptr, nullptr, nullptr};
        dbus_connection_register_object_path(conn, AGENT_PATH, &agentVtable, this);
        dbus_connection_add_filter(conn, &Impl::filter, this, nullptr);
        // No error argument: these go out without waiting for the replies.
        dbus_bus_add_match(conn,
                           "type='signal',sender='org.bluez',path='/',"
                           "interface='org.freedesktop.DBus.ObjectManager'",
                           nullptr);
        dbus_bus_add_match(conn,
                           "type='signal',sender='org.bluez',"
                           "interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
                           "path_namespace='/org/bluez'",
                           nullptr);
        dbus_bus_add_match(conn,
                           "type='signal',sender='org.freedesktop.DBus',"
                           "interface='org.freedesktop.DBus',member='NameOwnerChanged',"
                           "arg0='org.bluez'",
                           nullptr);
        queryBluez();
        return true;
    }

    // Leaves the bus -- politely when the app stops (a lost bus has nobody to
    // tell) -- and forgets everything BlueZ said.
    void disconnectBus()
    {
        if (!busLost && dbus_connection_get_is_connected(conn))
        {
            auto send = [this](DBusMessage *msg) {
                if (!msg)
                    return;
                dbus_message_set_no_reply(msg, TRUE);
                dbus_message_set_auto_start(msg, FALSE);
                dbus_connection_send(conn, msg, nullptr);
                dbus_message_unref(msg);
            };
            // (Closing the connection would end our discovery as well.)
            if (scanState != ScanState::Off && !adapterPath.empty())
                send(bluez(adapterPath, IFACE_ADAPTER, "StopDiscovery"));
            if (agentRegistered)
            {
                DBusMessage *msg = bluez("/org/bluez", IFACE_AGENT_MANAGER, "UnregisterAgent");
                if (msg)
                {
                    const char *path = AGENT_PATH;
                    dbus_message_append_args(msg, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
                }
                send(msg);
            }
            closePrompt(true);
            dbus_connection_flush(conn);
        }
        closePrompt(false);
        for (DBusPendingCall *pc : pending)
        {
            dbus_pending_call_cancel(pc);
            dbus_pending_call_unref(pc);
        }
        pending.clear();
        dbus_connection_remove_filter(conn, &Impl::filter, this);
        dbus_connection_unregister_object_path(conn, AGENT_PATH);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        conn = nullptr;
        watches.clear();
        timeouts.clear();
        busLost = false;
        resetBluez();
    }

    void run()
    {
        setThreadName("bt-manager");
        std::vector<struct pollfd> fds;
        std::vector<DBusWatch *> polled;
        while (running.load(std::memory_order_acquire))
        {
            if (!conn && (busTry || (busRetryArmed && due(mono_ms(), busRetryAt))))
                connectBus();
            // Signals first: a command must see the state BlueZ already
            // reported (e.g. a Cancel queued behind the key being stored).
            if (conn)
                dispatchAll();
            if (conn && busLost)
            {
                disconnectBus();
                // The daemon may still be there (it dropped only us): try
                // again shortly; otherwise its socket coming back wakes us.
                busRetryArmed = true;
                busRetryAt = mono_ms() + BUS_RETRY_MS;
                busWaitLogged = true; // the loss is logged already
            }
            // Without a bus, commands still run and find Bluetooth off.
            drainCommands();
            runDeadlines();
            publish();
            if (!running.load(std::memory_order_acquire))
                break;

            fds.clear();
            polled.clear();
            fds.push_back({wakeFd, POLLIN, 0});
            const bool waitingForBus = !conn && inotifyFd >= 0;
            if (waitingForBus)
                fds.push_back({inotifyFd, POLLIN, 0});
            const size_t first = fds.size();
            for (DBusWatch *w : watches)
            {
                if (!dbus_watch_get_enabled(w))
                    continue;
                const unsigned flags = dbus_watch_get_flags(w);
                short events = 0;
                if (flags & DBUS_WATCH_READABLE)
                    events |= POLLIN;
                if (flags & DBUS_WATCH_WRITABLE)
                    events |= POLLOUT;
                fds.push_back({dbus_watch_get_unix_fd(w), events, 0});
                polled.push_back(w);
            }
            if (poll(fds.data(), fds.size(), nextTimeout()) < 0)
            {
                if (errno == EINTR)
                    continue;
                log_e("bt-mgr: poll failed (%s)", strerror(errno));
                break;
            }
            if (fds[0].revents & POLLIN)
            {
                uint64_t v;
                (void)!read(wakeFd, &v, sizeof(v));
            }
            if (waitingForBus && (fds[1].revents & POLLIN))
                onBusInotify();
            for (size_t i = 0; i < polled.size(); i++)
            {
                const short re = fds[first + i].revents;
                if (!re || std::find(watches.begin(), watches.end(), polled[i]) == watches.end())
                    continue;
                unsigned flags = 0;
                if (re & POLLIN)
                    flags |= DBUS_WATCH_READABLE;
                if (re & POLLOUT)
                    flags |= DBUS_WATCH_WRITABLE;
                if (re & POLLHUP)
                    flags |= DBUS_WATCH_HANGUP;
                if (re & (POLLERR | POLLNVAL))
                    flags |= DBUS_WATCH_ERROR;
                dbus_watch_handle(polled[i], flags);
            }
            handleTimeouts();
        }
        if (conn)
            disconnectBus();
    }
};

Manager &Manager::instance()
{
    static Manager manager;
    return manager;
}

Manager::Manager() : _impl(new Impl(*this)) {}

Manager::~Manager()
{
    stop();
}

bool Manager::start(bool phonePairing)
{
    Impl &m = *_impl;
    if (m.thread.joinable())
        return true;
    dbus_threads_init_default();
    m.wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (m.wakeFd < 0)
    {
        log_e("bt-mgr: eventfd failed (%s) -- Bluetooth input off", strerror(errno));
        return false;
    }
    // The bus socket's directory, and its parent for the directory itself:
    // both can be missing until `bt on` runs.
    m.inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m.inotifyFd >= 0)
    {
        m.runWd = inotify_add_watch(m.inotifyFd, RUN_DIR, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
        m.watchBusDir();
        if (m.runWd < 0 && m.busWd < 0)
        {
            log_w("bt-mgr: cannot watch %s (%s) -- a system bus started later is missed",
                  BUS_DIR, strerror(errno));
            ::close(m.inotifyFd);
            m.inotifyFd = -1;
        }
    }
    m.busTry = true;
    m.phonePairing = phonePairing;
    m.running.store(true, std::memory_order_release);
    m.thread = std::thread([&m] { m.run(); });
    return true;
}

void Manager::stop()
{
    Impl &m = *_impl;
    if (!m.thread.joinable())
        return;
    m.running.store(false, std::memory_order_release);
    m.wake();
    m.thread.join(); // leaves the bus on its way out
    if (m.inotifyFd >= 0)
        ::close(m.inotifyFd);
    ::close(m.wakeFd);
    log_i("bt-mgr: stopped");

    // A later start() begins from a clean slate; the UI keeps polling the
    // (now empty) snapshot.
    _impl.reset(new Impl(*this));
    _generation.fetch_add(1, std::memory_order_acq_rel);
}

void Manager::scan(bool on)
{
    Impl *m = _impl.get();
    m->queue([m, on] {
        if (on)
            m->startScan();
        else
            m->stopScan();
    });
}

void Manager::pair(const std::string &id)
{
    Impl *m = _impl.get();
    m->queue([m, id] { m->doPair(id); });
}

void Manager::connect(const std::string &id, bool on)
{
    Impl *m = _impl.get();
    m->queue([m, id, on] { m->doConnect(id, on); });
}

void Manager::forget(const std::string &id)
{
    Impl *m = _impl.get();
    m->queue([m, id] { m->doForget(id); });
}

void Manager::promptAccept()
{
    Impl *m = _impl.get();
    m->queue([m] { m->doPromptAccept(); });
}

void Manager::promptCancel()
{
    Impl *m = _impl.get();
    m->queue([m] { m->doPromptCancel(); });
}

void Manager::screenVisible(bool visible)
{
    Impl *m = _impl.get();
    m->queue([m, visible] {
        if (!visible)
            m->stopScan();
    });
}

void Manager::setSessionActive(bool active)
{
    Impl *m = _impl.get();
    // Called on every state change: only a different value is worth a wake-up.
    const int8_t value = active ? 1 : 0;
    if (m->sessionSent.exchange(value) == value)
        return;
    m->queue([m, active] {
        m->sessionActive = active;
        if (active)
            m->stopScan();
    });
}

Snapshot Manager::snapshot() const
{
    std::lock_guard<std::mutex> lock(_impl->snapMutex);
    return _impl->snap;
}
} // namespace bt

#endif /* USE_BLUETOOTH */
