#include "cp_av.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

extern "C"
{
#include <libavcodec/avcodec.h> // AV_INPUT_BUFFER_PADDING_SIZE
}

#include <alsa/asoundlib.h> // blocking mic capture for the uplink
#include <opus/opus.h> // fixed-point Opus encoder for the wireless voice uplink

#include "common/clock.h"
#include "common/logger.h"
#include "common/threading.h" // uplink thread priority
#include "settings.h" // mic-gain
#include "cp_control_cipher.h"
#include "cp_crypto.h"
#include "cp_icons.h"
#include "cp_nalu.h"
#include "cp_plist.h"
#include "cp_rtsp.h"

namespace cp_av
{
Config configForDisplay(int width, int height, int widthMm, int heightMm, int fps, bool hevc)
{
    Config cfg;
    if (width > 0 && height > 0)
    {
        cfg.screenWidth = width;
        cfg.screenHeight = height;
    }
    // Pixels per millimetre of the default 800x480 / 154x90 mm screen, used to
    // synthesise a physical size when the display reports none. Keeps CarPlay's
    // UI at the same apparent size it has always had rather than scaling it by
    // whatever a stale hardcoded panel size implied.
    const double pxPerMm = (double)Config{}.screenWidth / (double)Config{}.screenWidthMm;
    cfg.screenWidthMm = widthMm > 0 ? widthMm : (int)(cfg.screenWidth / pxPerMm + 0.5);
    cfg.screenHeightMm = heightMm > 0 ? heightMm : (int)(cfg.screenHeight / pxPerMm + 0.5);
    if (fps > 0)
        cfg.fps = fps;
    cfg.hevc = hevc;
    return cfg;
}

namespace
{
const char *PLIST_CT = "application/x-apple-binary-plist";

// Capture received stream bytes to $FCP_CP_CAPTURE/<tag>.bin (append) for
// bring-up analysis. No-op when the env var is unset -- and then it must
// cost nothing: this sits on the per-recv hot path, so the env lookups are
// done once and no temporary buffer is ever built.
void capture(const char *tag, const uint8_t *data, size_t len)
{
    static const char *dir = getenv("FCP_CP_CAPTURE");
    static const bool screenToo = getenv("FCP_CP_CAPTURE_SCREEN") != nullptr;
    if (!dir || !data || len == 0)
        return;
    // The screen stream is ~100 KB/s; writing it to the SD card starves CMA
    // (dirty pages pinned -> cedrus allocation failures). Opt in explicitly.
    if (!screenToo && strcmp(tag, "screen") == 0)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/stream-%s.bin", dir, tag);
    if (FILE *f = fopen(path, "ab"))
    {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

// Append a decoded Annex-B access unit to $FCP_CP_VIDEO_OUT (a single elementary
// stream file) for offline decode/verify: `ffmpeg -i $FCP_CP_VIDEO_OUT out.png`.
// No-op when the env var is unset.
void videoOut(const uint8_t *annexB, size_t len)
{
    static const char *path = getenv("FCP_CP_VIDEO_OUT");
    if (!path || !annexB || len == 0)
        return;
    if (FILE *f = fopen(path, "ab"))
    {
        fwrite(annexB, 1, len, f);
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
void put16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
// CarPlay PCM audio-format bits are per (rate, channels): a mono voice format
// is what an input-capable stream (telephony / Siri) negotiates, a stereo one
// is output-only media. Mirrors the PCM/PCM_MONO masks in handleInfo.
bool isMonoPcm(int64_t fmt)
{
    constexpr int64_t MONO = 0x4 | 0x10 | 0x40 | 0x100 | 0x400 | 0x4000;   // 8/16/24/32/44.1/48k mono
    constexpr int64_t STEREO = 0x8 | 0x20 | 0x80 | 0x200 | 0x800 | 0x8000; // same rates, stereo
    return (fmt & MONO) != 0 && (fmt & STEREO) == 0;
}
// Sample rate of a 16-bit PCM audio-format bit (mono or stereo pair), or 0 if
// the format is not one of the recognised PCM bits.
int pcmRate(int64_t fmt)
{
    if (fmt & (0x4 | 0x8))       return 8000;
    if (fmt & (0x10 | 0x20))     return 16000;
    if (fmt & (0x40 | 0x80))     return 24000;
    if (fmt & (0x100 | 0x200))   return 32000;
    if (fmt & (0x400 | 0x800))   return 44100;
    if (fmt & (0x4000 | 0x8000)) return 48000;
    return 0;
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
    // The streamConnectionID is a 64-bit id that is often >= 2^63 (appears
    // negative as int64). The phone renders it UNSIGNED in the salt, so we must
    // too -- a signed render gives the wrong key and every frame fails to
    // decrypt. (The iAP tunnel already casts; this must match it.)
    return cp_crypto::hkdfSha512(_shared, "DataStream-Salt" + std::to_string((uint64_t)streamId),
                                 info, 32);
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
            // Same dead-peer keepalive as the control channel (cp_server.cpp):
            // the event/screen/iAP readers break on recv() <= 0, which only
            // happens for a vanished phone once the kernel gives up probing.
            int one = 1;
            ::setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
            int keepIdle = 5, keepIntvl = 2, keepCnt = 3;
            ::setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(keepIdle));
            ::setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &keepIntvl, sizeof(keepIntvl));
            ::setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &keepCnt, sizeof(keepCnt));
#endif
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
        // A short timeout when we have input to forward keeps touch responsive;
        // otherwise we only need to wake to answer the phone's own requests.
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, _inputSource ? 15 : 300);
        if (r < 0) break;

        if (r > 0 && (pfd.revents & POLLIN))
        {
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) { log_i("[cp-av] event channel EOF (recv=%zd)", n); break; }
            enc.insert(enc.end(), buf, buf + n);

            Bytes dec;
            if (!_eventCipher->decrypt(enc, dec))
            {
                log_w("[cp-av] event channel decrypt FAILED (%zu enc bytes buffered)", enc.size());
                break;
            }
            plain.insert(plain.end(), dec.begin(), dec.end());

            // The phone sends reverse-HTTP requests over this channel; each MUST
            // get a 200 or the session stalls. Parse and answer.
            std::vector<cp_rtsp::Request> reqs = cp_rtsp::parse(plain);
            for (const cp_rtsp::Request &req : reqs)
            {
                if (req.method.rfind("RTSP/", 0) == 0 || req.method.rfind("HTTP/", 0) == 0)
                    continue; // a response to one of our commands
                log_v("[cp-av] event < %s %s (%zuB)", req.method.c_str(), req.path.c_str(), req.body.size());
                cp_rtsp::Response res; // 200
                Bytes out = cp_rtsp::build(req, res);
                Bytes sealed = _eventCipher->encrypt(out);
                ::send(fd, sealed.data(), sealed.size(), MSG_NOSIGNAL);
            }
        }

        // Forward any queued outbound input (touch/button HID reports). All event
        // sends happen on this thread, so the cipher's write counter stays in order.
        if (_inputSource)
        {
            Bytes body;
            while (_inputSource->nextCommand(body))
                sendEventCommand(fd, body);
        }
    }
    log_v("[cp-av] event channel closed");
}

void AvSession::sendEventCommand(int fd, const Bytes &body)
{
    if (!_eventCipher)
        return;
    char head[160];
    int hn = snprintf(head, sizeof(head),
                      "POST /command RTSP/1.0\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "CSeq: %u\r\n\r\n",
                      PLIST_CT, body.size(), ++_eventCseq);
    Bytes msg(head, head + hn);
    msg.insert(msg.end(), body.begin(), body.end());
    Bytes sealed = _eventCipher->encrypt(msg);
    ::send(fd, sealed.data(), sealed.size(), MSG_NOSIGNAL);
}

// ── Screen video receiver: reframe + decrypt into Annex-B ───────────────
// The phone opens a TCP connection to the screen data port and sends a series of
// messages, each a 128-byte header + body:
//   bodySize = u32 LE @0, opcode = byte @4.
//   opcode 1 (VideoConfig): the hvcC/avcC atom, in the clear.
//   opcode 0 (VideoFrame):  ChaCha20-Poly1305 sealed with the 128-byte header as
//                           AAD and an 8-byte LE frame counter as the nonce; the
//                           plaintext is length-prefixed (4-byte BE) NAL units.
//   opcode 2 (KeepAlive):   empty body; carries no payload and no nonce.
// Both config and frames are re-emitted as Annex-B. The frame counter advances
// only on decoded frames. Mirrors LIVI cp/stack/screenStream.ts.
void AvSession::screenLoop(int fd, int64_t streamId)
{
    constexpr size_t HEADER_LEN = 128;
    constexpr uint8_t OP_VIDEO_FRAME = 0;
    constexpr uint8_t OP_VIDEO_CONFIG = 1;
    constexpr size_t MAX_BODY = 8 * 1024 * 1024;

    // Per-stream key: HKDF-SHA512(shared, "DataStream-Salt"<id>, "…-Output-…").
    const Bytes key = streamKey(streamId, "DataStream-Output-Encryption-Key");
    // Reusable AEAD context: key scheduled once, per-packet nonce re-init only
    // (the one-shot helper paid a full EVP setup on every single packet/frame).
    cp_crypto::ChachaStream aead;
    aead.initOpen(key.data());
    uint64_t counter = 0;
    bool codecReported = false;

    auto emit = [&](const uint8_t *annexB, size_t len) {
        if (!annexB || len == 0)
            return;
        videoOut(annexB, len);
        if (_sinks.onVideo)
            _sinks.onVideo(annexB, len);
    };

    // One accumulator, mutated in place: frames are decrypted where they lie
    // (ChaCha20 allows out == in) and the 4-byte NAL length prefixes are
    // overwritten with start codes. From the socket to the decoder's Message
    // a frame is copied once (into the Message), not five times.
    Bytes acc;
    size_t total = 0, frames = 0, configs = 0;
    while (_running)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, 300);
        if (r < 0) break;
        if (r == 0) continue;
        // recv straight into the accumulator's tail: no bounce buffer.
        constexpr size_t CHUNK = 32768;
        const size_t have = acc.size();
        acc.resize(have + CHUNK);
        ssize_t n = ::recv(fd, acc.data() + have, CHUNK, 0);
        if (n <= 0) break;
        acc.resize(have + (size_t)n);
        total += (size_t)n;
        capture("screen", acc.data() + have, (size_t)n); // raw capture (FCP_CP_CAPTURE)

        size_t off = 0;
        while (acc.size() - off >= HEADER_LEN)
        {
            uint8_t *hdr = acc.data() + off;
            const uint32_t bodySize =
                hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            if (bodySize > MAX_BODY)
            {
                log_w("[cp-av] screen implausible bodySize %u -- dropping stream", bodySize);
                return;
            }
            if (acc.size() - off < HEADER_LEN + bodySize)
                break; // message not fully received yet
            const uint8_t opcode = hdr[4];
            uint8_t *body = hdr + HEADER_LEN;

            if (opcode == OP_VIDEO_CONFIG)
            {
                bool hevc = true;
                Bytes annexB = cp_nalu::configToAnnexB(body, bodySize, hevc);
                if (!codecReported)
                {
                    codecReported = true;
                    log_i("[cp-av] screen codec=%s (config %uB -> %zuB param-sets)",
                          hevc ? "HEVC/h265" : "h264", bodySize, annexB.size());
                    if (_sinks.onVideoCodec)
                        _sinks.onVideoCodec(hevc);
                }
                emit(annexB.data(), annexB.size());
                configs++;
            }
            else if (opcode == OP_VIDEO_FRAME)
            {
                uint8_t *plain = body;
                size_t plainLen = bodySize;
                if (bodySize >= 16)
                {
                    // Decrypt in place inside the accumulator; the 128-byte
                    // header (the AAD) sits right before the ciphertext and
                    // is fully read before the first output byte lands.
                    uint8_t nonce[12];
                    cp_crypto::nonce64(counter, nonce);
                    if (!aead.open(nonce, body, bodySize, hdr, HEADER_LEN, body))
                    {
                        log_w("[cp-av] screen frame %llu decrypt FAILED (bodySize=%u) -- "
                              "key/nonce mismatch, stream unrecoverable",
                              (unsigned long long)counter, bodySize);
                        return; // AEAD desync: the counter can no longer align
                    }
                    counter++;
                    plainLen = bodySize - 16;
                }
                if (plainLen > 0 && cp_nalu::avccFrameToAnnexBInPlace(plain, plainLen))
                {
                    if (frames == 0)
                        log_i("[cp-av] screen first frame decrypted+reframed (%zuB annexB)", plainLen);
                    emit(plain, plainLen);
                    frames++;
                }
                else if (plainLen > 0)
                {
                    log_w("[cp-av] screen frame %zuB has a malformed NAL chain -- dropped", plainLen);
                }
            }
            // opcode 2 (keepalive) & others: empty body, no nonce -- skip.

            off += HEADER_LEN + bodySize;
        }
        if (off)
            acc.erase(acc.begin(), acc.begin() + off);
    }
    log_i("[cp-av] screen stream closed (%zu bytes, %zu frames, %zu config)", total, frames, configs);
}

uint16_t AvSession::openUdp(int &fd)
{
    fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    int no = 0;
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
    struct sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    if (::bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ::close(fd);
        fd = -1;
        return 0;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, (struct sockaddr *)&addr, &len);
    return ntohs(addr.sin6_port);
}

// The phone sends RTP/UDP audio packets to the data port: [12B RTP header]
// [ciphertext][16B tag][8B nonce LE], ChaCha20-Poly1305 sealed with the RTP
// timestamp+SSRC (header bytes 4..12) as AAD and the per-stream DataStream output
// key. We decrypt each access unit, decode it (AAC-LC/OPUS -> PCM, or LPCM
// passthrough), and emit S16 interleaved PCM. Mirrors LIVI cp/stack/audioStream.ts.
void AvSession::audioLoop(int dataFd, int ctrlFd, int64_t streamId, int type,
                          cp_audio::Codec codec, int rate, int channels, bool upmix,
                          Listener *uplink)
{
    setThreadName("cp-audio-dl");
    // Elevate above the SCHED_OTHER video decode (same level as the mic uplink
    // and the PcmAudio playback): this thread receives, decrypts and decodes the
    // caller/media audio and feeds the playback queue. Left at normal priority
    // the H.264 decoder starved it, so caller audio arrived in bursts and the
    // playback queue dried every ~0.5s (heard as heavy distortion). Tiny CPU
    // (a call is a PCM byte-swap); stays below the USB transport (FIFO 50).
    setThreadPriority(ThreadPriority::High); // SCHED_FIFO 10

    const Bytes key = streamKey(streamId, "DataStream-Output-Encryption-Key");
    // Reusable AEAD context: key scheduled once, per-packet nonce re-init only
    // (the one-shot helper paid a full EVP setup on every single packet/frame).
    cp_crypto::ChachaStream aead;
    aead.initOpen(key.data());
    cp_audio::Decoder dec;
    if (!dec.init(codec, rate, channels, upmix))
        log_w("[cp-av] audio %d decoder init failed -- draining only", type);
    log_i("[cp-av] audio %d loop (codec=%d %dHz %dch -> %dHz %dch)", type, (int)codec, rate, channels,
          dec.outRate(), dec.outChannels());

    uint8_t buf[4096];
    // Decrypt the access unit into a reusable scratch with ffmpeg's required
    // trailing padding (its bitstream readers over-read past `size`); decoding
    // straight from an exact-sized buffer is an out-of-bounds read.
    std::vector<uint8_t> au;
    au.reserve(4096);
    std::vector<int16_t> pcm;
    size_t totalBytes = 0, packets = 0, decFails = 0;
    // Throughput probe: compare audio delivered vs wall-clock so we can tell a
    // slow/starved feed from a bursty-but-realtime one.
    struct timespec t0{};
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int64_t lastLogMs = 0, lastPktMs = 0;
    size_t lastSamples = 0, totalSamples = 0;
    while (_running)
    {
        struct pollfd pfds[2] = {{dataFd, POLLIN, 0}, {ctrlFd, POLLIN, 0}};
        int r = ::poll(pfds, 2, 300);
        if (r < 0) break;
        if (r == 0)
        {
            // Call-mode safety net: if we are the active telephony stream and
            // the phone has gone silent for 3s, assume a missed TEARDOWN and
            // release media/alt audio. Opus DTX still emits refresh frames
            // every ~400ms, so a live call never trips this.
            if (type == STREAM_MAIN_AUDIO && _callActive.load() &&
                _callStreamId.load() == streamId)
            {
                struct timespec now{};
                clock_gettime(CLOCK_MONOTONIC, &now);
                const int64_t nowMs = (now.tv_sec - t0.tv_sec) * 1000 +
                                      (now.tv_nsec - t0.tv_nsec) / 1000000;
                if (nowMs - lastPktMs > 3000 && _callActive.exchange(false))
                {
                    _callStreamId.store(0);
                    log_w("[cp-av] telephony silent >3s -- call mode off");
                    if (_sinks.onCallState)
                        _sinks.onCallState(false);
                }
            }
            continue;
        }
        if (pfds[1].revents & POLLIN)
            ::recvfrom(ctrlFd, buf, sizeof(buf), 0, nullptr, nullptr); // drain RTCP
        if (!(pfds[0].revents & POLLIN))
            continue;
        struct sockaddr_storage src{};
        socklen_t srcLen = sizeof(src);
        ssize_t n = ::recvfrom(dataFd, buf, sizeof(buf), 0,
                               uplink ? (struct sockaddr *)&src : nullptr,
                               uplink ? &srcLen : nullptr);
        if (n < 12 + 24) // RTP header + tail (16B tag + 8B nonce)
            continue;
        // Telephony-priority: during a call, drain media/alt packets but skip
        // their ChaCha + AAC/opus decode + sink -- the voice stream needs that
        // CPU. markAudioActive keeps /feedback truthful for a stream whose
        // FIRST packet lands mid-call (an unlisted stream gets torn down).
        if (type != STREAM_MAIN_AUDIO && _callActive.load(std::memory_order_relaxed))
        {
            if (packets++ == 0)
                markAudioActive(streamId);
            continue;
        }
        const size_t len = (size_t)n;

        // Latch the phone's reply address from the first downlink packet. The
        // address+scope here is authoritative (where audio really comes from);
        // a SETUP seed may have given only the mic PORT plus a provisional
        // (control-channel) address that doesn't route for UDP on wired. So:
        // - seeded (peerLen set): refresh address+scope, KEEP the seeded port.
        // - unseeded: take the whole source (address+port).
        if (uplink && !uplink->peerAddrLatched.load())
        {
            char before[INET6_ADDRSTRLEN] = "?", after[INET6_ADDRSTRLEN] = "?";
            if (uplink->peerLen && uplink->peer.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&uplink->peer)->sin6_addr, before, sizeof(before));
            if (uplink->peerLen != 0 && src.ss_family == AF_INET6)
            {
                struct sockaddr_in6 *d = (struct sockaddr_in6 *)&uplink->peer;
                struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)&src;
                d->sin6_addr = s6->sin6_addr;          // authoritative address
                d->sin6_scope_id = s6->sin6_scope_id;  // and the routing scope
                // d->sin6_port stays the seeded mic port.
            }
            else
            {
                std::memcpy(&uplink->peer, &src, srcLen);
                uplink->peerLen = srcLen;
            }
            uplink->payloadType.store(buf[1] & 0x7f); // drop the marker bit, keep the PT
            uplink->havePeer.store(true);
            uplink->peerAddrLatched.store(true);
            if (uplink->peer.ss_family == AF_INET6)
                inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&uplink->peer)->sin6_addr, after, sizeof(after));
            log_i("[cp-av] audio %d uplink: peer addr seed=%s%%%u -> actual=%s%%%u port=%u PT=0x%02x", type,
                  before, uplink->peerLen ? ((struct sockaddr_in6 *)&uplink->peer)->sin6_scope_id : 0,
                  after, uplink->peer.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&uplink->peer)->sin6_scope_id : 0,
                  ntohs(((struct sockaddr_in6 *)&uplink->peer)->sin6_port), buf[1] & 0x7f);
        }

        // AAD = RTP timestamp+SSRC (bytes 4..12); nonce = 4 zero + last 8 bytes;
        // ciphertext+tag = bytes 12 .. len-8. Decrypt straight into `au`
        // (plaintext = ciphertext length), keeping ffmpeg's padding past it.
        uint8_t nonce[12] = {0};
        std::memcpy(nonce + 4, buf + len - 8, 8);
        const size_t ctTagLen = len - 8 - 12; // ciphertext + 16B tag
        if (ctTagLen < 16)
            continue;
        const size_t auLen = ctTagLen - 16;
        au.assign(auLen + AV_INPUT_BUFFER_PADDING_SIZE, 0);
        if (!aead.open(nonce, buf + 12, ctTagLen, buf + 4, 8, au.data()))
        {
            if (decFails++ == 0) // log once, not per packet
                log_w("[cp-av] audio %d decrypt failed (key/nonce mismatch)", type);
            continue;
        }

        pcm.clear();
        dec.decode(au.data(), (int)auLen, pcm);
        if (pcm.empty())
            continue;
        totalBytes += pcm.size() * 2;
        totalSamples += pcm.size() / (dec.outChannels() > 0 ? dec.outChannels() : 1);
        if (packets++ == 0)
        {
            log_i("[cp-av] audio %d first packet -> %zu samples @ %dHz %dch", type, pcm.size(),
                  dec.outRate(), dec.outChannels());
            markAudioActive(streamId); // list it in /feedback so the phone keeps it up
        }
        // Every ~2s report audio-ms delivered vs wall-clock-ms (100% = realtime).
        {
            struct timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            int64_t elapsedMs = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
            lastPktMs = elapsedMs; // liveness for the call-mode safety net
            if (elapsedMs - lastLogMs >= 2000)
            {
                const int rate = dec.outRate() > 0 ? dec.outRate() : 44100;
                int64_t audioMs = (int64_t)(totalSamples - lastSamples) * 1000 / rate;
                int64_t wallMs = elapsedMs - lastLogMs;
                log_v("[cp-av] audio %d throughput: %lld ms audio / %lld ms wall (%lld%%), %zu pkts",
                      type, (long long)audioMs, (long long)wallMs, (long long)(audioMs * 100 / (wallMs ? wallMs : 1)),
                      packets);
                lastLogMs = elapsedMs;
                lastSamples = totalSamples;
            }
        }
        if (_sinks.onAudio)
            _sinks.onAudio(type, dec.outRate(), dec.outChannels(),
                           (const uint8_t *)pcm.data(), pcm.size() * 2);
    }
    log_i("[cp-av] audio %d closed (%zu PCM bytes, %zu packets, %zu decrypt fails)", type, totalBytes,
          packets, decFails);
}

// ── Microphone uplink ───────────────────────────────────────────────────
// Capture 16 kHz mono PCM from the ADC and send it to the phone as RTP over
// ChaCha20-Poly1305, the exact mirror of the downlink framing in audioLoop:
//   [12B RTP][ciphertext][16B tag][8B nonce LE], nonce = 4 zero + 8B counter,
//   AAD = RTP bytes 4..12 (timestamp+SSRC), key = the per-stream Input key.
// Blocking ALSA capture feeds this thread directly, so sealing and sending
// run here at normal priority with no per-frame heap allocation.
void AvSession::uplinkLoop(Listener *l)
{
    const uint64_t myGen = l->uplinkGen;
    setThreadName("cp-uplink");
    // Elevate above the SCHED_OTHER video decode + playback: during a call the
    // H.264 decoder periodically monopolised the single CPU for 60-150ms, which
    // starved this thread so the capture queue backed up and had to be trimmed
    // (heard as periodic "sync loss"). This thread is almost always asleep and
    // drains only a few 7.5ms frames per wake, so a brief preemption of the
    // decoder costs nothing. Stays below the USB transport's realtime (FIFO 50).
    setThreadPriority(ThreadPriority::High); // SCHED_FIFO 10

    // Claim the single ALSA capture, waiting out any OLDER input stream's
    // uplink (which releases as soon as it notices it was superseded). Bail if
    // a newer stream supersedes us first, or the session is shutting down.
    bool claimed = false;
    while (_running.load() && _uplinkGen.load() == myGen)
    {
        if (!_captureBusy.exchange(true)) { claimed = true; break; }
        sleep_ms(10);
    }
    if (!claimed)
        return; // superseded before we got the device, or shutting down
    if (!_running.load() || _uplinkGen.load() != myGen)
    {
        _captureBusy.store(false); // superseded right after claiming -- hand it on
        return;
    }

    const int rate = l->rate > 0 ? l->rate : 16000;
    const bool useOpus = (l->uplinkCodec == cp_audio::Codec::Opus);
    const Bytes key = streamKey(l->streamId, "DataStream-Input-Encryption-Key");
    cp_crypto::ChachaStream aead; // key scheduled once; per-frame nonce re-init
    aead.initSeal(key.data());
    // PCM must be framed at the phone's packet cadence, NOT a fixed 20 ms: the
    // phone sends/expects LPCM in fixed-size packets of 7.5 ms (360 samples at
    // 48 kHz, 180 at 24 kHz -- confirmed on the wire; the SETUP dict carries no
    // packet-size field). Sending 20 ms (960-sample) packets made the phone
    // discard our mic entirely (the caller heard nothing). Opus keeps 20 ms:
    // its frame must be a valid Opus duration and its packets self-size.
    const int frameSamples = useOpus ? rate / 50            // 20 ms Opus
                                     : rate * 3 / 400;      // 7.5 ms LPCM
    const size_t frameBytes = (size_t)frameSamples * 2;  // mono S16 capture frame
    const uint32_t ssrc = 0;                             // CarPlay input streams use SSRC 0 (LIVI ref)

    // Direct blocking ALSA capture (no SDL): snd_pcm_readi() sleeps until one
    // RTP frame of samples exists, so this thread wakes exactly once per frame
    // instead of polling an intermediate queue (the old SDL path burned ~half
    // the core in GetQueuedAudioSize/Delay churn). The driver ring is the only
    // buffer; it cannot build a stale backlog, and an overrun surfaces as
    // -EPIPE -> snd_pcm_recover. Fall back to stereo (take the left channel)
    // if the hw refuses mono.
    //
    // ALWAYS capture at 48 kHz when the negotiated rate divides it: the sun4i
    // codec shares one PLL between the DAC and ADC, and opening the hw capture
    // at 16/24 kHz against the mixer's 48 kHz playback is refused with -EIO
    // (wireless voice negotiates 16/24k opus; wired is 48k and never hit it --
    // the old plug layer hid this by running the hw at 48k and resampling).
    // The exact-integer decimation (x2/x3, averaged) happens below.
    const int capRate = (48000 % rate == 0) ? 48000 : rate;
    const int decim = capRate / rate;
    snd_pcm_t *cap = nullptr;
    unsigned capChannels = 1;
    int err = snd_pcm_open(&cap, Settings::alsaDevice.value.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0)
    {
        log_e("[cp-av] uplink: capture open %s failed: %s -- no mic",
              Settings::alsaDevice.value.c_str(), snd_strerror(err));
        _captureBusy.store(false);
        return;
    }
    {
        snd_pcm_hw_params_t *hw;
        snd_pcm_hw_params_alloca(&hw);
        snd_pcm_hw_params_any(cap, hw);
        snd_pcm_hw_params_set_access(cap, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(cap, hw, SND_PCM_FORMAT_S16_LE);
        if (snd_pcm_hw_params_set_channels(cap, hw, 1) < 0)
        {
            capChannels = 2;
            snd_pcm_hw_params_set_channels(cap, hw, 2);
        }
        unsigned gotRate = (unsigned)capRate;
        snd_pcm_hw_params_set_rate_near(cap, hw, &gotRate, nullptr);
        snd_pcm_uframes_t period = (snd_pcm_uframes_t)(frameSamples * decim);
        snd_pcm_uframes_t ring = (snd_pcm_uframes_t)(frameSamples * decim) * 8;
        snd_pcm_hw_params_set_period_size_near(cap, hw, &period, nullptr);
        snd_pcm_hw_params_set_buffer_size_near(cap, hw, &ring);
        err = snd_pcm_hw_params(cap, hw);
        if (err < 0 || gotRate != (unsigned)capRate)
        {
            log_e("[cp-av] uplink: capture params failed (%s, rate %u want %d) -- no mic",
                  snd_strerror(err), gotRate, capRate);
            snd_pcm_close(cap);
            _captureBusy.store(false);
            return;
        }
        log_i("[cp-av] uplink: ALSA capture %u Hz %uch /%d -> %d Hz, period %lu, ring %lu",
              gotRate, capChannels, decim, rate, (unsigned long)period, (unsigned long)ring);
    }

    // The phone picks OPUS for wireless voice (telephony/Siri). This core has no
    // FPU, but libopus here is built fixed-point, so we encode on the uplink.
    // OPUS_APPLICATION_VOIP + voice signal + wideband keeps it SILK-only (cheap)
    // regardless of the sample rate; low complexity + ~24 kbps VBR fits the
    // arm926 budget. Wired CarPlay picks PCM and skips the encoder entirely.
    OpusEncoder *enc = nullptr;
    if (useOpus)
    {
        int err = 0;
        enc = opus_encoder_create(rate, 1, OPUS_APPLICATION_VOIP, &err);
        if (!enc || err != OPUS_OK)
        {
            log_e("[cp-av] uplink: opus_encoder_create(%d) failed (%d) -- no mic", rate, err);
            snd_pcm_close(cap);
            _captureBusy.store(false);
            return;
        }
        // Bitrate tiers mirror LIVI. The distortion was 24 kbps at 48k mono PLUS
        // an artificial wideband cap -- far too few bits for the rate, and the
        // forced downsample mangled it. Give opus the bits for the rate and let
        // it pick its own bandwidth; VOIP + voice signal + low complexity keep
        // the encode affordable on the FPU-less core.
        const int obitrate = rate <= 24000 ? 48000 : rate <= 32000 ? 64000 : 96000;
        opus_encoder_ctl(enc, OPUS_SET_BITRATE(obitrate));
        opus_encoder_ctl(enc, OPUS_SET_VBR(1));
        opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(1));
        opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(enc, OPUS_SET_DTX(0)); // keep RTP timestamps continuous
    }

    // Reused buffers; nothing in the loop allocates. The RTP packet must fit the
    // larger of a raw PCM frame or a worst-case Opus packet.
    const size_t kMaxOpus = 1500;
    const size_t payloadCap = useOpus ? kMaxOpus : frameBytes;
    std::vector<uint8_t> pcm(frameBytes);
    std::vector<int16_t> capBuf((size_t)frameSamples * decim * capChannels); // readi target
    std::vector<uint8_t> encOut(useOpus ? kMaxOpus : 0);
    std::vector<uint8_t> pkt(12 + payloadCap + 16 + 8);
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint64_t nonceCtr = 0;
    size_t sent = 0;
    // DC blocker (one-pole high-pass, ~8Hz @48k): the F1C ADC has a large fixed
    // DC bias (~-2300 LSB measured) that shifts the whole waveform off-centre;
    // the phone's AGC then amplifies that offset into audible distortion. y[n] =
    // x[n] - x[n-1] + R*y[n-1], R=0.999 in Q15 -- integer-only, removes DC/rumble
    // without touching the voice band. State persists across frames.
    constexpr int32_t DC_R_Q15 = 32735; // ~0.99902
    int32_t dcX1 = 0, dcY1 = 0;

    log_i("[cp-av] uplink loop started (stream %lld, %d Hz mono %s, %d-sample frames)",
          (long long)l->streamId, rate, useOpus ? "Opus" : "PCM", frameSamples);
    // Runs until the session stops OR a newer input stream supersedes us (the
    // phone re-SETUPs telephony repeatedly) -- then we release the mic for it.
    while (_running.load() && _uplinkGen.load() == myGen)
    {
        // Block until one RTP frame's worth of hw samples exists (the readi
        // sleep IS the loop pacing -- no polling, no backlog: the small driver
        // ring overruns instead, which recover() clears with a tiny gap).
        const int capFrames = frameSamples * decim;
        snd_pcm_sframes_t got = snd_pcm_readi(cap, capBuf.data(), (snd_pcm_uframes_t)capFrames);
        if (got < 0)
        {
            got = snd_pcm_recover(cap, (int)got, 1);
            if (got < 0)
                log_w("[cp-av] uplink: capture failed: %s", snd_strerror((int)got));
            sleep_ms(50); // pace retries: an unhappy device must not spin a core
            continue;
        }
        if (got < capFrames)
            continue; // short read around a recover: resync on the next frame

        // Drop frames until the downlink has latched where to send (keeps the
        // ring drained so no stale audio accumulates while waiting).
        if (!l->havePeer.load())
            continue;

        // Produce the payload: DC-block + gain, then Opus-encode (wireless) or
        // byte-swap to big-endian (the CarPlay LPCM wire order, mirroring the
        // downlink decode) -- a swap that must NOT be applied to Opus.
        const uint8_t *payload;
        size_t payloadLen;
        {
            int16_t *s16 = reinterpret_cast<int16_t *>(pcm.data());
            // Mono frame at the negotiated rate: average each group of `decim`
            // 48k hw frames (crude but adequate anti-alias for voice) and take
            // channel 0 when the hw insisted on stereo.
            if (capChannels == 1 && decim == 1)
                std::memcpy(s16, capBuf.data(), frameBytes);
            else
                for (int i = 0; i < frameSamples; i++)
                {
                    int32_t acc = 0;
                    const int16_t *src = capBuf.data() + (size_t)i * decim * capChannels;
                    for (int k = 0; k < decim; k++)
                        acc += src[(size_t)k * capChannels];
                    s16[i] = (int16_t)(acc / decim);
                }
            // DC blocker (removes the ADC's fixed bias) then software gain, both
            // with saturation. Do the high-pass FIRST so gain acts on centred
            // audio and does not amplify the offset into clipping.
            const int g = Settings::micGain;
            for (int i = 0; i < frameSamples; i++)
            {
                const int32_t x = s16[i];
                int32_t y = x - dcX1 + (int32_t)(((int64_t)DC_R_Q15 * dcY1) >> 15);
                dcX1 = x;
                dcY1 = y;
                if (g > 1)
                    y *= g;
                s16[i] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
            }

            if (useOpus)
            {
                int nb = opus_encode(enc, s16, frameSamples, encOut.data(),
                                     (opus_int32)encOut.size());
                if (nb < 0)
                {
                    log_w("[cp-av] uplink: opus_encode failed (%d)", nb);
                    continue;
                }
                payload = encOut.data();
                payloadLen = (size_t)nb;
            }
            else
            {
                for (int i = 0; i < frameSamples; i++)
                {
                    uint16_t u = (uint16_t)s16[i];
                    s16[i] = (int16_t)((u >> 8) | (u << 8));
                }
                payload = pcm.data();
                payloadLen = frameBytes;
            }
        }

        uint8_t *p = pkt.data();
        p[0] = 0x80;                      // RTP v2, no padding/ext/CC
        p[1] = l->payloadType.load();     // mirror the phone's payload type
        put16be(p + 2, seq);
        put32be(p + 4, ts);               // timestamp + SSRC are the AAD (bytes 4..12)
        put32be(p + 8, ssrc);

        uint8_t nonce[12] = {0};          // 4 zero bytes + 8-byte LE counter
        for (int i = 0; i < 8; i++)
            nonce[4 + i] = (uint8_t)(nonceCtr >> (8 * i));

        if (!aead.seal(nonce, payload, payloadLen, p + 4, 8, p + 12))
        {
            log_w("[cp-av] uplink: seal failed");
            continue;
        }
        // The nonce counter travels as the last 8 bytes, as the phone does downlink.
        for (int i = 0; i < 8; i++)
            p[12 + payloadLen + 16 + i] = (uint8_t)(nonceCtr >> (8 * i));

        const size_t sendLen = 12 + payloadLen + 16 + 8;
        ::sendto(l->fd, p, sendLen, MSG_NOSIGNAL,
                 (struct sockaddr *)&l->peer, l->peerLen);
        seq++;
        ts += (uint32_t)frameSamples;
        nonceCtr++;
        if (sent++ == 0)
            log_i("[cp-av] uplink: first mic frame sent to phone (stream %lld)",
                  (long long)l->streamId);
    }

    snd_pcm_close(cap);
    if (enc)
        opus_encoder_destroy(enc);
    _captureBusy.store(false);
    log_i("[cp-av] uplink loop stopped (stream %lld, %zu frames sent)", (long long)l->streamId, sent);
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
    if (m == "TEARDOWN")
    {
        // The body lists the streams being torn down (same shape as SETUP). End
        // call mode when the active telephony stream goes -- matched by id so
        // the phone's SETUP-new-then-TEARDOWN-old churn can't kill a live call.
        // A bodyless/streamless TEARDOWN ends the whole AV session.
        cp_plist::Value body = plistOf(req.body);
        const cp_plist::Value *streams = body.find("streams");
        bool endCall = !streams || streams->type != cp_plist::Value::Type::Array;
        if (!endCall)
            for (const cp_plist::Value &s : streams->array)
                if ((int)s.intOr("type") == STREAM_MAIN_AUDIO)
                {
                    const int64_t sid = s.intOr("streamConnectionID");
                    if (sid == 0 || sid == _callStreamId.load())
                        endCall = true;
                }
        if (endCall && _callActive.exchange(false))
        {
            _callStreamId.store(0);
            log_i("[cp-av] TEARDOWN telephony -- call mode off");
            if (_sinks.onCallState)
                _sinks.onCallState(false);
        }
        log_i("[cp-av] TEARDOWN");
        return true;
    }
    if (m == "POST" && endsWith("/feedback")) { res = handleFeedback(req); return true; }
    if (m == "POST" && endsWith("/command"))
    {
        // The phone's control commands are binary plists keyed by "type". The
        // CarPlay dock's car/home icon sends {type:'requestUI'} (no url) to ask
        // the head unit to bring its own UI up -- hand video focus back so the
        // app shows FastCarPlay (resumable), like AA's host-ui-requested.
        cp_plist::Value cmd;
        if (cp_plist::decode(req.body, cmd))
        {
            const cp_plist::Value *t = cmd.find("type");
            const std::string ctype =
                (t && t->type == cp_plist::Value::Type::Str) ? t->s : std::string();
            if (ctype == "requestUI")
            {
                log_i("[cp-av] requestUI -- phone asked for the head-unit UI");
                if (_sinks.onRequestNativeUI)
                    _sinks.onRequestNativeUI();
            }
        }
        return true; // ack
    }
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

    // ── audio formats (PCM/OPUS/AAC-LC) ──
    // NO 44.1k-family bits (0x400/0x800) anywhere: the F1C codec is 48k-native,
    // and the phone PREFERS 44.1k when both are offered (seen with AAC, then
    // again with wired LPCM media at 0x800) -- which forces a fractional
    // resample AND a 44.1k mic capture that the driver must refuse against 48k
    // playback (shared PLL). 8-32k + 48k are all integer-related to 48k.
    const int64_t PCM = 0x3fc | 0xc000;      // 8-32k mono+stereo, 48k mono+stereo
    const int64_t PCM_MONO = 0x154 | 0x4000; // 8/16/24/32k mono + 48k mono
    const int64_t OPUS = 0x70000000;        // OPUS 16/24/48k mono (output)
    const int64_t OPUS_VOICE = 0x30000000;  // OPUS 16k+24k mono (uplink input; SILK, no 48k hybrid)
    const int64_t AAC48 = 0x800000;         // AAC-LC 48k (media / type 102)
    auto af = [&](int type, const char *at, int64_t out, int64_t in, bool hasIn) {
        V d = V::map();
        d.set("type", V::integer(type));
        d.set("audioType", V::str(at));
        d.set("audioOutputFormats", V::integer(out));
        if (hasIn) d.set("audioInputFormats", V::integer(in));
        return d;
    };
    V audioFormats = arr({
        // OUTPUT advertises the full mono-PCM+OPUS voice set (as LIVI / the
        // baseline do): the phone validates the WHOLE audioFormats array, and
        // dropping OPUS -- or narrowing the PCM set -- makes it refuse the media
        // stream (type 102) AND telephony. INPUT (the mic uplink) offers OPUS at
        // 16k+24k only: both are SILK (cheap on this FPU-less core), telephony
        // tends to pick 16k (cellular wideband) and Siri 24k, while the heavy
        // 48k hybrid (SILK+CELT/MDCT) is excluded. Restricting it to 24k-only
        // stopped telephony negotiating at all (no call stream set up); PCM
        // stays broad so the array is still valid.
        af(100, "compatibility", PCM, PCM_MONO, true),
        af(101, "compatibility", PCM, 0, false),
        af(100, "default", PCM | OPUS, PCM_MONO | OPUS_VOICE, true),
        af(100, "alert", PCM | OPUS, 0, false),
        af(100, "media", PCM, 0, false),
        af(100, "telephony", PCM_MONO | OPUS, PCM_MONO | OPUS_VOICE, true),
        af(100, "speechRecognition", PCM_MONO | OPUS, PCM_MONO | OPUS_VOICE, true),
        af(101, "default", PCM | OPUS, 0, false),
        // Media: advertise 48k AAC ONLY. The F1C codec is 48k-native and does
        // NOT support 44.1k, so when the phone streamed 44.1k AAC (it picked
        // 44.1k even when both were offered) SDL float-resampled 44.1k->48k on
        // this FPU-less core -- CPU burn + ALSA underruns + dropped video
        // frames. Forcing 48k makes music hit the codec rate directly (no
        // resample) and matches the 48k dmix. iOS resamples its content to 48k
        // before encoding; LIVI advertises one AAC rate the same way.
        af(102, "media", AAC48, 0, false),
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
    info.set("sourceVersion", V::str("280.33.8"));
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
                // Pick the codec + PCM shape from the format the phone negotiated.
                const int64_t fmt = s.intOr("audioFormat");
                constexpr int64_t AAC44 = 0x400000, AAC48 = 0x800000, OPUS = 0x70000000;
                cp_audio::Codec codec = cp_audio::Codec::Pcm;
                int arate = 44100, achan = 2;
                bool upmix = false;
                if (fmt & (AAC44 | AAC48)) { codec = cp_audio::Codec::AacLc; arate = (fmt & AAC48) ? 48000 : 44100; achan = 2; }
                else if (fmt & OPUS)       { codec = cp_audio::Codec::Opus;  arate = 48000; achan = 1; upmix = true; }
                else if (pcmRate(fmt) > 0)
                {
                    // PCM: derive the real rate + channels from the chosen bit.
                    // Telephony/Siri downlink is 16 kHz MONO (audioFormat=0x10);
                    // decoding it as the old hard-coded 44.1 kHz stereo default
                    // split adjacent mono samples into L/R (a phasey comb) and
                    // ran 2.75x too fast. AAC music (type 102) is unaffected.
                    codec = cp_audio::Codec::Pcm;
                    achan = isMonoPcm(fmt) ? 1 : 2;
                    arate = pcmRate(fmt);
                }

                // Audio is UDP/RTP: bind a data + control (RTCP) port.
                uint16_t dataPort = openUdp(l->fd);
                uint16_t ctrlPort = openUdp(l->ctrlFd);
                if (!dataPort || !ctrlPort)
                {
                    log_w("[cp-av] SETUP audio %d: UDP bind failed", type);
                    continue;
                }
                const int dataFd = l->fd, ctrlFd = l->ctrlFd;

                // Mic uplink: the phone marks an input-capable stream with the
                // explicit "input" bool in the SETUP dict (confirmed live:
                //   audioType="telephony" input=true type=100 audioFormat=16).
                // Gate on that; when it is absent (older iOS?) fall back to the
                // advertised usage, then to the downlink format. Output-only
                // 'alert'/'media' carry input=false, so they never grab the mic.
                // We always capture 16 kHz mono PCM regardless of downlink codec.
                const cp_plist::Value *inputFlag = s.find("input");
                const std::string usage = s.strOr("audioType");
                auto inputCapable = [](const std::string &u) {
                    return u == "telephony" || u == "speechRecognition" ||
                           u == "default" || u == "compatibility";
                };
                bool wantUplink = type == STREAM_MAIN_AUDIO;
                if (inputFlag && inputFlag->type == cp_plist::Value::Type::Bool)
                    wantUplink = wantUplink && inputFlag->b;
                else
                    wantUplink = wantUplink && (usage.empty() ? isMonoPcm(fmt) : inputCapable(usage));
                Listener *lp = l.get();
                lp->streamId = streamId;
                // Uplink codec + rate from the negotiated input format. Wired
                // CarPlay picks PCM (sent as-is at the chosen mono rate); wireless
                // voice (telephony/Siri) picks OPUS, which we encode with libopus
                // at the negotiated 16/24/48k mono rate. Fall back to 16 kHz PCM.
                if (fmt & OPUS)
                {
                    lp->uplinkCodec = cp_audio::Codec::Opus;
                    lp->rate = (fmt & 0x40000000) ? 48000
                             : (fmt & 0x20000000) ? 24000 : 16000;
                }
                else
                {
                    lp->uplinkCodec = cp_audio::Codec::Pcm;
                    lp->rate = pcmRate(fmt) > 0 ? pcmRate(fmt) : 16000;
                }
                // Register for /feedback so the phone keeps this stream alive
                // (an unreported media stream is torn down every few seconds).
                markAudioStream(type, arate, streamId);
                // Telephony-priority: a call makes voice the ONLY active audio
                // (Siri = "speechRecognition" keeps the normal duck instead).
                // Order: id, then flag, then sink -- consumers that see the
                // flag must be able to match the stream, and the sink clears
                // the stale media/alt queues only after the gate is up.
                if (type == STREAM_MAIN_AUDIO && usage == "telephony")
                {
                    _callStreamId.store(streamId);
                    _callActive.store(true);
                    log_i("[cp-av] telephony SETUP -- call mode on (id=%lld)", (long long)streamId);
                    if (_sinks.onCallState)
                        _sinks.onCallState(true);
                }
                if (wantUplink)
                {
                    // The phone re-SETUPs a fresh input stream each time; tag
                    // this one so its uplink supersedes any earlier one.
                    lp->uplinkGen = ++_uplinkGen;
                    // Seed the uplink destination from SETUP so the mic can send
                    // IMMEDIATELY. Siri is listen-first -- it sends no downlink
                    // until it hears us -- so waiting for a downlink packet to
                    // learn the address dead-locks (calls worked only because
                    // the phone sends caller audio down first). Mirror LIVI:
                    // host = control-connection peer, port = the SETUP dict's
                    // dataPort, RTP payload type = the stream type.
                    const int phoneMicPort = (int)s.intOr("dataPort");
                    if (_havePeer && phoneMicPort > 0)
                    {
                        struct sockaddr_in6 dst = _peer;
                        dst.sin6_port = htons((uint16_t)phoneMicPort);
                        std::memcpy(&lp->peer, &dst, sizeof(dst));
                        lp->peerLen = sizeof(dst);
                        lp->payloadType.store((uint8_t)(type & 0x7f));
                        lp->havePeer.store(true);
                        log_i("[cp-av] audio %d uplink: seeded phone mic port %d from SETUP (PT=0x%02x)",
                              type, phoneMicPort, type & 0x7f);
                    }
                }

                // Wired CarPlay ships MUSIC as LPCM on the main-audio stream
                // (type 100, usage=media). Downstream, type picks the sink
                // channel: media must ride the deep-buffered main channel (the
                // voice path's small buffer + latency cap chopped it to bits)
                // and must be dropped in call mode like wireless media. So the
                // loop runs under the MEDIA type; /feedback keeps the real one.
                const int routeType = (type == STREAM_MAIN_AUDIO && usage == "media")
                                          ? STREAM_MAIN_HIGH_AUDIO : type;
                l->thread = std::thread([this, dataFd, ctrlFd, streamId, routeType, codec, arate,
                                         achan, upmix, lp, wantUplink] {
                    audioLoop(dataFd, ctrlFd, streamId, routeType, codec, arate, achan, upmix,
                              wantUplink ? lp : nullptr);
                });
                if (wantUplink)
                {
                    lp->uplinkThread = std::thread([this, lp] { uplinkLoop(lp); });
                    log_i("[cp-av] audio %d uplink enabled (mic -> phone, %d Hz mono %s)", type,
                          lp->rate, lp->uplinkCodec == cp_audio::Codec::Opus ? "Opus" : "PCM");
                }
                port = dataPort;
                entry.set("dataPort", cp_plist::Value::integer(dataPort));
                entry.set("controlPort", cp_plist::Value::integer(ctrlPort));
                entry.set("streamConnectionID", cp_plist::Value::integer(streamId));
                log_i("[cp-av] SETUP audio (type %d usage=%s) fmt=0x%llx codec=%d dataPort=%u ctrlPort=%u id=%lld uplink=%d",
                      type, usage.empty() ? "?" : usage.c_str(), (unsigned long long)fmt, (int)codec,
                      dataPort, ctrlPort, (long long)streamId, (int)wantUplink);
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
    if (_cfg.hevc) // else the phone streams H.264 (e.g. F1C200s cedrus: no HEVC)
        feats.array.push_back(cp_plist::Value::str("hevc"));
    feats.array.push_back(cp_plist::Value::str("iAPChannel"));
    feats.array.push_back(cp_plist::Value::str("viewAreas"));
    resp.set("enabledFeatures", feats);
    res.body = cp_plist::encode(resp);

    log_i("[cp-av] SETUP session -> eventPort=%u timingPort=%u keepAlivePort=%u (phone=%s model=%s)",
          eventPort, timingPort, keepAlivePort, body.strOr("name").c_str(), body.strOr("model").c_str());
    return res;
}

void AvSession::markAudioStream(int type, int rate, int64_t streamId)
{
    std::lock_guard<std::mutex> lk(_audioMetaMtx);
    for (auto &m : _audioMeta)
        if (m.type == type) { m = {type, rate, streamId, false}; return; } // latest wins
    _audioMeta.push_back({type, rate, streamId, false});
}

void AvSession::markAudioActive(int64_t streamId)
{
    std::lock_guard<std::mutex> lk(_audioMetaMtx);
    for (auto &m : _audioMeta)
        if (m.streamId == streamId) { m.active = true; return; }
}

cp_rtsp::Response AvSession::handleFeedback(const cp_rtsp::Request &)
{
    // Report every active audio stream (type + sample rate + id). An EMPTY list
    // makes the phone treat the buffered media stream (type 102) as dead and
    // tear it down every few seconds -- which kept music on the phone instead
    // of the head unit. Mirrors LIVI cp/stack/cpStack _buildFeedback (minus the
    // media-clock timestamp anchor, which we do not estimate yet).
    cp_plist::Value resp = cp_plist::Value::map();
    cp_plist::Value streams = cp_plist::Value::arr();
    {
        std::lock_guard<std::mutex> lk(_audioMetaMtx);
        for (const auto &m : _audioMeta)
        {
            if (!m.active)
                continue;
            cp_plist::Value s = cp_plist::Value::map();
            s.set("type", cp_plist::Value::integer(m.type));
            s.set("sampleRate", cp_plist::Value::integer(m.rate));
            s.set("streamConnectionID", cp_plist::Value::integer(m.streamId));
            streams.array.push_back(s);
        }
    }
    resp.set("streams", streams);
    cp_rtsp::Response res;
    res.headers["Content-Type"] = PLIST_CT;
    res.body = cp_plist::encode(resp);
    return res;
}

void AvSession::stop()
{
    if (!_running.exchange(false))
        return;
    _callActive.store(false); // no sink: the connection resets its own gate
    _callStreamId.store(0);
    auto shut = [](Listener &l) {
        // Join the worker threads BEFORE closing the sockets. The loops all exit
        // on _running (already cleared above) within one poll/sleep cycle,
        // and the uplink thread sends on l.fd every ~20 ms -- closing the fd
        // first would race a sendto/recvfrom against close() on the same fd
        // (use-after-close, and a cross-stream write if the number were reused).
        // Closing a UDP socket does not wake a poll() anyway, so there is no
        // reason to close first; teardown stays bounded by the ~300 ms poll.
        if (l.thread.joinable()) l.thread.join();
        if (l.uplinkThread.joinable()) l.uplinkThread.join();
        if (l.fd >= 0) { ::shutdown(l.fd, SHUT_RDWR); ::close(l.fd); l.fd = -1; }
        if (l.ctrlFd >= 0) { ::close(l.ctrlFd); l.ctrlFd = -1; }
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
