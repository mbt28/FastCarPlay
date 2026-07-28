#include "cp_iap2_link.h"

#include <unistd.h>

namespace cp_iap2
{
namespace
{
constexpr uint8_t LINK_START_HI = 0xFF;
constexpr uint8_t LINK_START_LO = 0x5A;
} // namespace

Iap2Link::Iap2Link(int fd) : _fd(fd)
{
    // The accessory's proposed link-sync. The peer answers with its own SYN,
    // which becomes authoritative; these values just seed negotiation. Sessions:
    // control (id 10, for CSMs), external-accessory, and file-transfer -- the
    // set an iPhone expects from a CarPlay accessory.
    _lsp.maxOutgoing = 4;
    _lsp.maxLen = 1500;
    _lsp.retransmissionTimeout = 4000;
    _lsp.ackTimeout = 1000;
    _lsp.maxRetransmissions = 4;
    _lsp.maxAck = 3;
    _lsp.sessions = {
        {CONTROL_SESSION_ID, 0, 1}, // control session
        {11, 2, 1},                 // external accessory
        {12, 1, 2},                 // file transfer
    };
}

bool Iap2Link::readExact(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = ::read(_fd, buf + got, n - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

bool Iap2Link::readPacket(LinkHeader &hdr, Bytes &payload)
{
    // Resync to the 0xFF5A start marker, skipping any detect-marker (0xFF5502…)
    // or noise bytes that precede a real link packet.
    uint8_t prev = 0, cur = 0;
    bool synced = false;
    // Prime with one byte.
    if (!readExact(&prev, 1))
        return false;
    while (!synced)
    {
        if (!readExact(&cur, 1))
            return false;
        if (prev == LINK_START_HI && cur == LINK_START_LO)
            synced = true;
        else
            prev = cur;
    }

    // We have the 2 start bytes; read the remaining 7 header bytes.
    Bytes pkt = {LINK_START_HI, LINK_START_LO};
    pkt.resize(9);
    if (!readExact(pkt.data() + 2, 7))
        return false;

    const uint16_t length = (uint16_t)((pkt[2] << 8) | pkt[3]);
    if (length < 9)
        return false;
    if (length > 9)
    {
        // Payload + its 1-byte checksum follow.
        const size_t rest = (size_t)length - 9;
        pkt.resize(9 + rest);
        if (!readExact(pkt.data() + 9, rest))
            return false;
    }
    return parsePacket(pkt, hdr, payload);
}

void Iap2Link::writePacket(uint8_t control, uint8_t seq, uint8_t sessionId, const Bytes &payload)
{
    // The ack field always carries the last in-sequence psn we've received.
    Bytes pkt = packPacket(control, seq, _lastRecvPsn, sessionId, payload);
    size_t off = 0;
    while (off < pkt.size())
    {
        ssize_t w = ::write(_fd, pkt.data() + off, pkt.size() - off);
        if (w <= 0)
        {
            _state = State::Dead;
            return;
        }
        off += (size_t)w;
    }
}

void Iap2Link::sendAck()
{
    writePacket(CONTROL_ACK, _sentPsn, 0, {});
}

bool Iap2Link::negotiate(bool initiate)
{
    _state = State::Detect;

    // Advertise iAP2 support with the detect marker.
    Bytes marker = detectMarker();
    if (::write(_fd, marker.data(), marker.size()) != (ssize_t)marker.size())
        return false;

    if (!initiate)
    {
        // Wait for the peer's detect marker before negotiating.
        Bytes in(marker.size());
        if (!readExact(in.data(), in.size()) || in != marker)
            return false;
    }

    _state = State::Negotiate;
    writePacket(CONTROL_SYN, _sentPsn, 0, packSync(_lsp));

    // Drive SYN/ACK until NORMAL.
    while (_state == State::Negotiate)
    {
        LinkHeader h;
        Bytes payload;
        if (!readPacket(h, payload))
        {
            _state = State::Dead;
            return false;
        }
        if (h.control & CONTROL_SYN)
        {
            LinkSync peer;
            if (parseSync(payload, peer))
                _lsp = peer; // the peer's link-sync is authoritative
            _lastRecvPsn = h.seq;
            sendAck();
        }
        if (h.control & CONTROL_ACK)
            _state = State::Normal;
    }
    return _state == State::Normal;
}

bool Iap2Link::sendControl(const Bytes &csm)
{
    if (_state != State::Normal)
        return false;
    _sentPsn = (uint8_t)(_sentPsn + 1);
    // A data packet carries the ACK bit and the session id (LIVI _send_data).
    writePacket(CONTROL_ACK, _sentPsn, CONTROL_SESSION_ID, csm);
    return _state == State::Normal;
}

bool Iap2Link::recvControl(Bytes &csm)
{
    while (_state == State::Normal)
    {
        LinkHeader h;
        Bytes payload;
        if (!readPacket(h, payload))
        {
            _state = State::Dead;
            return false;
        }
        // A data packet: only the ACK bit set, with a payload (LIVI's
        // "(control & ~ACK) == 0 and payload != None").
        const bool isData = ((h.control & ~CONTROL_ACK) == 0) && !payload.empty();
        if (isData && h.sessionId == CONTROL_SESSION_ID)
        {
            _lastRecvPsn = h.seq;
            sendAck();
            csm = payload;
            return true;
        }
        if (isData) // data on another session -- ack and skip
        {
            _lastRecvPsn = h.seq;
            sendAck();
        }
        // pure ACK / other control: consume and keep waiting.
    }
    return false;
}
} // namespace cp_iap2
