#include "cp_auth_setup.h"

#include "cp_crypto.h"

namespace cp_auth_setup
{
namespace
{
constexpr uint8_t MFISAP_VERSION = 0x01;

void appendU32BE(Bytes &out, uint32_t v)
{
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}
} // namespace

Bytes handle(const Bytes &request, MfiSigner &signer)
{
    if (request.size() != 33 || request[0] != MFISAP_VERSION)
        return {};

    const cp_crypto::Bytes peerPub(request.begin() + 1, request.begin() + 33);

    // Our ephemeral X25519 key + the shared secret.
    cp_crypto::X25519Pair eph = cp_crypto::x25519Generate();
    if (eph.pubRaw.empty())
        return {};
    cp_crypto::Bytes shared = cp_crypto::x25519Shared(eph.priv, peerPub);
    bool allZero = !shared.empty();
    for (uint8_t b : shared)
        if (b != 0)
            allZero = false;
    if (shared.empty() || allZero)
        return {};

    // AES key/iv from the shared secret: SHA1("AES-KEY"||secret), SHA1("AES-IV"||secret).
    const cp_crypto::Bytes keyLabel{'A', 'E', 'S', '-', 'K', 'E', 'Y'};
    const cp_crypto::Bytes ivLabel{'A', 'E', 'S', '-', 'I', 'V'};
    cp_crypto::Bytes keyMaterial = cp_crypto::sha1(cp_crypto::concat(keyLabel, shared));
    cp_crypto::Bytes ivMaterial = cp_crypto::sha1(cp_crypto::concat(ivLabel, shared));
    keyMaterial.resize(16);
    ivMaterial.resize(16);

    // The digest the coprocessor signs: the two public keys, hashed by the
    // algorithm the chip's protocol version dictates.
    const cp_crypto::Bytes ours(eph.pubRaw);
    cp_crypto::Bytes concatenated = cp_crypto::concat(ours, peerPub);
    cp_crypto::Bytes digest = (signer.protocolMajor() == 2) ? cp_crypto::sha1(concatenated)
                                                            : cp_crypto::sha256(concatenated);

    Bytes cert;
    Bytes sig;
    if (!signer.certificate(cert) || !signer.sign(digest, sig))
        return {};

    cp_crypto::Bytes encSig = cp_crypto::aesCtr128(keyMaterial, ivMaterial, sig);
    if (encSig.empty())
        return {};

    Bytes response;
    response.reserve(32 + 4 + cert.size() + 4 + encSig.size());
    response.insert(response.end(), eph.pubRaw.begin(), eph.pubRaw.end());
    appendU32BE(response, (uint32_t)cert.size());
    response.insert(response.end(), cert.begin(), cert.end());
    appendU32BE(response, (uint32_t)encSig.size());
    response.insert(response.end(), encSig.begin(), encSig.end());
    return response;
}
} // namespace cp_auth_setup
