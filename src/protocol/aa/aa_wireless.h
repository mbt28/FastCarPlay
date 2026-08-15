#ifndef SRC_PROTOCOL_AA_AA_WIRELESS
#define SRC_PROTOCOL_AA_AA_WIRELESS

#ifdef USE_AA_WIRELESS

#include "protocol/aa/aa_connection.h"
#include "protocol/aa/aa_bluetooth.h"
#include "protocol/wifi_ap.h"

// Wireless Android Auto backend: runs the Bluetooth bootstrap, then the normal
// GAL session over TCP (AaTcpTransport). The Bluetooth handshake hands the phone
// the AP credentials + this TCP endpoint; the phone joins the AP and connects,
// and from there it's the same session as wired. All the AA protocol logic is
// inherited from AaConnection.
//
// The Wi-Fi AP itself belongs to the system and is simply read (wifi_ap::read).
class AaWirelessConnection : public AaConnection
{
public:
    AaWirelessConnection();

    void start() override;
    void stop() override;

private:
    AaBluetooth _bluetooth;
};

#endif /* USE_AA_WIRELESS */
#endif /* SRC_PROTOCOL_AA_AA_WIRELESS */
