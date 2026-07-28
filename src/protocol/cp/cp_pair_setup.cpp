#include "cp_pair_setup.h"

#include "cp_crypto.h"
#include "cp_identity.h"
#include "cp_tlv8.h"
#include "common/logger.h"

namespace cp_pair_setup
{
namespace
{
constexpr uint8_t TLV_IDENTIFIER = 0x01;
constexpr uint8_t TLV_SALT = 0x02;
constexpr uint8_t TLV_PUBLIC_KEY = 0x03;
constexpr uint8_t TLV_PROOF = 0x04;
constexpr uint8_t TLV_ENCRYPTED = 0x05;
constexpr uint8_t TLV_STATE = 0x06;
constexpr uint8_t TLV_ERROR = 0x07;
constexpr uint8_t TLV_SIGNATURE = 0x0a;

constexpr uint8_t ERR_AUTHENTICATION = 2;
const char *SETUP_CODE = "3939";

const Bytes *find(const std::map<uint8_t, Bytes> &tlv, uint8_t type)
{
    auto it = tlv.find(type);
    return it == tlv.end() ? nullptr : &it->second;
}
} // namespace

Bytes PairSetup::handle(const Bytes &body)
{
    auto tlv = cp_tlv8::decode(body);
    const Bytes *state = find(tlv, TLV_STATE);
    const uint8_t s = (state && !state->empty()) ? (*state)[0] : 0;
    if (s == 1)
        return m2();
    if (s == 3)
        return m4(tlv);
    if (s == 5)
        return m6(tlv);
    return err(s);
}

Bytes PairSetup::m2()
{
    if (!_srp.start("Pair-Setup", SETUP_CODE))
        return err(2);
    _srpStarted = true;
    log_v("[pairSetup] M1->M2 (SRP start)");
    return cp_tlv8::encode({{TLV_STATE, {2}},
                            {TLV_PUBLIC_KEY, _srp.B()},
                            {TLV_SALT, _srp.salt()}});
}

Bytes PairSetup::m4(const std::map<uint8_t, Bytes> &tlv)
{
    const Bytes *A = find(tlv, TLV_PUBLIC_KEY);
    const Bytes *proof = find(tlv, TLV_PROOF);
    if (!_srpStarted || !A || !proof)
        return err(4);

    Bytes K, serverM2;
    if (!_srp.verify(*A, *proof, K, serverM2))
    {
        log_w("[pairSetup] SRP verify failed");
        return err(4);
    }
    _K = K;
    log_v("[pairSetup] M3->M4 (SRP verified)");
    return cp_tlv8::encode({{TLV_STATE, {4}}, {TLV_PROOF, serverM2}});
}

Bytes PairSetup::m6(const std::map<uint8_t, Bytes> &tlv)
{
    const Bytes *enc = find(tlv, TLV_ENCRYPTED);
    if (_K.empty() || !enc)
        return err(6);

    Bytes encKey = cp_crypto::hkdfSha512(_K, "Pair-Setup-Encrypt-Salt",
                                         "Pair-Setup-Encrypt-Info", 32);
    Bytes plain;
    if (!cp_crypto::chachaOpen(encKey, cp_crypto::nonceLabel("PS-Msg05"), *enc, {}, plain))
        return err(6);

    auto sub = cp_tlv8::decode(plain);
    const Bytes *ctrlId = find(sub, TLV_IDENTIFIER);
    const Bytes *ctrlLtpk = find(sub, TLV_PUBLIC_KEY);
    const Bytes *ctrlSig = find(sub, TLV_SIGNATURE);
    if (!ctrlId || !ctrlLtpk || !ctrlSig)
        return err(6);

    // The controller signed (controllerSignKey || identifier || its LTPK).
    Bytes ctrlSignKey = cp_crypto::hkdfSha512(_K, "Pair-Setup-Controller-Sign-Salt",
                                              "Pair-Setup-Controller-Sign-Info", 32);
    Bytes ctrlSignData = cp_crypto::concat(cp_crypto::concat(ctrlSignKey, *ctrlId), *ctrlLtpk);
    if (!cp_crypto::ed25519Verify(*ctrlLtpk, ctrlSignData, *ctrlSig))
    {
        log_w("[pairSetup] controller signature invalid");
        return err(6);
    }

    const std::string controllerId(ctrlId->begin(), ctrlId->end());
    cp_identity::savePairing(controllerId, *ctrlLtpk);

    // Our M6: accessory identifier + LTPK + signature.
    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();
    const Bytes accId(id.pairingId.begin(), id.pairingId.end());
    Bytes accSignKey = cp_crypto::hkdfSha512(_K, "Pair-Setup-Accessory-Sign-Salt",
                                             "Pair-Setup-Accessory-Sign-Info", 32);
    Bytes accSignData = cp_crypto::concat(cp_crypto::concat(accSignKey, accId), id.pubRaw);
    Bytes accSig = cp_crypto::ed25519Sign(id.privRaw, accSignData);

    Bytes subResp = cp_tlv8::encode({{TLV_IDENTIFIER, accId},
                                     {TLV_PUBLIC_KEY, id.pubRaw},
                                     {TLV_SIGNATURE, accSig}});
    Bytes sealed = cp_crypto::chachaSeal(encKey, cp_crypto::nonceLabel("PS-Msg06"), subResp);
    _complete = true;
    log_i("[pairSetup] M5->M6 paired (controller %s)", controllerId.c_str());
    return cp_tlv8::encode({{TLV_STATE, {6}}, {TLV_ENCRYPTED, sealed}});
}

Bytes PairSetup::err(uint8_t state)
{
    return cp_tlv8::encode({{TLV_STATE, {state}}, {TLV_ERROR, {ERR_AUTHENTICATION}}});
}
} // namespace cp_pair_setup
