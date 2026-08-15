#ifndef SRC_PROTOCOL_WIFI_AP
#define SRC_PROTOCOL_WIFI_AP

#include <string>

// The head unit's Wi-Fi access point, as configured by the SYSTEM -- we read it,
// we do not run it.
//
// The AP is started by init (hostapd + dnsmasq from /etc/hostapd.conf) and stays
// up whether or not this app is running, which is what makes it usable for
// debugging and deployment. We used to start our own hostapd instead, killing
// whatever was already running; that took the board off the network the moment a
// wireless protocol was selected, and swapped the SSID and subnet out from under
// anyone connected to it.
//
// The reason the app owned it before was drift: both wireless backends hand the
// phone these credentials over their Bluetooth bootstrap, so telling the phone
// Settings::wifiSsid while something else configured the radio meant the phone
// looked for a network that did not exist. That is solved here by reading the
// EFFECTIVE config rather than by owning the process -- what we tell the phone
// comes out of the same file hostapd was started from, so the two cannot
// disagree.
namespace wifi_ap
{
struct Params
{
    std::string iface;      // interface hostapd is bound to
    std::string ssid;       // what to tell the phone to join
    std::string passphrase; // WPA2 PSK
    int channel = 0;
    std::string ip;    // our IPv4 on that interface (the phone's endpoint)
    std::string bssid; // the interface MAC

    // The AP looks usable: we found a network name and the interface is up with
    // an address. Not proof that hostapd is beaconing, but it catches "the AP
    // was never started", which is the case worth reporting.
    bool valid() const { return !ssid.empty() && !ip.empty(); }
};

// Read the effective AP configuration. Cheap; call it when you need it rather
// than caching, so a change made through the UI is picked up.
Params read();

// Change the advertised network. Rewrites ssid/passphrase in the hostapd config
// (atomically) and restarts the AP so the change takes effect. Returns false and
// leaves the config untouched if the values are outside what WPA2 accepts or the
// file cannot be written.
bool configure(const std::string &ssid, const std::string &passphrase);
} // namespace wifi_ap

#endif /* SRC_PROTOCOL_WIFI_AP */
