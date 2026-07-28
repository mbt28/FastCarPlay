// mDNS responder test. Starts the CarPlay _airplay._tcp announcer, then plays a
// browsing client -- sends a PTR query and checks the reply carries our
// instance, port 7000, and the CarPlay TXT records (features/flags/pi/pk) an
// iPhone requires. Self-contained; coexists with a running avahi via REUSEPORT.
//
//   make cp_mdns_test && ../out/cp_mdns_test

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_mdns.h"

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok)
        failures++;
}

static std::string hex(const std::vector<uint8_t> &b)
{
    static const char *h = "0123456789abcdef";
    std::string s;
    for (uint8_t x : b) { s.push_back(h[x >> 4]); s.push_back(h[x & 0xf]); }
    return s;
}

// Does `hay` (raw packet) contain the byte sequence `needle`?
static bool contains(const std::vector<uint8_t> &hay, const std::string &needle)
{
    if (needle.empty() || hay.size() < needle.size())
        return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); i++)
        if (memcmp(hay.data() + i, needle.data(), needle.size()) == 0)
            return true;
    return false;
}

int main()
{
    char tmpl[] = "/tmp/fcp-mdns-XXXXXX";
    cp_identity::setStorageDir(mkdtemp(tmpl));
    const cp_identity::Identity &id = cp_identity::loadOrCreateIdentity();

    cp_mdns::Config cfg;
    cfg.instance = "FastCarPlay";
    cfg.txt = {
        "deviceid=AA:BB:CC:DD:EE:FF",
        "features=0x44540380,0x61", // 0x61 => isCarplay|carPlayControl|Pairing+Encryption
        "flags=0x4",
        "srcvers=550.1",
        "pi=" + id.pairingId,
        "pk=" + hex(id.pubRaw),
    };

    printf("mDNS responder test (instance %s, pi=%s)\n\n", cfg.instance.c_str(), id.pairingId.c_str());

    cp_mdns::MdnsResponder responder;
    check(responder.start(cfg), "responder started");

    // Browsing client: join the group on 5353 so we receive the response.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    struct sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(5353);
    check(bind(fd, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) == 0, "client bound 5353");
    struct ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
    mreq.imr_interface.s_addr = INADDR_ANY;
    setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Build a PTR query for _airplay._tcp.local.
    std::vector<uint8_t> q = {0x12, 0x34, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    for (const char *label : {"_airplay", "_tcp", "local"})
    {
        q.push_back((uint8_t)strlen(label));
        q.insert(q.end(), label, label + strlen(label));
    }
    q.push_back(0);
    q.push_back(0); q.push_back(12); // qtype PTR
    q.push_back(0); q.push_back(1);  // qclass IN

    struct sockaddr_in group{};
    group.sin_family = AF_INET;
    group.sin_addr.s_addr = inet_addr("224.0.0.251");
    group.sin_port = htons(5353);
    sendto(fd, q.data(), q.size(), 0, (struct sockaddr *)&group, sizeof(group));

    // Collect responses for ~2s; find ours (carries the instance + pi/pk).
    bool found = false;
    std::vector<uint8_t> ours;
    for (int i = 0; i < 20 && !found; i++)
    {
        struct pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0)
            continue;
        uint8_t buf[2048];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            continue;
        std::vector<uint8_t> pkt(buf, buf + n);
        if (contains(pkt, "FastCarPlay") && contains(pkt, "pi=" + id.pairingId))
        {
            ours = pkt;
            found = true;
        }
    }
    close(fd);
    responder.stop();

    printf("\n");
    check(found, "browsing client discovered our service");
    check(contains(ours, "features=0x44540380,0x61"), "TXT has CarPlay features");
    check(contains(ours, "flags=0x4"), "TXT has flags");
    check(contains(ours, "pk=" + hex(id.pubRaw)), "TXT has our public key (pk)");
    // SRV rdata contains port 7000 = 0x1B58.
    check(contains(ours, std::string{0x1b, 0x58}), "SRV advertises port 7000");

    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
