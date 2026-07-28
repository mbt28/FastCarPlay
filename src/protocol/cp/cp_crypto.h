#ifndef SRC_PROTOCOL_CP_CP_CRYPTO
#define SRC_PROTOCOL_CP_CP_CRYPTO

// Crypto primitives for the CarPlay handshake, all on OpenSSL 3.x (already
// linked). Mirrors LIVI's cp/stack/crypto.ts: X25519 ECDH, Ed25519 identity
// sign/verify, HKDF-SHA512, ChaCha20-Poly1305 AEAD, plus the SHA + AES-CTR the
// MFiSAP /auth-setup uses. One crypto dependency, per the roadmap.

#include <cstdint>
#include <string>
#include <vector>

namespace cp_crypto
{
using Bytes = std::vector<uint8_t>;

// ── X25519 ──────────────────────────────────────────────────────────────
struct X25519Pair
{
    Bytes priv;   // raw 32-byte private key
    Bytes pubRaw; // raw 32-byte public key
};
X25519Pair x25519Generate();
// ECDH shared secret (32 bytes) against a peer's bare 32-byte public key.
// Empty on failure (e.g. an all-zero / low-order peer key).
Bytes x25519Shared(const Bytes &privRaw, const Bytes &peerPubRaw);

// ── Ed25519 ─────────────────────────────────────────────────────────────
struct Ed25519Pair
{
    Bytes privRaw; // raw 32-byte seed
    Bytes pubRaw;  // raw 32-byte public key
};
Ed25519Pair ed25519Generate();
Bytes ed25519Sign(const Bytes &privRaw, const Bytes &data);
bool ed25519Verify(const Bytes &pubRaw, const Bytes &data, const Bytes &sig);

// ── Hashes ──────────────────────────────────────────────────────────────
Bytes sha1(const Bytes &data);
Bytes sha256(const Bytes &data);

// ── HKDF-SHA512 ─────────────────────────────────────────────────────────
Bytes hkdfSha512(const Bytes &ikm, const std::string &salt, const std::string &info,
                 size_t length = 32);

// ── AES-128-CTR (MFiSAP signature encryption) ───────────────────────────
Bytes aesCtr128(const Bytes &key16, const Bytes &iv16, const Bytes &data);

// ── ChaCha20-Poly1305 (RFC 8439) ────────────────────────────────────────
// Seal: returns ciphertext || 16-byte tag. Open: input is ciphertext || tag;
// returns false (leaving out untouched) on auth failure.
Bytes chachaSeal(const Bytes &key32, const Bytes &nonce12, const Bytes &plaintext,
                 const Bytes &aad = {});
bool chachaOpen(const Bytes &key32, const Bytes &nonce12, const Bytes &ctAndTag,
                const Bytes &aad, Bytes &out);

// 12-byte nonce: 4 zero bytes + 8-byte little-endian counter (AirPlay style).
Bytes nonce64(uint64_t counter);
// 12-byte nonce from an ASCII label right-aligned (HomeKit style, e.g. "PV-Msg02").
Bytes nonceLabel(const std::string &label);

// Verify an ECDSA/RSA signature over `data` using the public key in an X.509
// or PKCS#7 certificate blob (DER). Used to check the MFi coprocessor's
// signature validates against its own accessory certificate.
bool verifyCertSignature(const Bytes &certDer, const Bytes &data, const Bytes &signature);

// Small helpers.
Bytes concat(const Bytes &a, const Bytes &b);
} // namespace cp_crypto

#endif /* SRC_PROTOCOL_CP_CP_CRYPTO */
