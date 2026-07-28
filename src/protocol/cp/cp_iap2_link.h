#ifndef SRC_PROTOCOL_CP_CP_IAP2_LINK
#define SRC_PROTOCOL_CP_CP_IAP2_LINK

// iAP2 link-layer session over a reliable byte stream (a Bluetooth RFCOMM
// socket for wireless CarPlay, or USB bulk for wired). This drives the link
// state machine -- detect -> negotiate (SYN/ACK exchange of link-sync payloads)
// -> normal -- and then carries control-session messages (CSMs) as data
// packets on the control session. It is the transport under the CarPlay
// wireless handoff (cp_carplay_msg): once NORMAL, the accessory sends
// CarPlayStartSession to hand the phone the Wi-Fi credentials + :7000 endpoint.
//
// Mirrors LIVI cp/iap2/link_layer.py. Because RFCOMM (L2CAP) already delivers a
// reliable, ordered stream, this implementation relies on that and does not
// implement iAP2's retransmission window (LIVI supports the same via zero-ack);
// it still exchanges ACKs, which the peer expects. Blocking I/O on the caller's
// thread -- one link per connection, same shape as aa_aaw::runHandshake.

#include <atomic>
#include <cstdint>

#include "cp_iap2.h"

namespace cp_iap2
{
class Iap2Link
{
public:
    // The control session id iAP2 assigns for CSMs.
    static constexpr uint8_t CONTROL_SESSION_ID = 10;

    explicit Iap2Link(int fd);

    // Run detect + negotiate until the link is NORMAL. `initiate` = send our SYN
    // immediately (initiator); otherwise wait for the peer's detect marker
    // first (accessory-over-RFCOMM default). Returns true once NORMAL.
    bool negotiate(bool initiate);

    // Send one control-session message (a full 0x4040 CSM) as a data packet.
    bool sendControl(const Bytes &csm);

    // Block for the next control-session CSM from the peer, ACKing it. Returns
    // false on EOF / link death. Pure-ACK and non-control packets are consumed
    // internally.
    bool recvControl(Bytes &csm);

    bool alive() const { return _state != State::Dead; }

private:
    enum class State
    {
        Detect,
        Negotiate,
        Normal,
        Dead
    };

    bool readExact(uint8_t *buf, size_t n);
    // Read one whole link packet, resyncing to the 0xFF5A start (skips stray
    // detect-marker bytes). Returns false on EOF.
    bool readPacket(LinkHeader &hdr, Bytes &payload);
    void writePacket(uint8_t control, uint8_t seq, uint8_t sessionId, const Bytes &payload);
    void sendAck();
    // Pump one packet through the state machine. `csmOut` is set (and true
    // returned via *gotCsm) when a control-session data packet arrives.
    bool pumpUntilControl(Bytes &csmOut, bool waitForNormal, bool *gotCsm);

    int _fd;
    State _state = State::Detect;
    uint8_t _sentPsn = 99;             // our sequence number (pre-increment on data)
    uint8_t _lastRecvPsn = 0;          // last in-sequence psn we've seen (our ack field)
    LinkSync _lsp;                     // our advertised link-sync (seeds negotiation)
};
} // namespace cp_iap2

#endif /* SRC_PROTOCOL_CP_CP_IAP2_LINK */
