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
    while (_link.alive() && !_handoffDelivered)
    {
        Bytes csm;
        if (!_link.recvControl(csm))
            return false;

        uint16_t msgId = 0;
        std::vector<cp_iap2::CsmParam> params;
        if (!cp_iap2::parseCsm(csm, msgId, params))
            continue; // ignore anything that isn't a well-formed CSM

        if (!handle(msgId, csm))
            return false;
    }
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
        log_e("[cp] identification rejected by the phone");
        return false;

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
        log_i("[cp] authentication succeeded -> Wi-Fi handoff");
        return sendHandoff();

    case MSG_AUTH_FAILED:
        log_e("[cp] authentication failed");
        return false;

    default:
        // Unhandled CSM (vehicle status, power, etc.) -- ignore for bring-up.
        return true;
    }
}

bool CarplaySession::sendHandoff()
{
    // Declare availability, then hand off the Wi-Fi credentials + control port.
    if (!_link.sendControl(buildCarPlayAvailability(std::string(), std::string())))
        return false;
    if (!_link.sendControl(buildStartSession(_wifi)))
        return false;
    _handoffDelivered = true;
    log_i("[cp] CarPlayStartSession sent: ssid=%s port=%u -- phone should join now",
          _wifi.ssid.c_str(), _wifi.port);
    return true;
}
} // namespace cp_carplay
