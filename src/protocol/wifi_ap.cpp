#include "wifi_ap.h"

#if defined(USE_AA_WIRELESS) || defined(USE_CP_WIRELESS)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

#include "common/logger.h"
#include "settings.h"

#define HOSTAPD_CONF "/tmp/fcp-hostapd.conf"
#define DNSMASQ_CONF "/tmp/fcp-dnsmasq.conf"

static int run(const std::string &cmd)
{
    log_d("wifi: %s", cmd.c_str());
    return system(cmd.c_str());
}

// Kill every instance of a program by name. busybox (the F1C200s userland) has
// no pkill/pgrep, so match via pidof + kill instead of a cmdline pattern.
static void killByName(const char *name)
{
    run(std::string("kill $(pidof ") + name + ") 2>/dev/null");
}

// AP subnet from the AP IP (assumes /24, gateway = the AP IP).
static std::string subnet24(const std::string &ip)
{
    size_t dot = ip.find_last_of('.');
    return dot == std::string::npos ? ip : ip.substr(0, dot);
}

WifiAp::~WifiAp()
{
    stop();
}

bool WifiAp::writeConfigs()
{
    const std::string iface = Settings::wifiIface.value;

    std::ofstream hostapd(HOSTAPD_CONF);
    if (!hostapd)
        return false;
    hostapd << "interface=" << iface << "\n"
            << "driver=nl80211\n"
            << "ssid=" << Settings::wifiSsid.value << "\n"
            << "hw_mode=g\n"                    // 2.4 GHz
            << "channel=" << Settings::wifiChannel << "\n"
            << "ieee80211n=1\n"
            << "wmm_enabled=1\n"                // QoS: Android Auto expects it
            << "auth_algs=1\n"
            << "wpa=2\n"
            << "wpa_key_mgmt=WPA-PSK\n"
            << "rsn_pairwise=CCMP\n"
            << "wpa_passphrase=" << Settings::wifiPass.value << "\n";
    hostapd.close();

    const std::string net = subnet24(_ip);
    std::ofstream dnsmasq(DNSMASQ_CONF);
    if (!dnsmasq)
        return false;
    dnsmasq << "interface=" << iface << "\n"
            << "bind-interfaces\n"
            << "dhcp-range=" << net << ".10," << net << ".100,255.255.255.0,12h\n"
            << "dhcp-option=3," << _ip << "\n" // gateway = the head unit
            << "no-resolv\n";
    dnsmasq.close();
    return true;
}

bool WifiAp::start()
{
    if (_running)
        return true;

    _ip = Settings::apIp.value;
    const std::string iface = Settings::wifiIface.value;

    if (!writeConfigs())
    {
        log_e("wifi: can't write hostapd/dnsmasq config");
        return false;
    }

    // Clear any rfkill soft-block, then take the interface from anything that
    // manages it. Kill leftovers from a previous run FIRST — a stale hostapd
    // keeps the new one from grabbing wlan0 ("hostapd failed to start") — then
    // give the driver a moment to release the interface.
    run("rfkill unblock wifi 2>/dev/null");
    killByName("hostapd");
    killByName("dnsmasq");
    killByName("wpa_supplicant");
    usleep(300 * 1000);
    // NOTE: deliberately no "ip link set <iface> down" here. On the esp-hosted
    // driver that runs ndo_stop, which deinitialises the interface on the ESP32
    // side ("esp_stop: Deinitializing interface wlan0"), and nothing brings the
    // firmware back: bringing the link up and switching mode afterwards leaves
    // cfg80211 and hostapd both reporting an ENABLED AP that never beacons, so
    // the phone silently fails to associate. Verified on the F1C200s.
    //
    // It is not needed either -- hostapd switches the interface to AP itself on
    // start, and back to managed when it exits cleanly. Reset the type only to
    // recover from an ungracefully-killed hostapd that left it in AP mode; that
    // fails harmlessly (interface busy) when there is nothing to recover.
    run("iw dev " + iface + " set type managed 2>/dev/null");
    // IPv4 only: a plain "addr flush" would also drop the IPv6 link-local, and
    // since we never take the link down there is no down/up transition left to
    // regenerate it. Wireless CarPlay hands the phone that fe80 address as the
    // endpoint to connect back to, so losing it breaks the handoff.
    run("ip -4 addr flush dev " + iface + " 2>/dev/null");
    run("ip link set " + iface + " up");
    run("ip addr add " + _ip + "/24 dev " + iface);

    if (run("hostapd -B " + std::string(HOSTAPD_CONF)) != 0)
    {
        log_e("wifi: hostapd failed to start (is it installed?)");
        return false;
    }
    if (run("dnsmasq -C " + std::string(DNSMASQ_CONF)) != 0)
        log_w("wifi: dnsmasq failed to start (phone may not get an IP)");

    // The BSSID is the wlan MAC, read once the interface is up rather than on
    // entry: on the esp-hosted driver wlan0 only appears after the transport
    // comes up, and an early read would silently leave the BSSID empty.
    {
        std::ifstream mac("/sys/class/net/" + iface + "/address");
        std::getline(mac, _bssid);
    }

    _running = true;
    log_i("wifi: AP '%s' up on %s (%s, ch %d)", Settings::wifiSsid.value.c_str(),
          _ip.c_str(), _bssid.c_str(), Settings::wifiChannel.value);
    return true;
}

void WifiAp::stop()
{
    if (!_running)
        return;
    killByName("hostapd");
    killByName("dnsmasq");
    run("ip addr flush dev " + Settings::wifiIface.value + " 2>/dev/null");
    _running = false;
    log_v("wifi: AP stopped");
}

#endif /* USE_AA_WIRELESS || USE_CP_WIRELESS */
