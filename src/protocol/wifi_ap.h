#ifndef SRC_PROTOCOL_WIFI_AP
#define SRC_PROTOCOL_WIFI_AP

#if defined(USE_AA_WIRELESS) || defined(USE_CP_WIRELESS)

#include <string>

// Brings up the head unit's Wi-Fi access point (hostapd + dnsmasq) that the
// phone joins for wireless Android Auto or wireless CarPlay. 2.4 GHz to suit
// the ESP32 SoftAP. Config comes from Settings (wifi-ssid / -passphrase /
// -channel / -interface / wifi-ap-ip). Requires hostapd + dnsmasq on the
// system and root privileges.
//
// Both wireless backends hand the phone these same credentials over their
// Bluetooth bootstrap, so the AP has to be the one *we* configured -- relying
// on an externally managed hostapd lets its SSID drift out of sync with
// wifi-ssid, and the phone then fails to find the network it was told to join.
class WifiAp
{
public:
    ~WifiAp();

    bool start();
    void stop();

    // AP address the phone connects to, and the AP's BSSID (the wlan MAC).
    const std::string &ip() const { return _ip; }
    const std::string &bssid() const { return _bssid; }

private:
    bool writeConfigs();

    std::string _ip;
    std::string _bssid;
    bool _running = false;
};

#endif /* USE_AA_WIRELESS || USE_CP_WIRELESS */
#endif /* SRC_PROTOCOL_WIFI_AP */
