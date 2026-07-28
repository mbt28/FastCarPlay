// CarPlay control-channel integration test. Plays the phone across the real
// RTSP framing + encryption transition: pair-verify (plaintext) establishes the
// session keys, the channel switches to ChaCha20-Poly1305 framing, and then an
// /auth-setup runs OVER the encrypted channel and is verified against the real
// MFi chip. Essentially the full CarPlay handshake (pair-setup is one-time and
// tested separately).
//
//   make cp_control_test && ../out/cp_control_test [/dev/i2c-1] [addr]

#include <cstdio>
#include <cstdlib>
#include <string>

#include "protocol/cp/cp_auth_setup.h"
#include "protocol/cp/cp_control_channel.h"
#include "protocol/cp/cp_control_cipher.h"
#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_rtsp.h"
#include "protocol/cp/cp_tlv8.h"
#include "protocol/cp/mfi_auth.h"

using cp_crypto::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

class ChipSigner : public cp_auth_setup::MfiSigner
{
public:
    ChipSigner(MfiAuth &c, int m) : _c(c), _m(m) {}
    bool certificate(Bytes &o) override { return _c.readCertificate(o); }
    bool sign(const Bytes &d, Bytes &s) override { return _c.sign(d, s); }
    int protocolMajor() override { return _m; }

private:
    MfiAuth &_c;
    int _m;
};

// Build an RTSP request as it goes on the wire.
static Bytes rtsp(const std::string &method, const std::string &path, const Bytes &body)
{
    std::string head = method + " " + path + " RTSP/1.0\r\nCSeq: 1\r\nContent-Length: " +
                       std::to_string(body.size()) + "\r\n\r\n";
    Bytes out(head.begin(), head.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

// Extract the body of one RTSP response (Content-Length framed).
static Bytes rtspBody(Bytes buf)
{
    auto msgs = cp_rtsp::parse(buf);
    return msgs.empty() ? Bytes{} : msgs[0].body;
}

int main(int argc, char **argv)
{
    const char *bus = argc > 1 ? argv[1] : "/dev/i2c-1";
    uint8_t addr = argc > 2 ? (uint8_t)strtol(argv[2], nullptr, 0) : 0x10;

    char tmpl[] = "/tmp/fcp-cc-XXXXXX";
    cp_identity::setStorageDir(mkdtemp(tmpl));
    printf("CarPlay control-channel test (storage %s)\n\n", tmpl);

    MfiAuth chip;
    bool haveChip = chip.open(bus, addr);
    MfiAuth::Info info{};
    if (haveChip)
        haveChip = chip.identify(info);
    ChipSigner signer(chip, info.protocolMajor);
    printf("MFi chip: %s\n\n", haveChip ? "present" : "absent (auth-setup step skipped)");

    // A paired controller (as if pair-setup already ran).
    cp_crypto::Ed25519Pair ctrlLt = cp_crypto::ed25519Generate();
    const std::string ctrlId = "phone-control";
    cp_identity::savePairing(ctrlId, ctrlLt.pubRaw);
    const cp_identity::Identity &acc = cp_identity::loadOrCreateIdentity();

    cp_control_channel::ControlChannel channel(haveChip ? &signer : nullptr);

    // ── pair-verify over the plaintext channel ─────────────────────────────
    printf("pair-verify through the RTSP channel:\n");
    cp_crypto::X25519Pair cEph = cp_crypto::x25519Generate();
    Bytes m1 = cp_tlv8::encode({{0x06, {1}}, {0x03, cEph.pubRaw}});
    Bytes m2body = rtspBody(channel.process(rtsp("POST", "/pair-verify", m1)));
    auto t2 = cp_tlv8::decode(m2body);
    Bytes accEphPub = t2[0x03];
    check(!accEphPub.empty() && !t2[0x05].empty(), "M2 received over RTSP");

    Bytes shared = cp_crypto::x25519Shared(cEph.priv, accEphPub);
    Bytes encKey = cp_crypto::hkdfSha512(shared, "Pair-Verify-Encrypt-Salt",
                                         "Pair-Verify-Encrypt-Info", 32);
    Bytes plain;
    cp_crypto::chachaOpen(encKey, cp_crypto::nonceLabel("PV-Msg02"), t2[0x05], {}, plain);
    auto sub = cp_tlv8::decode(plain);
    Bytes signedByAcc = cp_crypto::concat(cp_crypto::concat(accEphPub, sub[0x01]), cEph.pubRaw);
    check(cp_crypto::ed25519Verify(acc.pubRaw, signedByAcc, sub[0x0a]), "accessory identity proven");

    Bytes idb(ctrlId.begin(), ctrlId.end());
    Bytes toSign = cp_crypto::concat(cp_crypto::concat(cEph.pubRaw, idb), accEphPub);
    Bytes ctrlSig = cp_crypto::ed25519Sign(ctrlLt.privRaw, toSign);
    Bytes inner = cp_tlv8::encode({{0x01, idb}, {0x0a, ctrlSig}});
    Bytes sealed = cp_crypto::chachaSeal(encKey, cp_crypto::nonceLabel("PV-Msg03"), inner);
    Bytes m3 = cp_tlv8::encode({{0x06, {3}}, {0x05, sealed}});
    Bytes m4body = rtspBody(channel.process(rtsp("POST", "/pair-verify", m3)));
    check(cp_tlv8::decode(m4body)[0x06] == Bytes{4}, "M4 state = 4");
    check(channel.paired() && channel.encrypted(), "channel verified + switched to encrypted");

    // ── the phone's matching cipher for the now-encrypted channel ──────────
    cp_control_cipher::ControlCipher phoneCipher(
        cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32),
        cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32));

    if (haveChip)
    {
        printf("\n/auth-setup (MFiSAP) over the encrypted channel:\n");
        cp_crypto::X25519Pair authEph = cp_crypto::x25519Generate();
        Bytes authReq;
        authReq.push_back(0x01);
        authReq.insert(authReq.end(), authEph.pubRaw.begin(), authEph.pubRaw.end());

        Bytes wire = phoneCipher.encrypt(rtsp("POST", "/auth-setup", authReq));
        Bytes encResp = channel.process(wire);
        Bytes respPlain;
        check(phoneCipher.decrypt(encResp, respPlain) && !respPlain.empty(),
              "encrypted response decrypts");
        Bytes body = rtspBody(respPlain);

        // Verify the MFiSAP reply exactly as an iPhone would.
        Bytes ourPub(body.begin(), body.begin() + 32);
        size_t off = 32;
        auto u32 = [&](size_t o) {
            return ((uint32_t)body[o] << 24) | ((uint32_t)body[o + 1] << 16) |
                   ((uint32_t)body[o + 2] << 8) | body[o + 3];
        };
        uint32_t certLen = u32(off); off += 4;
        Bytes cert(body.begin() + off, body.begin() + off + certLen); off += certLen;
        uint32_t sigLen = u32(off); off += 4;
        Bytes encSig(body.begin() + off, body.begin() + off + sigLen);

        Bytes aShared = cp_crypto::x25519Shared(authEph.priv, ourPub);
        Bytes aKey = cp_crypto::sha1(cp_crypto::concat(Bytes{'A','E','S','-','K','E','Y'}, aShared));
        Bytes aIv = cp_crypto::sha1(cp_crypto::concat(Bytes{'A','E','S','-','I','V'}, aShared));
        aKey.resize(16); aIv.resize(16);
        Bytes sig = cp_crypto::aesCtr128(aKey, aIv, encSig);
        Bytes signedData = cp_crypto::concat(ourPub, authEph.pubRaw);
        check(cp_crypto::verifyCertSignature(cert, signedData, sig),
              "MFi signature verifies over the encrypted channel");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
