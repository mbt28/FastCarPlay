#ifndef SRC_PROTOCOL_CP_CP_CARPLAY_MSG
#define SRC_PROTOCOL_CP_CP_CARPLAY_MSG

// CarPlay control-session messages -- the wireless trigger's payload. After the
// iAP2 link comes up over Bluetooth RFCOMM, the head unit and iPhone exchange
// these CSMs to negotiate a wireless CarPlay session. The pivotal one is
// CarPlayStartSession (0x4301): the accessory hands the phone the Wi-Fi SSID +
// passphrase + channel + its own IP + the :7000 control port + its pairing
// identity (pi/pk). The phone then joins that Wi-Fi AP and connects to the
// control server (src/protocol/cp/cp_server) -- the same handshake that already
// works. This is the wireless equivalent of the (iOS-26-deprecated) wired
// config-6 trigger. Message IDs and parameter layout mirror LIVI
// cp/iap2/control_session_message/{car_play,wifi}.py.
//
// Parameter encoding (iAP2 CSM): integers are raw big-endian, strings are
// UTF-8 with a trailing NUL, bools are one byte, and a "group" parameter's
// value is itself a param body ([u16 len][u16 id][value] repeated) -- i.e.
// nested CSM params. Built on the cp_iap2 CSM codec.

#include <cstdint>
#include <string>
#include <vector>

#include "cp_iap2.h"

namespace cp_carplay
{
using cp_iap2::Bytes;
using cp_iap2::CsmParam;

// Control-session message IDs (LIVI car_play.py / wifi.py / identification.py).
constexpr uint16_t MSG_START_IDENTIFICATION = 0x1D00;
constexpr uint16_t MSG_IDENTIFICATION_INFORMATION = 0x1D01;
constexpr uint16_t MSG_IDENTIFICATION_ACCEPTED = 0x1D02;
constexpr uint16_t MSG_IDENTIFICATION_REJECTED = 0x1D03;
constexpr uint16_t MSG_REQUEST_AUTH_CERTIFICATE = 0xAA00;
constexpr uint16_t MSG_AUTH_CERTIFICATE = 0xAA01;
constexpr uint16_t MSG_REQUEST_AUTH_CHALLENGE_RESPONSE = 0xAA02;
constexpr uint16_t MSG_AUTH_RESPONSE = 0xAA03;
constexpr uint16_t MSG_AUTH_FAILED = 0xAA04;
constexpr uint16_t MSG_AUTH_SUCCEEDED = 0xAA05;
constexpr uint16_t MSG_CARPLAY_AVAILABILITY = 0x4300;
constexpr uint16_t MSG_CARPLAY_START_SESSION = 0x4301;
constexpr uint16_t MSG_WIRELESS_CARPLAY_UPDATE = 0x4E0D;
constexpr uint16_t MSG_DEVICE_TRANSPORT_ID_NOTIFY = 0x4E0E;
constexpr uint16_t MSG_REQUEST_WIFI_INFORMATION = 0x5700;
constexpr uint16_t MSG_WIFI_INFORMATION = 0x5701;
constexpr uint16_t MSG_REQUEST_ACCESSORY_WIFI_CONFIG = 0x5702;
constexpr uint16_t MSG_ACCESSORY_WIFI_CONFIG = 0x5703;

// Wi-Fi security type (LIVI wifi.py SecurityType).
enum class WifiSecurity : uint8_t
{
    None = 0,
    Wep = 1,
    WpaWpa2 = 2,
    Wpa3Transition = 3,
    Wpa3Only = 4,
};

// Everything the phone needs to reach the CarPlay server over the wired USB-NCM
// link. Sent as CarPlayStartSession (0x4301) with wired_attributes instead of
// wireless_attributes: the accessory's link-local IPv6 on the NCM interface +
// the :7000 control port + pairing identity. The phone then opens the reverse
// control connection to [ip%iface]:port. Mirrors LIVI car_play.py
// CarPlayStartSessionWiredAttributes.
struct WiredSession
{
    std::string ipAddress;        // our fe80:: link-local on the usb0/NCM iface
    uint32_t port = 7000;         // CarPlay control port
    std::string deviceIdentifier; // pairing id / Bluetooth MAC string
    std::string publicKey;        // AirPlay-2 pairing identity (pi/pk)
    std::string sourceVersion;    // e.g. "280.33.8"
};

// Everything the phone needs to join our AP and reach the CarPlay server.
struct WirelessSession
{
    std::string ssid;
    std::string passphrase;
    uint8_t channel = 0;                          // Wi-Fi channel the AP runs on
    std::string ipAddress;                        // our IP on that AP (the phone's target)
    WifiSecurity security = WifiSecurity::WpaWpa2; // AP security
    uint32_t port = 7000;                         // CarPlay control port
    std::string deviceIdentifier;                 // pairing id (pi)
    std::string publicKey;                        // accessory Ed25519 public key, hex (pk)
    std::string sourceVersion;                    // e.g. "550.1"
};

// ── Value encoders (iAP2 CSM parameter values) ──────────────────────────
Bytes encU8(uint8_t v);
Bytes encU16(uint16_t v);
Bytes encU32(uint32_t v);
Bytes encBool(bool v);
Bytes encStr(const std::string &s); // UTF-8 + trailing NUL
// A "group" value: nested params serialized as a param body.
Bytes encGroup(const std::vector<CsmParam> &params);

// The accessory's iAP2 identity. Sent as IdentificationInformation after the
// phone's StartIdentification. The WirelessCarPlayTransportComponent is what
// makes the phone offer wireless CarPlay for this accessory.
struct AccessoryIdentity
{
    std::string name = "FastCarPlay";
    std::string modelIdentifier = "FastCarPlay1,1";
    std::string manufacturer = "FastCarPlay";
    std::string serialNumber = "0000000000000000";
    std::string firmwareVersion = "1.0";
    std::string hardwareVersion = "1.0";
    std::string currentLanguage = "en";
    Bytes bluetoothMac;                    // 6 raw bytes of the adapter MAC
    std::vector<uint16_t> messagesSent;     // CSM ids the accessory sends
    std::vector<uint16_t> messagesReceived; // CSM ids it accepts
};

// The default CSM id sets for a wireless-CarPlay accessory (identification,
// authentication, CarPlay session, Wi-Fi).
std::vector<uint16_t> defaultMessagesSent();
std::vector<uint16_t> defaultMessagesReceived();

// ── Builders ────────────────────────────────────────────────────────────
// IdentificationInformation (0x1D01): the accessory identity + a wireless
// CarPlay transport component keyed to the Bluetooth MAC.
Bytes buildIdentification(const AccessoryIdentity &id);

// IdentificationInformation (0x1D01) for WIRED CarPlay (config-6 / carkit): a
// USBHostTransportComponent (car_play_interface_number = the NCM interface, 3)
// instead of the Bluetooth/wireless components, power_providing = ADVANCED, and
// the message set a wired accessory declares. This is the identification LIVI
// converges to after dropping the droppable vehicle/location/route components,
// so it is sent directly (no reject/retry round-trip). Mirrors LIVI
// cp_handler.build_identification(carkit=True). Only id.name/modelIdentifier/
// manufacturer are used; the rest are wired-fixed.
Bytes buildWiredIdentification(const AccessoryIdentity &id);

// Declare wireless (and optionally wired) CarPlay availability. The transport
// identifiers are the Bluetooth/USB MACs the phone uses to correlate transports.
Bytes buildCarPlayAvailability(const std::string &btTransportId,
                               const std::string &usbTransportId = "");

// The wireless handoff: tells the phone which Wi-Fi to join and where CarPlay
// lives on it.
Bytes buildStartSession(const WirelessSession &s);

// The wired handoff (0x4301): hands the phone the accessory's link-local IPv6 on
// the USB-NCM interface + the :7000 port + pairing identity. The phone then
// connects to [ip%iface]:port over the NCM link. Mirrors LIVI
// _send_carplay_start_session (carkit branch).
Bytes buildWiredStartSession(const WiredSession &s);

// Advertise that wireless CarPlay is available/unavailable.
Bytes buildWirelessCarPlayUpdate(bool available);

// AccessoryWiFiConfigurationInformation (0x5703): the accessory's AP config,
// sent in reply to the phone's RequestAccessoryWiFiConfigurationInformation
// (0x5702). This is one of the two ways the phone learns the Wi-Fi credentials
// (the other being CarPlayStartSession). Params: 1=ssid, 2=passphrase,
// 3=security_type, 4=channel.
Bytes buildAccessoryWifiConfig(const std::string &ssid, const std::string &passphrase,
                               WifiSecurity security, uint8_t channel);

// ── iAP2 authentication (MFi coprocessor challenge/response) ─────────────
// The phone drives: RequestAuthenticationCertificate -> we send the MFi cert;
// RequestAuthenticationChallengeResponse{challenge} -> we sign it with the MFi
// chip and send the response; the phone then sends Succeeded/Failed. These use
// the same coprocessor as the auth-setup path (see mfi_auth / cp_auth_setup).
Bytes buildAuthCertificate(const Bytes &certificate); // 0xAA01
Bytes buildAuthResponse(const Bytes &response);       // 0xAA03
// Pull the challenge bytes out of a RequestAuthenticationChallengeResponse.
bool parseAuthChallenge(const Bytes &csm, Bytes &challenge);

// ── Parsers (for tests / handling the phone's replies) ──────────────────
// Split a param body (encGroup's inverse) back into sub-params.
bool parseGroup(const Bytes &value, std::vector<CsmParam> &params);
// Extract the wireless session an accessory would have sent (round-trip check).
bool parseStartSession(const Bytes &csm, WirelessSession &s);
} // namespace cp_carplay

#endif /* SRC_PROTOCOL_CP_CP_CARPLAY_MSG */
