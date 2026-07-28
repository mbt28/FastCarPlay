#ifndef SRC_PROTOCOL_CP_CP_IAP2
#define SRC_PROTOCOL_CP_CP_IAP2

// iAP2 codec -- the wired-CarPlay trigger protocol. The head unit speaks iAP2
// over USB to identify itself as a CarPlay accessory, which makes the iPhone
// bring up a USB-NCM link that carries CarPlay over IP (to the :7000 control
// server). This is the framing layer: link packets, the link-sync (SYN)
// payload, and control-session messages. Mirrors LIVI cp/iap2/link_layer.py
// and control_session_message. The USB transport + live handshake need a real
// iPhone on USB; this codec is what runs over it.

#include <cstdint>
#include <vector>

namespace cp_iap2
{
using Bytes = std::vector<uint8_t>;

// Link-packet control bits.
constexpr uint8_t CONTROL_SYN = 0x80;
constexpr uint8_t CONTROL_ACK = 0x40;
constexpr uint8_t CONTROL_EAK = 0x20;

// Sent to the device to detect/trigger iAP2 support.
inline Bytes detectMarker() { return {0xFF, 0x55, 0x02, 0x00, 0xEE, 0x10}; }

struct LinkHeader
{
    uint16_t length = 0; // total packet size
    uint8_t control = 0;
    uint8_t seq = 0;
    uint8_t ack = 0;
    uint8_t sessionId = 0;
};

// Build a whole link packet: 9-byte header (start 0xFF5A + fields + checksum),
// then, if payload is non-empty, the payload + its checksum.
Bytes packPacket(uint8_t control, uint8_t seq, uint8_t ack, uint8_t sessionId, const Bytes &payload);

// Parse one whole link packet. Validates the start marker and both checksums.
// Returns false on any mismatch.
bool parsePacket(const Bytes &pkt, LinkHeader &hdr, Bytes &payload);

// ── Link synchronization (the SYN payload) ──────────────────────────────
struct LspSession
{
    uint8_t id;
    uint8_t type;
    uint8_t version;
};
struct LinkSync
{
    uint8_t maxOutgoing = 0;
    uint16_t maxLen = 0;
    uint16_t retransmissionTimeout = 0;
    uint16_t ackTimeout = 0;
    uint8_t maxRetransmissions = 0;
    uint8_t maxAck = 0;
    std::vector<LspSession> sessions;
};
Bytes packSync(const LinkSync &sync);
bool parseSync(const Bytes &payload, LinkSync &sync);

// ── Control-session messages (CSM) ──────────────────────────────────────
struct CsmParam
{
    uint16_t id;
    Bytes value;
};
// [0x4040][u16 length][u16 msgId] then params: [u16 paramLen][u16 paramId][value].
Bytes packCsm(uint16_t msgId, const std::vector<CsmParam> &params);
bool parseCsm(const Bytes &msg, uint16_t &msgId, std::vector<CsmParam> &params);
} // namespace cp_iap2

#endif /* SRC_PROTOCOL_CP_CP_IAP2 */
