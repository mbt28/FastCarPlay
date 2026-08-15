#include "aa_wireless.h"

#ifdef USE_AA_WIRELESS

#include <memory>

#include "protocol/aa/aa_tcp_transport.h"
#include "common/logger.h"
#include "settings.h"

AaWirelessConnection::AaWirelessConnection()
    : AaConnection(std::make_unique<AaTcpTransport>())
{
    _method = "android-auto-wireless";
}

void AaWirelessConnection::start()
{
    // 1) The Wi-Fi AP the phone will join is the system's, already running.
    //    Read what it is actually advertising rather than what a preset says,
    //    so the credentials we hand the phone always describe a real network.
    const wifi_ap::Params ap = wifi_ap::read();
    if (!ap.valid())
        log_e("wireless: no system Wi-Fi AP (ssid '%s', ip '%s') -- the phone "
              "will have no network to join after the Bluetooth handoff",
              ap.ssid.c_str(), ap.ip.c_str());

    // 2) Bluetooth bootstrap: advertise the AA profile and, on connect, hand
    //    the phone the AP credentials + this TCP endpoint.
    aa_aaw::Params params;
    params.ip = ap.ip;
    params.port = AA_TCP_PORT;
    params.ssid = ap.ssid;
    params.passphrase = ap.passphrase;
    params.bssid = ap.bssid;
    params.channel = ap.channel;
    if (!_bluetooth.start(params))
        log_e("wireless: Bluetooth bootstrap failed to start");

    // 3) Run the GAL session over TCP (the phone connects after the handshake).
    AaConnection::start();
}

void AaWirelessConnection::stop()
{
    AaConnection::stop();
    _bluetooth.stop();
    // The AP is the system's: leave it running, it is the debug/deploy channel.
}

#endif /* USE_AA_WIRELESS */
