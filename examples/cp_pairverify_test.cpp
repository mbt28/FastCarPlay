// CarPlay pair-verify end-to-end test. Plays the phone (controller) against
// the accessory PairVerify responder -- pure symmetric crypto, no chip or
// phone -- and checks the full HAP pair-verify: M1..M4, both sides derive the
// same control-channel keys, a message encrypted by one is readable by the
// other, and an unpaired controller is rejected.
//
//   make cp_pairverify_test && ../out/cp_pairverify_test

#include <cstdio>
#include <cstdlib>
#include <string>

#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_identity.h"
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

// A controller (phone) with its own long-term Ed25519 identity.
struct Controller
{
    cp_crypto::Ed25519Pair lt = cp_crypto::ed25519Generate();
    std::string id = "phone-0001";
    cp_crypto::X25519Pair eph;
    Bytes shared, encKey;

    Bytes m1()
    {
        eph = cp_crypto::x25519Generate();
        return cp_tlv8::encode({{0x06, {1}}, {0x03, eph.pubRaw}}); // State=1, PublicKey
    }

    // Verify the accessory's M2 and produce M3. Returns empty on failure.
    Bytes m3(const Bytes &m2, const Bytes &accLtpk)
    {
        auto tlv = cp_tlv8::decode(m2);
        if (tlv[0x06].empty() || tlv[0x06][0] != 2)
            return {};
        Bytes accEphPub = tlv[0x03];
        Bytes enc = tlv[0x05];

        shared = cp_crypto::x25519Shared(eph.priv, accEphPub);
        encKey = cp_crypto::hkdfSha512(shared, "Pair-Verify-Encrypt-Salt",
                                       "Pair-Verify-Encrypt-Info", 32);
        Bytes plain;
        if (!cp_crypto::chachaOpen(encKey, cp_crypto::nonceLabel("PV-Msg02"), enc, {}, plain))
            return {};
        auto sub = cp_tlv8::decode(plain);
        Bytes accId = sub[0x01], accSig = sub[0x0a];

        // Accessory signed accEphPub || accId || ourEphPub.
        Bytes signedData = cp_crypto::concat(cp_crypto::concat(accEphPub, accId), eph.pubRaw);
        if (!cp_crypto::ed25519Verify(accLtpk, signedData, accSig))
            return {}; // accessory identity not proven

        // Sign ourEphPub || ourId || accEphPub with our LTSK.
        Bytes idb(id.begin(), id.end());
        Bytes toSign = cp_crypto::concat(cp_crypto::concat(eph.pubRaw, idb), accEphPub);
        Bytes sig = cp_crypto::ed25519Sign(lt.privRaw, toSign);
        Bytes innerSub = cp_tlv8::encode({{0x01, idb}, {0x0a, sig}});
        Bytes sealed = cp_crypto::chachaSeal(encKey, cp_crypto::nonceLabel("PV-Msg03"), innerSub);
        return cp_tlv8::encode({{0x06, {3}}, {0x05, sealed}}); // State=3, EncryptedData
    }

    Bytes controlWriteKey() const
    {
        return cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
    }
    Bytes controlReadKey() const
    {
        return cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
    }
};

int main()
{
    char tmpl[] = "/tmp/fcp-cp-XXXXXX";
    cp_identity::setStorageDir(mkdtemp(tmpl));
    printf("CarPlay pair-verify test (storage %s)\n\n", tmpl);

    const cp_identity::Identity &acc = cp_identity::loadOrCreateIdentity();
    check(acc.privRaw.size() == 32 && acc.pubRaw.size() == 32 && !acc.pairingId.empty(),
          "accessory identity created");

    // Simulate a prior pair-setup: the accessory knows this controller's LTPK.
    Controller phone;
    cp_identity::savePairing(phone.id, phone.lt.pubRaw);

    printf("\nhappy path (paired controller):\n");
    cp_pair_verify::PairVerify pv;
    Bytes m2 = pv.handle(phone.m1());
    check(!m2.empty(), "accessory answered M2");
    Bytes m3 = phone.m3(m2, acc.pubRaw);
    check(!m3.empty(), "controller verified the accessory and built M3");
    Bytes m4 = pv.handle(m3);
    check(pv.verified(), "accessory verified the controller");
    check(pv.controllerId() == phone.id, "controller id recorded");
    check(cp_tlv8::decode(m4)[0x06] == Bytes{4}, "M4 state = 4");

    printf("\ncontrol-channel keys agree (cross-mapped):\n");
    check(!pv.keys().readKey.empty() && pv.keys().readKey == phone.controlWriteKey(),
          "accessory read key == controller write key");
    check(pv.keys().writeKey == phone.controlReadKey(),
          "accessory write key == controller read key");

    printf("\nlive message over the derived keys:\n");
    {
        Bytes msg = {'h', 'e', 'l', 'l', 'o'};
        Bytes nonce = cp_crypto::nonce64(0);
        Bytes sealed = cp_crypto::chachaSeal(phone.controlWriteKey(), nonce, msg);
        Bytes opened;
        check(cp_crypto::chachaOpen(pv.keys().readKey, nonce, sealed, {}, opened) && opened == msg,
              "controller->accessory message decrypts");
    }

    printf("\nunpaired controller is rejected:\n");
    {
        Controller stranger;
        stranger.id = "not-paired";
        cp_pair_verify::PairVerify pv2;
        Bytes s2 = pv2.handle(stranger.m1());
        Bytes s3 = stranger.m3(s2, acc.pubRaw);
        Bytes s4 = pv2.handle(s3);
        check(!pv2.verified(), "not verified");
        check(cp_tlv8::decode(s4)[0x07] == Bytes{2}, "error = authentication");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
