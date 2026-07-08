#ifndef SRC_PROTOCOL_AA_AA_WIRELESS
#define SRC_PROTOCOL_AA_AA_WIRELESS

#ifdef USE_AA_WIRELESS

#include "protocol/aa/aa_connection.h"
#include "protocol/aa/aa_bluetooth.h"
#include "protocol/aa/aa_wifi.h"

// Wireless Android Auto backend: brings up the Wi-Fi AP and the Bluetooth
// bootstrap, then runs the normal GAL session over TCP (AaTcpTransport). The
// Bluetooth handshake hands the phone the AP credentials + this TCP endpoint;
// the phone joins the AP and connects, and from there it's the same session as
// wired. All the AA protocol logic is inherited from AaConnection.
class AaWirelessConnection : public AaConnection
{
public:
    AaWirelessConnection();

    void start() override;
    void stop() override;

private:
    AaWifi _wifi;
    AaBluetooth _bluetooth;
};

#endif /* USE_AA_WIRELESS */
#endif /* SRC_PROTOCOL_AA_AA_WIRELESS */
