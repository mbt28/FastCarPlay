#include "wifi_ap.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/logger.h"
#include "settings.h"

namespace wifi_ap
{
namespace
{
// WPA2 limits; hostapd refuses to start outside them, and a rejected config
// would leave the board with no AP at all.
constexpr int SSID_MIN = 1, SSID_MAX = 32;
constexpr int PASS_MIN = 8, PASS_MAX = 63;

std::string trim(std::string s)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// key=value, ignoring comments. hostapd.conf has no quoting or continuations,
// so this is the whole format.
bool configValue(const std::string &line, const char *key, std::string &out)
{
    if (line.empty() || line[0] == '#')
        return false;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;
    if (trim(line.substr(0, eq)) != key)
        return false;
    out = trim(line.substr(eq + 1));
    return true;
}

std::string ifaceIpv4(const std::string &iface)
{
    struct ifaddrs *ifas = nullptr;
    std::string out;
    if (getifaddrs(&ifas) != 0)
        return out;
    for (struct ifaddrs *a = ifas; a != nullptr; a = a->ifa_next)
    {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name)
            continue;
        char buf[INET_ADDRSTRLEN];
        auto *s4 = (struct sockaddr_in *)a->ifa_addr;
        if (inet_ntop(AF_INET, &s4->sin_addr, buf, sizeof(buf)) != nullptr)
            out = buf;
        break;
    }
    freeifaddrs(ifas);
    return out;
}

std::string ifaceMac(const std::string &iface)
{
    std::ifstream in("/sys/class/net/" + iface + "/address");
    std::string mac;
    std::getline(in, mac);
    return mac;
}
} // namespace

Params read()
{
    Params p;
    p.iface = Settings::wifiIface.value; // fallback if the file omits it

    const std::string path = Settings::hostapdConf.value;
    std::ifstream in(path);
    if (!in.is_open())
    {
        log_w("wifi: cannot read %s -- no system AP configured?", path.c_str());
        return p;
    }

    std::string line, value;
    while (std::getline(in, line))
    {
        if (configValue(line, "interface", value))
            p.iface = value;
        else if (configValue(line, "ssid", value))
            p.ssid = value;
        else if (configValue(line, "wpa_passphrase", value))
            p.passphrase = value;
        else if (configValue(line, "channel", value))
            p.channel = std::atoi(value.c_str());
    }

    p.ip = ifaceIpv4(p.iface);
    p.bssid = ifaceMac(p.iface);
    return p;
}

bool configure(const std::string &ssid, const std::string &passphrase)
{
    if ((int)ssid.size() < SSID_MIN || (int)ssid.size() > SSID_MAX)
    {
        log_e("wifi: SSID must be %d-%d characters", SSID_MIN, SSID_MAX);
        return false;
    }
    if ((int)passphrase.size() < PASS_MIN || (int)passphrase.size() > PASS_MAX)
    {
        log_e("wifi: passphrase must be %d-%d characters", PASS_MIN, PASS_MAX);
        return false;
    }
    // A newline would inject an arbitrary hostapd directive.
    if (ssid.find_first_of("\r\n") != std::string::npos ||
        passphrase.find_first_of("\r\n") != std::string::npos)
    {
        log_e("wifi: SSID/passphrase cannot contain newlines");
        return false;
    }

    const std::string path = Settings::hostapdConf.value;
    std::ifstream in(path);
    if (!in.is_open())
    {
        log_e("wifi: cannot read %s", path.c_str());
        return false;
    }

    // Rewrite in place, preserving every other directive and the comments: this
    // file is hand-maintained on the image and carries the driver settings the
    // AP depends on.
    std::ostringstream out;
    std::string line, value;
    bool wroteSsid = false, wrotePass = false;
    while (std::getline(in, line))
    {
        if (configValue(line, "ssid", value))
        {
            out << "ssid=" << ssid << "\n";
            wroteSsid = true;
        }
        else if (configValue(line, "wpa_passphrase", value))
        {
            out << "wpa_passphrase=" << passphrase << "\n";
            wrotePass = true;
        }
        else
        {
            out << line << "\n";
        }
    }
    in.close();
    if (!wroteSsid)
        out << "ssid=" << ssid << "\n";
    if (!wrotePass)
        out << "wpa_passphrase=" << passphrase << "\n";

    const std::string body = out.str();
    const std::string tmp = path + ".tmp";
    // 0600 + tmp/rename/fsync: it holds the passphrase, and a torn write would
    // leave the board with an AP that will not start.
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
    {
        log_e("wifi: cannot write %s > %s", tmp.c_str(), strerror(errno));
        return false;
    }
    const char *p = body.data();
    std::size_t left = body.size();
    while (left > 0)
    {
        const ssize_t n = write(fd, p, left);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            log_e("wifi: write failed %s > %s", tmp.c_str(), strerror(errno));
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        p += n;
        left -= (std::size_t)n;
    }
    if (fsync(fd) != 0)
    {
        log_e("wifi: cannot flush %s > %s", tmp.c_str(), strerror(errno));
        close(fd);
        unlink(tmp.c_str());
        return false;
    }
    close(fd);
    if (rename(tmp.c_str(), path.c_str()) != 0)
    {
        log_e("wifi: cannot replace %s > %s", path.c_str(), strerror(errno));
        unlink(tmp.c_str());
        return false;
    }

    // Restart through the init script rather than `ap off; ap on`: `ap off`
    // unloads the ESP transport module when the STA flag is absent (the normal
    // case), which takes the interface away entirely instead of just the AP.
    const std::string restart = Settings::apRestartCmd.value;
    if (restart.empty())
    {
        log_i("wifi: AP config updated to '%s' (restart it to apply)", ssid.c_str());
        return true;
    }
    log_i("wifi: AP -> '%s', restarting via %s", ssid.c_str(), restart.c_str());
    if (system(restart.c_str()) != 0)
        log_w("wifi: AP restart command failed -- the new SSID applies at next boot");
    return true;
}
} // namespace wifi_ap
