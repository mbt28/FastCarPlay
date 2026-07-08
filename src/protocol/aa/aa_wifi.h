#ifndef SRC_PROTOCOL_AA_AA_WIFI
#define SRC_PROTOCOL_AA_AA_WIFI

#ifdef USE_AA_WIRELESS

#include <string>

// Brings up the head unit's Wi-Fi access point (hostapd + dnsmasq) that the
// phone joins for wireless Android Auto. 2.4 GHz to suit the ESP32 SoftAP.
// Config comes from Settings (wifi-ssid / -passphrase / -channel / -interface
// / wifi-ap-ip). Requires hostapd + dnsmasq on the system and root privileges.
class AaWifi
{
public:
    ~AaWifi();

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

#endif /* USE_AA_WIRELESS */
#endif /* SRC_PROTOCOL_AA_AA_WIFI */
