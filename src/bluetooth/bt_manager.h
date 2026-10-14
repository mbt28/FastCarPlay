#ifndef SRC_BLUETOOTH_BT_MANAGER
#define SRC_BLUETOOTH_BT_MANAGER

// The app's one Bluetooth manager (BlueZ over a PRIVATE system-bus connection,
// own dispatch thread), running in every protocol mode:
//  - the only pairing agent ("DisplayYesNo", default agent): phones still pair
//    without a prompt; HID devices (keyboards, remotes) are accepted only
//    while the user is pairing that very device from the UI, or once bonded
//    and trusted (see docs/bluetooth-input.md for the exact rules);
//  - HID device management for the settings UI: discovery, pair -> trust ->
//    connect, connect/disconnect, forget;
//  - adapter policy: never powers the adapter down (a connected keyboard must
//    survive everything); non-discoverable unless a phone needs to pair.
//
// Threading: every public method is callable from any thread and never blocks
// on D-Bus -- commands are queued for the manager thread. The UI polls
// generation() once per tick and copies snapshot() only when it changed.

#ifdef USE_BLUETOOTH

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bt
{
// One row in a UI device list. All strings are display-ready and restricted to
// printable ASCII (the UI only embeds those glyphs).
struct DeviceRow
{
    std::string id;     // BlueZ object path, e.g. /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF
    std::string name;   // Alias, sanitised; the address when there is no usable name
    std::string detail; // right-hand text: "Connected  80%", "Not connected", "Keyboard"...
    bool connected = false;

    bool operator==(const DeviceRow &o) const
    {
        return id == o.id && name == o.name && detail == o.detail && connected == o.connected;
    }
};

enum class PromptKind : uint8_t
{
    None = 0,
    ShowCode = 1, // a code to type on the device; only Cancel
    Confirm = 2,  // a yes/no question; accept + Cancel
};

struct Prompt
{
    PromptKind kind = PromptKind::None;
    std::string title;       // e.g. "Pair K380"
    std::string code;        // pre-formatted, e.g. "1 2 3 4 5 6"; empty when none
    std::string hint;        // e.g. "Type the code on the keyboard, then press Enter (3 of 6 typed)"
    std::string acceptLabel; // Confirm only: "Pair", "Forget"...
};

struct Snapshot
{
    std::vector<DeviceRow> paired; // bonded HID devices, first-seen order (never re-sorted)
    std::vector<DeviceRow> found;  // discovered, unpaired HID devices, first-seen order
    bool scanning = false;
    bool scanAllowed = true;     // false while a wireless phone session is up
    bool phoneConnected = false; // a phone (any non-input device) is connected
    std::string status;      // header suffix: "Pairing K380...", "Pairing failed - wrong code"
    std::string summary;     // Wireless > Bluetooth row value: "Off", "None", "1 connected", "2 paired"
    Prompt prompt;
};

class Manager
{
public:
    static Manager &instance();

    // Starts the manager thread, which opens the private bus connection,
    // registers the agent and seeds the device cache. Without a system bus
    // yet (on the image dbus-daemon runs only while Bluetooth is on) it waits
    // for one, and it reconnects whenever the bus or bluetoothd comes back;
    // meanwhile Bluetooth simply shows as off. phonePairing: the selected
    // protocol pairs a phone over Bluetooth (wireless Android Auto /
    // CarPlay), whose module keeps the adapter discoverable; otherwise the
    // manager makes it non-discoverable. Returns false (and logs) only when
    // the thread cannot be set up.
    bool start(bool phonePairing);
    // Stops discovery, releases the agent, closes the bus, joins the thread.
    void stop();

    // --- UI commands (any thread; queued, never block) ------------------------
    void scan(bool on); // discovery for at most 30 s, HID devices only
    void pair(const std::string &id);
    void connect(const std::string &id, bool on);
    void forget(const std::string &id); // asks a Confirm prompt first
    void promptAccept();
    void promptCancel();
    // Whether one of the Bluetooth screens is on screen. Leaving them stops
    // discovery (and discovery never runs while the UI is hidden).
    void screenVisible(bool visible);
    // A wireless phone session is running: discovery is refused and stopped,
    // so inquiry never competes with it on the shared radio.
    void setSessionActive(bool active);

    // --- UI state ------------------------------------------------------------
    // Bumped on every change of the snapshot; one atomic load per UI tick.
    uint32_t generation() const { return _generation.load(std::memory_order_acquire); }
    Snapshot snapshot() const;

    struct Impl;

private:
    Manager();
    ~Manager();
    Manager(const Manager &) = delete;
    Manager &operator=(const Manager &) = delete;

    std::unique_ptr<Impl> _impl;
    std::atomic<uint32_t> _generation{0};
};
} // namespace bt

#endif /* USE_BLUETOOTH */
#endif /* SRC_BLUETOOTH_BT_MANAGER */
