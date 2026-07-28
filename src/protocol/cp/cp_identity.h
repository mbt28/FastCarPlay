#ifndef SRC_PROTOCOL_CP_CP_IDENTITY
#define SRC_PROTOCOL_CP_CP_IDENTITY

// The accessory's persistent CarPlay identity + the store of paired phones.
//
// identity: a long-term Ed25519 key pair and a stable pairing id. The public
// key is advertised (mDNS `pk`) and its private key signs the pair-verify
// proof; the pairing id is the `pi`. pairings: each paired controller's
// long-term public key (LTPK) keyed by its identifier, saved at pair-setup and
// looked up at pair-verify. Mirrors LIVI identity.ts / pairings.ts.
//
// Persisted under $HOME/.fastcarplay/cp/ (0700) so a phone stays paired across
// restarts, in the same plain-text style as the settings files.

#include <cstdint>
#include <string>
#include <vector>

namespace cp_identity
{
using Bytes = std::vector<uint8_t>;

struct Identity
{
    Bytes privRaw;         // Ed25519 seed (32B)
    Bytes pubRaw;          // Ed25519 public key (32B)
    std::string pairingId; // stable id (the `pi` TXT value)
};

// Load the accessory identity, creating + persisting one on first use.
const Identity &loadOrCreateIdentity();

// Paired-controller store.
void savePairing(const std::string &identifier, const Bytes &ltpk);
bool getPairing(const std::string &identifier, Bytes &ltpk);

// Override the storage directory (tests). Empty = default $HOME/.fastcarplay/cp.
void setStorageDir(const std::string &dir);
} // namespace cp_identity

#endif /* SRC_PROTOCOL_CP_CP_IDENTITY */
