// CarPlay pair-setup end-to-end test. Plays the phone (SRP-6a client) through
// M1..M6 against the accessory PairSetup responder, then chains into
// pair-verify to prove the pairing it stored actually authenticates a
// reconnect. Pure crypto, no chip/phone.
//
//   make cp_pairsetup_test && ../out/cp_pairsetup_test

#include <cstdio>
#include <cstdlib>
#include <string>

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_pair_setup.h"
#include "protocol/cp/cp_pair_verify.h"
#include "protocol/cp/cp_tlv8.h"

using cp_crypto::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

// ── SRP-6a client (RFC 5054 3072-bit, SHA-512), mirroring the server ──────
static const char *N_HEX =
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF6955817183"
    "995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33A"
    "85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7AB"
    "F5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864D8"
    "7602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E208"
    "E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF";
static constexpr int N_BYTES = 384;

static Bytes sha512(std::initializer_list<Bytes> parts)
{
    SHA512_CTX c;
    SHA512_Init(&c);
    for (const Bytes &p : parts)
        SHA512_Update(&c, p.data(), p.size());
    Bytes out(64);
    SHA512_Final(out.data(), &c);
    return out;
}
static Bytes bnBytes(const BIGNUM *n) { Bytes o(BN_num_bytes(n)); BN_bn2bin(n, o.data()); return o; }
static Bytes bnPad(const BIGNUM *n) { Bytes o(N_BYTES); BN_bn2binpad(n, o.data(), N_BYTES); return o; }
static BIGNUM *toBn(const Bytes &b) { return BN_bin2bn(b.data(), (int)b.size(), nullptr); }

// Runs the SRP client against (salt, B); fills A, M1, K. Returns false on error.
static bool srpClient(const Bytes &salt, const Bytes &Bbuf, Bytes &Aout, Bytes &M1out, Bytes &Kout)
{
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *N = nullptr; BN_hex2bn(&N, N_HEX);
    BIGNUM *g = BN_new(); BN_set_word(g, 5);
    BIGNUM *B = toBn(Bbuf), *A = BN_new(), *a = nullptr, *x = BN_new(), *v = BN_new();
    BIGNUM *k = nullptr, *u = nullptr, *kv = BN_new(), *base = BN_new(), *exp = BN_new();
    BIGNUM *ux = BN_new(), *S = BN_new();

    Bytes I = {'P','a','i','r','-','S','e','t','u','p'};
    Bytes p = {'3','9','3','9'};
    Bytes xh = sha512({salt, sha512({I, Bytes{':'}, p})});
    BN_bin2bn(xh.data(), (int)xh.size(), x);

    Bytes aB(32); RAND_bytes(aB.data(), 32); a = toBn(aB);
    BN_mod_exp(A, g, a, N, ctx);              // A = g^a
    Bytes Apad = bnPad(A);

    Bytes kh = sha512({bnBytes(N), bnPad(g)}); k = toBn(kh);   // k = H(N|PAD(g))
    BN_mod_exp(v, g, x, N, ctx);              // v = g^x
    Bytes uh = sha512({Apad, Bbuf}); u = toBn(uh);            // u = H(PAD(A)|PAD(B))

    // S = (B - k*v)^(a + u*x) mod N
    BN_mod_mul(kv, k, v, N, ctx);
    BN_mod_sub(base, B, kv, N, ctx);          // non-negative in [0,N)
    BN_mul(ux, u, x, ctx);
    BN_add(exp, a, ux);
    BN_mod_exp(S, base, exp, N, ctx);
    Kout = sha512({bnBytes(S)});

    Bytes hN = sha512({bnBytes(N)}), hg = sha512({bnBytes(g)});
    Bytes hXor(hN.size());
    for (size_t i = 0; i < hN.size(); i++) hXor[i] = hN[i] ^ hg[i];
    M1out = sha512({hXor, sha512({I}), salt, Apad, Bbuf, Kout});
    Aout = Apad;

    BN_CTX_free(ctx);
    BN_free(N); BN_free(g); BN_free(B); BN_free(A); BN_free(a); BN_free(x); BN_free(v);
    BN_free(k); BN_free(u); BN_free(kv); BN_free(base); BN_free(exp); BN_free(ux); BN_free(S);
    return !Kout.empty();
}

int main()
{
    char tmpl[] = "/tmp/fcp-ps-XXXXXX";
    cp_identity::setStorageDir(mkdtemp(tmpl));
    printf("CarPlay pair-setup test (storage %s)\n\n", tmpl);

    // Controller (phone) long-term identity.
    cp_crypto::Ed25519Pair ctrlLt = cp_crypto::ed25519Generate();
    const std::string ctrlId = "phone-pairsetup";

    cp_pair_setup::PairSetup ps;

    // M1 -> M2 (SRP start).
    Bytes m2 = ps.handle(cp_tlv8::encode({{0x06, {1}}}));
    auto t2 = cp_tlv8::decode(m2);
    check(t2[0x06] == Bytes{2} && t2[0x03].size() == 384 && t2[0x02].size() == 16,
          "M2: state 2, B (384B), salt (16B)");

    // Client SRP -> M3 (A, proof).
    Bytes A, M1, K;
    check(srpClient(t2[0x02], t2[0x03], A, M1, K), "client ran SRP");
    Bytes m4 = ps.handle(cp_tlv8::encode({{0x06, {3}}, {0x03, A}, {0x04, M1}}));
    auto t4 = cp_tlv8::decode(m4);
    check(t4[0x06] == Bytes{4} && !t4[0x04].empty(), "M4: state 4 with server proof");

    // Client verifies the server proof M2 == H(PAD(A) | M1 | K).
    check(t4[0x04] == sha512({A, M1, K}), "server proof verifies (SRP mutual auth)");

    // M5: encrypted controller identity (id, LTPK, signature).
    Bytes encKey = cp_crypto::hkdfSha512(K, "Pair-Setup-Encrypt-Salt", "Pair-Setup-Encrypt-Info", 32);
    Bytes ctrlSignKey = cp_crypto::hkdfSha512(K, "Pair-Setup-Controller-Sign-Salt",
                                              "Pair-Setup-Controller-Sign-Info", 32);
    Bytes idb(ctrlId.begin(), ctrlId.end());
    Bytes signData = cp_crypto::concat(cp_crypto::concat(ctrlSignKey, idb), ctrlLt.pubRaw);
    Bytes sig = cp_crypto::ed25519Sign(ctrlLt.privRaw, signData);
    Bytes sub = cp_tlv8::encode({{0x01, idb}, {0x03, ctrlLt.pubRaw}, {0x0a, sig}});
    Bytes sealed = cp_crypto::chachaSeal(encKey, cp_crypto::nonceLabel("PS-Msg05"), sub);
    Bytes m6 = ps.handle(cp_tlv8::encode({{0x06, {5}}, {0x05, sealed}}));
    auto t6 = cp_tlv8::decode(m6);
    check(t6[0x06] == Bytes{6} && !t6[0x05].empty() && ps.complete(),
          "M6: state 6, encrypted accessory identity, complete");

    // Client verifies M6: decrypt + check the accessory's signature.
    printf("\nM6 accessory identity:\n");
    {
        Bytes plain;
        bool opened = cp_crypto::chachaOpen(encKey, cp_crypto::nonceLabel("PS-Msg06"), t6[0x05], {}, plain);
        check(opened, "M6 decrypts");
        auto accSub = cp_tlv8::decode(plain);
        Bytes accId = accSub[0x01], accLtpk = accSub[0x03], accSig = accSub[0x0a];
        Bytes accSignKey = cp_crypto::hkdfSha512(K, "Pair-Setup-Accessory-Sign-Salt",
                                                 "Pair-Setup-Accessory-Sign-Info", 32);
        Bytes accSignData = cp_crypto::concat(cp_crypto::concat(accSignKey, accId), accLtpk);
        check(cp_crypto::ed25519Verify(accLtpk, accSignData, accSig),
              "accessory signature verifies (accessory identity proven)");
    }

    // The payoff: pair-setup stored our LTPK, so a pair-verify reconnect
    // authenticates. Drive it with the same controller identity.
    printf("\npair-setup -> pair-verify reconnect:\n");
    {
        cp_crypto::X25519Pair cEph = cp_crypto::x25519Generate();
        cp_pair_verify::PairVerify pv;
        Bytes v2 = pv.handle(cp_tlv8::encode({{0x06, {1}}, {0x03, cEph.pubRaw}}));
        auto tv2 = cp_tlv8::decode(v2);
        Bytes accEphPub = tv2[0x03];
        Bytes vShared = cp_crypto::x25519Shared(cEph.priv, accEphPub);
        Bytes vEncKey = cp_crypto::hkdfSha512(vShared, "Pair-Verify-Encrypt-Salt",
                                              "Pair-Verify-Encrypt-Info", 32);
        Bytes vToSign = cp_crypto::concat(cp_crypto::concat(cEph.pubRaw, idb), accEphPub);
        Bytes vSig = cp_crypto::ed25519Sign(ctrlLt.privRaw, vToSign);
        Bytes vInner = cp_tlv8::encode({{0x01, idb}, {0x0a, vSig}});
        Bytes vSealed = cp_crypto::chachaSeal(vEncKey, cp_crypto::nonceLabel("PV-Msg03"), vInner);
        pv.handle(cp_tlv8::encode({{0x06, {3}}, {0x05, vSealed}}));
        check(pv.verified() && pv.controllerId() == ctrlId,
              "reconnect authenticates against the stored pairing");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
