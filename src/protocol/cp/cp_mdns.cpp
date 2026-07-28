#include "cp_mdns.h"

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "common/logger.h"

namespace cp_mdns
{
namespace
{
constexpr const char *MDNS_GROUP = "224.0.0.251";
constexpr uint16_t MDNS_PORT = 5353;

// DNS types.
constexpr uint16_t TYPE_A = 1;
constexpr uint16_t TYPE_PTR = 12;
constexpr uint16_t TYPE_TXT = 16;
constexpr uint16_t TYPE_SRV = 33;
constexpr uint16_t CLASS_IN = 1;
constexpr uint16_t FLUSH = 0x8000; // cache-flush bit on unique records

void put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(v >> 8); b.push_back(v & 0xff); }
void put32(std::vector<uint8_t> &b, uint32_t v)
{
    b.push_back(v >> 24); b.push_back(v >> 16); b.push_back(v >> 8); b.push_back(v & 0xff);
}

// Encode a dotted name as length-prefixed labels + a zero terminator. No
// compression (valid, and simplest).
void putName(std::vector<uint8_t> &b, const std::string &name)
{
    size_t start = 0;
    while (start < name.size())
    {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos)
            dot = name.size();
        const size_t len = dot - start;
        b.push_back((uint8_t)len);
        b.insert(b.end(), name.begin() + start, name.begin() + dot);
        start = dot + 1;
    }
    b.push_back(0);
}

// One resource record header, then rdata is appended by the caller via a
// length back-patch.
void putRecord(std::vector<uint8_t> &b, const std::string &name, uint16_t type,
               uint16_t klass, uint32_t ttl, const std::vector<uint8_t> &rdata)
{
    putName(b, name);
    put16(b, type);
    put16(b, klass);
    put32(b, ttl);
    put16(b, (uint16_t)rdata.size());
    b.insert(b.end(), rdata.begin(), rdata.end());
}

// Read a DNS name from a packet at `off`, following compression pointers.
// Returns the lower-cased dotted name; advances `off` past the name (in the
// non-pointer case) via `consumed`.
std::string readName(const uint8_t *pkt, size_t len, size_t off, size_t &consumed)
{
    std::string name;
    size_t p = off;
    bool jumped = false;
    consumed = 0;
    for (int guard = 0; guard < 128 && p < len; guard++)
    {
        const uint8_t l = pkt[p];
        if (l == 0)
        {
            if (!jumped) consumed = p + 1 - off;
            break;
        }
        if ((l & 0xc0) == 0xc0)
        {
            if (p + 1 >= len) break;
            if (!jumped) consumed = p + 2 - off;
            p = ((l & 0x3f) << 8) | pkt[p + 1];
            jumped = true;
            continue;
        }
        if (p + 1 + l > len) break;
        if (!name.empty()) name.push_back('.');
        for (size_t i = 0; i < l; i++)
            name.push_back((char)tolower(pkt[p + 1 + i]));
        p += 1 + l;
    }
    return name;
}
} // namespace

MdnsResponder::~MdnsResponder() { stop(); }

uint32_t MdnsResponder::primaryIPv4()
{
    struct ifaddrs *ifa = nullptr;
    if (getifaddrs(&ifa) != 0)
        return 0;
    uint32_t addr = 0;
    for (struct ifaddrs *i = ifa; i; i = i->ifa_next)
    {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
            continue;
        if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP))
            continue;
        addr = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
        break;
    }
    freeifaddrs(ifa);
    return addr;
}

std::vector<uint8_t> MdnsResponder::buildAnswer(uint16_t queryId) const
{
    const std::string type = _config.serviceType + ".local";        // _airplay._tcp.local
    const std::string full = _config.instance + "." + type;         // FastCarPlay._airplay._tcp.local

    std::vector<uint8_t> pkt;
    put16(pkt, queryId);       // id (0 for unsolicited)
    put16(pkt, 0x8400);        // flags: response + authoritative
    put16(pkt, 0);             // qdcount
    put16(pkt, 4);             // ancount: PTR + SRV + TXT + A
    put16(pkt, 0);             // nscount
    put16(pkt, 0);             // arcount

    // PTR: service type -> instance
    {
        std::vector<uint8_t> rd;
        putName(rd, full);
        putRecord(pkt, type, TYPE_PTR, CLASS_IN, 4500, rd);
    }
    // SRV: instance -> host:port
    {
        std::vector<uint8_t> rd;
        put16(rd, 0); // priority
        put16(rd, 0); // weight
        put16(rd, _config.port);
        putName(rd, _config.hostname);
        putRecord(pkt, full, TYPE_SRV, CLASS_IN | FLUSH, 120, rd);
    }
    // TXT: instance -> key=value records
    {
        std::vector<uint8_t> rd;
        if (_config.txt.empty())
            rd.push_back(0); // an empty TXT is a single zero-length string
        for (const std::string &s : _config.txt)
        {
            rd.push_back((uint8_t)s.size());
            rd.insert(rd.end(), s.begin(), s.end());
        }
        putRecord(pkt, full, TYPE_TXT, CLASS_IN | FLUSH, 4500, rd);
    }
    // A: host -> IPv4
    {
        std::vector<uint8_t> rd;
        put32(rd, ntohl(_config.ipv4));
        putRecord(pkt, _config.hostname, TYPE_A, CLASS_IN | FLUSH, 120, rd);
    }
    return pkt;
}

bool MdnsResponder::start(const Config &config)
{
    _config = config;
    if (_config.ipv4 == 0)
        _config.ipv4 = primaryIPv4();

    _fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (_fd < 0)
        return false;

    int yes = 1;
    setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(_fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes)); // coexist with avahi
#endif

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(MDNS_PORT);
    if (bind(_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        log_e("mdns: bind 5353 failed");
        close(_fd);
        _fd = -1;
        return false;
    }

    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_GROUP);
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
    unsigned char ttl = 255;
    setsockopt(_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    _active = true;
    _thread = std::thread(&MdnsResponder::loop, this);
    log_i("mdns: announcing %s.%s on :%d", _config.instance.c_str(),
          _config.serviceType.c_str(), _config.port);
    return true;
}

void MdnsResponder::loop()
{
    struct sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_addr.s_addr = inet_addr(MDNS_GROUP);
    group.sin_port = htons(MDNS_PORT);

    // Announce a few times on start (unsolicited).
    std::vector<uint8_t> announce = buildAnswer(0);
    for (int i = 0; i < 3 && _active; i++)
    {
        sendto(_fd, announce.data(), announce.size(), 0, (struct sockaddr *)&group, sizeof(group));
        struct timespec ts{0, 250 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }

    const std::string wantType = _config.serviceType + ".local";
    const std::string wantInstance = _config.instance + "." + wantType;
    uint8_t buf[2048];

    while (_active)
    {
        struct pollfd pfd{_fd, POLLIN, 0};
        if (poll(&pfd, 1, 500) <= 0)
            continue;

        struct sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n = recvfrom(_fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromLen);
        if (n < 12)
            continue;

        const uint16_t flags = (buf[2] << 8) | buf[3];
        if (flags & 0x8000)
            continue; // a response, not a query
        const uint16_t qd = (buf[4] << 8) | buf[5];

        // Does any question ask for our service or instance?
        bool forUs = false;
        size_t off = 12;
        for (uint16_t q = 0; q < qd && off < (size_t)n; q++)
        {
            size_t consumed = 0;
            std::string name = readName(buf, n, off, consumed);
            off += consumed + 4; // name + qtype + qclass
            if (name == wantType || name == wantInstance || name == _config.hostname)
            {
                forUs = true;
                break;
            }
        }
        if (!forUs)
            continue;

        const uint16_t id = (buf[0] << 8) | buf[1];
        std::vector<uint8_t> answer = buildAnswer(id);
        sendto(_fd, answer.data(), answer.size(), 0, (struct sockaddr *)&group, sizeof(group));
    }
}

void MdnsResponder::stop()
{
    if (!_active && _fd < 0)
        return;
    _active = false;
    if (_thread.joinable())
        _thread.join();
    if (_fd >= 0)
    {
        close(_fd);
        _fd = -1;
    }
}
} // namespace cp_mdns
