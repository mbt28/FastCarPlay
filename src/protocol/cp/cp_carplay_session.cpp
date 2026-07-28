#include "cp_carplay_session.h"

#include "cp_auth_setup.h" // MfiSigner
#include "common/logger.h"

namespace cp_carplay
{
CarplaySession::CarplaySession(cp_iap2::Iap2Link &link, cp_auth_setup::MfiSigner &signer,
                               const AccessoryIdentity &identity, const WirelessSession &wifi)
    : _link(link), _signer(signer), _identity(identity), _wifi(wifi)
{
}

bool CarplaySession::run()
{
    // Keep the iAP2 control link open for the whole connection -- NOT just until
    // the handoff. The phone treats this Bluetooth link as the persistent CarPlay
    // control transport while it joins the Wi-Fi AP and brings up AV over IP;
    // closing it right after the handoff aborts the CarPlay session. We loop,
    // answering identification/auth and then any later CSMs, until the phone
    // drops the link (recvControl returns false).
    while (_link.alive())
    {
        Bytes csm;
        if (!_link.recvControl(csm))
            break;

        uint16_t msgId = 0;
        std::vector<cp_iap2::CsmParam> params;
        if (!cp_iap2::parseCsm(csm, msgId, params))
            continue; // ignore anything that isn't a well-formed CSM

        log_i("[cp] <- CSM 0x%04X (%zu bytes, %zu params)", msgId, csm.size(), params.size());
        if (!handle(msgId, csm))
            break;
    }
    log_w("[cp] session loop ended (link %s, handoff %s)", _link.alive() ? "alive" : "dropped",
          _handoffDelivered ? "delivered" : "not delivered");
    return _handoffDelivered;
}

bool CarplaySession::handle(uint16_t msgId, const Bytes &csm)
{
    switch (msgId)
    {
    case MSG_START_IDENTIFICATION:
        log_i("[cp] StartIdentification -> IdentificationInformation");
        return _link.sendControl(buildIdentification(_identity));

    case MSG_IDENTIFICATION_ACCEPTED:
        _identified = true;
        log_i("[cp] identification accepted");
        return true;

    case MSG_IDENTIFICATION_REJECTED:
    {
        // The rejection's params name the identification fields the phone
        // objected to (by their IdentificationInformation param id). Log them so
        // we know exactly what to fix.
        uint16_t mid = 0;
        std::vector<cp_iap2::CsmParam> rp;
        cp_iap2::parseCsm(csm, mid, rp);
        for (const cp_iap2::CsmParam &p : rp)
            log_e("[cp] identification REJECTED: field param id %u", p.id);
        if (rp.empty())
            log_e("[cp] identification rejected (no field ids given)");
        return false;
    }

    case MSG_REQUEST_AUTH_CERTIFICATE:
    {
        Bytes cert;
        if (!_signer.certificate(cert))
        {
            log_e("[cp] MFi certificate read failed");
            return false;
        }
        log_i("[cp] RequestAuthenticationCertificate -> %zu-byte MFi cert", cert.size());
        return _link.sendControl(buildAuthCertificate(cert));
    }

    case MSG_REQUEST_AUTH_CHALLENGE_RESPONSE:
    {
        Bytes challenge;
        if (!parseAuthChallenge(csm, challenge))
            return false;
        Bytes sig;
        if (!_signer.sign(challenge, sig))
        {
            log_e("[cp] MFi challenge signing failed");
            return false;
        }
        log_i("[cp] signed %zu-byte challenge -> %zu-byte response", challenge.size(), sig.size());
        return _link.sendControl(buildAuthResponse(sig));
    }

    case MSG_AUTH_SUCCEEDED:
        _authenticated = true;
        log_i("[cp] authentication succeeded");
        return true;

    case MSG_AUTH_FAILED:
        log_e("[cp] authentication failed");
        return false;

    case MSG_REQUEST_ACCESSORY_WIFI_CONFIG:
        // The phone asks for our AP credentials before joining.
        log_i("[cp] RequestAccessoryWiFiConfiguration -> AP creds (ssid=%s ch=%u)",
              _wifi.ssid.c_str(), _wifi.channel);
        return _link.sendControl(
            buildAccessoryWifiConfig(_wifi.ssid, _wifi.passphrase, _wifi.security, _wifi.channel));

    case MSG_CARPLAY_AVAILABILITY:
        // The phone signals wireless CarPlay is available; we answer with the
        // session start (Wi-Fi link-local + control port) and it joins the AP.
        log_i("[cp] CarPlayAvailability -> CarPlayStartSession (ip=%s port=%u)",
              _wifi.ipAddress.c_str(), _wifi.port);
        if (!_link.sendControl(buildStartSession(_wifi)))
            return false;
        _handoffDelivered = true;
        log_i("[cp] CarPlayStartSession sent -- phone should join %s now", _wifi.ssid.c_str());
        return true;

    default:
        // Unhandled CSM (vehicle status, power, transport id, etc.). Log it so
        // we can see anything the phone expects that we don't yet answer.
        log_w("[cp] unhandled CSM 0x%04X (ignored)", msgId);
        return true;
    }
}
} // namespace cp_carplay
