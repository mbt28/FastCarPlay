#include "cp_carplay_msg.h"

namespace cp_carplay
{
namespace
{
// Serialize a list of params as a CSM param body: [u16 len][u16 id][value]...
// This is the same layout packCsm uses for the top level; a "group" parameter
// nests one of these as its value.
Bytes packParamBody(const std::vector<CsmParam> &params)
{
    Bytes body;
    for (const CsmParam &p : params)
    {
        const uint16_t len = (uint16_t)(4 + p.value.size());
        body.push_back(len >> 8);
        body.push_back(len & 0xff);
        body.push_back(p.id >> 8);
        body.push_back(p.id & 0xff);
        body.insert(body.end(), p.value.begin(), p.value.end());
    }
    return body;
}
} // namespace

Bytes encU8(uint8_t v) { return {v}; }
Bytes encU16(uint16_t v) { return {(uint8_t)(v >> 8), (uint8_t)(v & 0xff)}; }
Bytes encU32(uint32_t v)
{
    return {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)(v & 0xff)};
}
Bytes encBool(bool v) { return {(uint8_t)(v ? 1 : 0)}; }
Bytes encStr(const std::string &s)
{
    Bytes b(s.begin(), s.end());
    b.push_back(0); // iAP2 strings are NUL-terminated UTF-8
    return b;
}
Bytes encGroup(const std::vector<CsmParam> &params) { return packParamBody(params); }

namespace
{
// A list of CSM ids packed as raw big-endian u16s (the identification bitmap).
Bytes encIdList(const std::vector<uint16_t> &ids)
{
    Bytes b;
    for (uint16_t id : ids)
    {
        b.push_back(id >> 8);
        b.push_back(id & 0xff);
    }
    return b;
}
} // namespace

std::vector<uint16_t> defaultMessagesSent()
{
    return {
        MSG_IDENTIFICATION_INFORMATION,
        MSG_CARPLAY_AVAILABILITY,
        MSG_CARPLAY_START_SESSION,
        MSG_WIRELESS_CARPLAY_UPDATE,
        MSG_DEVICE_TRANSPORT_ID_NOTIFY,
        MSG_WIFI_INFORMATION,
        MSG_ACCESSORY_WIFI_CONFIG,
    };
}

std::vector<uint16_t> defaultMessagesReceived()
{
    return {
        MSG_START_IDENTIFICATION,
        MSG_IDENTIFICATION_ACCEPTED,
        MSG_IDENTIFICATION_REJECTED,
        MSG_REQUEST_WIFI_INFORMATION,
        MSG_REQUEST_ACCESSORY_WIFI_CONFIG,
    };
}

Bytes buildIdentification(const AccessoryIdentity &id)
{
    // BluetoothTransportComponent (param 17): id, name, supports-iap2 flag, MAC.
    Bytes bt = encGroup({
        {0, encU16(1)},                // transport component id
        {1, encStr("Bluetooth")},      // name
        {2, {}},                       // supports_iap2_connection (empty flag)
        {3, id.bluetoothMac},          // bluetooth_transport_mac (6 raw bytes)
    });

    // WirelessCarPlayTransportComponent (param 24): the wireless CarPlay flag.
    Bytes wcp = encGroup({
        {0, encU16(2)},          // transport component id
        {1, encStr("CarPlay")},  // name
        {2, {}},                 // supports_iap2_connection
        {4, {}},                 // supports_car_play (empty flag)
    });

    std::vector<CsmParam> params = {
        {0, encStr(id.name)},
        {1, encStr(id.modelIdentifier)},
        {2, encStr(id.manufacturer)},
        {3, encStr(id.serialNumber)},
        {4, encStr(id.firmwareVersion)},
        {5, encStr(id.hardwareVersion)},
        {6, encIdList(id.messagesSent)},
        {7, encIdList(id.messagesReceived)},
        {8, encU8(0)},  // power_providing_capability = NONE
        {9, encU16(0)}, // maximum_current_drawn_from_device
        {12, encStr(id.currentLanguage)},
        {13, encStr(id.currentLanguage)}, // supported_language (single entry)
        {17, bt},
        {24, wcp},
    };
    return cp_iap2::packCsm(MSG_IDENTIFICATION_INFORMATION, params);
}

Bytes buildCarPlayAvailability(const std::string &btTransportId, const std::string &usbTransportId)
{
    std::vector<CsmParam> params;
    if (!usbTransportId.empty())
    {
        // wired_attributes (param 0): available=true, usb transport id.
        params.push_back({0, encGroup({{0, encBool(true)}, {1, encStr(usbTransportId)}})});
    }
    // wireless_attributes (param 1): available=true, bluetooth transport id.
    params.push_back({1, encGroup({{0, encBool(true)}, {1, encStr(btTransportId)}})});
    return cp_iap2::packCsm(MSG_CARPLAY_AVAILABILITY, params);
}

Bytes buildStartSession(const WirelessSession &s)
{
    // wireless_attributes (param 1): ssid, passphrase, channel, ip, security.
    Bytes wireless = encGroup({
        {0, encStr(s.ssid)},
        {1, encStr(s.passphrase)},
        {2, encU8(s.channel)},
        {3, encStr(s.ipAddress)},
        {4, encU8((uint8_t)s.security)},
    });

    std::vector<CsmParam> params = {
        {1, wireless},
        {2, encU32(s.port)},
        {3, encStr(s.deviceIdentifier)},
        {4, encStr(s.publicKey)},
        {5, encStr(s.sourceVersion)},
    };
    return cp_iap2::packCsm(MSG_CARPLAY_START_SESSION, params);
}

Bytes buildWirelessCarPlayUpdate(bool available)
{
    // status (param 0): 1 = AVAILABLE, 0 = UNAVAILABLE.
    return cp_iap2::packCsm(MSG_WIRELESS_CARPLAY_UPDATE, {{0, encU8(available ? 1 : 0)}});
}

bool parseGroup(const Bytes &value, std::vector<CsmParam> &params)
{
    params.clear();
    size_t p = 0;
    while (p + 4 <= value.size())
    {
        const uint16_t len = (uint16_t)((value[p] << 8) | value[p + 1]);
        const uint16_t id = (uint16_t)((value[p + 2] << 8) | value[p + 3]);
        if (len < 4 || p + len > value.size())
            return false;
        params.push_back({id, Bytes(value.begin() + p + 4, value.begin() + p + len)});
        p += len;
    }
    return p == value.size();
}

namespace
{
std::string decStr(const Bytes &b)
{
    // Drop the trailing NUL if present.
    size_t n = b.size();
    if (n && b[n - 1] == 0)
        n--;
    return std::string(b.begin(), b.begin() + n);
}
uint32_t decU32(const Bytes &b)
{
    uint32_t v = 0;
    for (uint8_t x : b)
        v = (v << 8) | x;
    return v;
}
} // namespace

bool parseStartSession(const Bytes &csm, WirelessSession &s)
{
    uint16_t msgId = 0;
    std::vector<CsmParam> params;
    if (!cp_iap2::parseCsm(csm, msgId, params) || msgId != MSG_CARPLAY_START_SESSION)
        return false;

    bool sawWireless = false;
    for (const CsmParam &p : params)
    {
        switch (p.id)
        {
        case 1: // wireless_attributes
        {
            std::vector<CsmParam> w;
            if (!parseGroup(p.value, w))
                return false;
            for (const CsmParam &wp : w)
            {
                switch (wp.id)
                {
                case 0: s.ssid = decStr(wp.value); break;
                case 1: s.passphrase = decStr(wp.value); break;
                case 2: s.channel = wp.value.empty() ? 0 : wp.value[0]; break;
                case 3: s.ipAddress = decStr(wp.value); break;
                case 4: s.security = (WifiSecurity)(wp.value.empty() ? 0 : wp.value[0]); break;
                }
            }
            sawWireless = true;
            break;
        }
        case 2: s.port = decU32(p.value); break;
        case 3: s.deviceIdentifier = decStr(p.value); break;
        case 4: s.publicKey = decStr(p.value); break;
        case 5: s.sourceVersion = decStr(p.value); break;
        }
    }
    return sawWireless;
}
} // namespace cp_carplay
