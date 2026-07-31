#include "cp_wired_connection.h"

#ifdef USE_CP_WIRED

#include <chrono>
#include <cstring>
#include <fstream>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "common/logger.h"
#include "protocol/message.h"
#include "protocol/protocol_const.h"
#include "settings.h"

#include "cp_carplay_msg.h"
#include "cp_crypto.h"
#include "cp_hid.h"
#include "cp_iap2.h"
#include "cp_iap2_link.h"
#include "cp_identity.h"
#include "cp_plist.h"

using Bytes = std::vector<uint8_t>;

namespace
{
constexpr const char *MAIN_DISPLAY_UUID = "b7e6c5a0-1111-4000-8000-000000000001";

Bytes hidCommand(const char *uuid, const Bytes &report)
{
    cp_plist::Value m = cp_plist::Value::map();
    m.set("type", cp_plist::Value::str("hidSendReport"));
    m.set("uuid", cp_plist::Value::str(uuid));
    m.set("hidReport", cp_plist::Value::bytes(report));
    return cp_plist::encode(m);
}
Bytes siriCommand(int action)
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

std::string readLine(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// The sysfs serial + bus-port ("2-1") of the first plugged Apple device.
bool findPhone(std::string &serial, std::string &busPort)
{
    DIR *d = opendir("/sys/bus/usb/devices");
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; (e = readdir(d));)
    {
        std::string base = std::string("/sys/bus/usb/devices/") + e->d_name + "/";
        if (readLine(base + "idVendor") != "05ac")
            continue;
        std::string s = readLine(base + "serial");
        if (s.empty())
            continue;
        serial = s;
        busPort = e->d_name;
        found = true;
        break;
    }
    closedir(d);
    return found;
}

std::string ifaceLinkLocal(const std::string &iface)
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return "";
    std::string out;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET6 || iface != p->ifa_name)
            continue;
        auto *s6 = (struct sockaddr_in6 *)p->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr))
            continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
        out = buf;
        break;
    }
    freeifaddrs(ifa);
    return out;
}

void writeSys(const std::string &path, const std::string &val)
{
    std::ofstream f(path);
    f << val;
    f.flush();
}

// Claiming the usbmux interface re-enumerates the device and can knock the
// kernel cdc_ncm/ipheth off the config-6 NCM ifaces (dropping usb0). Rebind them
// so the kernel USB-ethernet link (usb0) comes back for the AV path.
void rebindNcm(const std::string &busPort)
{
    writeSys("/sys/bus/usb/drivers/ipheth/bind", busPort + ":6.2");
    writeSys("/sys/bus/usb/drivers/cdc_ncm/bind", busPort + ":6.3");
}
} // namespace

CpWiredConnection::CpWiredConnection()
{
    _method = "carplay-wired";
    _codec.store(Settings::carplayHevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
}

CpWiredConnection::~CpWiredConnection() { stop(); }

// ── AV sinks -> app queues (identical shape to CpConnection) ────────────────
void CpWiredConnection::onVideoCodec(bool hevc)
{
    _codec.store(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
    log_i("CarPlay screen codec: %s", hevc ? "HEVC/h265" : "h264");
}

void CpWiredConnection::onVideo(const Bytes &annexB)
{
    if (annexB.empty())
        return;
    auto msg = Message::Payload(CMD_VIDEO_DATA, (int32_t)annexB.size(), AV_INPUT_BUFFER_PADDING_SIZE);
    uint8_t *p = msg->data();
    if (!p)
        return;
    std::memcpy(p, annexB.data(), annexB.size());
    videoStream.pushDiscard(std::move(msg));
    _transfered.fetch_add((uint32_t)annexB.size(), std::memory_order_release);
    _frames.fetch_add(1, std::memory_order_relaxed);
    if (_state.load() != PROTOCOL_STATUS_CONNECTED)
        _state.store(PROTOCOL_STATUS_CONNECTED);
}

void CpWiredConnection::onAudio(int type, int rate, int channels, const Bytes &pcm)
{
    if (pcm.empty())
        return;
    auto fmtType = [](int r, int ch) -> uint32_t {
        if (r == 8000 && ch == 1) return 3;
        if (r == 48000 && ch == 2) return 4;
        if (r == 16000 && ch == 1) return 5;
        if (r == 24000 && ch == 1) return 6;
        if (r == 16000 && ch == 2) return 7;
        return 0;
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
    m->setOffset(12);
    (type == 102 ? audioStreamMain : audioStreamAux).pushDiscard(std::move(m));
}

void CpWiredConnection::onSessionConnect()
{
    log_i("CarPlay: phone connected the :7000 control channel");
    clearInput();
    if (_state.load() != PROTOCOL_STATUS_CONNECTED)
        _state.store(PROTOCOL_STATUS_ONLINE);
}

void CpWiredConnection::onSessionDisconnect()
{
    log_i("CarPlay: control channel closed");
    clearInput();
    _state.store(PROTOCOL_STATUS_LINKING);
}

// ── Input: app writeQueue -> CarPlay event-channel commands ─────────────────
bool CpWiredConnection::nextCommand(Bytes &body)
{
    std::lock_guard<std::mutex> lk(_inputMutex);
    if (_inputQueue.empty())
        return false;
    body = std::move(_inputQueue.front());
    _inputQueue.pop_front();
    return true;
}

void CpWiredConnection::enqueueCommand(Bytes body)
{
    if (body.empty())
        return;
    std::lock_guard<std::mutex> lk(_inputMutex);
    if (_inputQueue.size() >= 256)
        _inputQueue.pop_front();
    _inputQueue.push_back(std::move(body));
}

void CpWiredConnection::clearInput()
{
    std::lock_guard<std::mutex> lk(_inputMutex);
    _inputQueue.clear();
}

void CpWiredConnection::handleInput(const Message &m)
{
    const int W = cp_av::Config{}.screenWidth;
    const int H = cp_av::Config{}.screenHeight;
    auto clampPx = [](int v, int max) { return v < 0 ? 0 : (v > max ? max : v); };

    switch (m.type())
    {
    case CMD_TOUCH:
    {
        const int action = m.getInt(0);
        const int px = clampPx(m.getInt(4) * W / 10000, W);
        const int py = clampPx(m.getInt(8) * H / 10000, H);
        const bool down = (action == 14 || action == 15);
        enqueueCommand(hidCommand(cp_hid::TOUCH_UUID, cp_hid::touchReport(px, py, down)));
        break;
    }
    case CMD_MULTI_TOUCH:
    {
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
        auto media = [&](int idx) {
            enqueueCommand(hidCommand(cp_hid::MEDIA_UUID, cp_hid::mediaReport(idx)));
            enqueueCommand(hidCommand(cp_hid::MEDIA_UUID, cp_hid::mediaReport(cp_hid::MEDIA_NONE)));
        };
        auto knobTap = [&](bool home, bool back) {
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, home, back)));
            enqueueCommand(hidCommand(cp_hid::KNOB_UUID, cp_hid::knobReport(false, false, false)));
        };
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
        case BTN_SIRI:
            enqueueCommand(siriCommand(2));
            enqueueCommand(siriCommand(3));
            break;
        case BTN_SCREEN_REFRESH: enqueueCommand(forceKeyFrameCommand()); break;
        case 500: // video focus -- re-project (same as the home Resume row)
            requestVideoFocus();
            break;
        case 501: // video release -- hand the screen back to the FastCarPlay UI
            log_i("[cp-in] video release -- backgrounding to FastCarPlay");
            _videoFocused.store(false);
            break;
        default: break;
        }
        break;
    }
    default: break;
    }
}

// Re-show the phone's screen after the user handed it back to FastCarPlay. The
// CarPlay session was never dropped, so we just re-focus and force a keyframe so
// the picture returns immediately rather than at the next natural I-frame.
void CpWiredConnection::requestVideoFocus()
{
    _videoFocused.store(true);
    enqueueCommand(forceKeyFrameCommand());
}

void CpWiredConnection::writerLoop()
{
    while (_active.load())
    {
        if (!writeQueue.waitFor(_active, 200))
            break;
        std::unique_ptr<Message> m;
        while ((m = writeQueue.pop()))
            handleInput(*m);
    }
}

// ── The wired trigger ───────────────────────────────────────────────────────
bool CpWiredConnection::runWiredSession()
{
    std::string serial, busPort;
    if (!findPhone(serial, busPort))
        return false; // no phone plugged; caller waits + retries

    if (!_mux.start(serial))
    {
        log_w("carplay-wired: usbmux/config-6 bring-up failed for %s", serial.c_str());
        return false;
    }

    // Wait for the kernel USB-NCM link (usb0). Claiming the usbmux interface can
    // knock cdc_ncm off, so rebind once if it doesn't appear.
    const std::string iface = "usb0";
    std::string fe80;
    for (int i = 0; i < 40 && _active.load(); i++)
    {
        fe80 = ifaceLinkLocal(iface);
        if (!fe80.empty())
            break;
        if (i == 6)
            rebindNcm(busPort);
        usleep(250000);
    }
    if (fe80.empty())
    {
        log_w("carplay-wired: %s has no IPv6 link-local -- NCM link down", iface.c_str());
        _mux.stop();
        return false;
    }

    // carkit TLS iAP2 stream via libimobiledevice over our usbmux socket.
    setenv("USBMUXD_SOCKET_ADDRESS", ("UNIX:" + _mux.socketPath()).c_str(), 1);
    std::string udid = serial.size() == 24 ? serial.substr(0, 8) + "-" + serial.substr(8) : serial;
    idevice_t dev = nullptr;
    lockdownd_client_t ld = nullptr;
    lockdownd_service_descriptor_t svc = nullptr;
    idevice_connection_t conn = nullptr;
    auto cleanup = [&] {
        if (conn)
            idevice_disconnect(conn);
        if (svc)
            lockdownd_service_descriptor_free(svc);
        if (ld)
            lockdownd_client_free(ld);
        if (dev)
            idevice_free(dev);
        _mux.stop();
    };
    if (idevice_new_with_options(&dev, udid.c_str(), IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS ||
        lockdownd_client_new_with_handshake(dev, &ld, "fastcarplay-wired") != LOCKDOWN_E_SUCCESS ||
        lockdownd_start_service(ld, "com.apple.carkit.service", &svc) != LOCKDOWN_E_SUCCESS || !svc ||
        idevice_connect(dev, svc->port, &conn) != IDEVICE_E_SUCCESS ||
        (svc->ssl_enabled && idevice_connection_enable_ssl(conn) != IDEVICE_E_SUCCESS))
    {
        log_w("carplay-wired: carkit open failed (paired? unlocked?)");
        cleanup();
        return false;
    }
    log_i("carplay-wired: carkit TLS iAP2 channel up, NCM %s = %s", iface.c_str(), fe80.c_str());

    // Single-thread SSL <-> socketpair pump (libimobiledevice's conn is not safe
    // for concurrent send+receive; receive_timeout returns TIMEOUT even with data).
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
    {
        cleanup();
        return false;
    }
    const int linkFd = sp[0], bridgeFd = sp[1];
    fcntl(bridgeFd, F_SETFL, fcntl(bridgeFd, F_GETFL, 0) | O_NONBLOCK);
    std::atomic<bool> pumpRun{true};
    std::thread pump([&] {
        char buf[8192];
        while (pumpRun.load() && _active.load())
        {
            for (;;)
            {
                ssize_t n = read(bridgeFd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                uint32_t sent = 0, off = 0;
                while (off < (uint32_t)n)
                {
                    if (idevice_connection_send(conn, buf + off, (uint32_t)n - off, &sent) !=
                        IDEVICE_E_SUCCESS)
                    {
                        pumpRun.store(false);
                        break;
                    }
                    off += sent;
                }
            }
            uint32_t got = 0;
            idevice_error_t e = idevice_connection_receive_timeout(conn, buf, sizeof(buf), &got, 100);
            if (got > 0)
                (void)!write(bridgeFd, buf, got);
            else if (e != IDEVICE_E_SUCCESS && e != IDEVICE_E_TIMEOUT)
                break;
        }
        shutdown(bridgeFd, SHUT_RDWR);
    });

    cp_iap2::Iap2Link link(linkFd);
    bool ok = link.negotiate(true);
    if (ok)
    {
        log_i("carplay-wired: iAP2 link NORMAL -- driving control session");
        _state.store(PROTOCOL_STATUS_LINKING);
        cp_carplay::AccessoryIdentity acc;
        acc.name = Settings::btName.value;
        bool sentStart = false;
        Bytes csm;
        while (_active.load() && link.recvControl(csm))
        {
            uint16_t msgId = 0;
            std::vector<cp_iap2::CsmParam> params;
            if (!cp_iap2::parseCsm(csm, msgId, params))
                continue;
            switch (msgId)
            {
            case cp_carplay::MSG_START_IDENTIFICATION:
                link.sendControl(cp_carplay::buildWiredIdentification(acc));
                break;
            case cp_carplay::MSG_IDENTIFICATION_ACCEPTED:
                log_i("carplay-wired: identification accepted");
                break;
            case cp_carplay::MSG_REQUEST_AUTH_CERTIFICATE:
                link.sendControl(cp_carplay::buildAuthCertificate(_mfiCert));
                break;
            case cp_carplay::MSG_REQUEST_AUTH_CHALLENGE_RESPONSE:
            {
                Bytes challenge, sig;
                if (cp_carplay::parseAuthChallenge(csm, challenge) && _chip.sign(challenge, sig))
                    link.sendControl(cp_carplay::buildAuthResponse(sig));
                else
                    log_w("carplay-wired: MFi sign failed (%s)", _chip.lastError());
                break;
            }
            case cp_carplay::MSG_AUTH_SUCCEEDED:
                log_i("carplay-wired: MFi authentication succeeded");
                break;
            case cp_carplay::MSG_AUTH_FAILED:
                log_w("carplay-wired: MFi authentication failed");
                break;
            case cp_carplay::MSG_CARPLAY_AVAILABILITY:
                if (!sentStart)
                {
                    cp_carplay::WiredSession ws;
                    ws.ipAddress = fe80;
                    ws.port = 7000;
                    ws.deviceIdentifier = _deviceId;
                    ws.publicKey = _pk;
                    ws.sourceVersion = "280.33.8";
                    link.sendControl(cp_carplay::buildWiredStartSession(ws));
                    sentStart = true;
                    log_i("carplay-wired: CarPlayStartSession -> [%s]:7000", fe80.c_str());
                }
                break;
            default:
                break;
            }
        }
    }
    else
        log_w("carplay-wired: iAP2 negotiation failed");

    pumpRun.store(false);
    shutdown(linkFd, SHUT_RDWR);
    if (pump.joinable())
        pump.join();
    ::close(linkFd);
    ::close(bridgeFd);
    cleanup();
    return true; // a session ran (or ended); the trigger loop decides whether to retry
}

void CpWiredConnection::triggerLoop()
{
    while (_active.load())
    {
        runWiredSession();
        // Session ended (phone unplugged / carkit dropped) or the phone wasn't
        // ready. Reset state and wait a moment before the next attempt.
        if (_active.load())
        {
            _state.store(PROTOCOL_STATUS_LINKING);
            for (int i = 0; i < 8 && _active.load(); i++)
                usleep(250000);
        }
    }
}

void CpWiredConnection::start()
{
    if (_started)
        return;
    _started = true;
    _active.store(true);

    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();
    _pk = hex(id.pubRaw);
    Bytes pidBytes(id.pairingId.begin(), id.pairingId.end());
    Bytes dh = cp_crypto::sha256(pidBytes);
    char devId[18];
    snprintf(devId, sizeof(devId), "%02X:%02X:%02X:%02X:%02X:%02X", dh[0] | 0x02, dh[1], dh[2], dh[3], dh[4],
             dh[5]);
    _deviceId = devId;

    _haveChip = _chip.open(Settings::mfiI2cBus.value, (uint8_t)Settings::mfiI2cAddr.value);
    MfiAuth::Info info{};
    if (_haveChip)
        _haveChip = _chip.identify(info);
    if (_haveChip)
        _haveChip = _chip.readCertificate(_mfiCert);
    if (_haveChip)
    {
        _protoMajor = info.protocolMajor;
        _signer = std::make_unique<ChipSigner>(_chip, info.protocolMajor);
        _mfiInfo = info.protocolMajor == 3 ? "MFi 3.0" : "MFi 2.0C";
    }
    else
        log_w("carplay-wired: MFi chip absent on %s -- auth will fail",
              Settings::mfiI2cBus.value.c_str());

    cp_av::Sinks sinks;
    sinks.onVideoCodec = [this](bool hevc) { onVideoCodec(hevc); };
    sinks.onVideo = [this](const Bytes &b) { onVideo(b); };
    sinks.onAudio = [this](int t, int r, int c, const Bytes &p) { onAudio(t, r, c, p); };
    cp_av::Config avcfg;
    avcfg.hevc = Settings::carplayHevc;
    _server.setAvConfig(avcfg);
    _server.setAvSinks(sinks);
    _server.setInputSource(this);
    _server.setLifecycle([this] { onSessionConnect(); }, [this] { onSessionDisconnect(); });

    if (!_server.start(7000, _haveChip ? _signer.get() : nullptr))
    {
        log_e("carplay-wired: failed to start :7000 server (already running?)");
        _state.store(PROTOCOL_STATUS_ERROR);
        return;
    }

    _writer = std::thread([this] { writerLoop(); });
    _trigger = std::thread([this] { triggerLoop(); });
    _state.store(PROTOCOL_STATUS_LINKING);
    log_i("carplay-wired ready: %s -- plug in the iPhone (unlocked) over USB", _mfiInfo.c_str());
}

void CpWiredConnection::stop()
{
    if (!_started)
        return;
    _active.store(false);
    writeQueue.notify();
    if (_writer.joinable())
        _writer.join();
    if (_trigger.joinable())
        _trigger.join();
    _mux.stop();
    _server.stop();
    _started = false;
    _state.store(PROTOCOL_STATUS_NO_DEVICE);
}

const std::string CpWiredConnection::status() const
{
    char buf[160];
    snprintf(buf, sizeof(buf), "carplay-wired %s codec=%s frames=%u", _mfiInfo.c_str(),
             _codec.load() == AV_CODEC_ID_HEVC ? "h265" : "h264",
             _frames.load(std::memory_order_relaxed));
    return buf;
}

#endif /* USE_CP_WIRED */
