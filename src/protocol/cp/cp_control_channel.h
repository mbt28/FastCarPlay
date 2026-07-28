#ifndef SRC_PROTOCOL_CP_CP_CONTROL_CHANNEL
#define SRC_PROTOCOL_CP_CP_CONTROL_CHANNEL

// The CarPlay control channel: RTSP/HTTP framing on top of the transport,
// dispatching /pair-setup, /pair-verify and /auth-setup to their responders,
// and switching to ChaCha20-Poly1305 framing once pair-verify establishes the
// session keys. A slice of LIVI cpStack.ts -- just the handshake; the stream
// setup (SETUP/RECORD, screen/audio/HID) comes later.
//
// Byte-in / byte-out so it is transport-agnostic (USB-NCM, TCP, a test pipe):
// feed received bytes, send back what it returns.

#include <cstdint>
#include <memory>
#include <vector>

#include <netinet/in.h>

#include "cp_auth_setup.h"
#include "cp_av.h"
#include "cp_control_cipher.h"
#include "cp_pair_setup.h"
#include "cp_pair_verify.h"
#include "cp_rtsp.h"

namespace cp_control_channel
{
using Bytes = std::vector<uint8_t>;

class ControlChannel
{
public:
    // `signer` (the MFi chip) is optional -- without it /auth-setup returns an
    // error, but pairing still works.
    explicit ControlChannel(cp_auth_setup::MfiSigner *signer = nullptr) : _signer(signer) {}

    // Feed received bytes; returns bytes to send back. Handles partial reads
    // and the plaintext->encrypted transition internally.
    Bytes process(const Bytes &incoming);

    // The controller's address, forwarded to the AV session for UDP timing.
    void setPeer(const struct sockaddr_in6 &peer) { _peer = peer; _havePeer = true; }

    bool paired() const { return _verify.verified(); }
    bool encrypted() const { return _cipher != nullptr; }

private:
    cp_rtsp::Response route(const cp_rtsp::Request &req);

    cp_auth_setup::MfiSigner *_signer;
    cp_pair_setup::PairSetup _setup;
    cp_pair_verify::PairVerify _verify;
    std::unique_ptr<cp_control_cipher::ControlCipher> _cipher;
    std::unique_ptr<cp_av::AvSession> _av; // the AV layer, created after pairing
    struct sockaddr_in6 _peer{};
    bool _havePeer = false;
    Bytes _cipherIn;  // ciphertext awaiting whole frames (encrypted mode)
    Bytes _plainIn;   // plaintext RTSP awaiting whole messages
    bool _activateCipherAfterResponse = false;
};
} // namespace cp_control_channel

#endif /* SRC_PROTOCOL_CP_CP_CONTROL_CHANNEL */
