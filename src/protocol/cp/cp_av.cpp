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
    _timingThread = std::thread([this, phoneTimingPort] { timingLoop(phoneTimingPort); });
    return port;
}

void AvSession::timingLoop(uint16_t phoneTimingPort)
{
    // Where to send our requests: the phone's address (from the control
    // connection) on its advertised timing port, same link scope.
    struct sockaddr_in6 dst = _peer;
    dst.sin6_port = htons(phoneTimingPort);

    auto sendRequest = [&] {
        if (!_havePeer || !phoneTimingPort)
            return;
        uint8_t pkt[32] = {0};
        pkt[0] = 0x80;
        pkt[1] = PT_REQUEST;
        pkt[2] = 0; pkt[3] = 7;               // length in 32-bit words - 1
        putNtp(pkt + 24, ntp64Now());          // our transmit time (T1)
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
                // The phone syncs to us: echo its transmit as originate, stamp T2/T3.
                uint8_t resp[32] = {0};
                resp[0] = 0x80;
                resp[1] = PT_RESPONSE;
                resp[2] = 0; resp[3] = 7;
                std::memcpy(resp + 8, msg + 24, 8);   // request transmit -> originate
                putNtp(resp + 16, ntp64Now());        // T2 receive
                putNtp(resp + 24, ntp64Now());        // T3 transmit
                ::sendto(_timingFd, resp, sizeof(resp), 0, (struct sockaddr *)&from, fl);
            }
            // PT_RESPONSE (our request's answer): bring-up ignores the offset math.
        }

        // Drive a request roughly every second.
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

    Bytes enc, plain;
    uint8_t buf[4096];
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
        if (!_eventCipher->decrypt(enc, dec))
        {
            log_w("[cp-av] event channel decrypt failed");
            break;
        }
        plain.insert(plain.end(), dec.begin(), dec.end());

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
    cp_plist::Value info = cp_plist::Value::map();
    info.set("name", cp_plist::Value::str("FastCarPlay"));
    info.set("deviceID", cp_plist::Value::str("FastCarPlay"));
    info.set("model", cp_plist::Value::str("FastCarPlay1,1"));
    info.set("sourceVersion", cp_plist::Value::str("550.1"));

    cp_rtsp::Response res;
    res.headers["Content-Type"] = PLIST_CT;
    res.body = cp_plist::encode(info);
    log_i("[cp-av] GET /info -> capabilities");
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
        keepAlivePort = openListener(_keepAlive, "keepAlive", [](int fd) {
            uint8_t b[256];
            while (::recv(fd, b, sizeof(b), 0) > 0) {} // drain until closed
        });
        resp.set("keepAlivePort", cp_plist::Value::integer(keepAlivePort));
    }
    cp_plist::Value feats = cp_plist::Value::arr();
    // Match the exact feature set a working head unit (LIVI, cluster+HEVC)
    // advertises: iOS 26 rejects the session SETUP if this set doesn't line up.
    feats.array.push_back(cp_plist::Value::str("hevc"));
    feats.array.push_back(cp_plist::Value::str("iAPChannel"));
    feats.array.push_back(cp_plist::Value::str("viewAreas"));
    feats.array.push_back(cp_plist::Value::str("altScreen"));
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
    shut(_keepAlive);
    for (auto &l : _streams)
        shut(*l);
    _streams.clear();

    if (_timingFd >= 0)
    {
        ::shutdown(_timingFd, SHUT_RDWR);
        ::close(_timingFd);
        _timingFd = -1;
    }
    if (_timingThread.joinable())
        _timingThread.join();
}
} // namespace cp_av
