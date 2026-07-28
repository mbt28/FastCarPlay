// Known-answer + round-trip tests for the CarPlay crypto substrate. No chip,
// no phone -- pure crypto, so it runs anywhere and pins the primitives before
// the handshake is built on them.
//
//   make cp_crypto_test && ../out/cp_crypto_test

#include <cstdio>
#include <cstring>

#include "protocol/cp/cp_crypto.h"

using cp_crypto::Bytes;

static int failures = 0;

static Bytes fromHex(const char *hex)
{
    Bytes out;
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
    {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        out.push_back((uint8_t)(nib(hex[i]) << 4 | nib(hex[i + 1])));
    }
    return out;
}

static void check(bool ok, const char *what)
{
    printf("  %-54s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

int main()
{
    printf("CarPlay crypto substrate tests\n\n");

    // X25519 RFC 7748 test vector.
    printf("X25519 (RFC 7748):\n");
    {
        Bytes a = fromHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        Bytes bPub = fromHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        Bytes expect = fromHex("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
        check(cp_crypto::x25519Shared(a, bPub) == expect, "shared secret matches vector");
    }

    // X25519 ECDH agreement (two fresh keys agree).
    printf("\nX25519 agreement:\n");
    {
        auto x = cp_crypto::x25519Generate();
        auto y = cp_crypto::x25519Generate();
        check(!x.pubRaw.empty() && !y.pubRaw.empty(), "keygen");
        check(cp_crypto::x25519Shared(x.priv, y.pubRaw) == cp_crypto::x25519Shared(y.priv, x.pubRaw),
              "both sides derive the same secret");
    }

    // Ed25519 sign/verify.
    printf("\nEd25519:\n");
    {
        auto k = cp_crypto::ed25519Generate();
        Bytes msg = fromHex("deadbeefcafe");
        Bytes sig = cp_crypto::ed25519Sign(k.privRaw, msg);
        check(sig.size() == 64, "signature is 64 bytes");
        check(cp_crypto::ed25519Verify(k.pubRaw, msg, sig), "verify accepts a good signature");
        sig[0] ^= 1;
        check(!cp_crypto::ed25519Verify(k.pubRaw, msg, sig), "verify rejects a tampered signature");
    }

    // SHA vectors.
    printf("\nSHA:\n");
    check(cp_crypto::sha1(fromHex("616263")) == fromHex("a9993e364706816aba3e25717850c26c9cd0d89d"),
          "SHA-1(\"abc\")");
    check(cp_crypto::sha256(fromHex("616263")) ==
              fromHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
          "SHA-256(\"abc\")");

    // HKDF-SHA512 round-trip (deterministic output, non-empty, right length).
    printf("\nHKDF-SHA512:\n");
    {
        Bytes ikm = fromHex("0b0b0b0b0b0b0b0b0b0b0b");
        Bytes a = cp_crypto::hkdfSha512(ikm, "salt", "info", 32);
        Bytes b = cp_crypto::hkdfSha512(ikm, "salt", "info", 32);
        check(a.size() == 32 && a == b, "deterministic 32-byte output");
        check(cp_crypto::hkdfSha512(ikm, "salt", "other", 32) != a, "info separates outputs");
    }

    // AES-128-CTR: encrypt is its own inverse (CTR keystream XOR).
    printf("\nAES-128-CTR:\n");
    {
        Bytes key = fromHex("000102030405060708090a0b0c0d0e0f");
        Bytes iv = fromHex("0f0e0d0c0b0a09080706050403020100");
        Bytes pt = fromHex("00112233445566778899aabbccddeeff");
        Bytes ct = cp_crypto::aesCtr128(key, iv, pt);
        check(ct != pt && cp_crypto::aesCtr128(key, iv, ct) == pt, "CTR encrypt/decrypt round-trips");
    }

    // ChaCha20-Poly1305 seal/open + auth failure.
    printf("\nChaCha20-Poly1305:\n");
    {
        Bytes key = fromHex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f");
        Bytes nonce = cp_crypto::nonce64(1);
        Bytes pt = fromHex("4c616469657320616e642047656e746c656d656e");
        Bytes aad = fromHex("50515253c0c1c2c3c4c5c6c7");
        Bytes sealed = cp_crypto::chachaSeal(key, nonce, pt, aad);
        check(sealed.size() == pt.size() + 16, "sealed = ciphertext + 16-byte tag");
        Bytes opened;
        check(cp_crypto::chachaOpen(key, nonce, sealed, aad, opened) && opened == pt,
              "open recovers the plaintext");
        sealed[0] ^= 1;
        check(!cp_crypto::chachaOpen(key, nonce, sealed, aad, opened), "open rejects tampered data");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
