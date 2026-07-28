#include "cp_srp.h"

#include <openssl/bn.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

namespace cp_srp
{
namespace
{
constexpr int N_BYTES = 384; // 3072-bit group

// RFC 5054 3072-bit group modulus, g = 5.
const char *N_HEX =
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

// SHA-512 of the concatenation of parts.
Bytes sha512(std::initializer_list<Bytes> parts)
{
    SHA512_CTX ctx;
    SHA512_Init(&ctx);
    for (const Bytes &p : parts)
        SHA512_Update(&ctx, p.data(), p.size());
    Bytes out(SHA512_DIGEST_LENGTH);
    SHA512_Final(out.data(), &ctx);
    return out;
}

// Minimal big-endian bytes.
Bytes bnBytes(const BIGNUM *n)
{
    Bytes out(BN_num_bytes(n));
    BN_bn2bin(n, out.data());
    return out;
}

// Big-endian, left-padded to N_BYTES.
Bytes bnPad(const BIGNUM *n)
{
    Bytes out(N_BYTES);
    BN_bn2binpad(n, out.data(), N_BYTES);
    return out;
}

BIGNUM *toBn(const Bytes &b) { return BN_bin2bn(b.data(), (int)b.size(), nullptr); }
} // namespace

bool cp_srp::Server::start(const std::string &username, const std::string &password)
{
    _username = username;
    _salt.resize(16);
    if (RAND_bytes(_salt.data(), 16) != 1)
        return false;

    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *N = nullptr;
    BN_hex2bn(&N, N_HEX);
    BIGNUM *g = BN_new();
    BN_set_word(g, 5);
    BIGNUM *k = nullptr, *v = BN_new(), *x = BN_new(), *b = nullptr, *B = BN_new();
    BIGNUM *gb = BN_new(), *kv = BN_new();
    bool ok = false;

    if (!ctx || !N || !g || !v || !x || !B || !gb || !kv)
        goto done;

    {
        // k = H(N | PAD(g))
        Bytes kh = sha512({bnBytes(N), bnPad(g)});
        k = toBn(kh);

        // x = H(salt | H(I | ":" | p)); v = g^x mod N
        Bytes I(username.begin(), username.end());
        Bytes p(password.begin(), password.end());
        Bytes inner = sha512({I, Bytes{':'}, p});
        Bytes xh = sha512({_salt, inner});
        BN_bin2bn(xh.data(), (int)xh.size(), x);
        if (!BN_mod_exp(v, g, x, N, ctx))
            goto done;

        // b random (32B), B = (k*v + g^b) mod N
        Bytes bBytes(32);
        if (RAND_bytes(bBytes.data(), 32) != 1)
            goto done;
        b = toBn(bBytes);
        if (!BN_mod_exp(gb, g, b, N, ctx) || !BN_mod_mul(kv, k, v, N, ctx) ||
            !BN_mod_add(B, kv, gb, N, ctx))
            goto done;

        _b = bBytes;
        _v = bnBytes(v);
        _B = bnPad(B);
        ok = true;
    }

done:
    if (ctx) BN_CTX_free(ctx);
    if (N) BN_free(N);
    if (g) BN_free(g);
    if (k) BN_free(k);
    if (v) BN_free(v);
    if (x) BN_free(x);
    if (b) BN_free(b);
    if (B) BN_free(B);
    if (gb) BN_free(gb);
    if (kv) BN_free(kv);
    return ok;
}

bool cp_srp::Server::verify(const Bytes &Abuf, const Bytes &clientM1, Bytes &K, Bytes &serverM2)
{
    BN_CTX *ctx = BN_CTX_new();
    BIGNUM *N = nullptr;
    BN_hex2bn(&N, N_HEX);
    BIGNUM *g = BN_new();
    BN_set_word(g, 5);
    BIGNUM *A = toBn(Abuf), *v = toBn(_v), *b = toBn(_b);
    BIGNUM *u = nullptr, *vu = BN_new(), *Avu = BN_new(), *S = BN_new(), *Amod = BN_new();
    bool ok = false;

    if (!ctx || !N || !g || !A || !v || !b || !vu || !Avu || !S || !Amod)
        goto done;

    // A % N == 0 is illegal.
    if (!BN_mod(Amod, A, N, ctx) || BN_is_zero(Amod))
        goto done;

    {
        // u = H(PAD(A) | PAD(B)); S = (A * v^u)^b mod N; K = H(bytes(S))
        Bytes Apad(N_BYTES);
        BN_bn2binpad(A, Apad.data(), N_BYTES);
        Bytes uh = sha512({Apad, _B});
        u = toBn(uh);
        if (!BN_mod_exp(vu, v, u, N, ctx) || !BN_mod_mul(Avu, A, vu, N, ctx) ||
            !BN_mod_exp(S, Avu, b, N, ctx))
            goto done;
        Bytes Kh = sha512({bnBytes(S)});

        // M1 = H( (H(N) XOR H(g)) | H(I) | salt | PAD(A) | PAD(B) | K )
        Bytes hN = sha512({bnBytes(N)});
        Bytes hg = sha512({bnBytes(g)});
        Bytes hXor(hN.size());
        for (size_t i = 0; i < hN.size(); i++)
            hXor[i] = hN[i] ^ hg[i];
        Bytes I(_username.begin(), _username.end());
        Bytes hI = sha512({I});
        Bytes expectM1 = sha512({hXor, hI, _salt, Apad, _B, Kh});

        if (expectM1.size() != clientM1.size() ||
            CRYPTO_memcmp(expectM1.data(), clientM1.data(), expectM1.size()) != 0)
            goto done;

        // M2 = H(PAD(A) | M1 | K)
        serverM2 = sha512({Apad, clientM1, Kh});
        K = Kh;
        ok = true;
    }

done:
    if (ctx) BN_CTX_free(ctx);
    if (N) BN_free(N);
    if (g) BN_free(g);
    if (A) BN_free(A);
    if (v) BN_free(v);
    if (b) BN_free(b);
    if (u) BN_free(u);
    if (vu) BN_free(vu);
    if (Avu) BN_free(Avu);
    if (S) BN_free(S);
    if (Amod) BN_free(Amod);
    return ok;
}
} // namespace cp_srp
