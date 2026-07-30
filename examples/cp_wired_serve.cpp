// cp_wired_serve -- full wired CarPlay session over USB (config-6 / carkit).
//
// Extends cp_wired_probe from "the phone reaches :7000" to a real CarPlay
// session: after the iAP2 CarPlayStartSession handoff, it runs the actual
// cp_server(:7000) + MFi signer + AV sinks, so the phone completes pair-setup
// and streams video/audio, which we count (and dump the first video frames).
// This proves the wired transport carries the same AV stack as wireless.
//
// Prereqs (run as root): phone in config 6, a usbmux socket served (LIVI muxd or
// cp_usbmux), kernel usb0 up with an fe80, a fresh pair record, MFi chip powered.
//
//   sudo ./cp_wired_serve <dashed-udid> <usbmux-sock> [usb-iface=usb0] [i2c-bus=/dev/i2c-1]

#include <arpa/inet.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

#include "common/logger.h"
#include "protocol/cp/cp_av.h"
#include "protocol/cp/cp_carplay_msg.h"
#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_iap2.h"
#include "protocol/cp/cp_iap2_link.h"
#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_server.h"
#include "protocol/cp/mfi_auth.h"

using cp_iap2::Bytes;

namespace
{
std::atomic<bool> g_run{true};
std::atomic<uint32_t> g_videoFrames{0};
std::atomic<uint64_t> g_videoBytes{0};
std::atomic<uint32_t> g_audioChunks{0};

// The MFi signer cp_server uses for :7000 auth-setup (same chip the iAP2 auth uses).
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

std::string hex(const Bytes &b)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b)
    {
        s.push_back(h[x >> 4]);
        s.push_back(h[x & 0xf]);
    }
    return s;
}

std::string ifaceLinkLocal(const std::string &iface)
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return "";
    std::string out;
    for (struct ifaddrs *p = ifa; p; p = p->ifa_next)
    {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET6 || iface != p->ifa_name)
            continue;
        auto *s6 = (struct sockaddr_in6 *)p->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr))
            continue;
        char buf[INET6_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf));
        out = buf;
        break;
    }
    freeifaddrs(ifa);
    return out;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("usage: %s <dashed-udid> <usbmux-sock> [usb-iface=usb0] [i2c-bus=/dev/i2c-1]\n", argv[0]);
        return 2;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char *udid = argv[1];
    const std::string sock = argv[2];
    const std::string iface = argc > 3 ? argv[3] : "usb0";
    const std::string i2cbus = argc > 4 ? argv[4] : "/dev/i2c-1";
    const char *lvl = getenv("FCP_LOG");
    set_log_level(lvl ? atoi(lvl) : (int)Logger::Level::Info);

    setenv("USBMUXD_SOCKET_ADDRESS", ("UNIX:" + sock).c_str(), 1);

    // ── Identity + MFi chip ──────────────────────────────────────────────
    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();
    MfiAuth mfi;
    MfiAuth::Info mfiInfo{};
    bool haveChip = mfi.open(i2cbus, 0x10) && mfi.identify(mfiInfo);
    Bytes cert;
    if (haveChip)
        haveChip = mfi.readCertificate(cert);
    if (!haveChip)
    {
        printf("[mfi] chip/cert unavailable (%s) -- auth will fail\n", mfi.lastError());
        return 1;
    }
    ChipSigner signer(mfi, mfiInfo.protocolMajor);
    printf("[mfi] MFi %s, cert %zu bytes; pi=%s pk=%s\n",
           mfiInfo.protocolMajor == 3 ? "3.0" : "2.0C", cert.size(), id.pairingId.c_str(),
           hex(id.pubRaw).c_str());

    // Stable device id from the pairing id (shared by the handoff).
    Bytes pidBytes(id.pairingId.begin(), id.pairingId.end());
    Bytes dh = cp_crypto::sha256(pidBytes);
    char deviceId[18];
    snprintf(deviceId, sizeof(deviceId), "%02X:%02X:%02X:%02X:%02X:%02X", dh[0] | 0x02, dh[1], dh[2], dh[3],
             dh[4], dh[5]);

    // ── The real :7000 CarPlay server + AV sinks ─────────────────────────
    cp_server::Server server;
    cp_av::Sinks sinks;
    sinks.onVideoCodec = [](bool hevc) { printf("[av] screen codec: %s\n", hevc ? "HEVC" : "H.264"); };
    sinks.onVideo = [](const Bytes &annexB) {
        uint32_t n = g_videoFrames.fetch_add(1) + 1;
        g_videoBytes.fetch_add(annexB.size());
        if (n <= 3 || n % 60 == 0)
            printf("[av] video frame #%u (%zu bytes, total %.1f KB)\n", n, annexB.size(),
                   g_videoBytes.load() / 1024.0);
    };
    sinks.onAudio = [](int type, int rate, int ch, const Bytes &pcm) {
        uint32_t n = g_audioChunks.fetch_add(1) + 1;
        if (n <= 3 || n % 200 == 0)
            printf("[av] audio chunk #%u type=%d %dHz x%d (%zu bytes)\n", n, type, rate, ch, pcm.size());
    };
    cp_av::Config avcfg;
    avcfg.hevc = true; // Pi5 decodes HEVC; F1C would set false
    server.setAvConfig(avcfg);
    server.setAvSinks(sinks);
    server.setLifecycle([] { printf("[:7000] >>> phone opened the CarPlay control channel <<<\n"); },
                        [] { printf("[:7000] control channel closed\n"); });
    if (!server.start(7000, &signer))
    {
        printf("[:7000] failed to start (already running?)\n");
        return 1;
    }
    printf("[:7000] CarPlay control server up\n");

    // ── carkit TLS iAP2 stream via libimobiledevice ──────────────────────
    idevice_t dev = nullptr;
    lockdownd_client_t ld = nullptr;
    lockdownd_service_descriptor_t svc = nullptr;
    idevice_connection_t conn = nullptr;
    if (idevice_new_with_options(&dev, udid, IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS ||
        lockdownd_client_new_with_handshake(dev, &ld, "fastcarplay-wired") != LOCKDOWN_E_SUCCESS ||
        lockdownd_start_service(ld, "com.apple.carkit.service", &svc) != LOCKDOWN_E_SUCCESS || !svc ||
        idevice_connect(dev, svc->port, &conn) != IDEVICE_E_SUCCESS ||
        (svc->ssl_enabled && idevice_connection_enable_ssl(conn) != IDEVICE_E_SUCCESS))
    {
        printf("[carkit] failed to open TLS iAP2 channel (paired? unlocked? UNIX: sock? dashed udid?)\n");
        server.stop();
        return 1;
    }
    printf("[carkit] TLS iAP2 channel up\n");

    // ── Single-thread SSL<->socketpair pump (see cp_wired_probe notes) ────
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    const int linkFd = sp[0], bridgeFd = sp[1];
    fcntl(bridgeFd, F_SETFL, fcntl(bridgeFd, F_GETFL, 0) | O_NONBLOCK);
    std::thread pump([&] {
        char buf[8192];
        while (g_run.load())
        {
            for (;;)
            {
                ssize_t n = read(bridgeFd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                uint32_t sent = 0, off = 0;
                while (off < (uint32_t)n)
                {
                    if (idevice_connection_send(conn, buf + off, (uint32_t)n - off, &sent) != IDEVICE_E_SUCCESS)
                    {
                        g_run.store(false);
                        break;
                    }
                    off += sent;
                }
            }
            uint32_t got = 0;
            idevice_error_t e = idevice_connection_receive_timeout(conn, buf, sizeof(buf), &got, 100);
            if (got > 0)
                (void)!write(bridgeFd, buf, got);
            else if (e != IDEVICE_E_SUCCESS && e != IDEVICE_E_TIMEOUT)
                break;
        }
        shutdown(bridgeFd, SHUT_RDWR);
    });

    // ── iAP2 link + wired control-session handshake ──────────────────────
    std::string fe80 = ifaceLinkLocal(iface);
    printf("[ncm] %s fe80 = %s\n", iface.c_str(), fe80.empty() ? "(none!)" : fe80.c_str());

    cp_iap2::Iap2Link link(linkFd);
    if (!link.negotiate(true))
    {
        printf("[iap2] negotiation failed\n");
        g_run.store(false);
        pump.join();
        server.stop();
        return 1;
    }
    printf("[iap2] link NORMAL\n");

    cp_carplay::AccessoryIdentity acc;
    acc.name = "FastCarPlay";
    acc.modelIdentifier = "FastCarPlay1,1";
    acc.manufacturer = "FastCarPlay";

    std::thread ctrl([&] {
        bool sentStart = false;
        Bytes csm;
        while (g_run.load() && link.recvControl(csm))
        {
            uint16_t msgId = 0;
            std::vector<cp_iap2::CsmParam> params;
            if (!cp_iap2::parseCsm(csm, msgId, params))
                continue;
            switch (msgId)
            {
            case cp_carplay::MSG_START_IDENTIFICATION:
                printf("[csm] StartIdentification -> ident\n");
                link.sendControl(cp_carplay::buildWiredIdentification(acc));
                break;
            case cp_carplay::MSG_IDENTIFICATION_ACCEPTED:
                printf("[csm] identification ACCEPTED\n");
                break;
            case cp_carplay::MSG_IDENTIFICATION_REJECTED:
                printf("[csm] identification REJECTED\n");
                break;
            case cp_carplay::MSG_REQUEST_AUTH_CERTIFICATE:
                link.sendControl(cp_carplay::buildAuthCertificate(cert));
                break;
            case cp_carplay::MSG_REQUEST_AUTH_CHALLENGE_RESPONSE:
            {
                Bytes challenge, sig;
                if (cp_carplay::parseAuthChallenge(csm, challenge) && mfi.sign(challenge, sig))
                    link.sendControl(cp_carplay::buildAuthResponse(sig));
                else
                    printf("[csm] auth sign FAILED (%s)\n", mfi.lastError());
                break;
            }
            case cp_carplay::MSG_AUTH_SUCCEEDED:
                printf("[csm] MFi authentication SUCCEEDED\n");
                break;
            case cp_carplay::MSG_CARPLAY_AVAILABILITY:
                if (!sentStart)
                {
                    cp_carplay::WiredSession ws;
                    ws.ipAddress = fe80;
                    ws.port = 7000;
                    ws.deviceIdentifier = deviceId;
                    ws.publicKey = hex(id.pubRaw);
                    ws.sourceVersion = "280.33.8";
                    printf("[csm] CarPlayAvailability -> CarPlayStartSession ip=%s\n", fe80.c_str());
                    link.sendControl(cp_carplay::buildWiredStartSession(ws));
                    sentStart = true;
                }
                break;
            default:
                break;
            }
        }
        printf("[iap2] control session ended\n");
    });

    printf("Ready -- the phone should now pair-setup + stream CarPlay over USB. Ctrl-C to stop.\n");
    // Periodic stats until the control session or a signal ends it.
    for (int i = 0; g_run.load() && i < 6000; i++)
    {
        struct pollfd pfd { linkFd, 0, 0 };
        poll(&pfd, 1, 1000);
        if (i && i % 10 == 0)
            printf("[stats] video=%u frames %.1f KB, audio=%u chunks\n", g_videoFrames.load(),
                   g_videoBytes.load() / 1024.0, g_audioChunks.load());
    }

    g_run.store(false);
    shutdown(linkFd, SHUT_RDWR);
    ctrl.join();
    pump.join();
    server.stop();
    return 0;
}
