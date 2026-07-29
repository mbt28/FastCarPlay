#include "cp_av.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.h"
#include "cp_control_cipher.h"
#include "cp_crypto.h"
#include "cp_icons.h"
#include "cp_plist.h"
#include "cp_rtsp.h"

namespace cp_av
{
namespace
{
const char *PLIST_CT = "application/x-apple-binary-plist";

// Capture received stream bytes to $FCP_CP_CAPTURE/<tag>.bin (append) for
// bring-up analysis. No-op when the env var is unset.
void capture(const char *tag, const Bytes &b)
{
    const char *dir = getenv("FCP_CP_CAPTURE");
    if (!dir || b.empty())
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/stream-%s.bin", dir, tag);
    if (FILE *f = fopen(path, "ab"))
    {
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
    }
}

cp_plist::Value plistOf(const Bytes &body)
{
    cp_plist::Value v;
    if (!body.empty())
        cp_plist::decode(body, v);
    return v;
}

// Now as a 64-bit NTP timestamp (seconds since 1900 << 32 | fraction). The phone
// derives a clock offset from these, so a monotonically increasing wall-clock is
// enough for bring-up.
uint64_t ntp64Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t sec = (uint64_t)ts.tv_sec + 2208988800ULL; // 1970 -> 1900 epoch
    uint64_t frac = ((uint64_t)ts.tv_nsec << 32) / 1000000000ULL;
    return (sec << 32) | frac;
}
void putNtp(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * (7 - i)));
}
uint64_t getNtp(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = (v << 8) | p[i];
    return v;
}
constexpr uint8_t PT_REQUEST = 210;
constexpr uint8_t PT_RESPONSE = 211;
} // namespace

AvSession::AvSession(const Bytes &pairVerifyShared, const Config &cfg)
    : _cfg(cfg), _shared(pairVerifyShared)
{
}

AvSession::~AvSession() { stop(); }

Bytes AvSession::streamKey(int64_t streamId, const char *info)
{
    return cp_crypto::hkdfSha512(_shared, "DataStream-Salt" + std::to_string(streamId), info, 32);
}

uint16_t AvSession::openListener(Listener &l, const char *tag, std::function<void(int)> onClient)
{
    l.fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (l.fd < 0)
        return 0;
    int yes = 1, no = 0;
    ::setsockopt(l.fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    ::setsockopt(l.fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));

    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = 0; // ephemeral
    if (::bind(l.fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || ::listen(l.fd, 1) != 0)
    {
        ::close(l.fd);
        l.fd = -1;
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(l.fd, (struct sockaddr *)&addr, &len);
    l.port = ntohs(addr.sin6_port);

    int lfd = l.fd;
    std::string name = tag;
    l.thread = std::thread([this, lfd, name, onClient] {
        while (_running)
        {
            struct pollfd pfd{lfd, POLLIN, 0};
            if (::poll(&pfd, 1, 300) <= 0)
                continue;
            int c = ::accept(lfd, nullptr, nullptr);
            if (c < 0)
                continue;
            log_i("[cp-av] %s connected", name.c_str());
            onClient(c);
            ::close(c);
        }
    });
    return l.port;
}

// ── UDP timing (RTCP-style NTP) ─────────────────────────────────────────
uint16_t AvSession::startTiming(uint16_t phoneTimingPort)
{
    _timingFd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (_timingFd < 0)
        return 0;
    int no = 0;
    ::setsockopt(_timingFd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    if (::bind(_timingFd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ::close(_timingFd);
        _timingFd = -1;
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(_timingFd, (struct sockaddr *)&addr, &len);
    uint16_t port = ntohs(addr.sin6_port);
    log_i("[cp-av] timing UDP on :%u -> phone timingPort=%u (havePeer=%d)", port, phoneTimingPort,
          (int)_havePeer);
    _timingThread = std::thread([this, phoneTimingPort] { timingLoop(phoneTimingPort); });
    return port;
}

void AvSession::timingLoop(uint16_t phoneTimingPort)
{
    struct sockaddr_in6 dst = _peer;
    dst.sin6_port = htons(phoneTimingPort);

    // Our clock in the phone's NTP domain (raw wall-clock steered by the measured
    // offset). The phone is the timing reference; if we report a wildly off clock
    // it tears the session down, so we sync ours to it (as LIVI does).
    auto synced = [&]() -> uint64_t { return (uint64_t)((int64_t)ntp64Now() + _clockOffsetNtp); };

    uint64_t pendingT1 = 0;
    auto sendRequest = [&] {
        if (!_havePeer || !phoneTimingPort)
            return;
        uint8_t pkt[32] = {0};
        pkt[0] = 0x80;
        pkt[1] = PT_REQUEST;
        pkt[2] = 0; pkt[3] = 7;
        pendingT1 = synced();
        putNtp(pkt + 24, pendingT1); // our transmit time (T1)
        ::sendto(_timingFd, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    };

    sendRequest();
    int64_t lastSend = 0;
    while (_running && _timingFd >= 0)
    {
        struct pollfd pfd{_timingFd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 200);
        if (r < 0) break;

        if (r > 0 && (pfd.revents & POLLIN))
        {
            uint8_t msg[64];
            struct sockaddr_in6 from{};
            socklen_t fl = sizeof(from);
            ssize_t n = ::recvfrom(_timingFd, msg, sizeof(msg), 0, (struct sockaddr *)&from, &fl);
            if (n >= 32 && msg[1] == PT_REQUEST)
            {
                // The phone syncs to us: echo its transmit, stamp our T2/T3 in the
                // synced domain so it measures ~zero offset against us.
                uint8_t resp[32] = {0};
                resp[0] = 0x80;
                resp[1] = PT_RESPONSE;
                resp[2] = 0; resp[3] = 7;
                std::memcpy(resp + 8, msg + 24, 8);
                putNtp(resp + 16, synced());
                putNtp(resp + 24, synced());
                ::sendto(_timingFd, resp, sizeof(resp), 0, (struct sockaddr *)&from, fl);
            }
            else if (n >= 32 && msg[1] == PT_RESPONSE)
            {
                // Answer to our request -> steer our clock onto the phone's.
                uint64_t t1 = getNtp(msg + 8), t2 = getNtp(msg + 16), t3 = getNtp(msg + 24);
                uint64_t t4 = synced();
                if (t1 == pendingT1)
                {
                    int64_t offset = ((int64_t)(t2 - t1) + (int64_t)(t3 - t4)) / 2;
                    _clockOffsetNtp += offset;
                    pendingT1 = 0;
                }
            }
        }

        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        int64_t nowMs = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
        if (nowMs - lastSend >= 1000)
        {
            sendRequest();
            lastSend = nowMs;
        }
    }
}

// ── Event channel ───────────────────────────────────────────────────────
void AvSession::eventLoop(int fd)
{
    // Events keys from the pair-verify secret (NOT key-swapped: we write with
    // Events-Write, read with Events-Read).
    Bytes writeKey = cp_crypto::hkdfSha512(_shared, "Events-Salt", "Events-Write-Encryption-Key", 32);
    Bytes readKey = cp_crypto::hkdfSha512(_shared, "Events-Salt", "Events-Read-Encryption-Key", 32);
    _eventCipher = std::make_unique<cp_control_cipher::ControlCipher>(readKey, writeKey);

    log_i("[cp-av] event channel loop started (fd %d)", fd);
    Bytes enc, plain;
    uint8_t buf[4096];
    while (_running)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 300);
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { log_i("[cp-av] event channel EOF (recv=%zd)", n); break; }
        log_i("[cp-av] event channel rx %zd bytes", n);
        enc.insert(enc.end(), buf, buf + n);

        Bytes dec;
        if (!_eventCipher->decrypt(enc, dec))
        {
            log_w("[cp-av] event channel decrypt FAILED (%zu enc bytes buffered)", enc.size());
            break;
        }
        plain.insert(plain.end(), dec.begin(), dec.end());
        if (!dec.empty())
            log_i("[cp-av] event channel decrypted %zu bytes", dec.size());

        // The phone sends reverse-HTTP requests over this channel; each MUST get
        // a 200 or the session stalls. Parse and answer.
        std::vector<cp_rtsp::Request> reqs = cp_rtsp::parse(plain);
        for (const cp_rtsp::Request &req : reqs)
        {
            if (req.method.rfind("RTSP/", 0) == 0 || req.method.rfind("HTTP/", 0) == 0)
                continue; // a response to one of our commands
            log_i("[cp-av] event < %s %s (%zuB)", req.method.c_str(), req.path.c_str(), req.body.size());
            cp_rtsp::Response res; // 200
            Bytes out = cp_rtsp::build(req, res);
            Bytes sealed = _eventCipher->encrypt(out);
            ::send(fd, sealed.data(), sealed.size(), MSG_NOSIGNAL);
        }
    }
    log_v("[cp-av] event channel closed");
}

// ── Media stream receivers (milestone A: receive + capture) ─────────────
void AvSession::screenLoop(int fd, int64_t streamId)
{
    (void)streamId;
    uint8_t buf[16384];
    size_t total = 0;
    while (_running)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 300);
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        total += (size_t)n;
        capture("screen", Bytes(buf, buf + n));
    }
    log_i("[cp-av] screen stream closed (%zu bytes received)", total);
}

void AvSession::audioLoop(int fd, int64_t streamId, int type)
{
    (void)streamId;
    uint8_t buf[8192];
    size_t total = 0;
    while (_running)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 300);
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        total += (size_t)n;
        char tag[32];
        snprintf(tag, sizeof(tag), "audio-%d", type);
        capture(tag, Bytes(buf, buf + n));
    }
    log_i("[cp-av] audio stream %d closed (%zu bytes received)", type, total);
}

// ── iAP2-over-CarPlay tunnel (stream 130) ───────────────────────────────
void AvSession::tunnelLoop(int fd, int64_t seed)
{
    // DataStream keys, salted with the SETUP seed (PRIu64 decimal). Layer 1 is
    // the same ChaCha20-Poly1305 stream framing as the control channel, so we
    // reuse ControlCipher with the DataStream keys.
    const std::string salt = "DataStream-Salt" + std::to_string((uint64_t)seed);
    Bytes readKey = cp_crypto::hkdfSha512(_shared, salt, "DataStream-Output-Encryption-Key", 32);
    Bytes writeKey = cp_crypto::hkdfSha512(_shared, salt, "DataStream-Input-Encryption-Key", 32);
    cp_control_cipher::ControlCipher cipher(readKey, writeKey);
    log_i("[cp-av] iAP tunnel connected (salt=%s)", salt.c_str());

    Bytes enc, plain;
    uint8_t buf[16384];
    size_t off = 0;
    while (_running)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 300);
        if (r < 0) break;
        if (r == 0) continue;
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        enc.insert(enc.end(), buf, buf + n);

        Bytes dec;
        if (!cipher.decrypt(enc, dec))
        {
            log_w("[cp-av] iAP tunnel DataStream decrypt failed");
            break;
        }
        plain.insert(plain.end(), dec.begin(), dec.end());

        // Layer 2: APTransportPackage -- 32-byte BE header (size@0, messageType@16).
        while (plain.size() - off >= 32)
        {
            uint32_t size = (plain[off] << 24) | (plain[off + 1] << 16) | (plain[off + 2] << 8) | plain[off + 3];
            if (size < 32 || size > 4 * 1024 * 1024)
            {
                log_w("[cp-av] iAP tunnel implausible package size %u", size);
                return;
            }
            if (plain.size() - off < size)
                break;
            uint32_t messageType = (plain[off + 16] << 24) | (plain[off + 17] << 16) |
                                   (plain[off + 18] << 8) | plain[off + 19];
            Bytes iap(plain.begin() + off + 32, plain.begin() + off + size);
            off += size;
            if (messageType == 0x636f6d6d) // 'comm'
                log_i("[cp-av] tunnel iAP2 msg (%zu bytes): %02x %02x %02x %02x %02x %02x",
                      iap.size(), iap.size() > 0 ? iap[0] : 0, iap.size() > 1 ? iap[1] : 0,
                      iap.size() > 2 ? iap[2] : 0, iap.size() > 3 ? iap[3] : 0,
                      iap.size() > 4 ? iap[4] : 0, iap.size() > 5 ? iap[5] : 0);
            else
                log_i("[cp-av] tunnel package type 0x%08x (%u bytes)", messageType, size);
        }
        if (off)
        {
            plain.erase(plain.begin(), plain.begin() + off);
            off = 0;
        }
    }
    log_i("[cp-av] iAP tunnel closed");
}

// ── RTSP AV request routing ─────────────────────────────────────────────
bool AvSession::handle(const cp_rtsp::Request &req, cp_rtsp::Response &res)
{
    const std::string &m = req.method;
    std::string path = req.path;
    size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);

    auto endsWith = [&](const char *s) {
        std::string suf = s;
        return path.size() >= suf.size() && path.compare(path.size() - suf.size(), suf.size(), suf) == 0;
    };

    if (m == "GET" && endsWith("/info")) { res = handleInfo(req); return true; }
    if (m == "SETUP") { res = handleSetup(req); return true; }
    if (m == "RECORD") { log_i("[cp-av] RECORD -- session started"); return true; }
    if (m == "SET_PARAMETER" || m == "GET_PARAMETER" || m == "OPTIONS" || m == "FLUSH") return true;
    if (m == "TEARDOWN") { log_i("[cp-av] TEARDOWN"); return true; }
    if (m == "POST" && endsWith("/feedback")) { res = handleFeedback(req); return true; }
    if (m == "POST" && endsWith("/command")) { return true; } // ack; commands handled later
    return false;
}

cp_rtsp::Response AvSession::handleInfo(const cp_rtsp::Request &)
{
    using V = cp_plist::Value;
    // The accessory's full AV capabilities. Without this (displays / audio
    // formats / features / modes) the phone tears the session down after RECORD.
    // Mirrors LIVI cp/stack/getInfo.ts buildInfoPlist (main-screen only).
    const char *MAIN_UUID = "b7e6c5a0-1111-4000-8000-000000000001";

    auto arr = [](std::vector<V> items) {
        V a = V::arr();
        a.array = std::move(items);
        return a;
    };

    // ── display (main screen) ──
    V display = V::map();
    display.set("uuid", V::str(MAIN_UUID));
    display.set("type", V::integer(STREAM_MAIN_SCREEN)); // 110
    display.set("maxFPS", V::integer(_cfg.fps));
    display.set("widthPixels", V::integer(_cfg.screenWidth));
    display.set("heightPixels", V::integer(_cfg.screenHeight));
    display.set("widthPhysical", V::integer(_cfg.screenWidthMm));
    display.set("heightPhysical", V::integer(_cfg.screenHeightMm));
    display.set("features", V::integer(0x08 | 0x02)); // high-fidelity touch | knobs
    display.set("primaryInputDevice", V::integer(3));  // knobs (input refined later)
    // We advertise the "viewAreas" feature, so the display must carry the view
    // area geometry (a full-screen area + safe area). Omitting it while claiming
    // the feature makes the phone tear the session down after RECORD.
    {
        V safe = V::map();
        safe.set("widthPixels", V::integer(_cfg.screenWidth));
        safe.set("heightPixels", V::integer(_cfg.screenHeight));
        safe.set("originXPixels", V::integer(0));
        safe.set("originYPixels", V::integer(0));
        safe.set("drawUIOutsideSafeArea", V::boolean(true));
        V view = V::map();
        view.set("widthPixels", V::integer(_cfg.screenWidth));
        view.set("heightPixels", V::integer(_cfg.screenHeight));
        view.set("originXPixels", V::integer(0));
        view.set("originYPixels", V::integer(0));
        view.set("safeArea", safe);
        display.set("viewAreas", arr({view}));
        display.set("initialViewArea", V::integer(0));
    }

    // ── audio formats (PCM/OPUS/AAC-LC at 44.1k) ──
    const int64_t PCM = 0x3fc | 0xc00;      // voice + media 44.1k mono+stereo
    const int64_t PCM_MONO = 0x154 | 0x400; // voice mono + media mono
    const int64_t OPUS = 0x70000000;        // OPUS 16/24/48k mono
    const int64_t AAC_LC = 0x400000;        // AAC-LC 44.1k
    auto af = [&](int type, const char *at, int64_t out, int64_t in, bool hasIn) {
        V d = V::map();
        d.set("type", V::integer(type));
        d.set("audioType", V::str(at));
        d.set("audioOutputFormats", V::integer(out));
        if (hasIn) d.set("audioInputFormats", V::integer(in));
        return d;
    };
    V audioFormats = arr({
        af(100, "compatibility", PCM, PCM_MONO, true),
        af(101, "compatibility", PCM, 0, false),
        af(100, "default", PCM | OPUS, PCM_MONO | OPUS, true),
        af(100, "alert", PCM | OPUS, 0, false),
        af(100, "media", PCM, 0, false),
        af(100, "telephony", PCM_MONO | OPUS, PCM_MONO | OPUS, true),
        af(100, "speechRecognition", PCM_MONO | OPUS, PCM_MONO | OPUS, true),
        af(101, "default", PCM | OPUS, 0, false),
        af(102, "media", AAC_LC, 0, false),
    });

    // ── audio latencies (informational) ──
    auto lat = [&](int type, const char *at, bool hasAt) {
        V d = V::map();
        d.set("type", V::integer(type));
        d.set("inputLatencyMicros", V::integer(0));
        d.set("outputLatencyMicros", V::integer(0));
        if (hasAt) d.set("audioType", V::str(at));
        return d;
    };
    V audioLatencies = arr({
        lat(100, "", false), lat(100, "default", true), lat(100, "media", true),
        lat(100, "telephony", true), lat(100, "speechRecognition", true), lat(100, "alert", true),
        lat(101, "", false), lat(101, "default", true), lat(102, "default", true),
    });

    // ── modes (resources + app states) ──
    auto resource = [&](int id) {
        V r = V::map();
        r.set("resourceID", V::integer(id));
        r.set("transferType", V::integer(1));       // take
        r.set("transferPriority", V::integer(100));  // nice-to-have
        r.set("takeConstraint", V::integer(100));    // anytime
        r.set("borrowConstraint", V::integer(100));
        r.set("unborrowConstraint", V::integer(100));
        return r;
    };
    V modes = V::map();
    modes.set("resources", arr({resource(1), resource(2)}));
    V appState1 = V::map(); appState1.set("appStateID", V::integer(2)); appState1.set("state", V::boolean(false));
    V appState2 = V::map(); appState2.set("appStateID", V::integer(1)); appState2.set("speechMode", V::integer(-1));
    V appState3 = V::map(); appState3.set("appStateID", V::integer(3)); appState3.set("state", V::boolean(false));
    modes.set("appStates", arr({appState1, appState2, appState3}));

    // ── the info dict ──
    V info = V::map();
    info.set("sourceVersion", V::str("550.1"));
    info.set("features", V::integer(0x615653aee2LL)); // CarPlay feature bitmask
    info.set("statusFlags", V::integer(4));
    info.set("model", V::str("FastCarPlay1,1"));
    info.set("manufacturer", V::str("FastCarPlay"));
    info.set("deviceID", V::str("FastCarPlay"));
    info.set("name", V::str("FastCarPlay"));
    info.set("rightHandDrive", V::boolean(false));
    info.set("keepAliveLowPower", V::boolean(true));
    info.set("keepAliveSendStatsAsBody", V::boolean(false));
    info.set("modes", modes);
    info.set("audioLatencies", audioLatencies);
    info.set("audioFormats", audioFormats);
    info.set("extendedFeatures", arr({V::str("vocoderInfo"), V::str("enhancedRequestCarUI")}));
    info.set("displays", arr({display}));

    // ── HID: a multitouch touchscreen (CarPlay needs an input device) ──
    auto fingerCollection = [&](int xMax, int yMax) {
        return Bytes{
            0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x38, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
            0x15, 0x00, 0x25, 0x01, 0x09, 0x33, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95, 0x07,
            0x81, 0x03, 0x05, 0x01, 0x26, (uint8_t)(xMax & 0xff), (uint8_t)((xMax >> 8) & 0xff),
            0x09, 0x30, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x26, (uint8_t)(yMax & 0xff),
            (uint8_t)((yMax >> 8) & 0xff), 0x09, 0x31, 0x81, 0x02, 0xc0};
    };
    Bytes hidDesc = {0x05, 0x0d, 0x09, 0x04, 0xa1, 0x01}; // digitizers / touch screen / app
    for (int c = 0; c < 2; c++)                            // 2 contacts
    {
        Bytes fc = fingerCollection(_cfg.screenWidth, _cfg.screenHeight);
        hidDesc.insert(hidDesc.end(), fc.begin(), fc.end());
    }
    hidDesc.push_back(0xc0); // end collection
    auto hidDevice = [&](const char *uuid, const char *name, const Bytes &desc) {
        V h = V::map();
        h.set("hidProductID", V::integer(1));
        h.set("hidVendorID", V::integer(2));
        h.set("hidCountryCode", V::integer(0));
        h.set("uuid", V::str(uuid));
        h.set("name", V::str(name));
        h.set("displayUUID", V::str(MAIN_UUID));
        h.set("hidDescriptor", V::bytes(desc));
        return h;
    };
    // Full input set (touch + knob + media + telephony) matching a working HU;
    // the display declares knobs primary, so a knob device must be present.
    Bytes knobDesc = {0x05,0x01,0x09,0x08,0xa1,0x01,0x05,0x09,0x09,0x01,0x15,0x00,0x25,0x01,0x75,
        0x01,0x95,0x01,0x81,0x02,0x05,0x0c,0x0a,0x23,0x02,0x0a,0x24,0x02,0x95,0x02,0x81,0x02,0x95,
        0x05,0x81,0x01,0x05,0x01,0x09,0x01,0xa1,0x00,0x09,0x30,0x09,0x31,0x15,0x81,0x25,0x7f,0x75,
        0x08,0x95,0x02,0x81,0x02,0xc0,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x01,0x81,0x06,0xc0};
    Bytes mediaDesc = {0x05,0x0c,0x09,0x01,0xa1,0x01,0x15,0x00,0x25,0x06,0x05,0x0c,0x0a,0x00,0x00,
        0x0a,0xb0,0x00,0x0a,0xb1,0x00,0x0a,0xcd,0x00,0x0a,0xb5,0x00,0x0a,0xb6,0x00,0x0a,0x9e,0x02,
        0x75,0x08,0x95,0x01,0x81,0x00,0xc0};
    Bytes telDesc = {0x05,0x0b,0x09,0x07,0xa1,0x01,0x15,0x00,0x25,0x11,0x05,0x0b,0x09,0x00,0x09,
        0x20,0x09,0x21,0x09,0x26,0x09,0x2f,0x09,0xb0,0x09,0xb1,0x09,0xb2,0x09,0xb3,0x09,0xb4,0x09,
        0xb5,0x09,0xb6,0x09,0xb7,0x09,0xb8,0x09,0xb9,0x09,0xba,0x09,0xbb,0x05,0x07,0x09,0x2a,0x75,
        0x08,0x95,0x01,0x81,0x00,0xc0};
    info.set("hidDevices", arr({
        hidDevice("2a2a2a2a", "FastCarPlay Touchscreen", hidDesc),
        hidDevice("2a2a2a2b", "FastCarPlay Knob", knobDesc),
        hidDevice("2a2a2a2c", "FastCarPlay Media", mediaDesc),
        hidDevice("2a2a2a2d", "FastCarPlay Telephony", telDesc),
    }));

    info.set("bluetoothIDs", arr({V::str("2c:cf:67:fb:12:de")}));

    // OEM icon shown on the CarPlay home screen. iOS requires it -- without an
    // icon there is nothing to place on the springboard and it tears the session
    // down right after RECORD.
    auto icon = [&](const Bytes &png, int px) {
        V ic = V::map();
        ic.set("imageData", V::bytes(png));
        ic.set("widthPixels", V::integer(px));
        ic.set("heightPixels", V::integer(px));
        ic.set("prerendered", V::boolean(true));
        return ic;
    };
    info.set("oemIconVisible", V::boolean(true));
    info.set("oemIconLabel", V::str("FastCarPlay"));
    info.set("oemIcons", arr({icon(FCP_ICON_120, 120), icon(FCP_ICON_180, 180), icon(FCP_ICON_256, 256)}));

    if (_cfg.hevc)
        info.set("hevcInfo", V::map());

    cp_rtsp::Response res;
    res.headers["Content-Type"] = PLIST_CT;
    res.body = cp_plist::encode(info);
    if (const char *dir = getenv("FCP_CP_CAPTURE"))
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/info-response.bin", dir);
        if (FILE *f = fopen(path, "wb")) { fwrite(res.body.data(), 1, res.body.size(), f); fclose(f); }
    }
    log_i("[cp-av] GET /info -> capabilities (%zu bytes)", res.body.size());
    return res;
}

cp_rtsp::Response AvSession::handleSetup(const cp_rtsp::Request &req)
{
    cp_rtsp::Response res;
    res.headers["Content-Type"] = PLIST_CT;

    cp_plist::Value body = plistOf(req.body);
    const cp_plist::Value *streams = body.find("streams");

    if (streams && streams->type == cp_plist::Value::Type::Array)
    {
        // Stream-level SETUP: allocate a data port per stream.
        cp_plist::Value resp = cp_plist::Value::map();
        cp_plist::Value respStreams = cp_plist::Value::arr();
        for (const cp_plist::Value &s : streams->array)
        {
            const int type = (int)s.intOr("type");
            const int64_t streamId = s.intOr("streamConnectionID");
            auto l = std::make_unique<Listener>();
            uint16_t port = 0;
            cp_plist::Value entry = cp_plist::Value::map();
            entry.set("type", cp_plist::Value::integer(type));

            if (type == STREAM_MAIN_SCREEN || type == STREAM_ALT_SCREEN)
            {
                port = openListener(*l, "screen", [this, streamId](int fd) { screenLoop(fd, streamId); });
                entry.set("dataPort", cp_plist::Value::integer(port));
                log_i("[cp-av] SETUP screen (type %d) dataPort=%u id=%lld", type, port, (long long)streamId);
            }
            else if (type == STREAM_MAIN_AUDIO || type == STREAM_ALT_AUDIO || type == STREAM_MAIN_HIGH_AUDIO)
            {
                port = openListener(*l, "audio", [this, streamId, type](int fd) { audioLoop(fd, streamId, type); });
                entry.set("dataPort", cp_plist::Value::integer(port));
                // Echo the phone's streamConnectionID back or it rejects the stream.
                if (body.find("streams"))
                    entry.set("streamConnectionID", cp_plist::Value::integer(streamId));
                log_i("[cp-av] SETUP audio (type %d) dataPort=%u id=%lld", type, port, (long long)streamId);
            }
            else if (type == STREAM_DATA)
            {
                // The iAP2-over-CarPlay tunnel. Key salt uses the stream "seed".
                const int64_t seed = s.intOr("seed");
                port = openListener(*l, "iap-tunnel", [this, seed](int fd) { tunnelLoop(fd, seed); });
                entry.set("streamID", cp_plist::Value::integer(1));
                entry.set("dataPort", cp_plist::Value::integer(port));
                log_i("[cp-av] SETUP iAP tunnel (type 130) dataPort=%u seed=%lld", port,
                      (long long)seed);
            }
            else
            {
                log_w("[cp-av] SETUP stream type %d not handled", type);
                continue;
            }
            if (port)
                _streams.push_back(std::move(l));
            respStreams.array.push_back(entry);
        }
        resp.set("streams", respStreams);
        res.body = cp_plist::encode(resp);
        return res;
    }

    // Session-level SETUP: open the event channel + drive UDP clock sync against
    // the phone's timing port (without it the phone tears the session down).
    uint16_t eventPort = openListener(_event, "event", [this](int fd) { eventLoop(fd); });
    uint16_t timingPort = startTiming((uint16_t)body.intOr("timingPort"));

    cp_plist::Value resp = cp_plist::Value::map();
    resp.set("eventPort", cp_plist::Value::integer(eventPort));
    resp.set("timingPort", cp_plist::Value::integer(timingPort));
    // The phone asks for a low-power keep-alive channel; it expects a port back
    // when it set keepAliveLowPower, and drops the session otherwise.
    uint16_t keepAlivePort = 0;
    const cp_plist::Value *ka = body.find("keepAliveLowPower");
    if (ka && ka->type == cp_plist::Value::Type::Bool && ka->b)
    {
        // The keep-alive channel is UDP (dgram), not TCP -- the phone sends
        // low-power keepalive datagrams and expects the port to simply absorb
        // them. A TCP listener here makes the phone's UDP probes bounce (ICMP
        // port unreachable) and it tears the session down.
        _keepAliveFd = ::socket(AF_INET6, SOCK_DGRAM, 0);
        if (_keepAliveFd >= 0)
        {
            int no = 0;
            ::setsockopt(_keepAliveFd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
            struct sockaddr_in6 ka6{};
            ka6.sin6_family = AF_INET6;
            ka6.sin6_addr = in6addr_any;
            if (::bind(_keepAliveFd, (struct sockaddr *)&ka6, sizeof(ka6)) == 0)
            {
                socklen_t l = sizeof(ka6);
                ::getsockname(_keepAliveFd, (struct sockaddr *)&ka6, &l);
                keepAlivePort = ntohs(ka6.sin6_port);
                _keepAliveThread = std::thread([this] {
                    uint8_t b[256];
                    while (_running && _keepAliveFd >= 0)
                    {
                        struct pollfd p{_keepAliveFd, POLLIN, 0};
                        if (::poll(&p, 1, 300) > 0)
                            ::recvfrom(_keepAliveFd, b, sizeof(b), 0, nullptr, nullptr); // absorb
                    }
                });
                resp.set("keepAlivePort", cp_plist::Value::integer(keepAlivePort));
            }
            else { ::close(_keepAliveFd); _keepAliveFd = -1; }
        }
    }
    cp_plist::Value feats = cp_plist::Value::arr();
    feats.array.push_back(cp_plist::Value::str("hevc"));
    feats.array.push_back(cp_plist::Value::str("iAPChannel"));
    feats.array.push_back(cp_plist::Value::str("viewAreas"));
    resp.set("enabledFeatures", feats);
    res.body = cp_plist::encode(resp);

    log_i("[cp-av] SETUP session -> eventPort=%u timingPort=%u keepAlivePort=%u (phone=%s model=%s)",
          eventPort, timingPort, keepAlivePort, body.strOr("name").c_str(), body.strOr("model").c_str());
    return res;
}

cp_rtsp::Response AvSession::handleFeedback(const cp_rtsp::Request &)
{
    // Minimal feedback: an empty streams list keeps the phone's media clock happy.
    cp_plist::Value resp = cp_plist::Value::map();
    resp.set("streams", cp_plist::Value::arr());
    cp_rtsp::Response res;
    res.headers["Content-Type"] = PLIST_CT;
    res.body = cp_plist::encode(resp);
    return res;
}

void AvSession::stop()
{
    if (!_running.exchange(false))
        return;
    auto shut = [](Listener &l) {
        if (l.fd >= 0) { ::shutdown(l.fd, SHUT_RDWR); ::close(l.fd); l.fd = -1; }
        if (l.thread.joinable()) l.thread.join();
    };
    shut(_event);
    for (auto &l : _streams)
        shut(*l);
    _streams.clear();

    if (_timingFd >= 0) { ::shutdown(_timingFd, SHUT_RDWR); ::close(_timingFd); _timingFd = -1; }
    if (_timingThread.joinable())
        _timingThread.join();
    if (_keepAliveFd >= 0) { ::shutdown(_keepAliveFd, SHUT_RDWR); ::close(_keepAliveFd); _keepAliveFd = -1; }
    if (_keepAliveThread.joinable())
        _keepAliveThread.join();
}
} // namespace cp_av
