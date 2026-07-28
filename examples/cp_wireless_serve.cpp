// cp_wireless_serve -- the wireless CarPlay accessory daemon. Ties the whole
// stack together for a live test with a real iPhone:
//   * MFi chip (authentication)
//   * the :7000 CarPlay control server + mDNS advert (the receiving stack)
//   * the Bluetooth iAP2/CarPlay profiles (cp_bt) that make the phone offer
//     wireless CarPlay, run the iAP2 handshake, and hand off the Wi-Fi creds.
//
//   sudo cp_wireless_serve <ssid> <passphrase> <ap-ip> [channel] [/dev/i2c-1] [addr]
//
// The Wi-Fi AP itself (hostapd + dnsmasq on <ap-ip>) must already be running --
// pass the SAME ssid/passphrase/ip/channel here so the handoff matches. After
// pairing the phone over Bluetooth it should appear in Settings > General >
// CarPlay; selecting it runs the handshake and the phone joins the AP.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <string>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <unistd.h>

#include "common/logger.h"
#include "protocol/cp/cp_bluetooth.h"
#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_mdns.h"
#include "protocol/cp/cp_server.h"
#include "protocol/cp/mfi_auth.h"

using cp_crypto::Bytes;

class ChipSigner : public cp_auth_setup::MfiSigner
{
public:
    ChipSigner(MfiAuth &c, int m) : _c(c), _m(m) {}
    bool certificate(Bytes &o) override { return _c.readCertificate(o); }
    bool sign(const Bytes &d, Bytes &s) override { return _c.sign(d, s); }
    int protocolMajor() override { return _m; }
private:
    MfiAuth &_c; int _m;
};

static volatile std::sig_atomic_t g_quit = 0;
static void onSignal(int) { g_quit = 1; }

static std::string hex(const Bytes &b)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) { s.push_back(h[x >> 4]); s.push_back(h[x & 0xf]); }
    return s;
}

// Wireless CarPlay runs over the AP interface's IPv6 link-local (fe80::). The
// phone connects to this address (scoped to its own Wi-Fi link) on the control
// port. Returns the bare fe80 string, or empty if the AP interface has none yet.
static std::string wlanLinkLocal(const char *iface)
{
    struct ifaddrs *ifas = nullptr;
    std::string out;
    if (getifaddrs(&ifas) != 0)
        return out;
    for (struct ifaddrs *a = ifas; a; a = a->ifa_next)
    {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET6 || strcmp(a->ifa_name, iface) != 0)
            continue;
        auto *s6 = (struct sockaddr_in6 *)a->ifa_addr;
        if (!IN6_IS_ADDR_LINKLOCAL(&s6->sin6_addr))
            continue;
        char buf[INET6_ADDRSTRLEN];
        if (inet_ntop(AF_INET6, &s6->sin6_addr, buf, sizeof(buf)))
            out = buf;
        break;
    }
    freeifaddrs(ifas);
    return out;
}

static std::string ifaceMac(const char *iface)
{
    std::ifstream f(std::string("/sys/class/net/") + iface + "/address");
    std::string m;
    std::getline(f, m);
    return m;
}

int main(int argc, char **argv)
{
    const char *ssid = argc > 1 ? argv[1] : "FastCarPlay-AP";
    const char *pass = argc > 2 ? argv[2] : "drive1234";
    const char *apIp = argc > 3 ? argv[3] : "192.168.4.1";
    int channel = argc > 4 ? atoi(argv[4]) : 36;
    const char *bus = argc > 5 ? argv[5] : "/dev/i2c-1";
    uint8_t addr = argc > 6 ? (uint8_t)strtol(argv[6], nullptr, 0) : 0x10;
    const char *wlan = argc > 7 ? argv[7] : "wlan0";

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // FCP_LOG=<0..6> (default Info) so the Bluetooth + session activity is visible.
    const char *lvl = getenv("FCP_LOG");
    Logger::instance().setLevel(lvl ? atoi(lvl) : (int)Logger::Level::Info);

    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();

    MfiAuth chip;
    bool haveChip = chip.open(bus, addr);
    MfiAuth::Info info{};
    if (haveChip)
        haveChip = chip.identify(info);
    ChipSigner signer(chip, info.protocolMajor);

    printf("FastCarPlay wireless CarPlay accessory\n");
    printf("  identity pi=%s pk=%s\n", id.pairingId.c_str(), hex(id.pubRaw).c_str());
    printf("  MFi chip: %s%s\n", haveChip ? "MFi " : "absent (auth will fail!)",
           haveChip ? (info.protocolMajor == 3 ? "3.0" : "2.0C") : "");
    // Wireless CarPlay uses the AP interface's IPv6 link-local + its MAC.
    std::string fe80 = wlanLinkLocal(wlan);
    std::string wlanMac = ifaceMac(wlan);
    printf("  Wi-Fi AP: ssid=%s ch=%d  %s fe80=%s mac=%s\n", ssid, channel, wlan,
           fe80.empty() ? "(NO link-local yet!)" : fe80.c_str(), wlanMac.c_str());
    if (fe80.empty())
        fprintf(stderr, "  WARN: %s has no IPv6 link-local -- is the AP up? falling back to %s\n",
                wlan, apIp);

    // A stable device id derived from the pairing id, shared by mDNS + handoff.
    Bytes pidBytes(id.pairingId.begin(), id.pairingId.end());
    Bytes dh = cp_crypto::sha256(pidBytes);
    char deviceId[18];
    snprintf(deviceId, sizeof(deviceId), "%02X:%02X:%02X:%02X:%02X:%02X",
             dh[0] | 0x02, dh[1], dh[2], dh[3], dh[4], dh[5]);

    cp_mdns::Config mdns;
    mdns.instance = "FastCarPlay";
    mdns.txt = {
        std::string("deviceid=") + deviceId,
        "features=0x44540380,0x61",
        "flags=0x4",
        "srcvers=550.1",
        "pi=" + id.pairingId,
        "pk=" + hex(id.pubRaw),
    };

    cp_mdns::MdnsResponder responder;
    cp_server::Server server;
    if (!server.start(7000, haveChip ? &signer : nullptr) || !responder.start(mdns))
    {
        fprintf(stderr, "failed to start :7000 / mDNS (already running?)\n");
        return 1;
    }

    // The wireless session handed to the phone over Bluetooth.
    cp_bt::Config cfg;
    cfg.alias = "FastCarPlay";
    cfg.signer = haveChip ? &signer : nullptr;
    cfg.identity.messagesSent = cp_carplay::defaultMessagesSent();
    cfg.identity.messagesReceived = cp_carplay::defaultMessagesReceived();
    cfg.wifi.ssid = ssid;
    cfg.wifi.passphrase = pass;
    cfg.wifi.channel = (uint8_t)channel;
    cfg.wifi.ipAddress = fe80.empty() ? apIp : fe80; // fe80 link-local for wireless
    cfg.wifi.security = cp_carplay::WifiSecurity::WpaWpa2;
    cfg.wifi.port = 7000;
    cfg.wifi.deviceIdentifier = wlanMac.empty() ? id.pairingId : wlanMac;
    cfg.wifi.publicKey = hex(id.pubRaw);
    cfg.wifi.sourceVersion = "550.1";

    cp_bt::CpBluetooth bt;
    if (!bt.start(cfg))
    {
        fprintf(stderr, "Bluetooth bootstrap failed (is bluetoothd running? root?)\n");
        server.stop();
        responder.stop();
        return 1;
    }

    printf("  advertising _airplay._tcp on :7000, deviceid=%s\n", deviceId);
    printf("Ready. Pair the iPhone, then pick FastCarPlay in Settings > General > CarPlay.\n");
    printf("Ctrl-C to stop.\n");
    while (!g_quit)
        pause();

    printf("\nstopping\n");
    bt.stop();
    server.stop();
    responder.stop();
    return 0;
}
