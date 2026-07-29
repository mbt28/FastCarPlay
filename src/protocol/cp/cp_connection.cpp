#include "cp_connection.h"

#ifdef USE_CP_WIRELESS

#include <cstring>
#include <fstream>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

#include "common/logger.h"
#include "protocol/message.h"
#include "protocol/protocol_const.h"
#include "settings.h"

#include "cp_carplay_msg.h"
#include "cp_crypto.h"
#include "cp_identity.h"

using Bytes = std::vector<uint8_t>;

namespace
{
// An MfiSigner backed by the i2c auth coprocessor (mirrors the daemon's).
class ChipSigner : public cp_auth_setup::MfiSigner
{
public:
    ChipSigner(MfiAuth &c, int major) : _c(c), _major(major) {}
    bool certificate(Bytes &o) override { return _c.readCertificate(o); }
    bool sign(const Bytes &d, Bytes &s) override { return _c.sign(d, s); }
    int protocolMajor() override { return _major; }

private:
    MfiAuth &_c;
    int _major;
};

std::string hex(const Bytes &b)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b)
    {
        s.push_back(h[x >> 4]);
        s.push_back(h[x & 0xf]);
    }
    return s;
}

// Wireless CarPlay runs over the AP interface's IPv6 link-local (fe80::); the
// phone connects to it (scoped to its own Wi-Fi link) on :7000. Returns the bare
// fe80 string, or empty if the interface has none yet.
std::string wlanLinkLocal(const std::string &iface)
{
    struct ifaddrs *ifas = nullptr;
    std::string out;
    if (getifaddrs(&ifas) != 0)
        return out;
    for (struct ifaddrs *a = ifas; a; a = a->ifa_next)
    {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET6 || iface != a->ifa_name)
            continue;
        auto *s6 = (struct sockaddr_in6 *)a->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr))
            continue;
        char buf[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf)))
            out = buf;
        break;
    }
    freeifaddrs(ifas);
    return out;
}

std::string ifaceMac(const std::string &iface)
{
    std::ifstream f("/sys/class/net/" + iface + "/address");
    std::string m;
    std::getline(f, m);
    return m;
}
} // namespace

CpConnection::CpConnection()
{
    _method = "carplay-wireless";
}

CpConnection::~CpConnection() { stop(); }

void CpConnection::onVideoCodec(bool hevc)
{
    _codec.store(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    log_i("CarPlay screen codec: %s", hevc ? "HEVC/h265" : "h264");
}

void CpConnection::onVideo(const Bytes &annexB)
{
    if (annexB.empty())
        return;
    // Carlinkit-shaped video message: raw Annex-B at offset 0 (the decoder reads
    // data()/length() only). The padding is what avcodec's parser requires.
    auto msg = Message::Payload(CMD_VIDEO_DATA, (int32_t)annexB.size(), AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t *p = msg->data();
    if (!p)
        return; // allocation failed; drop the frame rather than crash
    std::memcpy(p, annexB.data(), annexB.size());
    videoStream.pushDiscard(std::move(msg));
    _transfered.fetch_add((uint32_t)annexB.size(), std::memory_order_release);
    _frames.fetch_add(1, std::memory_order_relaxed);
    if (_state.load() != PROTOCOL_STATUS_CONNECTED)
        _state.store(PROTOCOL_STATUS_CONNECTED);
}

void CpConnection::onSessionConnect()
{
    log_i("CarPlay: phone connected the :7000 control channel");
    if (_state.load() != PROTOCOL_STATUS_CONNECTED)
        _state.store(PROTOCOL_STATUS_ONLINE);
}

void CpConnection::onSessionDisconnect()
{
    log_i("CarPlay: control channel closed");
    _state.store(PROTOCOL_STATUS_LINKING); // back to waiting for the phone
}

void CpConnection::start()
{
    if (_started)
        return;
    _started = true;

    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();

    _haveChip = _chip.open(Settings::mfiI2cBus.value, (uint8_t)Settings::mfiI2cAddr.value);
    MfiAuth::Info info{};
    if (_haveChip)
        _haveChip = _chip.identify(info);
    if (_haveChip)
    {
        _signer = std::make_unique<ChipSigner>(_chip, info.protocolMajor);
        _mfiInfo = info.protocolMajor == 3 ? "MFi 3.0" : "MFi 2.0C";
    }
    else
        log_w("CarPlay: MFi chip absent on %s -- auth will fail",
              Settings::mfiI2cBus.value.c_str());

    // Wireless CarPlay runs over the AP interface's fe80 link-local + its MAC.
    const std::string iface = Settings::wifiIface.value;
    std::string fe80 = wlanLinkLocal(iface);
    std::string mac = ifaceMac(iface);
    if (fe80.empty())
        log_w("CarPlay: %s has no IPv6 link-local (is the AP up?) -- falling back to %s",
              iface.c_str(), Settings::apIp.value.c_str());

    // Stable device id from the pairing id, shared by mDNS + the handoff.
    Bytes pidBytes(id.pairingId.begin(), id.pairingId.end());
    Bytes dh = cp_crypto::sha256(pidBytes);
    char deviceId[18];
    snprintf(deviceId, sizeof(deviceId), "%02X:%02X:%02X:%02X:%02X:%02X", dh[0] | 0x02, dh[1], dh[2],
             dh[3], dh[4], dh[5]);

    // Feed decoded video into videoStream; track the session for the state.
    cp_av::Sinks sinks;
    sinks.onVideoCodec = [this](bool hevc) { onVideoCodec(hevc); };
    sinks.onVideo = [this](const Bytes &b) { onVideo(b); };
    _server.setAvSinks(sinks);
    _server.setLifecycle([this] { onSessionConnect(); }, [this] { onSessionDisconnect(); });

    cp_mdns::Config mdns;
    mdns.instance = Settings::btName.value;
    mdns.txt = {
        std::string("deviceid=") + deviceId,
        "features=0x44540380,0x61",
        "flags=0x4",
        "srcvers=550.1",
        "pi=" + id.pairingId,
        "pk=" + hex(id.pubRaw),
    };

    if (!_server.start(7000, _haveChip ? _signer.get() : nullptr) || !_mdns.start(mdns))
    {
        log_e("CarPlay: failed to start :7000 / mDNS (already running?)");
        _state.store(PROTOCOL_STATUS_ERROR);
        return;
    }

    cp_bt::Config cfg;
    cfg.alias = Settings::btName.value;
    cfg.signer = _haveChip ? _signer.get() : nullptr;
    cfg.identity.messagesSent = cp_carplay::defaultMessagesSent();
    cfg.identity.messagesReceived = cp_carplay::defaultMessagesReceived();
    cfg.wifi.ssid = Settings::wifiSsid.value;
    cfg.wifi.passphrase = Settings::wifiPass.value;
    cfg.wifi.channel = (uint8_t)Settings::wifiChannel.value;
    cfg.wifi.ipAddress = fe80.empty() ? Settings::apIp.value : fe80;
    cfg.wifi.security = cp_carplay::WifiSecurity::WpaWpa2;
    cfg.wifi.port = 7000;
    cfg.wifi.deviceIdentifier = mac.empty() ? id.pairingId : mac;
    cfg.wifi.publicKey = hex(id.pubRaw);
    cfg.wifi.sourceVersion = "550.1";

    if (!_bt.start(cfg))
    {
        log_e("CarPlay: Bluetooth bootstrap failed (bluetoothd running? dbus?)");
        _server.stop();
        _mdns.stop();
        _state.store(PROTOCOL_STATUS_ERROR);
        return;
    }

    _state.store(PROTOCOL_STATUS_LINKING);
    log_i("CarPlay wireless ready: %s, AP %s ch%d on %s (fe80=%s), pair the iPhone then pick it in "
          "Settings > General > CarPlay",
          _mfiInfo.c_str(), Settings::wifiSsid.value.c_str(), Settings::wifiChannel.value,
          iface.c_str(), fe80.empty() ? "none" : fe80.c_str());
}

void CpConnection::stop()
{
    if (!_started)
        return;
    _bt.stop();
    _server.stop();
    _mdns.stop();
    _started = false;
    _state.store(PROTOCOL_STATUS_NO_DEVICE);
}

const std::string CpConnection::status() const
{
    char buf[160];
    snprintf(buf, sizeof(buf), "carplay-wireless %s codec=%s frames=%u", _mfiInfo.c_str(),
             _codec.load() == AV_CODEC_ID_HEVC ? "h265" : "h264",
             _frames.load(std::memory_order_relaxed));
    return buf;
}

#endif /* USE_CP_WIRELESS */
