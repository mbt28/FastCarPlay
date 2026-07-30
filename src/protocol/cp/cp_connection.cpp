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
#include "cp_hid.h"
#include "cp_identity.h"
#include "cp_plist.h"

using Bytes = std::vector<uint8_t>;

namespace
{
// Must match the main display uuid advertised in cp_av handleInfo.
constexpr const char *MAIN_DISPLAY_UUID = "b7e6c5a0-1111-4000-8000-000000000001";

// Event-channel command bodies (binary plists) the phone understands.
Bytes hidCommand(const char *uuid, const Bytes &report)
{
    cp_plist::Value m = cp_plist::Value::map();
    m.set("type", cp_plist::Value::str("hidSendReport"));
    m.set("uuid", cp_plist::Value::str(uuid));
    m.set("hidReport", cp_plist::Value::bytes(report));
    return cp_plist::encode(m);
}

Bytes siriCommand(int action) // 2 = button down, 3 = button up (a tap = down+up)
{
    cp_plist::Value params = cp_plist::Value::map();
    params.set("siriAction", cp_plist::Value::integer(action));
    cp_plist::Value m = cp_plist::Value::map();
    m.set("type", cp_plist::Value::str("requestSiri"));
    m.set("params", params);
    return cp_plist::encode(m);
}

Bytes forceKeyFrameCommand()
{
    cp_plist::Value params = cp_plist::Value::map();
    params.set("uuid", cp_plist::Value::str(MAIN_DISPLAY_UUID));
    cp_plist::Value m = cp_plist::Value::map();
    m.set("type", cp_plist::Value::str("forceKeyFrame"));
    m.set("params", params);
    return cp_plist::encode(m);
}

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
    // videoCodec() is read (to start the decoder) before start() runs, so fix the
    // default here: HEVC unless we'll be advertising H.264 only (F1C200s cedrus).
    _codec.store(Settings::carplayHevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
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

void CpConnection::onAudio(int type, int rate, int channels, const Bytes &pcm)
{
    if (pcm.empty())
        return;
    // The app's PcmAudio reads a format-type u32 at offset 0 (its _configTable:
    // 4 = 48k stereo, 3/5/6/7 = 8k/16k/24k/16k mono/stereo, anything else =
    // 44.1k stereo) and S16 interleaved PCM from offset 12.
    auto fmtType = [](int r, int ch) -> uint32_t {
        if (r == 8000 && ch == 1) return 3;
        if (r == 48000 && ch == 2) return 4;
        if (r == 16000 && ch == 1) return 5;
        if (r == 24000 && ch == 1) return 6;
        if (r == 16000 && ch == 2) return 7;
        return 0; // -> PcmAudio default (44.1k stereo)
    }(rate, channels);

    auto m = Message::Payload(CMD_AUDIO_DATA, (int32_t)(12 + pcm.size()));
    uint8_t *p = m->data();
    if (!p)
        return;
    p[0] = (uint8_t)(fmtType & 0xff);
    p[1] = (uint8_t)((fmtType >> 8) & 0xff);
    p[2] = (uint8_t)((fmtType >> 16) & 0xff);
    p[3] = (uint8_t)((fmtType >> 24) & 0xff);
    std::memset(p + 4, 0, 8);
    std::memcpy(p + 12, pcm.data(), pcm.size());
    m->setOffset(12); // data()/length() now expose the PCM; getInt(0) still sees the type

    // type 102 = buffered media (music) -> main; 100/101 = nav/speech/alert -> aux.
    (type == 102 ? audioStreamMain : audioStreamAux).pushDiscard(std::move(m));
}

void CpConnection::onSessionConnect()
{
    log_i("CarPlay: phone connected the :7000 control channel");
    clearInput(); // drop anything stale from a previous session
    if (_state.load() != PROTOCOL_STATUS_CONNECTED)
        _state.store(PROTOCOL_STATUS_ONLINE);
}

void CpConnection::onSessionDisconnect()
{
    log_i("CarPlay: control channel closed");
    clearInput();
    _state.store(PROTOCOL_STATUS_LINKING); // back to waiting for the phone
}

// ── Input: app writeQueue -> CarPlay event-channel commands ─────────────
bool CpConnection::nextCommand(Bytes &body)
{
    std::lock_guard<std::mutex> lk(_inputMutex);
    if (_inputQueue.empty())
        return false;
    body = std::move(_inputQueue.front());
    _inputQueue.pop_front();
    return true;
}

void CpConnection::enqueueCommand(Bytes body)
{
    if (body.empty())
        return;
    std::lock_guard<std::mutex> lk(_inputMutex);
    if (_inputQueue.size() >= 256)
        _inputQueue.pop_front(); // cap: drop the oldest rather than grow unbounded
    _inputQueue.push_back(std::move(body));
}

void CpConnection::clearInput()
{
    std::lock_guard<std::mutex> lk(_inputMutex);
    _inputQueue.clear();
}

void CpConnection::handleInput(const Message &m)
{
    // The screen we advertise in /info is 800x480 (cp_av::Config default); touch
    // coordinates must be in that pixel space.
    const int W = cp_av::Config{}.screenWidth;
    const int H = cp_av::Config{}.screenHeight;
    auto clampPx = [](int v, int max) { return v < 0 ? 0 : (v > max ? max : v); };

    switch (m.type())
    {
    case CMD_TOUCH:
    {
        // action 14=down, 15=move, 16=up; x/y are normalised * 10000.
        const int action = m.getInt(0);
        const int px = clampPx(m.getInt(4) * W / 10000, W);
        const int py = clampPx(m.getInt(8) * H / 10000, H);
        const bool down = (action == 14 || action == 15);
        if (action != 15) // log taps (down/up), not every move
            log_i("[cp-in] touch %s px=%d py=%d", action == 14 ? "down" : "up", px, py);
        enqueueCommand(hidCommand(cp_hid::TOUCH_UUID, cp_hid::touchReport(px, py, down)));
        break;
    }
    case CMD_MULTI_TOUCH:
    {
        // N contacts, 16 bytes each: float x@0, float y@4, u32 state@8, u32 id@12.
        std::vector<cp_hid::Contact> contacts;
        const uint8_t *d = m.data();
        const int n = d ? m.length() / 16 : 0;
        for (int i = 0; i < n && (int)contacts.size() < cp_hid::TOUCH_CONTACTS; i++)
        {
            const uint8_t *p = d + i * 16;
            float fx = 0, fy = 0;
            uint32_t state = 0;
            std::memcpy(&fx, p, 4);
            std::memcpy(&fy, p + 4, 4);
            std::memcpy(&state, p + 8, 4);
            const int px = clampPx((int)(fx * W), W);
            const int py = clampPx((int)(fy * H), H);
            contacts.push_back({(int)contacts.size(), px, py, state != MT_ACTION_UP});
        }
        enqueueCommand(hidCommand(cp_hid::TOUCH_UUID, cp_hid::touchReport(contacts)));
        break;
    }
    case CMD_CONTROL:
    {
        const int btn = m.getInt(0);
        log_i("[cp-in] control btn=%d", btn);
        auto media = [&](int idx) {
            enqueueCommand(hidCommand(cp_hid::MEDIA_UUID, cp_hid::mediaReport(idx)));
            enqueueCommand(hidCommand(cp_hid::MEDIA_UUID, cp_hid::mediaReport(cp_hid::MEDIA_NONE)));
        };
        auto knobTap = [&](bool home, bool back) {
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, home, back)));
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, false, false)));
        };
        // Directional nav: the knob's X/Y are absolute axes, so deflect fully in
        // the pressed direction then re-center -- one focus nudge per press.
        auto knobMove = [&](int dx, int dy) {
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, false, false, dx, dy, 0)));
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, false, false, 0, 0, 0)));
        };
        switch (btn)
        {
        case BTN_HOME: knobTap(true, false); break;
        case BTN_BACK: knobTap(false, true); break;
        case BTN_LEFT: knobMove(-127, 0); break;
        case BTN_RIGHT: knobMove(127, 0); break;
        case BTN_UP: knobMove(0, -127); break;
        case BTN_DOWN: knobMove(0, 127); break;
        case BTN_SELECT_DOWN:
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(true, false, false)));
            break;
        case BTN_SELECT_UP:
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, false, false)));
            break;
        case BTN_PLAY: media(cp_hid::MEDIA_PLAY); break;
        case BTN_PAUSE: media(cp_hid::MEDIA_PAUSE); break;
        case BTN_203: media(cp_hid::MEDIA_PLAY_PAUSE); break;
        case BTN_NEXT_TRACK: media(cp_hid::MEDIA_NEXT); break;
        case BTN_PREVIOUS_TRACK: media(cp_hid::MEDIA_PREV); break;
        case BTN_SIRI: // a tap: button down then up
            enqueueCommand(siriCommand(2));
            enqueueCommand(siriCommand(3));
            break;
        case BTN_SCREEN_REFRESH: enqueueCommand(forceKeyFrameCommand()); break;
        default: break; // dpad/others: no CarPlay HID mapping yet
        }
        break;
    }
    default: break; // CMD_HEARTBEAT etc. -- nothing to forward
    }
}

void CpConnection::writerLoop()
{
    while (_active.load())
    {
        if (!writeQueue.waitFor(_active, 200))
            break; // _active cleared
        std::unique_ptr<Message> m;
        while ((m = writeQueue.pop()))
            handleInput(*m);
    }
}

void CpConnection::start()
{
    if (_started)
        return;
    _started = true;
    _active.store(true);

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
    sinks.onAudio = [this](int t, int r, int c, const Bytes &p) { onAudio(t, r, c, p); };
    // Advertise HEVC only where the decoder can do it; the F1C200s cedrus is
    // H.264-only, so carplay-hevc=false makes the phone stream H.264.
    cp_av::Config avcfg;
    avcfg.hevc = Settings::carplayHevc;
    _server.setAvConfig(avcfg);
    _server.setAvSinks(sinks);
    _server.setInputSource(this); // touch/buttons the AV session forwards to the phone
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

    _writer = std::thread([this] { writerLoop(); }); // app writeQueue -> HID over event channel
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
    // Stop the writer first (no more producing), then the server (its stop joins
    // the accept thread, so the AV session's event thread -- the only caller of
    // nextCommand -- is finished before we return and can be destroyed).
    _active.store(false);
    writeQueue.notify(); // wake the writer out of waitFor
    if (_writer.joinable())
        _writer.join();
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
