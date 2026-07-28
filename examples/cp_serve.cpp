// cp_serve -- the CarPlay accessory daemon. Ties identity + mDNS + the TCP
// control server + the MFi chip into one runnable process an iPhone can
// discover and connect to. This is the P1 endpoint: everything from the
// handshake stack, waiting for a real controller.
//
//   cp_serve [/dev/i2c-1] [addr]
//
// Note: a phone still needs a trigger to *initiate* -- BLE for wireless, or
// USB-NCM/iAP2 for wired -- which is the next layer. This serves the control
// endpoint and advertises it; discovery works today.

#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <string>

#include <unistd.h>

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

int main(int argc, char **argv)
{
    const char *bus = argc > 1 ? argv[1] : "/dev/i2c-1";
    uint8_t addr = argc > 2 ? (uint8_t)strtol(argv[2], nullptr, 0) : 0x10;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();

    MfiAuth chip;
    bool haveChip = chip.open(bus, addr);
    MfiAuth::Info info{};
    if (haveChip)
        haveChip = chip.identify(info);
    ChipSigner signer(chip, info.protocolMajor);

    printf("FastCarPlay CarPlay accessory\n");
    printf("  identity pi=%s\n", id.pairingId.c_str());
    printf("  identity pk=%s\n", hex(id.pubRaw).c_str());
    printf("  MFi chip: %s%s\n", haveChip ? "MFi " : "absent",
           haveChip ? (info.protocolMajor == 3 ? "3.0" : "2.0C") : "");

    // A stable device id derived from the pairing id (an iPhone treats it as an
    // opaque accessory id).
    Bytes pidBytes(id.pairingId.begin(), id.pairingId.end());
    Bytes dh = cp_crypto::sha256(pidBytes);
    char deviceId[18];
    snprintf(deviceId, sizeof(deviceId), "%02X:%02X:%02X:%02X:%02X:%02X",
             dh[0] | 0x02, dh[1], dh[2], dh[3], dh[4], dh[5]); // locally-administered

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
        fprintf(stderr, "failed to start (is :7000 or the mDNS socket busy?)\n");
        return 1;
    }

    printf("  advertising _airplay._tcp on :7000, deviceid=%s\n", deviceId);
    printf("Ready. Ctrl-C to stop.\n");
    while (!g_quit)
        pause();

    printf("\nstopping\n");
    server.stop();
    responder.stop();
    return 0;
}
