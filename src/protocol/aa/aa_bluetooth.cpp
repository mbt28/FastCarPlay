#include "aa_bluetooth.h"

#ifdef USE_AA_WIRELESS

#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "common/logger.h"
#include "settings.h"

#define BLUEZ "org.bluez"
#define PROPS_IFACE "org.freedesktop.DBus.Properties"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define PROFILE_MGR_IFACE "org.bluez.ProfileManager1"
#define AGENT_MGR_IFACE "org.bluez.AgentManager1"
#define AGENT_IFACE "org.bluez.Agent1"
#define PROFILE_IFACE "org.bluez.Profile1"

#define AGENT_PATH "/fcp/agent"
#define AA_PROFILE_PATH "/fcp/aa"
#define HFP_PROFILE_PATH "/fcp/hfp"

// The Android Auto wireless RFCOMM UUID and a hands-free UUID (the phone only
// offers wireless AA when it also sees a car/hands-free profile).
#define AA_UUID "4de17a00-52cb-11e6-bdf4-0800200c9a66"
#define HFP_HF_UUID "0000111e-0000-1000-8000-00805f9b34fb"

// Explicit SDP record for the AA RFCOMM service, passed to RegisterProfile so
// bluetoothd publishes it over the air. Some bluetoothd versions (e.g. 5.79)
// do not auto-generate an SDP record for a custom-UUID profile from Role +
// Channel alone, so the phone's SDP query finds nothing and gives up after
// bonding. This record is the AA UUID + L2CAP/RFCOMM(ch 8) + browse group.
static const char *AA_SDP_RECORD =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
    "<record>"
    "  <attribute id=\"0x0001\"><sequence>"
    "    <uuid value=\"4de17a00-52cb-11e6-bdf4-0800200c9a66\" />"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0004\"><sequence>"
    "    <sequence><uuid value=\"0x0100\" /></sequence>"
    "    <sequence><uuid value=\"0x0003\" /><uint8 value=\"0x08\" /></sequence>"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0005\"><sequence>"
    "    <uuid value=\"0x1002\" />"
    "  </sequence></attribute>"
    "  <attribute id=\"0x0100\"><text value=\"Android Auto Wireless\" /></attribute>"
    "</record>";

// Reply to a BlueZ method call with an empty return (i.e. "accepted").
static void emptyReply(DBusConnection *conn, DBusMessage *msg)
{
    DBusMessage *reply = dbus_message_new_method_return(msg);
    if (reply)
    {
        dbus_connection_send(conn, reply, nullptr);
        dbus_message_unref(reply);
    }
}

AaBluetooth::~AaBluetooth()
{
    stop();
}

bool AaBluetooth::setAdapterProp(const char *prop, int type, const void *value)
{
    DBusMessage *msg = dbus_message_new_method_call(BLUEZ, _adapter.c_str(), PROPS_IFACE, "Set");
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
    DBusMessage *reply =
        dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err))
    {
        log_w("bt: set %s failed > %s", prop, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (reply)
        dbus_message_unref(reply);
    return true;
}

bool AaBluetooth::setupAdapter()
{
    // The controller may be rfkill soft-blocked at boot; Powered=true can't
    // bring it up until it's unblocked. Give bluetoothd a moment to see the
    // now-unblocked adapter before setting properties.
    if (system("rfkill unblock bluetooth 2>/dev/null") != 0)
        log_v("bt: rfkill unblock returned non-zero (may be fine)");
    usleep(300 * 1000);

    dbus_bool_t yes = TRUE;
    dbus_uint32_t zero = 0;
    const char *alias = Settings::btName.value.c_str();

    setAdapterProp("Powered", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("Alias", DBUS_TYPE_STRING, &alias);
    setAdapterProp("Pairable", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("PairableTimeout", DBUS_TYPE_UINT32, &zero);
    setAdapterProp("Discoverable", DBUS_TYPE_BOOLEAN, &yes);
    setAdapterProp("DiscoverableTimeout", DBUS_TYPE_UINT32, &zero);
    log_i("bt: adapter '%s' powered, pairable + discoverable", alias);
    return true;
}

bool AaBluetooth::registerAgent()
{
    static DBusObjectPathVTable vtable;
    vtable.message_function = &AaBluetooth::agentMessage;
    vtable.unregister_function = nullptr;
    if (!dbus_connection_register_object_path(_conn, AGENT_PATH, &vtable, this))
    {
        log_e("bt: can't register agent object");
        return false;
    }

    // AgentManager1.RegisterAgent(path, "DisplayYesNo") -> Numeric Comparison
    // (or Just Works) pairing. DisplayYesNo satisfies phones that require MITM
    // protection (e.g. Samsung Galaxy S21, which rejects plain Just-Works SSP
    // with Authentication Failure 0x05); both outcomes are auto-accepted by our
    // agent handler. Avoid "KeyboardDisplay", which could ask for a passkey/PIN
    // the handler doesn't implement.
    const char *path = AGENT_PATH;
    const char *cap = "DisplayYesNo";
    for (const char *method : {"RegisterAgent", "RequestDefaultAgent"})
    {
        DBusMessage *msg =
            dbus_message_new_method_call(BLUEZ, "/org/bluez", AGENT_MGR_IFACE, method);
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
            log_w("bt: %s > %s", method, err.message);
            dbus_error_free(&err);
        }
        else if (reply)
            dbus_message_unref(reply);
    }
    log_i("bt: pairing agent registered");
    return true;
}

// Add a {sv} dict entry (string key, basic-typed variant value).
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

bool AaBluetooth::registerProfile(const char *path, const char *uuid, bool server,
                                  const char *serviceRecord, const char *name)
{
    static DBusObjectPathVTable vtable;
    vtable.message_function = &AaBluetooth::profileMessage;
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
    if (server)
    {
        dbus_uint16_t channel = 8; // AA custom profile lives on RFCOMM ch 8
        addDictEntry(&dict, "Channel", DBUS_TYPE_UINT16, &channel);
    }
    // Explicit SDP record so bluetoothd publishes the service over the air
    // (versions that don't auto-generate one otherwise leave it invisible).
    if (serviceRecord)
        addDictEntry(&dict, "ServiceRecord", DBUS_TYPE_STRING, &serviceRecord);
    dbus_bool_t no = FALSE, yes = TRUE;
    addDictEntry(&dict, "RequireAuthentication", DBUS_TYPE_BOOLEAN, &no);
    addDictEntry(&dict, "RequireAuthorization", DBUS_TYPE_BOOLEAN, &no);
    addDictEntry(&dict, "AutoConnect", DBUS_TYPE_BOOLEAN, &yes);
    dbus_message_iter_close_container(&it, &dict);

    DBusError err;
    dbus_error_init(&err);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(_conn, msg, 2000, &err);
    dbus_message_unref(msg);
    if (dbus_error_is_set(&err))
    {
        log_w("bt: RegisterProfile %s > %s", uuid, err.message);
        dbus_error_free(&err);
        return false;
    }
    if (reply)
        dbus_message_unref(reply);
    log_i("bt: registered profile %s (%s)", uuid, role);
    return true;
}

DBusHandlerResult AaBluetooth::agentMessage(DBusConnection *conn, DBusMessage *msg, void *)
{
    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    // Just-works pairing: accept everything, no reply body needed.
    log_d("bt: agent %s", member);
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

DBusHandlerResult AaBluetooth::profileMessage(DBusConnection *conn, DBusMessage *msg, void *user)
{
    AaBluetooth *self = static_cast<AaBluetooth *>(user);
    const char *member = dbus_message_get_member(msg);
    if (!member)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (strcmp(member, "NewConnection") == 0)
    {
        // Signature (o, h, a{sv}): object path, RFCOMM fd, properties.
        DBusMessageIter it;
        int fd = -1;
        if (dbus_message_iter_init(msg, &it))
        {
            dbus_message_iter_next(&it); // skip object path
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UNIX_FD)
                dbus_message_iter_get_basic(&it, &fd);
        }
        log_i("bt: NewConnection on %s (fd %d)", dbus_message_get_path(msg), fd);
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

void AaBluetooth::onNewConnection(int fd)
{
    // Only the AA profile's RFCOMM carries the aaw handshake; HFP connections
    // are accepted (so the phone is happy) but we just drain/close them.
    _handshakes.emplace_back([this, fd] {
        aa_aaw::runHandshake(fd, _params, _running);
        ::close(fd);
    });
}

void AaBluetooth::dispatchLoop()
{
    while (_running && dbus_connection_read_write_dispatch(_conn, 200))
        ;
}

bool AaBluetooth::start(const aa_aaw::Params &params)
{
    _params = params;

    DBusError err;
    dbus_error_init(&err);
    _conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!_conn)
    {
        log_e("bt: can't connect to system bus > %s", err.message);
        dbus_error_free(&err);
        return false;
    }

    _running = true;
    setupAdapter();
    registerAgent();
    registerProfile(AA_PROFILE_PATH, AA_UUID, true, AA_SDP_RECORD, "Android Auto Wireless");
    // HFP is provided by an external Audio Gateway backend (bluealsad -p hfp-ag
    // -p hfp-hf). Do NOT register 0x111e here: a client registration serves no
    // record and, worse, claims the UUID so the backend can't publish the
    // standalone Hands-Free record the phone looks for before starting AA.

    _dispatch = std::thread(&AaBluetooth::dispatchLoop, this);
    log_i("bt: wireless Android Auto profile advertised");
    return true;
}

void AaBluetooth::stop()
{
    if (!_running)
        return;
    _running = false;
    if (_dispatch.joinable())
        _dispatch.join();
    for (std::thread &t : _handshakes)
        if (t.joinable())
            t.join();
    _handshakes.clear();
    if (_conn)
    {
        dbus_connection_unref(_conn);
        _conn = nullptr;
    }
    log_v("bt: stopped");
}

#endif /* USE_AA_WIRELESS */
