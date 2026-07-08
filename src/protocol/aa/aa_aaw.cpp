#include "aa_aaw.h"

#ifdef USE_AA_WIRELESS

#include <cerrno>
#include <cstring>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "common/logger.h"

// aaw message ids (from the aaw MessageId enum).
#define AAW_START_REQUEST 1
#define AAW_INFO_REQUEST 2
#define AAW_INFO_RESPONSE 3
#define AAW_VERSION_REQUEST 4
#define AAW_VERSION_RESPONSE 5
#define AAW_CONNECTION_STATUS 6
#define AAW_START_RESPONSE 7
#define AAW_PING 8
#define AAW_PONG 9

namespace aa_aaw
{

// --- minimal protobuf wire helpers ---
static void putVarint(std::vector<uint8_t> &out, uint64_t value)
{
    while (value > 0x7f)
    {
        out.push_back((value & 0x7f) | 0x80);
        value >>= 7;
    }
    out.push_back(value & 0x7f);
}

static void pbVarint(std::vector<uint8_t> &out, int field, uint64_t value)
{
    putVarint(out, (uint64_t)(field << 3) | 0); // wire type 0
    putVarint(out, value);
}

static void pbString(std::vector<uint8_t> &out, int field, const std::string &s)
{
    putVarint(out, (uint64_t)(field << 3) | 2); // wire type 2
    putVarint(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

// Wrap a protobuf body in the RFCOMM frame [len u16BE][msgId u16BE][body].
static std::vector<uint8_t> frame(uint16_t msgId, const std::vector<uint8_t> &body)
{
    std::vector<uint8_t> f;
    f.push_back((body.size() >> 8) & 0xff);
    f.push_back(body.size() & 0xff);
    f.push_back((msgId >> 8) & 0xff);
    f.push_back(msgId & 0xff);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}

static int channelToFreq(int channel)
{
    if (channel <= 13)
        return 2412 + (channel - 1) * 5;
    if (channel == 14)
        return 2484;
    return 5180 + (channel - 36) * 5;
}

std::vector<uint8_t> versionRequest(int channel)
{
    // WPP version 6.0 + the AP frequency as a packed repeated field (3).
    std::vector<uint8_t> freqBody;
    putVarint(freqBody, channelToFreq(channel));
    std::vector<uint8_t> body;
    pbVarint(body, 1, 6);
    pbVarint(body, 2, 0);
    body.push_back(0x22); // field 4, wire type 2 (packed)
    putVarint(body, freqBody.size());
    body.insert(body.end(), freqBody.begin(), freqBody.end());
    return frame(AAW_VERSION_REQUEST, body);
}

std::vector<uint8_t> startRequest(const std::string &ip, uint16_t port)
{
    std::vector<uint8_t> body;
    pbString(body, 1, ip);
    pbVarint(body, 2, port);
    return frame(AAW_START_REQUEST, body);
}

std::vector<uint8_t> infoResponse(const std::string &ssid, const std::string &pass,
                                  const std::string &bssid)
{
    std::vector<uint8_t> body;
    pbString(body, 1, ssid);
    pbString(body, 2, pass);
    pbString(body, 3, bssid);
    pbVarint(body, 4, 8); // security_mode = WPA2_PERSONAL
    pbVarint(body, 5, 0); // access_point_type = STATIC
    return frame(AAW_INFO_RESPONSE, body);
}

// --- RFCOMM socket helpers ---
static bool recvExactly(int fd, uint8_t *dst, size_t n, std::atomic<bool> &active)
{
    size_t got = 0;
    while (got < n && active.load())
    {
        struct pollfd pfd{fd, POLLIN, 0};
        int r = poll(&pfd, 1, 200);
        if (r <= 0)
            continue;
        ssize_t k = recv(fd, dst + got, n - got, 0);
        if (k <= 0)
            return false;
        got += (size_t)k;
    }
    return got == n;
}

static bool recvFrame(int fd, uint16_t &msgId, std::vector<uint8_t> &body, std::atomic<bool> &active)
{
    uint8_t head[4];
    if (!recvExactly(fd, head, 4, active))
        return false;
    uint16_t len = (head[0] << 8) | head[1];
    msgId = (head[2] << 8) | head[3];
    body.resize(len);
    return len == 0 || recvExactly(fd, body.data(), len, active);
}

static bool sendAll(int fd, const std::vector<uint8_t> &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t k = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (k <= 0)
            return false;
        sent += (size_t)k;
    }
    return true;
}

bool runHandshake(int fd, const Params &params, std::atomic<bool> &active)
{
    log_i("aaw: RFCOMM handshake starting");
    if (!sendAll(fd, versionRequest(params.channel)))
        return false;

    bool credentialsSent = false;
    while (active.load())
    {
        uint16_t msgId = 0;
        std::vector<uint8_t> body;
        if (!recvFrame(fd, msgId, body, active))
            break;

        switch (msgId)
        {
        case AAW_VERSION_RESPONSE:
            log_d("aaw: version response, sending start request");
            if (!sendAll(fd, startRequest(params.ip, params.port)))
                return credentialsSent;
            break;

        case AAW_INFO_REQUEST:
            log_i("aaw: info request, sending Wi-Fi credentials");
            if (!sendAll(fd, infoResponse(params.ssid, params.passphrase, params.bssid)))
                return credentialsSent;
            credentialsSent = true;
            break;

        case AAW_CONNECTION_STATUS:
            log_i("aaw: phone reported connection status (%zu bytes)", body.size());
            break;

        case AAW_PING:
        {
            // Echo the ping back as a pong to keep the RFCOMM link alive.
            std::vector<uint8_t> pong = frame(AAW_PONG, body);
            if (!sendAll(fd, pong))
                return credentialsSent;
            break;
        }

        case AAW_START_RESPONSE:
            break; // expected, no action

        default:
            log_d("aaw: unhandled msgId %u (%zu bytes)", msgId, body.size());
            break;
        }
    }

    log_i("aaw: RFCOMM handshake ended (credentials %s)", credentialsSent ? "sent" : "not sent");
    return credentialsSent;
}

} // namespace aa_aaw

#endif /* USE_AA_WIRELESS */
