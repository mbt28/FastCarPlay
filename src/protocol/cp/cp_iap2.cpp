#include "cp_iap2.h"

namespace cp_iap2
{
namespace
{
constexpr uint16_t LINK_START = 0xFF5A;
constexpr uint16_t CSM_START = 0x4040;
constexpr uint8_t LSP_VERSION = 0x01;

// iAP2 checksum: the byte that makes the running 8-bit sum zero.
uint8_t checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum = (uint8_t)(sum + data[i]);
    return (uint8_t)(-(int)sum);
}

void put16(Bytes &b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); }
uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
} // namespace

Bytes packPacket(uint8_t control, uint8_t seq, uint8_t ack, uint8_t sessionId, const Bytes &payload)
{
    const uint16_t length = payload.empty() ? 9 : (uint16_t)(payload.size() + 10);

    Bytes header;
    put16(header, LINK_START);
    put16(header, length);
    header.push_back(control);
    header.push_back(seq);
    header.push_back(ack);
    header.push_back(sessionId);

    Bytes pkt = header;
    pkt.push_back(checksum(header.data(), header.size()));
    if (!payload.empty())
    {
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        pkt.push_back(checksum(payload.data(), payload.size()));
    }
    return pkt;
}

bool parsePacket(const Bytes &pkt, LinkHeader &hdr, Bytes &payload)
{
    if (pkt.size() < 9 || get16(pkt.data()) != LINK_START)
        return false;
    // Header checksum: the 9 header bytes must sum to zero.
    uint8_t hsum = 0;
    for (int i = 0; i < 9; i++)
        hsum = (uint8_t)(hsum + pkt[i]);
    if (hsum != 0)
        return false;

    hdr.length = get16(pkt.data() + 2);
    hdr.control = pkt[4];
    hdr.seq = pkt[5];
    hdr.ack = pkt[6];
    hdr.sessionId = pkt[7];

    if (hdr.length != pkt.size())
        return false;

    payload.clear();
    if (hdr.length > 9)
    {
        const size_t payloadLen = hdr.length - 10;
        if (9 + payloadLen + 1 != pkt.size())
            return false;
        payload.assign(pkt.begin() + 9, pkt.begin() + 9 + payloadLen);
        uint8_t psum = 0;
        for (size_t i = 0; i < payloadLen + 1; i++)
            psum = (uint8_t)(psum + pkt[9 + i]);
        if (psum != 0)
            return false;
    }
    return true;
}

Bytes packSync(const LinkSync &sync)
{
    Bytes p;
    p.push_back(LSP_VERSION);
    p.push_back(sync.maxOutgoing);
    put16(p, sync.maxLen);
    put16(p, sync.retransmissionTimeout);
    put16(p, sync.ackTimeout);
    p.push_back(sync.maxRetransmissions);
    p.push_back(sync.maxAck);
    for (const LspSession &s : sync.sessions)
    {
        p.push_back(s.id);
        p.push_back(s.type);
        p.push_back(s.version);
    }
    return p;
}

bool parseSync(const Bytes &payload, LinkSync &sync)
{
    if (payload.size() < 10 || payload[0] != LSP_VERSION)
        return false;
    sync.maxOutgoing = payload[1];
    sync.maxLen = get16(payload.data() + 2);
    sync.retransmissionTimeout = get16(payload.data() + 4);
    sync.ackTimeout = get16(payload.data() + 6);
    sync.maxRetransmissions = payload[8];
    sync.maxAck = payload[9];
    sync.sessions.clear();
    for (size_t i = 10; i + 3 <= payload.size(); i += 3)
        sync.sessions.push_back({payload[i], payload[i + 1], payload[i + 2]});
    return true;
}

Bytes packCsm(uint16_t msgId, const std::vector<CsmParam> &params)
{
    Bytes body;
    for (const CsmParam &param : params)
    {
        const uint16_t paramLen = (uint16_t)(4 + param.value.size());
        put16(body, paramLen);
        put16(body, param.id);
        body.insert(body.end(), param.value.begin(), param.value.end());
    }
    Bytes msg;
    put16(msg, CSM_START);
    put16(msg, (uint16_t)(6 + body.size()));
    put16(msg, msgId);
    msg.insert(msg.end(), body.begin(), body.end());
    return msg;
}

bool parseCsm(const Bytes &msg, uint16_t &msgId, std::vector<CsmParam> &params)
{
    if (msg.size() < 6 || get16(msg.data()) != CSM_START)
        return false;
    const uint16_t length = get16(msg.data() + 2);
    if (length < 6 || length > msg.size())
        return false;
    msgId = get16(msg.data() + 4);

    params.clear();
    size_t p = 6;
    while (p + 4 <= length)
    {
        const uint16_t paramLen = get16(msg.data() + p);
        const uint16_t paramId = get16(msg.data() + p + 2);
        if (paramLen < 4 || p + paramLen > length)
            return false;
        params.push_back({paramId, Bytes(msg.begin() + p + 4, msg.begin() + p + paramLen)});
        p += paramLen;
    }
    return true;
}
} // namespace cp_iap2
