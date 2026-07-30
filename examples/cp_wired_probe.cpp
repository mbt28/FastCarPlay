// cp_wired_probe -- live wired-CarPlay (config-6/carkit) handshake probe.
//
// Prereqs (run as root): the phone is in USB config 6 and a usbmux socket is
// being served for it (e.g. LIVI's muxd at /tmp/livi-usbmux-<serial8>.sock, or
// our own cp_usbmux later). The kernel cdc_ncm driver has brought up usb0 with
// an fe80 link-local. A fresh pair record exists (idevicepair pair).
//
// What it does, end to end:
//   1. libimobiledevice: open com.apple.carkit.service + enable SSL = the TLS
//      iAP2 control stream (idevice_connection_t).
//   2. Bridge that connection to a socketpair fd so the existing cp_iap2::Iap2Link
//      (which wants a byte-stream fd) can drive it.
//   3. Iap2Link.negotiate() -> the iAP2 link goes NORMAL.
//   4. Drive the wired control-session handshake: StartIdentification ->
//      IdentificationInformation (USBHostTransport) -> Accepted; MFi auth
//      cert/challenge via the real i2c chip; on CarPlayAvailability send
//      CarPlayStartSession pointing the phone at [fe80::usb0]:7000.
//   5. Listen on :7000 and log when the phone opens the reverse control
//      connection -- that is the proof the wired transport works end to end.
//
// Usage: sudo ./cp_wired_probe <dashed-udid> <usbmux-sock> [usb-iface=usb0] [i2c-bus=/dev/i2c-1]

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <mutex>
#include <net/if.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "common/logger.h"
#include "protocol/cp/cp_carplay_msg.h"
#include "protocol/cp/cp_iap2.h"
#include "protocol/cp/cp_iap2_link.h"
#include "protocol/cp/mfi_auth.h"

using cp_iap2::Bytes;

namespace
{
volatile bool g_run = true;

// The accessory's link-local IPv6 (EUI-64) on the given interface, as a bare
// string (no %zone -- the phone supplies its own NCM interface scope).
std::string ifaceLinkLocal(const std::string &iface)
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return "";
    std::string out;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET6)
            continue;
        if (iface != p->ifa_name)
            continue;
        auto *sin6 = (struct sockaddr_in6 *)p->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr))
            continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
        out = buf;
        break;
    }
    freeifaddrs(ifa);
    return out;
}

// Accept-logger on [::]:7000 -- proves the phone opens the reverse control link.
void listen7000()
{
    int s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s < 0)
    {
        printf("[:7000] socket: %s\n", strerror(errno));
        return;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_addr = in6addr_any;
    a.sin6_port = htons(7000);
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
    {
        printf("[:7000] bind: %s\n", strerror(errno));
        close(s);
        return;
    }
    listen(s, 4);
    printf("[:7000] listening for the phone's CarPlay control connection...\n");
    while (g_run)
    {
        struct pollfd pfd { s, POLLIN, 0 };
        if (poll(&pfd, 1, 500) <= 0)
            continue;
        struct sockaddr_in6 peer;
        socklen_t pl = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &pl);
        if (c < 0)
            continue;
        char ip[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &peer.sin6_addr, ip, sizeof(ip));
        printf("\n>>> PHONE CONNECTED to :7000 from [%s]:%u -- WIRED CARPLAY TRANSPORT WORKS <<<\n\n",
               ip, ntohs(peer.sin6_port));
        // Peek the first bytes so we can see the CarPlay/RTSP handshake begin.
        unsigned char b[64];
        ssize_t n = recv(c, b, sizeof(b), 0);
        printf("[:7000] first %zd bytes:", n);
        for (ssize_t i = 0; i < n; i++)
            printf(" %02x", b[i]);
        printf("\n");
        close(c);
    }
    close(s);
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("usage: %s <dashed-udid> <usbmux-sock> [usb-iface=usb0] [i2c-bus=/dev/i2c-1]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffer so a kill doesn't eat output
    const char *udid = argv[1];
    const std::string sock = argv[2];
    const std::string iface = argc > 3 ? argv[3] : "usb0";
    const std::string i2cbus = argc > 4 ? argv[4] : "/dev/i2c-1";
    set_log_level(4); // Debug -- see the iAP2 link negotiation

    std::string sockAddr = "UNIX:" + sock;
    setenv("USBMUXD_SOCKET_ADDRESS", sockAddr.c_str(), 1);

    // 1) carkit TLS iAP2 stream via libimobiledevice.
    idevice_t dev = nullptr;
    if (idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS)
    {
        printf("idevice_new failed (udid dashed? usbmux sock served? UNIX: prefix?)\n");
        return 1;
    }
    lockdownd_client_t ld = nullptr;
    if (lockdownd_client_new_with_handshake(dev, &ld, "fastcarplay-wired") != LOCKDOWN_E_SUCCESS)
    {
        printf("lockdownd handshake failed (paired? unlocked?)\n");
        return 1;
    }
    lockdownd_service_descriptor_t svc = nullptr;
    if (lockdownd_start_service(ld, "com.apple.carkit.service", &svc) != LOCKDOWN_E_SUCCESS || !svc)
    {
        printf("start com.apple.carkit.service failed\n");
        return 1;
    }
    idevice_connection_t conn = nullptr;
    if (idevice_connect(dev, svc->port, &conn) != IDEVICE_E_SUCCESS)
    {
        printf("connect carkit port %d failed\n", svc->port);
        return 1;
    }
    if (svc->ssl_enabled && idevice_connection_enable_ssl(conn) != IDEVICE_E_SUCCESS)
    {
        printf("enable_ssl failed\n");
        return 1;
    }
    printf("[carkit] TLS iAP2 channel up (port %d ssl=%d)\n", svc->port, svc->ssl_enabled);

    // 2) Bridge the connection to a socketpair fd for Iap2Link.
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0)
    {
        printf("socketpair: %s\n", strerror(errno));
        return 1;
    }
    const int linkFd = sp[0];
    const int bridgeFd = sp[1];

    auto dump = [](const char *dir, const char *b, ssize_t n) {
        printf("[bridge] %s %zd:", dir, n);
        for (ssize_t i = 0; i < n && i < 24; i++)
            printf(" %02x", (unsigned char)b[i]);
        printf("%s\n", n > 24 ? " ..." : "");
    };
    // Single-threaded pump: libimobiledevice's SSL connection is NOT safe for a
    // concurrent send + receive from two threads (the phone goes silent). One
    // thread: non-blocking drain of the link fd -> SSL send, then a timed SSL
    // receive -> link fd. No concurrent SSL access.
    fcntl(bridgeFd, F_SETFL, fcntl(bridgeFd, F_GETFL, 0) | O_NONBLOCK);
    std::thread pump([&] {
        char buf[8192];
        int shownTx = 0, shownRx = 0;
        while (g_run)
        {
            // link -> phone (drain everything Iap2Link has queued)
            for (;;)
            {
                ssize_t n = read(bridgeFd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                if (shownTx++ < 12)
                    dump("link->phone", buf, n);
                uint32_t sent = 0, off = 0;
                while (off < (uint32_t)n)
                {
                    if (idevice_connection_send(conn, buf + off, (uint32_t)n - off, &sent) != IDEVICE_E_SUCCESS)
                    {
                        g_run = false;
                        break;
                    }
                    off += sent;
                }
            }
            // phone -> link. NOTE: receive_timeout returns IDEVICE_E_TIMEOUT even
            // when it got some bytes (it just didn't fill the buffer / hit the
            // deadline), so use `got` regardless of the code -- only a real error
            // with no data is fatal.
            uint32_t got = 0;
            idevice_error_t e = idevice_connection_receive_timeout(conn, buf, sizeof(buf), &got, 100);
            if (got > 0)
            {
                if (shownRx++ < 12)
                    dump("phone->link", buf, got);
                (void)!write(bridgeFd, buf, got);
            }
            else if (e != IDEVICE_E_SUCCESS && e != IDEVICE_E_TIMEOUT)
            {
                printf("[bridge] phone recv error %d\n", e);
                break;
            }
        }
        shutdown(bridgeFd, SHUT_RDWR);
    });

    // 3) MFi chip (cert + signer).
    MfiAuth mfi;
    Bytes cert;
    MfiAuth::Info mfiInfo;
    // identify() sets the protocol major (2 = 20-byte SHA-1, 3 = 32-byte SHA-256)
    // which sign() needs to accept the phone's challenge -- skip it and sign()
    // rejects the digest length.
    if (mfi.open(i2cbus, 0x10) && mfi.identify(mfiInfo) && mfi.readCertificate(cert))
        printf("[mfi] chip up: devVer=0x%02x proto=%d digest=%u sig=%u, cert %zu bytes\n",
               mfiInfo.deviceVersion, mfiInfo.protocolMajor, mfiInfo.digestLen, mfiInfo.signatureLen,
               cert.size());
    else
        printf("[mfi] WARNING: chip/identify/cert unavailable (%s) -- auth will fail\n", mfi.lastError());

    // 4) The accessory's fe80 on the NCM interface (the phone's :7000 target).
    std::string fe80 = ifaceLinkLocal(iface);
    printf("[ncm] %s link-local = %s\n", iface.c_str(), fe80.empty() ? "(none!)" : fe80.c_str());

    // 5) :7000 accept-logger.
    std::thread lst(listen7000);

    // 6) iAP2 link negotiation.
    cp_iap2::Iap2Link link(linkFd);
    if (!link.negotiate(true))
    {
        printf("[iap2] negotiation did not reach NORMAL\n");
        g_run = false;
        pump.join();
        lst.join();
        return 1;
    }
    printf("[iap2] link NORMAL -- driving wired CarPlay control session\n");

    cp_carplay::AccessoryIdentity id;
    id.name = "FastCarPlay";
    id.modelIdentifier = "FastCarPlay1,1";
    id.manufacturer = "FastCarPlay";

    bool sentStart = false;
    Bytes csm;
    while (g_run && link.recvControl(csm))
    {
        uint16_t msgId = 0;
        std::vector<cp_iap2::CsmParam> params;
        if (!cp_iap2::parseCsm(csm, msgId, params))
        {
            printf("[csm] unparseable (%zu bytes)\n", csm.size());
            continue;
        }
        printf("[csm] <- 0x%04X (%zu params)\n", msgId, params.size());
        switch (msgId)
        {
        case cp_carplay::MSG_START_IDENTIFICATION: // 0x1D00
            printf("[csm] -> IdentificationInformation (wired)\n");
            link.sendControl(cp_carplay::buildWiredIdentification(id));
            break;
        case cp_carplay::MSG_IDENTIFICATION_ACCEPTED: // 0x1D02
            printf("[csm] identification ACCEPTED\n");
            break;
        case cp_carplay::MSG_IDENTIFICATION_REJECTED: // 0x1D03
            printf("[csm] identification REJECTED -- fields the phone objects to:");
            for (auto &p : params)
                printf(" 0x%02X", p.id);
            printf("\n");
            break;
        case cp_carplay::MSG_REQUEST_AUTH_CERTIFICATE: // 0xAA00
            printf("[csm] -> AuthenticationCertificate (%zu bytes)\n", cert.size());
            link.sendControl(cp_carplay::buildAuthCertificate(cert));
            break;
        case cp_carplay::MSG_REQUEST_AUTH_CHALLENGE_RESPONSE: // 0xAA02
        {
            Bytes challenge, sig;
            if (cp_carplay::parseAuthChallenge(csm, challenge) && mfi.sign(challenge, sig))
            {
                printf("[csm] -> AuthenticationResponse (challenge %zu -> sig %zu)\n", challenge.size(),
                       sig.size());
                link.sendControl(cp_carplay::buildAuthResponse(sig));
            }
            else
                printf("[csm] auth sign FAILED (%s)\n", mfi.lastError());
            break;
        }
        case cp_carplay::MSG_AUTH_SUCCEEDED: // 0xAA05
            printf("[csm] MFi authentication SUCCEEDED\n");
            break;
        case cp_carplay::MSG_AUTH_FAILED: // 0xAA04
            printf("[csm] MFi authentication FAILED\n");
            break;
        case cp_carplay::MSG_CARPLAY_AVAILABILITY: // 0x4300
        {
            if (sentStart)
                break;
            cp_carplay::WiredSession ws;
            ws.ipAddress = fe80;
            ws.port = 7000;
            ws.deviceIdentifier = "aa:bb:cc:dd:ee:ff"; // placeholder BT-MAC id
            ws.publicKey = "0000000000000000000000000000000000000000000000000000000000000000";
            ws.sourceVersion = "280.33.8";
            printf("[csm] -> CarPlayStartSession ip=%s port=7000\n", fe80.c_str());
            link.sendControl(cp_carplay::buildWiredStartSession(ws));
            sentStart = true;
            break;
        }
        default:
            break;
        }
    }

    printf("[iap2] control session ended\n");
    g_run = false;
    pump.join();
    lst.join();
    return 0;
}
