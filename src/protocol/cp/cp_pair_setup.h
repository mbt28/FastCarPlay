#ifndef SRC_PROTOCOL_CP_CP_PAIR_SETUP
#define SRC_PROTOCOL_CP_CP_PAIR_SETUP

// CarPlay pair-setup responder (accessory side). Unauthenticated SRP-6a
// (3072-bit, SHA-512, fixed PIN "3939") followed by an encrypted Ed25519
// long-term-key exchange (M5/M6). The controller's LTPK is persisted for
// later pair-verify; there is no MFi inside pair-setup. Mirrors LIVI
// pairSetup.ts. HAP TLV8, M1->M2->M3->M4->M5->M6.

#include <cstdint>
#include <map>
#include <vector>

#include "cp_srp.h"

namespace cp_pair_setup
{
using Bytes = std::vector<uint8_t>;

class PairSetup
{
public:
    // Handle one pair-setup TLV8 request, return the TLV8 response body.
    Bytes handle(const Bytes &body);
    bool complete() const { return _complete; }

private:
    Bytes m2();
    Bytes m4(const std::map<uint8_t, Bytes> &tlv);
    Bytes m6(const std::map<uint8_t, Bytes> &tlv);
    Bytes err(uint8_t state);

    cp_srp::Server _srp;
    bool _srpStarted = false;
    Bytes _K;
    bool _complete = false;
};
} // namespace cp_pair_setup

#endif /* SRC_PROTOCOL_CP_CP_PAIR_SETUP */
