#ifndef SRC_PROTOCOL_CP_CP_AUTH_SETUP
#define SRC_PROTOCOL_CP_CP_AUTH_SETUP

// CarPlay /auth-setup (MFiSAP) responder, accessory side. Proves genuine MFi
// licensing to the phone: it replies with our ephemeral X25519 public key, our
// MFi certificate, and the coprocessor's signature over the two public keys,
// with the signature AES-128-CTR encrypted under a key derived from the shared
// secret. Mirrors LIVI cp/stack/authSetup.ts. Runs over the already-encrypted
// control channel.
//
// The signer is abstracted (as in LIVI) so this depends only on "get cert /
// sign digest / which protocol", never on the I2C driver -- a dongle firmware
// could serve the same contract. The concrete signer wraps the P0 MfiAuth.

#include <cstdint>
#include <vector>

namespace cp_auth_setup
{
using Bytes = std::vector<uint8_t>;

class MfiSigner
{
public:
    virtual ~MfiSigner() = default;
    virtual bool certificate(Bytes &out) = 0;         // accessory MFi certificate
    virtual bool sign(const Bytes &digest, Bytes &sig) = 0; // chip signature over digest
    virtual int protocolMajor() = 0;                  // 2 (2.0C, SHA-1) or 3 (3.0, SHA-256)
};

// Handle one /auth-setup request. `request` is the raw body:
//   [1B version=1][32B controller X25519 public key]
// Returns the raw response body on success:
//   [32B our X25519 pub][4B certLen BE][cert][4B sigLen BE][encrypted signature]
// or an empty vector if the request is malformed or signing fails.
Bytes handle(const Bytes &request, MfiSigner &signer);
} // namespace cp_auth_setup

#endif /* SRC_PROTOCOL_CP_CP_AUTH_SETUP */
