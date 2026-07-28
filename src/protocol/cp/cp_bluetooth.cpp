#include "cp_bluetooth.h"

#ifdef USE_CP_WIRELESS

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "common/logger.h"
#include "cp_carplay_session.h"
#include "cp_iap2_link.h"

#define BLUEZ "org.bluez"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define PROFILE_MGR_IFACE "org.bluez.ProfileManager1"
#define AGENT_MGR_IFACE "org.bluez.AgentManager1"

#define AGENT_PATH "/fcp/cp/agent"
#define IAP_SERVER_PATH "/fcp/cp/iap_server"
#define IAP_CLIENT_PATH "/fcp/cp/iap_client"
#define CARPLAY_PATH "/fcp/cp/carplay"

// iAP2-over-Bluetooth server/client UUIDs + the CarPlay service UUID. The phone
// offers wireless CarPlay when it sees the CarPlay service record advertised.
#define IAP_SERVER_UUID "00000000-deca-fade-deca-deafdecacaff"
#define IAP_CLIENT_UUID "00000000-deca-fade-deca-deafdecacafe"
#define CARPLAY_UUID "ec884348-cd41-40a2-9727-575d50bf1fd3"

#define IAP_CHANNEL 3
#define CARPLAY_CHANNEL 4

// SDP records so bluetoothd publishes the services over the air (a Role+Channel
// registration alone is invisible to the phone's SDP query on some bluetoothd
// versions). Mirrors LIVI cp/iap2/transport/bluetooth.py.
static const char *IAP_SDP_RECORD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<record>"
    "  <attribute id=\"0x0001\"><sequence><uuid value=\"" IAP_SERVER_UUID "\" /></sequence></attribute>"
    "  <attribute id=\"0x0004\"><sequence>"
    "    <sequence><uuid value=\"0x0100\" /></sequence>"
    "    <sequence><uuid value=\"0x0003\" /><uint8 value=\"0x03\" /></sequence>"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0005\"><sequence><uuid value=\"0x1002\" /></sequence></attribute>"
    "  <attribute id=\"0x0008\"><uint8 value=\"0xff\" /></attribute>"
    "  <attribute id=\"0x0009\"><sequence>"
    "    <sequence><uuid value=\"0x1101\" /><uint16 value=\"0x0100\" /></sequence>"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0100\"><text value=\"Wireless iAP\" /></attribute>"
    "</record>";

static const char *CARPLAY_SDP_RECORD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<record>"
    "  <attribute id=\"0x0001\"><sequence><uuid value=\"" CARPLAY_UUID "\" /></sequence></attribute>"
    "  <attribute id=\"0x0004\"><sequence>"
    "    <sequence><uuid value=\"0x0100\" /></sequence>"
    "    <sequence><uuid value=\"0x0003\" /><uint8 value=\"0x04\" /></sequence>"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0005\"><sequence><uuid value=\"0x1002\" /></sequence></attribute>"
    "  <attribute id=\"0x0008\"><uint8 value=\"0xff\" /></attribute>"
    "  <attribute id=\"0x0009\"><sequence>"
    "    <sequence><uuid value=\"0x1101\" /><uint16 value=\"0x0100\" /></sequence>"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0100\"><text value=\"CarPlay\" /></attribute>"
    "</record>";

namespace cp_bt
{
static void emptyReply(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply)
    {
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
    }
}

CpBluetooth::~CpBluetooth() { stop(); }

bool CpBluetooth::setAdapterProp(const char *prop, int type, const void *value)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, _cfg.adapter.c_str(), PROPS_IFACE, "Set");
    if (!msg)
        return false;

    const char *iface = ADAPTER_IFACE;
    DBusMessageIter it, var;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
    char sig[2] = {(char)type, 0};
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, value);
    dbus_message_iter_close_container(&it, &var);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err))
    {
        log_w("cp-bt: set %s failed > %s", prop, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (reply)
        dbus_message_unref(reply);
    return true;
}

// Read the adapter's Bluetooth address ("AA:BB:CC:DD:EE:FF") into 6 raw bytes.
bool CpBluetooth::adapterAddress(cp_carplay::Bytes &mac)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, _cfg.adapter.c_str(), PROPS_IFACE, "Get");
    if (!msg)
        return false;
    const char *iface = ADAPTER_IFACE, *prop = "Address";
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err))
    {
        dbus_error_free(&err);
        return false;
    }
    const char *addr = nullptr;
    DBusMessageIter it, var;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT)
    {
        dbus_message_iter_recurse(&it, &var);
        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&var, &addr);
    }
    bool ok = false;
    if (addr)
    {
        unsigned b[6];
        if (sscanf(addr, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6)
        {
            mac.assign(6, 0);
            for (int i = 0; i < 6; i++)
                mac[i] = (uint8_t)b[i];
            ok = true;
        }
    }
    dbus_message_unref(reply);
    return ok;
}

bool CpBluetooth::setupAdapter()
{
    if (system("rfkill unblock bluetooth 2>/dev/null") != 0)
        log_v("cp-bt: rfkill unblock returned non-zero (may be fine)");
    usleep(300 * 1000);

    dbus_bool_t yes = TRUE;
    dbus_uint32_t zero = 0;
    const char *alias = _cfg.alias.c_str();

    setAdapterProp("Powered", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("Alias", DBUS_TYPE_STRING, &alias);
    setAdapterProp("Pairable", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("PairableTimeout", DBUS_TYPE_UINT32, &zero);
    setAdapterProp("Discoverable", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("DiscoverableTimeout", DBUS_TYPE_UINT32, &zero);
    log_i("cp-bt: adapter '%s' powered, pairable + discoverable", alias);
    return true;
}

bool CpBluetooth::registerAgent()
{
    static DBusObjectPathVTable vtable;
    vtable.message_function = &CpBluetooth::agentMessage;
    vtable.unregister_function = nullptr;
    if (!dbus_connection_register_object_path(_conn, AGENT_PATH, &vtable, this))
    {
        log_e("cp-bt: can't register agent object");
        return false;
    }

    const char *path = AGENT_PATH;
    const char *cap = "DisplayYesNo";
    for (const char *method : {"RegisterAgent", "RequestDefaultAgent"})
    {
        DBusMessage *msg = dbus_message_new_method_call(BLUEZ, "/org/bluez", AGENT_MGR_IFACE, method);
        DBusMessageIter it;
        dbus_message_iter_init_append(msg, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
        if (strcmp(method, "RegisterAgent") == 0)
            dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &cap);

        DBusError err;
        dbus_error_init(&err);
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
        dbus_message_unref(msg);
        if (dbus_error_is_set(&err))
        {
            log_w("cp-bt: %s > %s", method, err.message);
            dbus_error_free(&err);
        }
        else if (reply)
            dbus_message_unref(reply);
    }
    log_i("cp-bt: pairing agent registered");
    return true;
}

static void addDictEntry(DBusMessageIter *dict, const char *key, int type, const void *value)
{
    DBusMessageIter entry, var;
    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    char sig[2] = {(char)type, 0};
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, sig, &var);
    dbus_message_iter_append_basic(&var, type, value);
    dbus_message_iter_close_container(&entry, &var);
    dbus_message_iter_close_container(dict, &entry);
}

bool CpBluetooth::registerProfile(const char *path, const char *uuid, bool server, uint16_t channel,
                                  const char *serviceRecord, const char *name)
{
    static DBusObjectPathVTable vtable;
    vtable.message_function = &CpBluetooth::profileMessage;
    vtable.unregister_function = nullptr;
    dbus_connection_register_object_path(_conn, path, &vtable, this);

    DBusMessage *msg =
        dbus_message_new_method_call(BLUEZ, "/org/bluez", PROFILE_MGR_IFACE, "RegisterProfile");
    DBusMessageIter it, dict;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_OBJECT_PATH, &path);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &uuid);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);

    const char *role = server ? "server" : "client";
    addDictEntry(&dict, "Role", DBUS_TYPE_STRING, &role);
    if (name)
        addDictEntry(&dict, "Name", DBUS_TYPE_STRING, &name);
    if (server && channel)
    {
        dbus_uint16_t ch = channel;
        addDictEntry(&dict, "Channel", DBUS_TYPE_UINT16, &ch);
    }
    if (serviceRecord)
        addDictEntry(&dict, "ServiceRecord", DBUS_TYPE_STRING, &serviceRecord);
    dbus_bool_t no = FALSE, yes = TRUE;
    addDictEntry(&dict, "RequireAuthentication", DBUS_TYPE_BOOLEAN, &no);
    addDictEntry(&dict, "RequireAuthorization", DBUS_TYPE_BOOLEAN, &no);
    if (!server)
        addDictEntry(&dict, "AutoConnect", DBUS_TYPE_BOOLEAN, &yes);
    dbus_message_iter_close_container(&it, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err))
    {
        log_w("cp-bt: RegisterProfile %s > %s", uuid, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (reply)
        dbus_message_unref(reply);
    log_i("cp-bt: registered profile %s (%s)", uuid, role);
    return true;
}

DBusHandlerResult CpBluetooth::agentMessage(DBusConnection *conn, DBusMessage *msg, void *)
{
    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    log_d("cp-bt: agent %s", member);
    if (strcmp(member, "RequestConfirmation") == 0 || strcmp(member, "RequestAuthorization") == 0 ||
        strcmp(member, "AuthorizeService") == 0 || strcmp(member, "Cancel") == 0 ||
        strcmp(member, "Release") == 0 || strcmp(member, "DisplayPasskey") == 0 ||
        strcmp(member, "DisplayPinCode") == 0)
    {
        emptyReply(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

DBusHandlerResult CpBluetooth::profileMessage(DBusConnection *conn, DBusMessage *msg, void *user)
{
    CpBluetooth *self = static_cast<CpBluetooth *>(user);
    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "NewConnection") == 0)
    {
        DBusMessageIter it;
        int fd = -1;
        if (dbus_message_iter_init(msg, &it))
        {
            dbus_message_iter_next(&it); // skip object path
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UNIX_FD)
                dbus_message_iter_get_basic(&it, &fd);
        }
        log_i("cp-bt: NewConnection on %s (fd %d)", dbus_message_get_path(msg), fd);
        if (fd >= 0)
            self->onNewConnection(fd);
        emptyReply(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (strcmp(member, "RequestDisconnection") == 0 || strcmp(member, "Release") == 0)
    {
        emptyReply(conn, msg);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

void CpBluetooth::onNewConnection(int fd)
{
    _sessions.emplace_back([this, fd] {
        cp_iap2::Iap2Link link(fd);
        if (link.negotiate(false))
        {
            log_i("cp-bt: iAP2 link up -- running CarPlay session");
            cp_carplay::CarplaySession session(link, *_cfg.signer, _cfg.identity, _cfg.wifi);
            if (session.run())
                log_i("cp-bt: Wi-Fi handoff delivered; phone should join the AP");
            else
                log_w("cp-bt: CarPlay session ended before handoff");
        }
        else
        {
            log_w("cp-bt: iAP2 link negotiation failed");
        }
        ::close(fd);
    });
}

void CpBluetooth::dispatchLoop()
{
    while (_running && dbus_connection_read_write_dispatch(_conn, 200))
        ;
}

bool CpBluetooth::start(const Config &cfg)
{
    _cfg = cfg;

    DBusError err;
    dbus_error_init(&err);
    _conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!_conn)
    {
        log_e("cp-bt: can't connect to system bus > %s", err.message);
        dbus_error_free(&err);
        return false;
    }

    _running = true;
    setupAdapter();

    // Fill in our identity's Bluetooth MAC from the adapter (the phone matches
    // transports by it). Fall back to whatever the caller set.
    cp_carplay::Bytes mac;
    if (adapterAddress(mac))
        _cfg.identity.bluetoothMac = mac;

    registerAgent();
    registerProfile(IAP_SERVER_PATH, IAP_SERVER_UUID, true, IAP_CHANNEL, IAP_SDP_RECORD,
                    "Wireless iAP");
    registerProfile(IAP_CLIENT_PATH, IAP_CLIENT_UUID, false, 0, nullptr, nullptr);
    registerProfile(CARPLAY_PATH, CARPLAY_UUID, true, CARPLAY_CHANNEL, CARPLAY_SDP_RECORD, "CarPlay");

    _dispatch = std::thread(&CpBluetooth::dispatchLoop, this);
    log_i("cp-bt: wireless CarPlay profiles advertised");
    return true;
}

void CpBluetooth::stop()
{
    if (!_running)
        return;
    _running = false;
    if (_dispatch.joinable())
        _dispatch.join();
    for (std::thread &t : _sessions)
        if (t.joinable())
            t.join();
    _sessions.clear();
    if (_conn)
    {
        dbus_connection_unref(_conn);
        _conn = nullptr;
    }
    log_v("cp-bt: stopped");
}
} // namespace cp_bt

#endif /* USE_CP_WIRELESS */
