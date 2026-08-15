// The access point is configured by the system; we read it and, from the
// Wireless UI, rewrite the network name and passphrase in place.
//
// This exercises the rewrite, because getting it wrong is expensive: the file
// also carries the driver/mode directives the AP depends on, and hostapd simply
// refuses to start on a malformed one -- leaving a head unit with no network at
// all, which is the same channel used to fix it.
//
//   make wifi_ap_test && ../out/wifi_ap_test

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "protocol/wifi_ap.h"
#include "settings.h"

static int failures = 0;

static void check(bool ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static std::string slurp(const std::string &path)
{
    std::ifstream in(path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void writeConf(const std::string &path, const std::string &body)
{
    std::ofstream out(path, std::ios::trunc);
    out << body;
}

int main()
{
    char tmpl[] = "/tmp/fcp-wifiap-XXXXXX";
    const char *dir = mkdtemp(tmpl);
    if (dir == nullptr)
    {
        perror("mkdtemp");
        return 1;
    }
    const std::string conf = std::string(dir) + "/hostapd.conf";
    Settings::hostapdConf.value = conf;
    Settings::apRestartCmd.value = ""; // no AP to restart in a test

    const std::string original =
        "# SoftAP config -- comment mentioning ssid= should be ignored\n"
        "interface=wlan0\n"
        "driver=nl80211\n"
        "ssid=f1c200s-ap\n"
        "hw_mode=g\n"
        "channel=6\n"
        "ieee80211n=1\n"
        "wpa=2\n"
        "wpa_key_mgmt=WPA-PSK\n"
        "rsn_pairwise=CCMP\n"
        "wpa_passphrase=originalpass\n";
    writeConf(conf, original);

    printf("read the effective config:\n");
    wifi_ap::Params p = wifi_ap::read();
    check(p.ssid == "f1c200s-ap", "ssid parsed");
    check(p.passphrase == "originalpass", "passphrase parsed");
    check(p.channel == 6, "channel parsed");
    check(p.iface == "wlan0", "interface parsed");

    printf("\nrewrite keeps every other directive:\n");
    check(wifi_ap::configure("new-network", "newpassphrase"), "configure() accepted");
    const std::string after = slurp(conf);
    check(after.find("ssid=new-network\n") != std::string::npos, "ssid replaced");
    check(after.find("wpa_passphrase=newpassphrase\n") != std::string::npos, "passphrase replaced");
    check(after.find("ssid=f1c200s-ap") == std::string::npos, "old ssid gone");
    check(after.find("originalpass") == std::string::npos, "old passphrase gone");
    for (const char *keep : {"driver=nl80211", "hw_mode=g", "channel=6", "ieee80211n=1",
                             "wpa=2", "wpa_key_mgmt=WPA-PSK", "rsn_pairwise=CCMP",
                             "interface=wlan0"})
    {
        static char label[80];
        snprintf(label, sizeof(label), "preserved: %s", keep);
        check(after.find(keep) != std::string::npos, label);
    }
    check(after.find("# SoftAP config") != std::string::npos, "comments preserved");

    printf("\nre-read sees the new values:\n");
    p = wifi_ap::read();
    check(p.ssid == "new-network", "ssid round-trips");
    check(p.passphrase == "newpassphrase", "passphrase round-trips");
    check(p.channel == 6, "channel untouched");

    printf("\nvalues hostapd would reject are refused, file untouched:\n");
    const std::string before = slurp(conf);
    check(!wifi_ap::configure("", "newpassphrase"), "empty ssid refused");
    check(!wifi_ap::configure(std::string(33, 'x'), "newpassphrase"), "33-char ssid refused");
    check(!wifi_ap::configure("ok", "short"), "7-char passphrase refused");
    check(!wifi_ap::configure("ok", std::string(64, 'x')), "64-char passphrase refused");
    check(!wifi_ap::configure("ok\nevil=1", "newpassphrase"), "newline injection refused");
    check(slurp(conf) == before, "file unchanged after every rejection");

    printf("\nmissing file is reported, not crashed on:\n");
    Settings::hostapdConf.value = std::string(dir) + "/does-not-exist.conf";
    p = wifi_ap::read();
    check(p.ssid.empty() && !p.valid(), "read() returns an invalid Params");
    check(!wifi_ap::configure("some-ap", "somepassphrase"), "configure() fails cleanly");

    unlink(conf.c_str());
    rmdir(dir);
    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
