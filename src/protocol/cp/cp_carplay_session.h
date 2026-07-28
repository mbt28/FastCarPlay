#ifndef SRC_PROTOCOL_CP_CP_CARPLAY_SESSION
#define SRC_PROTOCOL_CP_CP_CARPLAY_SESSION

// Wireless CarPlay accessory session: the CSM state machine that runs over a
// negotiated iAP2 link (cp_iap2_link, established over Bluetooth RFCOMM). It
// answers the iPhone's identification + MFi-authentication requests, then hands
// the phone the Wi-Fi credentials (CarPlayStartSession). After that the phone
// joins the accessory's Wi-Fi AP and connects to the :7000 control server
// (cp_server) -- the receiving stack that already works. This is the wireless
// counterpart to the (iOS-26-deprecated) wired config-6 trigger.
//
// Sequence (accessory side, driven by the phone):
//   <- StartIdentification            -> IdentificationInformation
//   <- IdentificationAccepted
//   <- RequestAuthenticationCertificate        -> AuthenticationCertificate (MFi cert)
//   <- RequestAuthenticationChallengeResponse  -> AuthenticationResponse (MFi sign)
//   <- AuthenticationSucceeded
//   (accessory) -> CarPlayStartSession  ...... Wi-Fi handoff, phone joins the AP

#include "cp_carplay_msg.h"
#include "cp_iap2_link.h"

namespace cp_auth_setup { class MfiSigner; }

namespace cp_carplay
{
class CarplaySession
{
public:
    CarplaySession(cp_iap2::Iap2Link &link, cp_auth_setup::MfiSigner &signer,
                   const AccessoryIdentity &identity, const WirelessSession &wifi);

    // Run the handshake until the Wi-Fi handoff has been delivered (returns
    // true) or the link dies / the phone rejects us (returns false). Assumes the
    // link is already NORMAL.
    bool run();

    // True once CarPlayStartSession has been sent (the phone can now join Wi-Fi).
    bool handoffDelivered() const { return _handoffDelivered; }

private:
    // Dispatch one received CSM. Returns false to stop the loop.
    bool handle(uint16_t msgId, const Bytes &csm);
    bool sendHandoff();

    cp_iap2::Iap2Link &_link;
    cp_auth_setup::MfiSigner &_signer;
    AccessoryIdentity _identity;
    WirelessSession _wifi;
    bool _identified = false;
    bool _authenticated = false;
    bool _handoffDelivered = false;
};
} // namespace cp_carplay

#endif /* SRC_PROTOCOL_CP_CP_CARPLAY_SESSION */
