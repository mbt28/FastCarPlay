// Wireless CarPlay session test: the real CarplaySession (accessory) runs over
// a socketpair link against a scripted "iPhone" that drives the full sequence
// -- StartIdentification, identification-accept, MFi cert + challenge, auth-
// success -- and verifies the accessory answers correctly and finally hands off
// the Wi-Fi credentials (CarPlayStartSession). A fake MfiSigner stands in for
// the coprocessor. No Bluetooth/phone/chip.
//
//   make cp_carplay_session_test && ../out/cp_carplay_session_test

#include <cstdio>
#include <sys/socket.h>
#include <thread>

#include "protocol/cp/cp_auth_setup.h"
#include "protocol/cp/cp_carplay_session.h"
#include "protocol/cp/cp_iap2_link.h"

using cp_iap2::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

// A deterministic stand-in for the MFi coprocessor.
static const Bytes FAKE_CERT = {0x30, 0x82, 0x02, 0x5f, 0x11, 0x22, 0x33};
static Bytes fakeSign(const Bytes &challenge)
{
    Bytes sig(64);
    for (size_t i = 0; i < sig.size(); i++)
        sig[i] = (uint8_t)(challenge[i % challenge.size()] ^ 0x5A);
    return sig;
}
class FakeSigner : public cp_auth_setup::MfiSigner
{
public:
    bool certificate(Bytes &out) override { out = FAKE_CERT; return true; }
    bool sign(const Bytes &digest, Bytes &sig) override { sig = fakeSign(digest); return true; }
    int protocolMajor() override { return 3; }
};

static uint16_t idOf(const Bytes &csm) { return (uint16_t)((csm[4] << 8) | csm[5]); }

int main()
{
    printf("wireless CarPlay session\n\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { perror("socketpair"); return 1; }

    cp_iap2::Iap2Link accLink(sv[0]);
    cp_iap2::Iap2Link phoneLink(sv[1]);

    FakeSigner signer;
    cp_carplay::AccessoryIdentity ident;
    ident.bluetoothMac = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ident.messagesSent = cp_carplay::defaultMessagesSent();
    ident.messagesReceived = cp_carplay::defaultMessagesReceived();

    cp_carplay::WirelessSession wifi;
    wifi.ssid = "FastCarPlay-AP";
    wifi.passphrase = "drive1234";
    wifi.channel = 36;
    wifi.ipAddress = "192.168.4.1";
    wifi.port = 7000;
    wifi.deviceIdentifier = "pi-1";
    wifi.publicKey = "abcd";
    wifi.sourceVersion = "550.1";

    // Accessory side: negotiate, then run the session, on its own thread.
    bool sessionOk = false;
    cp_carplay::CarplaySession session(accLink, signer, ident, wifi);
    std::thread acc([&] {
        if (accLink.negotiate(false))
            sessionOk = session.run();
    });

    // Phone side: negotiate as initiator, then script the handshake.
    bool ok = phoneLink.negotiate(true);
    check(ok, "phone link negotiates");

    Bytes rx;
    // 1) StartIdentification -> IdentificationInformation
    phoneLink.sendControl(cp_iap2::packCsm(cp_carplay::MSG_START_IDENTIFICATION, {}));
    check(phoneLink.recvControl(rx) && idOf(rx) == cp_carplay::MSG_IDENTIFICATION_INFORMATION,
          "accessory replies IdentificationInformation");
    {
        // Confirm the wireless CarPlay transport component is declared.
        uint16_t mid; std::vector<cp_carplay::CsmParam> ps; bool wcp = false;
        cp_iap2::parseCsm(rx, mid, ps);
        for (auto &p : ps) if (p.id == 24) wcp = true;
        check(wcp, "identity declares wireless CarPlay transport");
    }

    // 2) IdentificationAccepted
    phoneLink.sendControl(cp_iap2::packCsm(cp_carplay::MSG_IDENTIFICATION_ACCEPTED, {}));

    // 3) RequestAuthenticationCertificate -> AuthenticationCertificate
    phoneLink.sendControl(cp_iap2::packCsm(cp_carplay::MSG_REQUEST_AUTH_CERTIFICATE, {}));
    check(phoneLink.recvControl(rx) && idOf(rx) == cp_carplay::MSG_AUTH_CERTIFICATE,
          "accessory replies AuthenticationCertificate");
    {
        uint16_t mid; std::vector<cp_carplay::CsmParam> ps;
        cp_iap2::parseCsm(rx, mid, ps);
        check(ps.size() == 1 && ps[0].value == FAKE_CERT, "certificate bytes match the signer");
    }

    // 4) RequestAuthenticationChallengeResponse{challenge} -> AuthenticationResponse
    Bytes challenge(32, 0x5A);
    phoneLink.sendControl(
        cp_iap2::packCsm(cp_carplay::MSG_REQUEST_AUTH_CHALLENGE_RESPONSE, {{0, challenge}}));
    check(phoneLink.recvControl(rx) && idOf(rx) == cp_carplay::MSG_AUTH_RESPONSE,
          "accessory replies AuthenticationResponse");
    {
        uint16_t mid; std::vector<cp_carplay::CsmParam> ps;
        cp_iap2::parseCsm(rx, mid, ps);
        check(ps.size() == 1 && ps[0].value == fakeSign(challenge),
              "response is the MFi signature over the challenge");
    }

    // 5) AuthenticationSucceeded -> CarPlayAvailability + CarPlayStartSession
    phoneLink.sendControl(cp_iap2::packCsm(cp_carplay::MSG_AUTH_SUCCEEDED, {}));
    check(phoneLink.recvControl(rx) && idOf(rx) == cp_carplay::MSG_CARPLAY_AVAILABILITY,
          "accessory declares CarPlayAvailability");
    check(phoneLink.recvControl(rx) && idOf(rx) == cp_carplay::MSG_CARPLAY_START_SESSION,
          "accessory hands off CarPlayStartSession");
    {
        cp_carplay::WirelessSession got;
        check(cp_carplay::parseStartSession(rx, got) && got.ssid == wifi.ssid &&
                  got.port == 7000 && got.passphrase == wifi.passphrase,
              "handoff carries the correct Wi-Fi credentials + port");
    }

    acc.join();
    check(sessionOk && session.handoffDelivered(), "session completes with handoff delivered");

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
