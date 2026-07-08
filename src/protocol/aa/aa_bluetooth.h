#ifndef SRC_PROTOCOL_AA_AA_BLUETOOTH
#define SRC_PROTOCOL_AA_AA_BLUETOOTH

#ifdef USE_AA_WIRELESS

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "protocol/aa/aa_aaw.h"

// Bluetooth bootstrap for wireless Android Auto via BlueZ over D-Bus. Powers
// the adapter, makes it pairable/discoverable, installs an auto-accept pairing
// agent, and registers the Android Auto RFCOMM profile (plus a fake hands-free
// profile so the phone offers wireless AA). When the phone connects the AA
// channel, its RFCOMM fd is handed to the aaw handshake, which gives the phone
// the Wi-Fi credentials + TCP endpoint. Requires a running bluetoothd + dbus.
class AaBluetooth
{
public:
    ~AaBluetooth();

    // params carries the AP IP / TCP port / SSID etc. handed to the phone.
    bool start(const aa_aaw::Params &params);
    void stop();

private:
    // D-Bus message handlers for the agent and profile objects (BlueZ calls us).
    static DBusHandlerResult agentMessage(DBusConnection *c, DBusMessage *m, void *user);
    static DBusHandlerResult profileMessage(DBusConnection *c, DBusMessage *m, void *user);

    bool setupAdapter();
    bool registerAgent();
    bool registerProfile(const char *path, const char *uuid, bool server);
    bool setAdapterProp(const char *prop, int type, const void *value);
    void onNewConnection(int fd);
    void dispatchLoop();

    DBusConnection *_conn = nullptr;
    std::string _adapter = "/org/bluez/hci0";
    aa_aaw::Params _params;
    std::thread _dispatch;
    std::vector<std::thread> _handshakes;
    std::atomic<bool> _running{false};
};

#endif /* USE_AA_WIRELESS */
#endif /* SRC_PROTOCOL_AA_AA_BLUETOOTH */
