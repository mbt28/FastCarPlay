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
    // 1) Head unit Wi-Fi AP the phone will join.
    if (!_wifi.start())
        log_e("wireless: Wi-Fi AP failed to start");

    // 2) Bluetooth bootstrap: advertise the AA profile and, on connect, hand
    //    the phone the AP credentials + this TCP endpoint.
    aa_aaw::Params params;
    params.ip = _wifi.ip();
    params.port = AA_TCP_PORT;
    params.ssid = Settings::wifiSsid.value;
    params.passphrase = Settings::wifiPass.value;
    params.bssid = _wifi.bssid();
    params.channel = Settings::wifiChannel;
    if (!_bluetooth.start(params))
        log_e("wireless: Bluetooth bootstrap failed to start");

    // 3) Run the GAL session over TCP (the phone connects after the handshake).
    AaConnection::start();
}

void AaWirelessConnection::stop()
{
    AaConnection::stop();
    _bluetooth.stop();
    _wifi.stop();
}

#endif /* USE_AA_WIRELESS */
