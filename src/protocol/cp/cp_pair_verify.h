#ifndef SRC_PROTOCOL_CP_CP_PAIR_VERIFY
#define SRC_PROTOCOL_CP_CP_PAIR_VERIFY

// CarPlay pair-verify responder (accessory side). A reconnecting phone proves
// it holds a controller LTPK stored at pair-setup, via ephemeral X25519 +
// Ed25519 signatures (HAP pair-verify, M1->M2->M3->M4, TLV8). On success it
// yields the per-direction ChaCha20-Poly1305 control-channel keys and the
// connection switches to encrypted framing. Mirrors LIVI pairVerify.ts.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cp_pair_verify
{
using Bytes = std::vector<uint8_t>;

struct ControlKeys
{
    Bytes readKey;  // accessory reads (decrypts) controller->accessory with this
    Bytes writeKey; // accessory writes (encrypts) accessory->controller with this
};

class PairVerify
{
public:
    // Handle one pair-verify TLV8 request, return the TLV8 response body.
    // On an M3 that authenticates, verified() becomes true and keys()/
    // controllerId() are populated.
    Bytes handle(const Bytes &body);

    bool verified() const { return _verified; }
    const std::string &controllerId() const { return _controllerId; }
    const ControlKeys &keys() const { return _keys; }
    const Bytes &sharedSecret() const { return _shared; } // for per-stream keys

private:
    Bytes m2(const std::map<uint8_t, Bytes> &tlv);
    Bytes m4(const std::map<uint8_t, Bytes> &tlv);
    Bytes err(uint8_t state);

    Bytes _ephPub;
    Bytes _clientEphPub;
    Bytes _shared;
    Bytes _encKey;
    ControlKeys _keys;
    bool _verified = false;
    std::string _controllerId;
};
} // namespace cp_pair_verify

#endif /* SRC_PROTOCOL_CP_CP_PAIR_VERIFY */
