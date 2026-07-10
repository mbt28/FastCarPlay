#include "aa_wifi.h"

#ifdef USE_AA_WIRELESS

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "common/logger.h"
#include "settings.h"

#define HOSTAPD_CONF "/tmp/fcp-hostapd.conf"
#define DNSMASQ_CONF "/tmp/fcp-dnsmasq.conf"

static int run(const std::string &cmd)
{
    log_d("wifi: %s", cmd.c_str());
    return system(cmd.c_str());
}

// AP subnet from the AP IP (assumes /24, gateway = the AP IP).
static std::string subnet24(const std::string &ip)
{
    size_t dot = ip.find_last_of('.');
    return dot == std::string::npos ? ip : ip.substr(0, dot);
}

AaWifi::~AaWifi()
{
    stop();
}

bool AaWifi::writeConfigs()
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

bool AaWifi::start()
{
    if (_running)
        return true;

    _ip = Settings::apIp.value;
    const std::string iface = Settings::wifiIface.value;

    // The AP BSSID is the wlan interface MAC.
    {
        std::ifstream mac("/sys/class/net/" + iface + "/address");
        std::getline(mac, _bssid);
    }

    if (!writeConfigs())
    {
        log_e("wifi: can't write hostapd/dnsmasq config");
        return false;
    }

    // Clear any rfkill soft-block, then take the interface from anything that
    // manages it (wpa_supplicant / NetworkManager would fight hostapd).
    run("rfkill unblock wifi 2>/dev/null");
    run("pkill -f 'wpa_supplicant.*" + iface + "' 2>/dev/null");
    run("ip link set " + iface + " down 2>/dev/null");
    run("ip addr flush dev " + iface + " 2>/dev/null");
    run("ip link set " + iface + " up");
    run("ip addr add " + _ip + "/24 dev " + iface);

    // Kill a hostapd left over from a previous run, else the new one fails to
    // grab the interface ("hostapd failed to start").
    run("pkill -f 'hostapd.*" HOSTAPD_CONF "' 2>/dev/null");
    if (run("hostapd -B " + std::string(HOSTAPD_CONF)) != 0)
    {
        log_e("wifi: hostapd failed to start (is it installed?)");
        return false;
    }
    run("pkill -f 'dnsmasq.*" DNSMASQ_CONF "' 2>/dev/null");
    if (run("dnsmasq -C " + std::string(DNSMASQ_CONF)) != 0)
        log_w("wifi: dnsmasq failed to start (phone may not get an IP)");

    _running = true;
    log_i("wifi: AP '%s' up on %s (%s, ch %d)", Settings::wifiSsid.value.c_str(),
          _ip.c_str(), _bssid.c_str(), Settings::wifiChannel.value);
    return true;
}

void AaWifi::stop()
{
    if (!_running)
        return;
    run("pkill -f 'hostapd.*" HOSTAPD_CONF "' 2>/dev/null");
    run("pkill -f 'dnsmasq.*" DNSMASQ_CONF "' 2>/dev/null");
    run("ip addr flush dev " + Settings::wifiIface.value + " 2>/dev/null");
    _running = false;
    log_v("wifi: AP stopped");
}

#endif /* USE_AA_WIRELESS */
