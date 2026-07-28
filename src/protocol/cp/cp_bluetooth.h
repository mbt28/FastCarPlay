#ifndef SRC_PROTOCOL_CP_CP_BLUETOOTH
#define SRC_PROTOCOL_CP_CP_BLUETOOTH

#ifdef USE_CP_WIRELESS

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <dbus/dbus.h>

#include "cp_carplay_msg.h"

namespace cp_auth_setup { class MfiSigner; }

// Bluetooth bootstrap for wireless CarPlay via BlueZ over D-Bus. Powers the
// adapter, makes it pairable/discoverable, installs an auto-accept pairing
// agent, and registers the iAP2 + CarPlay RFCOMM profiles with SDP records so
// the iPhone recognises the head unit as a CarPlay accessory (it appears in
// Settings > General > CarPlay). When the phone connects the iAP channel, its
// RFCOMM fd is wrapped in an iAP2 link and driven by CarplaySession, which
// authenticates with the MFi chip and hands the phone the Wi-Fi credentials.
// After that the phone joins the AP and connects the :7000 CarPlay server.
// Mirrors aa_bluetooth; requires a running bluetoothd + dbus + root.
namespace cp_bt
{
struct Config
{
    std::string adapter = "/org/bluez/hci0";
    std::string alias = "FastCarPlay";
    cp_carplay::WirelessSession wifi;       // credentials handed to the phone
    cp_carplay::AccessoryIdentity identity; // our iAP2 identity (bt MAC filled in)
    cp_auth_setup::MfiSigner *signer = nullptr;
};

class CpBluetooth
{
public:
    ~CpBluetooth();

    bool start(const Config &cfg);
    void stop();

private:
    static DBusHandlerResult agentMessage(DBusConnection *c, DBusMessage *m, void *user);
    static DBusHandlerResult profileMessage(DBusConnection *c, DBusMessage *m, void *user);

    bool setupAdapter();
    bool adapterAddress(cp_carplay::Bytes &mac);
    bool registerAgent();
    bool registerProfile(const char *path, const char *uuid, bool server, uint16_t channel,
                         const char *serviceRecord, const char *name);
    bool setAdapterProp(const char *prop, int type, const void *value);
    void onNewConnection(int fd);
    void dispatchLoop();

    DBusConnection *_conn = nullptr;
    Config _cfg;
    std::thread _dispatch;
    std::vector<std::thread> _sessions;
    std::atomic<bool> _running{false};
};
} // namespace cp_bt

#endif /* USE_CP_WIRELESS */
#endif /* SRC_PROTOCOL_CP_CP_BLUETOOTH */
