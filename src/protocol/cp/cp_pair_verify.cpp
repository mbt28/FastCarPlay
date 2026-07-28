#include "cp_pair_verify.h"

#include "cp_crypto.h"
#include "cp_identity.h"
#include "cp_tlv8.h"
#include "common/logger.h"

namespace cp_pair_verify
{
namespace
{
// TLV8 types (HAP pairing).
constexpr uint8_t TLV_IDENTIFIER = 0x01;
constexpr uint8_t TLV_PUBLIC_KEY = 0x03;
constexpr uint8_t TLV_ENCRYPTED = 0x05;
constexpr uint8_t TLV_STATE = 0x06;
constexpr uint8_t TLV_ERROR = 0x07;
constexpr uint8_t TLV_SIGNATURE = 0x0a;

constexpr uint8_t ERR_AUTHENTICATION = 2;

const cp_tlv8::Bytes *find(const std::map<uint8_t, Bytes> &tlv, uint8_t type)
{
    auto it = tlv.find(type);
    return it == tlv.end() ? nullptr : &it->second;
}
} // namespace

Bytes PairVerify::handle(const Bytes &body)
{
    auto tlv = cp_tlv8::decode(body);
    const Bytes *state = find(tlv, TLV_STATE);
    const uint8_t s = (state && !state->empty()) ? (*state)[0] : 0;
    if (s == 1)
        return m2(tlv);
    if (s == 3)
        return m4(tlv);
    return err(s);
}

Bytes PairVerify::m2(const std::map<uint8_t, Bytes> &tlv)
{
    const Bytes *clientEphPub = find(tlv, TLV_PUBLIC_KEY);
    if (!clientEphPub || clientEphPub->size() != 32)
        return err(2);

    cp_crypto::X25519Pair eph = cp_crypto::x25519Generate();
    if (eph.pubRaw.empty())
        return err(2);
    _ephPub = eph.pubRaw;
    _clientEphPub = *clientEphPub;
    _shared = cp_crypto::x25519Shared(eph.priv, *clientEphPub);
    if (_shared.empty())
        return err(2);

    _encKey = cp_crypto::hkdfSha512(_shared, "Pair-Verify-Encrypt-Salt",
                                    "Pair-Verify-Encrypt-Info", 32);

    // Sign ownEphPub || ownPairingId || peerEphPub with the accessory LTSK.
    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();
    const Bytes accId(id.pairingId.begin(), id.pairingId.end());
    Bytes toSign = cp_crypto::concat(cp_crypto::concat(_ephPub, accId), *clientEphPub);
    Bytes sig = cp_crypto::ed25519Sign(id.privRaw, toSign);

    Bytes sub = cp_tlv8::encode({{TLV_IDENTIFIER, accId}, {TLV_SIGNATURE, sig}});
    Bytes sealed = cp_crypto::chachaSeal(_encKey, cp_crypto::nonceLabel("PV-Msg02"), sub);

    log_v("[pairVerify] M1->M2");
    return cp_tlv8::encode({{TLV_STATE, {2}},
                            {TLV_PUBLIC_KEY, _ephPub},
                            {TLV_ENCRYPTED, sealed}});
}

Bytes PairVerify::m4(const std::map<uint8_t, Bytes> &tlv)
{
    const Bytes *enc = find(tlv, TLV_ENCRYPTED);
    if (_encKey.empty() || _shared.empty() || _ephPub.empty() || _clientEphPub.empty() || !enc)
        return err(4);

    Bytes plain;
    if (!cp_crypto::chachaOpen(_encKey, cp_crypto::nonceLabel("PV-Msg03"), *enc, {}, plain))
        return err(4);

    auto sub = cp_tlv8::decode(plain);
    const Bytes *ctrlId = find(sub, TLV_IDENTIFIER);
    const Bytes *ctrlSig = find(sub, TLV_SIGNATURE);
    if (!ctrlId || !ctrlSig)
        return err(4);

    const std::string controllerId(ctrlId->begin(), ctrlId->end());
    Bytes ctrlLtpk;
    if (!cp_identity::getPairing(controllerId, ctrlLtpk))
    {
        log_w("[pairVerify] unknown controller %s", controllerId.c_str());
        return err(4);
    }

    // Controller signed peerEphPub(ours) || ownPairingId || ownEphPub(theirs).
    Bytes sigData = cp_crypto::concat(cp_crypto::concat(_clientEphPub, *ctrlId), _ephPub);
    if (!cp_crypto::ed25519Verify(ctrlLtpk, sigData, *ctrlSig))
    {
        log_w("[pairVerify] controller signature invalid");
        return err(4);
    }

    // Control-channel keys. The label names are the controller's view, so the
    // accessory reads with the controller's WRITE key and writes with its READ key.
    _keys.readKey = cp_crypto::hkdfSha512(_shared, "Control-Salt", "Control-Write-Encryption-Key", 32);
    _keys.writeKey = cp_crypto::hkdfSha512(_shared, "Control-Salt", "Control-Read-Encryption-Key", 32);
    _verified = true;
    _controllerId = controllerId;
    log_i("[pairVerify] M3->M4 verified (controller %s)", controllerId.c_str());
    return cp_tlv8::encode({{TLV_STATE, {4}}});
}

Bytes PairVerify::err(uint8_t state)
{
    return cp_tlv8::encode({{TLV_STATE, {state}}, {TLV_ERROR, {ERR_AUTHENTICATION}}});
}
} // namespace cp_pair_verify
