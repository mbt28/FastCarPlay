// iAP2 link-layer loopback test: two Iap2Link endpoints negotiate over a
// socketpair (detect -> SYN/ACK -> normal), then the "accessory" sends a
// CarPlayStartSession Wi-Fi handoff over the control session and the "phone"
// side receives + parses it. Exercises the real link state machine both ways.
// No Bluetooth/phone -- socketpair stands in for the RFCOMM stream.
//
//   make cp_iap2_link_test && ../out/cp_iap2_link_test

#include <cstdio>
#include <sys/socket.h>
#include <thread>

#include "protocol/cp/cp_carplay_msg.h"
#include "protocol/cp/cp_iap2_link.h"

using cp_iap2::Bytes;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

int main()
{
    printf("iAP2 link loopback\n\n");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
    {
        perror("socketpair");
        return 1;
    }

    cp_iap2::Iap2Link accessory(sv[0]);
    cp_iap2::Iap2Link phone(sv[1]);

    bool accOk = false, phoneOk = false;
    // Accessory waits for the peer marker; phone initiates. Both negotiate to
    // NORMAL concurrently (each keeps reading, so neither blocks the other).
    std::thread ta([&] { accOk = accessory.negotiate(false); });
    std::thread tp([&] { phoneOk = phone.negotiate(true); });
    ta.join();
    tp.join();

    check(accOk, "accessory reaches NORMAL");
    check(phoneOk, "phone reaches NORMAL");
    check(accessory.alive() && phone.alive(), "both links alive after negotiate");

    if (accOk && phoneOk)
    {
        cp_carplay::WirelessSession s;
        s.ssid = "FastCarPlay-AP";
        s.passphrase = "drive1234";
        s.channel = 36;
        s.ipAddress = "192.168.4.1";
        s.security = cp_carplay::WifiSecurity::WpaWpa2;
        s.port = 7000;
        s.deviceIdentifier = "pi-abc";
        s.publicKey = "deadbeef";
        s.sourceVersion = "550.1";

        Bytes sent = cp_carplay::buildStartSession(s);

        Bytes got;
        bool recvd = false;
        std::thread rx([&] { recvd = phone.recvControl(got); });
        bool txOk = accessory.sendControl(sent);
        rx.join();

        check(txOk, "accessory sends CarPlayStartSession over control session");
        check(recvd && got == sent, "phone receives the exact CSM bytes");

        cp_carplay::WirelessSession out;
        check(cp_carplay::parseStartSession(got, out) && out.ssid == s.ssid &&
                  out.passphrase == s.passphrase && out.port == 7000,
              "handoff parses back to the same Wi-Fi session");
    }

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
