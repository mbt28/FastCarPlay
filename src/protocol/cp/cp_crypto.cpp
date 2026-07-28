#include "cp_crypto.h"

#include <cstring>

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/pkcs7.h>
#include <openssl/ecdsa.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>

namespace cp_crypto
{
namespace
{
// RAII for the EVP handles so error paths do not leak.
struct PkeyCtx
{
    EVP_PKEY_CTX *p = nullptr;
    ~PkeyCtx() { if (p) EVP_PKEY_CTX_free(p); }
};
struct Pkey
{
    EVP_PKEY *p = nullptr;
    ~Pkey() { if (p) EVP_PKEY_free(p); }
};

EVP_PKEY *rawKey(int type, const Bytes &raw, bool isPrivate)
{
    return isPrivate ? EVP_PKEY_new_raw_private_key(type, nullptr, raw.data(), raw.size())
                     : EVP_PKEY_new_raw_public_key(type, nullptr, raw.data(), raw.size());
}
} // namespace

Bytes concat(const Bytes &a, const Bytes &b)
{
    Bytes out;
    out.reserve(a.size() + b.size());
    out.insert(out.end(), a.begin(), a.end());
    out.insert(out.end(), b.begin(), b.end());
    return out;
}

// ── X25519 ──────────────────────────────────────────────────────────────

X25519Pair x25519Generate()
{
    X25519Pair pair;
    PkeyCtx ctx;
    ctx.p = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    Pkey key;
    if (!ctx.p || EVP_PKEY_keygen_init(ctx.p) <= 0 || EVP_PKEY_keygen(ctx.p, &key.p) <= 0)
        return pair;

    size_t len = 32;
    pair.priv.resize(32);
    pair.pubRaw.resize(32);
    if (EVP_PKEY_get_raw_private_key(key.p, pair.priv.data(), &len) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.p, pair.pubRaw.data(), &len) <= 0)
    {
        pair.priv.clear();
        pair.pubRaw.clear();
    }
    return pair;
}

Bytes x25519Shared(const Bytes &privRaw, const Bytes &peerPubRaw)
{
    Pkey priv, peer;
    priv.p = rawKey(EVP_PKEY_X25519, privRaw, true);
    peer.p = rawKey(EVP_PKEY_X25519, peerPubRaw, false);
    if (!priv.p || !peer.p)
        return {};

    PkeyCtx ctx;
    ctx.p = EVP_PKEY_CTX_new(priv.p, nullptr);
    size_t len = 0;
    if (!ctx.p || EVP_PKEY_derive_init(ctx.p) <= 0 ||
        EVP_PKEY_derive_set_peer(ctx.p, peer.p) <= 0 ||
        EVP_PKEY_derive(ctx.p, nullptr, &len) <= 0)
        return {};

    Bytes out(len);
    if (EVP_PKEY_derive(ctx.p, out.data(), &len) <= 0)
        return {};
    out.resize(len);
    return out;
}

// ── Ed25519 ─────────────────────────────────────────────────────────────

Ed25519Pair ed25519Generate()
{
    Ed25519Pair pair;
    PkeyCtx ctx;
    ctx.p = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    Pkey key;
    if (!ctx.p || EVP_PKEY_keygen_init(ctx.p) <= 0 || EVP_PKEY_keygen(ctx.p, &key.p) <= 0)
        return pair;

    size_t len = 32;
    pair.privRaw.resize(32);
    pair.pubRaw.resize(32);
    if (EVP_PKEY_get_raw_private_key(key.p, pair.privRaw.data(), &len) <= 0 ||
        EVP_PKEY_get_raw_public_key(key.p, pair.pubRaw.data(), &len) <= 0)
    {
        pair.privRaw.clear();
        pair.pubRaw.clear();
    }
    return pair;
}

Bytes ed25519Sign(const Bytes &privRaw, const Bytes &data)
{
    Pkey key;
    key.p = rawKey(EVP_PKEY_ED25519, privRaw, true);
    if (!key.p)
        return {};

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    Bytes sig(64);
    size_t sigLen = sig.size();
    bool ok = md && EVP_DigestSignInit(md, nullptr, nullptr, nullptr, key.p) == 1 &&
              EVP_DigestSign(md, sig.data(), &sigLen, data.data(), data.size()) == 1;
    if (md)
        EVP_MD_CTX_free(md);
    if (!ok)
        return {};
    sig.resize(sigLen);
    return sig;
}

bool ed25519Verify(const Bytes &pubRaw, const Bytes &data, const Bytes &sig)
{
    Pkey key;
    key.p = rawKey(EVP_PKEY_ED25519, pubRaw, false);
    if (!key.p)
        return false;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    bool ok = md && EVP_DigestVerifyInit(md, nullptr, nullptr, nullptr, key.p) == 1 &&
              EVP_DigestVerify(md, sig.data(), sig.size(), data.data(), data.size()) == 1;
    if (md)
        EVP_MD_CTX_free(md);
    return ok;
}

// ── Hashes ──────────────────────────────────────────────────────────────

Bytes sha1(const Bytes &data)
{
    Bytes out(SHA_DIGEST_LENGTH);
    SHA1(data.data(), data.size(), out.data());
    return out;
}

Bytes sha256(const Bytes &data)
{
    Bytes out(SHA256_DIGEST_LENGTH);
    SHA256(data.data(), data.size(), out.data());
    return out;
}

// ── HKDF-SHA512 ─────────────────────────────────────────────────────────

Bytes hkdfSha512(const Bytes &ikm, const std::string &salt, const std::string &info, size_t length)
{
    Bytes out(length);
    EVP_KDF *kdf = EVP_KDF_fetch(nullptr, "HKDF", nullptr);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : nullptr;
    if (kdf)
        EVP_KDF_free(kdf);
    if (!ctx)
        return {};

    char digest[] = "SHA512";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)ikm.data(), ikm.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt.data(), salt.size()),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info.data(), info.size()),
        OSSL_PARAM_construct_end()};

    bool ok = EVP_KDF_derive(ctx, out.data(), out.size(), params) == 1;
    EVP_KDF_CTX_free(ctx);
    return ok ? out : Bytes{};
}

// ── AES-128-CTR ─────────────────────────────────────────────────────────

Bytes aesCtr128(const Bytes &key16, const Bytes &iv16, const Bytes &data)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    Bytes out(data.size());
    int outLen = 0, finalLen = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key16.data(), iv16.data()) == 1 &&
              EVP_EncryptUpdate(ctx, out.data(), &outLen, data.data(), (int)data.size()) == 1 &&
              EVP_EncryptFinal_ex(ctx, out.data() + outLen, &finalLen) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(outLen + finalLen);
    return out;
}

// ── ChaCha20-Poly1305 ───────────────────────────────────────────────────

Bytes chachaSeal(const Bytes &key32, const Bytes &nonce12, const Bytes &plaintext, const Bytes &aad)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return {};
    Bytes out(plaintext.size() + 16);
    int outLen = 0, finalLen = 0, tmp = 0;
    bool ok = EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, key32.data(), nonce12.data()) == 1;
    if (ok && !aad.empty())
        ok = EVP_EncryptUpdate(ctx, nullptr, &tmp, aad.data(), (int)aad.size()) == 1;
    ok = ok && EVP_EncryptUpdate(ctx, out.data(), &outLen, plaintext.data(), (int)plaintext.size()) == 1 &&
         EVP_EncryptFinal_ex(ctx, out.data() + outLen, &finalLen) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, 16, out.data() + plaintext.size()) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return {};
    out.resize(plaintext.size() + 16);
    return out;
}

bool chachaOpen(const Bytes &key32, const Bytes &nonce12, const Bytes &ctAndTag, const Bytes &aad,
                Bytes &out)
{
    if (ctAndTag.size() < 16)
        return false;
    const size_t ctLen = ctAndTag.size() - 16;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    Bytes plain(ctLen);
    int outLen = 0, finalLen = 0, tmp = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, key32.data(), nonce12.data()) == 1;
    if (ok && !aad.empty())
        ok = EVP_DecryptUpdate(ctx, nullptr, &tmp, aad.data(), (int)aad.size()) == 1;
    ok = ok && EVP_DecryptUpdate(ctx, plain.data(), &outLen, ctAndTag.data(), (int)ctLen) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, 16,
                             (void *)(ctAndTag.data() + ctLen)) == 1 &&
         EVP_DecryptFinal_ex(ctx, plain.data() + outLen, &finalLen) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        return false;
    plain.resize(outLen + finalLen);
    out = std::move(plain);
    return true;
}

Bytes nonce64(uint64_t counter)
{
    Bytes n(12, 0);
    for (int i = 0; i < 8; i++)
        n[4 + i] = (uint8_t)((counter >> (8 * i)) & 0xff); // little-endian
    return n;
}

Bytes nonceLabel(const std::string &label)
{
    Bytes n(12, 0);
    const size_t len = label.size() > 8 ? 8 : label.size();
    memcpy(n.data() + 4 + (8 - len), label.data(), len);
    return n;
}

// ── Certificate signature verification ──────────────────────────────────

bool verifyCertSignature(const Bytes &certDer, const Bytes &data, const Bytes &signature)
{
    // The MFi accessory certificate arrives as a PKCS#7 SignedData blob; the
    // leaf X.509 inside carries the EC public key that matches the chip's
    // signing key. Pull the first certificate out and verify with it.
    X509 *cert = nullptr;
    const unsigned char *p = certDer.data();

    PKCS7 *p7 = d2i_PKCS7(nullptr, &p, (long)certDer.size());
    if (p7)
    {
        STACK_OF(X509) *certs = nullptr;
        if (PKCS7_type_is_signed(p7))
            certs = p7->d.sign->cert;
        else if (PKCS7_type_is_signedAndEnveloped(p7))
            certs = p7->d.signed_and_enveloped->cert;
        if (certs && sk_X509_num(certs) > 0)
            cert = X509_dup(sk_X509_value(certs, 0));
        PKCS7_free(p7);
    }
    if (!cert)
    {
        // Fall back to a bare X.509.
        p = certDer.data();
        cert = d2i_X509(nullptr, &p, (long)certDer.size());
    }
    if (!cert)
        return false;

    EVP_PKEY *pub = X509_get_pubkey(cert);
    bool ok = false;
    if (pub)
    {
        // MFi 3.0 signs SHA-256(data) with ECDSA-P256 and returns the raw
        // 64-byte r||s; OpenSSL's verify wants the DER Ecdsa-Sig-Value, so
        // wrap it. A DER signature (2.0C RSA, or already-DER) is passed as-is.
        Bytes der = signature;
        if (EVP_PKEY_base_id(pub) == EVP_PKEY_EC && signature.size() == 64)
        {
            ECDSA_SIG *sig = ECDSA_SIG_new();
            BIGNUM *r = BN_bin2bn(signature.data(), 32, nullptr);
            BIGNUM *s = BN_bin2bn(signature.data() + 32, 32, nullptr);
            if (sig && r && s && ECDSA_SIG_set0(sig, r, s) == 1)
            {
                unsigned char *out = nullptr;
                int n = i2d_ECDSA_SIG(sig, &out);
                if (n > 0)
                {
                    der.assign(out, out + n);
                    OPENSSL_free(out);
                }
                r = s = nullptr; // owned by sig now
            }
            else
            {
                BN_free(r);
                BN_free(s);
            }
            if (sig)
                ECDSA_SIG_free(sig);
        }

        EVP_MD_CTX *md = EVP_MD_CTX_new();
        ok = md && EVP_DigestVerifyInit(md, nullptr, EVP_sha256(), nullptr, pub) == 1 &&
             EVP_DigestVerify(md, der.data(), der.size(), data.data(), data.size()) == 1;
        if (md)
            EVP_MD_CTX_free(md);
        EVP_PKEY_free(pub);
    }
    X509_free(cert);
    return ok;
}

} // namespace cp_crypto
