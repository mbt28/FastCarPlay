#ifndef SRC_PROTOCOL_CP_CP_MDNS
#define SRC_PROTOCOL_CP_CP_MDNS

// A minimal mDNS responder that announces exactly one service -- the CarPlay
// head unit's `_airplay._tcp` on :7000 -- so an iPhone discovers it. Announces
// on start and answers PTR/SRV/TXT/A queries for the service. A few hundred
// lines instead of Avahi (no daemon, no D-Bus), which is what the F1C200s
// needs. Not a general browser/resolver.

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace cp_mdns
{
struct Config
{
    std::string instance = "FastCarPlay";       // service instance name
    std::string serviceType = "_airplay._tcp";  // CarPlay = AirPlay-2 discovery
    std::string hostname = "fastcarplay.local"; // A-record name
    uint16_t port = 7000;
    uint32_t ipv4 = 0;                    // network byte order; 0 = auto-detect
    std::vector<std::string> txt;         // "key=value" records (deviceid, features, pi, pk...)
};

class MdnsResponder
{
public:
    ~MdnsResponder();
    bool start(const Config &config);
    void stop();

    // Auto-detect the first non-loopback IPv4 (network byte order); 0 if none.
    static uint32_t primaryIPv4();

private:
    void loop();
    std::vector<uint8_t> buildAnswer(uint16_t queryId) const;

    Config _config;
    int _fd = -1;
    std::atomic<bool> _active{false};
    std::thread _thread;
};
} // namespace cp_mdns

#endif /* SRC_PROTOCOL_CP_CP_MDNS */
