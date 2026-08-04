#include "cp_usbmux.h"

#ifdef USE_CP_WIRED

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <plist/plist.h>

#include "common/logger.h"

namespace cp_usbmux
{
namespace
{
constexpr uint32_t MUX_MAGIC = 0xFEEDFACE;
constexpr uint32_t P_VERSION = 0, P_SETUP = 2, P_TCP = 6;
constexpr uint8_t TH_FIN = 0x01, TH_SYN = 0x02, TH_RST = 0x04, TH_ACK = 0x10;
constexpr uint8_t EP_OUT = 0x04, EP_IN = 0x85;
constexpr uint32_t TX_WIN = 131072, MAX_PAYLOAD = 16384;

const char *APPLE_VID = "05ac";
const char *SYS_USB = "/sys/bus/usb/devices";
const char *LOCKDOWN_STORE = "/var/lib/lockdown";

using Bytes = std::vector<uint8_t>;

std::string readSys(const std::string &path)
{
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

// Find the Apple device with this serial; fills bus/dev/sysdir. Any serial if empty.
bool findDev(const std::string &serial, int &bus, int &dev, std::string &sysdir)
{
    DIR *d = opendir(SYS_USB);
    if (!d)
        return false;
    bool found = false;
    for (struct dirent *e; (e = readdir(d));)
    {
        std::string base = std::string(SYS_USB) + "/" + e->d_name + "/";
        if (readSys(base + "idVendor") != APPLE_VID)
            continue;
        std::string s = readSys(base + "serial");
        if (s.empty() || (!serial.empty() && s != serial))
            continue;
        sysdir = base;
        bus = atoi(readSys(base + "busnum").c_str());
        dev = atoi(readSys(base + "devnum").c_str());
        found = true;
        break;
    }
    closedir(d);
    return found;
}

int numConfigs(const std::string &sysdir)
{
    std::string s = readSys(sysdir + "bNumConfigurations");
    return s.empty() ? 0 : atoi(s.c_str());
}

// ── usbfs I/O ────────────────────────────────────────────────────────────
bool usbControl(int fd, uint8_t brt, uint8_t br, uint16_t wv, uint16_t wi, uint16_t wl, uint8_t *out)
{
    struct usbdevfs_ctrltransfer ct;
    memset(&ct, 0, sizeof(ct));
    ct.bRequestType = brt;
    ct.bRequest = br;
    ct.wValue = wv;
    ct.wIndex = wi;
    ct.wLength = wl;
    ct.timeout = 3000;
    ct.data = out;
    return ioctl(fd, USBDEVFS_CONTROL, &ct) >= 0;
}

bool usbClaim(int fd, unsigned iface)
{
    return ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) == 0;
}

// Returns bytes transferred (>=0) or -1 (errno set). ep bit7 = IN.
int usbBulk(int fd, uint8_t ep, void *data, unsigned len, unsigned timeout)
{
    struct usbdevfs_bulktransfer bt;
    memset(&bt, 0, sizeof(bt));
    bt.ep = ep;
    bt.len = len;
    bt.timeout = timeout;
    bt.data = data;
    return ioctl(fd, USBDEVFS_BULK, &bt);
}

void put32be(Bytes &b, uint32_t v)
{
    b.push_back(v >> 24);
    b.push_back(v >> 16);
    b.push_back(v >> 8);
    b.push_back(v);
}
void put16be(Bytes &b, uint16_t v)
{
    b.push_back(v >> 8);
    b.push_back(v);
}
uint32_t get32be(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t get16be(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// ── lockdown store ─────────────────────────────────────────────────────────
std::string readFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string readBuid()
{
    std::string data = readFile(std::string(LOCKDOWN_STORE) + "/SystemConfiguration.plist");
    if (!data.empty())
    {
        plist_t root = nullptr;
        plist_from_memory(data.data(), (uint32_t)data.size(), &root, nullptr);
        if (root)
        {
            plist_t b = plist_dict_get_item(root, "SystemBUID");
            char *s = nullptr;
            if (b)
                plist_get_string_val(b, &s);
            std::string out = s ? s : "";
            free(s);
            plist_free(root);
            if (!out.empty())
                return out;
        }
    }
    std::string u = readSys("/proc/sys/kernel/random/uuid");
    for (auto &c : u)
        c = (char)toupper((unsigned char)c);
    return u;
}

// Pair record raw bytes: try udid / upper / lower .plist.
std::string readPairRecord(const std::string &udid)
{
    auto up = udid, lo = udid;
    for (auto &c : up)
        c = (char)toupper((unsigned char)c);
    for (auto &c : lo)
        c = (char)tolower((unsigned char)c);
    for (const std::string &name : {udid, up, lo})
    {
        std::string d = readFile(std::string(LOCKDOWN_STORE) + "/" + name + ".plist");
        if (!d.empty())
            return d;
    }
    return "";
}

void savePairRecord(const std::string &udid, const std::string &data)
{
    std::ofstream f(std::string(LOCKDOWN_STORE) + "/" + udid + ".plist", std::ios::binary);
    f.write(data.data(), (std::streamsize)data.size());
}
} // namespace

// ── A TCP-over-mux connection ──────────────────────────────────────────────
class MuxHost;
class MuxConn
{
public:
    MuxConn(MuxHost *host, uint16_t sport, uint16_t dport) : _host(host), _sport(sport), _dport(dport) {}

    uint16_t sport() const { return _sport; }
    void tcp(uint8_t flags, const uint8_t *payload, size_t len);

    // Called by the reader thread when a mux TCP packet arrives for this conn.
    void onPacket(uint8_t flags, uint32_t seq, uint32_t /*ack*/, uint16_t win, const uint8_t *payload,
                  size_t len);

    void send(const uint8_t *data, size_t len)
    {
        size_t i = 0;
        while (i < len)
        {
            size_t chunk = std::min<size_t>(MAX_PAYLOAD, len - i);
            tcp(TH_ACK, data + i, chunk);
            {
                std::lock_guard<std::mutex> lk(_m);
                _txSeq += (uint32_t)chunk;
            }
            i += chunk;
        }
    }

    // Blocks for the next inbound chunk; empty result = closed.
    Bytes recv()
    {
        std::unique_lock<std::mutex> lk(_m);
        _cv.wait(lk, [&] { return !_rq.empty() || _closed; });
        if (!_rq.empty())
        {
            Bytes b = std::move(_rq.front());
            _rq.pop_front();
            return b;
        }
        return {};
    }

    bool waitConnected(int ms)
    {
        std::unique_lock<std::mutex> lk(_m);
        _cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return _connected || _closed; });
        return _connected && !_closed;
    }

    bool closed() const { return _closed; }
    void close()
    {
        if (!_closed)
            tcp(TH_FIN | TH_ACK, nullptr, 0);
    }
    void markClosed()
    {
        std::lock_guard<std::mutex> lk(_m);
        _closed = true;
        _cv.notify_all();
    }

private:
    MuxHost *_host;
    uint16_t _sport, _dport;
    uint32_t _txSeq = 0, _txAck = 0;
    bool _connected = false, _closed = false;
    std::deque<Bytes> _rq;
    std::mutex _m;
    std::condition_variable _cv;
};

// ── The mux host: usbmux framing over the bulk endpoints ───────────────────
class MuxHost
{
public:
    ~MuxHost() { close(); }

    bool open(const std::string &serial)
    {
        _serial = serial;
        int bus = 0, dev = 0;
        std::string sysdir;
        if (!findDev(serial, bus, dev, sysdir))
            return false;
        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", bus, dev);
        _fd = ::open(node, O_RDWR);
        if (_fd < 0)
        {
            log_e("cp-usbmux: open %s: %s (root?)", node, strerror(errno));
            return false;
        }
        // Claim the usbmux interface (1). The kernel may briefly hold it right
        // after the config switch, so retry.
        bool claimed = false;
        for (int i = 0; i < 12 && !claimed; i++)
        {
            claimed = usbClaim(_fd, 1);
            if (!claimed)
                usleep(300000);
        }
        if (!claimed)
        {
            log_e("cp-usbmux: claim iface 1 failed: %s", strerror(errno));
            return false;
        }
        // usbmux version handshake: [proto=0][len=20][ver=2][0][0] (no magic).
        Bytes ver;
        put32be(ver, P_VERSION);
        put32be(ver, 20);
        put32be(ver, 2);
        put32be(ver, 0);
        put32be(ver, 0);
        if (usbBulk(_fd, EP_OUT, ver.data(), (unsigned)ver.size(), 2000) < 0)
        {
            log_e("cp-usbmux: version write failed: %s", strerror(errno));
            return false;
        }
        uint8_t tmp[4096];
        usbBulk(_fd, EP_IN, tmp, sizeof(tmp), 2000); // read + discard the version reply
        _run = true;
        _reader = std::thread([this] { readerLoop(); });
        const uint8_t setup = 0x07;
        muxSend(P_SETUP, &setup, 1);
        return true;
    }

    void close()
    {
        _run = false;
        // The reader blocks in usbBulk(EP_IN) with a 1s timeout, so it exits
        // within a second of _run going false.
        if (_reader.joinable())
            _reader.join();
        {
            std::lock_guard<std::mutex> lk(_connMutex);
            for (auto &kv : _conns)
                kv.second->markClosed();
            _conns.clear();
        }
        if (_fd >= 0)
        {
            unsigned iface = 1;
            ioctl(_fd, USBDEVFS_RELEASEINTERFACE, &iface);
            ::close(_fd);
            _fd = -1;
        }
    }

    // Frame + send a mux packet: [proto][16+len][magic][tx][rx][payload].
    void muxSend(uint32_t proto, const uint8_t *payload, size_t len)
    {
        std::lock_guard<std::mutex> lk(_wlock);
        if (_fd < 0)
            return;
        Bytes pkt;
        put32be(pkt, proto);
        put32be(pkt, (uint32_t)(16 + len));
        put32be(pkt, MUX_MAGIC);
        put16be(pkt, (uint16_t)_muxTx++);
        put16be(pkt, (uint16_t)_muxRx);
        if (payload && len)
            pkt.insert(pkt.end(), payload, payload + len);
        usbBulk(_fd, EP_OUT, pkt.data(), (unsigned)pkt.size(), 2000);
    }

    // Open a mux TCP connection to a device port (lockdown 62078, carkit port…).
    std::shared_ptr<MuxConn> connect(uint16_t dport)
    {
        uint16_t sport;
        {
            std::lock_guard<std::mutex> lk(_sportLock);
            sport = _nextSport++;
        }
        auto conn = std::make_shared<MuxConn>(this, sport, dport);
        {
            std::lock_guard<std::mutex> lk(_connMutex);
            _conns[sport] = conn;
        }
        conn->tcp(TH_SYN, nullptr, 0);
        if (!conn->waitConnected(5000))
        {
            std::lock_guard<std::mutex> lk(_connMutex);
            _conns.erase(sport);
            return nullptr;
        }
        return conn;
    }

    void dropConn(uint16_t sport)
    {
        std::lock_guard<std::mutex> lk(_connMutex);
        _conns.erase(sport);
    }

private:
    void readerLoop()
    {
        std::vector<uint8_t> buf(16384); // usbfs caps a single bulk transfer at MAX_USBFS_BUFFER_SIZE (16K) on some HCDs (MUSB)
        Bytes rx;
        while (_run)
        {
            int n = usbBulk(_fd, EP_IN, buf.data(), (unsigned)buf.size(), 1000);
            if (n < 0)
            {
                if (errno == ETIMEDOUT)
                    continue;
                if (_run)
                    log_w("cp-usbmux: usb reader ended: %s", strerror(errno));
                break;
            }
            if (n == 0)
                continue;
            rx.insert(rx.end(), buf.data(), buf.data() + n);
            size_t off = 0;
            while (rx.size() - off >= 8)
            {
                uint32_t proto = get32be(&rx[off]);
                uint32_t len = get32be(&rx[off + 4]);
                if (len < 8 || rx.size() - off < len)
                    break;
                const uint8_t *pkt = &rx[off];
                if (len >= 16)
                    _muxRx = get16be(pkt + 12);
                if (proto == P_TCP && len >= 36)
                {
                    uint16_t dp = get16be(pkt + 18); // TCP dport (= our sport)
                    uint32_t seq = get32be(pkt + 20);
                    uint32_t ack = get32be(pkt + 24);
                    uint8_t flags = pkt[29];
                    uint16_t win = get16be(pkt + 30);
                    std::shared_ptr<MuxConn> conn;
                    {
                        std::lock_guard<std::mutex> lk(_connMutex);
                        auto it = _conns.find(dp);
                        if (it != _conns.end())
                            conn = it->second;
                    }
                    if (conn)
                        conn->onPacket(flags, seq, ack, win, pkt + 36, len - 36);
                }
                off += len;
            }
            if (off > 0)
                rx.erase(rx.begin(), rx.begin() + off);
        }
    }

    std::string _serial;
    int _fd = -1;
    std::atomic<bool> _run{false};
    std::thread _reader;
    std::mutex _wlock;
    uint32_t _muxTx = 0, _muxRx = 0;
    std::map<uint16_t, std::shared_ptr<MuxConn>> _conns;
    std::mutex _connMutex;
    uint16_t _nextSport = 1;
    std::mutex _sportLock;
};

// Snapshots seq/ack under _m (released before the USB write), so it can be
// called from both the reader thread (onPacket) and the relay thread (send)
// without holding _m across muxSend or nesting locks.
void MuxConn::tcp(uint8_t flags, const uint8_t *payload, size_t len)
{
    uint32_t seq, ack;
    {
        std::lock_guard<std::mutex> lk(_m);
        seq = _txSeq;
        ack = _txAck;
    }
    Bytes th;
    put16be(th, _sport);
    put16be(th, _dport);
    put32be(th, seq);
    put32be(th, ack);
    th.push_back(0x50); // data offset
    th.push_back(flags);
    put16be(th, (uint16_t)(TX_WIN >> 8));
    put16be(th, 0);
    put16be(th, 0);
    if (payload && len)
        th.insert(th.end(), payload, payload + len);
    _host->muxSend(P_TCP, th.data(), th.size());
}

void MuxConn::onPacket(uint8_t flags, uint32_t seq, uint32_t, uint16_t, const uint8_t *payload, size_t len)
{
    if ((flags & TH_SYN) && (flags & TH_ACK))
    {
        {
            std::lock_guard<std::mutex> lk(_m);
            _txSeq += 1;
            _txAck = seq + 1;
            _connected = true;
        }
        tcp(TH_ACK, nullptr, 0);
        std::lock_guard<std::mutex> lk(_m);
        _cv.notify_all();
        return;
    }
    if (flags & TH_RST)
    {
        markClosed();
        return;
    }
    if (payload && len)
    {
        {
            std::lock_guard<std::mutex> lk(_m);
            _txAck += (uint32_t)len;
            _rq.emplace_back(payload, payload + len);
            _cv.notify_all();
        }
        tcp(TH_ACK, nullptr, 0);
    }
    if (flags & TH_FIN)
    {
        {
            std::lock_guard<std::mutex> lk(_m);
            _txAck += 1;
        }
        tcp(TH_ACK, nullptr, 0);
        markClosed();
    }
}

// ── usbmuxd-compatible UNIX socket (plist protocol) ────────────────────────
namespace
{
std::string plistStr(plist_t dict, const char *key)
{
    plist_t v = plist_dict_get_item(dict, key);
    if (!v)
        return "";
    char *s = nullptr;
    plist_get_string_val(v, &s);
    std::string out = s ? s : "";
    free(s);
    return out;
}
uint64_t plistUint(plist_t dict, const char *key)
{
    plist_t v = plist_dict_get_item(dict, key);
    if (!v)
        return 0;
    uint64_t u = 0;
    plist_get_uint_val(v, &u);
    return u;
}
} // namespace

class MuxServer
{
public:
    MuxServer(MuxHost *host, std::string sockPath, std::string serial)
        : _host(host), _sockPath(std::move(sockPath)), _serial(std::move(serial))
    {
    }
    ~MuxServer() { stop(); }

    bool start()
    {
        unlink(_sockPath.c_str());
        _srv = socket(AF_UNIX, SOCK_STREAM, 0);
        if (_srv < 0)
            return false;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, _sockPath.c_str(), sizeof(a.sun_path) - 1);
        if (bind(_srv, (struct sockaddr *)&a, sizeof(a)) != 0)
        {
            log_e("cp-usbmux: bind %s: %s", _sockPath.c_str(), strerror(errno));
            ::close(_srv);
            _srv = -1;
            return false;
        }
        chmod(_sockPath.c_str(), 0777);
        listen(_srv, 16);
        _run = true;
        _accept = std::thread([this] { acceptLoop(); });
        return true;
    }

    void stop()
    {
        _run = false;
        if (_srv >= 0)
        {
            shutdown(_srv, SHUT_RDWR);
            ::close(_srv);
            _srv = -1;
        }
        if (_accept.joinable())
            _accept.join();
        unlink(_sockPath.c_str());
    }

private:
    void acceptLoop()
    {
        while (_run)
        {
            int c = accept(_srv, nullptr, nullptr);
            if (c < 0)
                break;
            std::thread([this, c] { client(c); }).detach();
        }
    }

    bool recvn(int fd, void *buf, size_t n)
    {
        uint8_t *p = (uint8_t *)buf;
        size_t got = 0;
        while (got < n)
        {
            ssize_t k = ::recv(fd, p + got, n - got, 0);
            if (k <= 0)
                return false;
            got += (size_t)k;
        }
        return true;
    }

    // Read one usbmux request: [u32 size LE][u32 ver][u32 msg][u32 tag][xml].
    bool recvPacket(int fd, uint32_t &tag, plist_t &req)
    {
        uint8_t hdr[4];
        if (!recvn(fd, hdr, 4))
            return false;
        uint32_t size = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        if (size < 16 || size > (1u << 20))
            return false;
        std::vector<uint8_t> body(size - 4);
        if (!recvn(fd, body.data(), body.size()))
            return false;
        tag = body[8] | (body[9] << 8) | (body[10] << 16) | ((uint32_t)body[11] << 24);
        req = nullptr;
        plist_from_memory((const char *)body.data() + 12, (uint32_t)(body.size() - 12), &req, nullptr);
        return req != nullptr;
    }

    void reply(int fd, uint32_t tag, plist_t obj)
    {
        char *xml = nullptr;
        uint32_t xlen = 0;
        plist_to_xml(obj, &xml, &xlen);
        std::vector<uint8_t> pkt;
        uint32_t total = 16 + xlen;
        auto le32 = [&](uint32_t v) {
            pkt.push_back(v);
            pkt.push_back(v >> 8);
            pkt.push_back(v >> 16);
            pkt.push_back(v >> 24);
        };
        le32(total);
        le32(1); // version
        le32(8); // message = PLIST
        le32(tag);
        pkt.insert(pkt.end(), xml, xml + xlen);
        (void)!::send(fd, pkt.data(), pkt.size(), MSG_NOSIGNAL);
        free(xml);
    }

    void replyResult(int fd, uint32_t tag, uint64_t number)
    {
        plist_t d = plist_new_dict();
        plist_dict_set_item(d, "MessageType", plist_new_string("Result"));
        plist_dict_set_item(d, "Number", plist_new_uint(number));
        reply(fd, tag, d);
        plist_free(d);
    }

    plist_t deviceEntry()
    {
        plist_t props = plist_new_dict();
        plist_dict_set_item(props, "ConnectionType", plist_new_string("USB"));
        plist_dict_set_item(props, "SerialNumber", plist_new_string(_serial.c_str()));
        plist_dict_set_item(props, "DeviceID", plist_new_uint(1));
        plist_dict_set_item(props, "LocationID", plist_new_uint(0));
        plist_dict_set_item(props, "ProductID", plist_new_uint(0x12a8));
        plist_t e = plist_new_dict();
        plist_dict_set_item(e, "DeviceID", plist_new_uint(1));
        plist_dict_set_item(e, "MessageType", plist_new_string("Attached"));
        plist_dict_set_item(e, "Properties", props);
        return e;
    }

    void client(int c)
    {
        for (;;)
        {
            uint32_t tag = 0;
            plist_t req = nullptr;
            if (!recvPacket(c, tag, req))
                break;
            std::string mt = plistStr(req, "MessageType");
            if (mt == "ReadBUID")
            {
                plist_t d = plist_new_dict();
                plist_dict_set_item(d, "BUID", plist_new_string(readBuid().c_str()));
                reply(c, tag, d);
                plist_free(d);
            }
            else if (mt == "ListDevices")
            {
                plist_t list = plist_new_array();
                plist_array_append_item(list, deviceEntry());
                plist_t d = plist_new_dict();
                plist_dict_set_item(d, "DeviceList", list);
                reply(c, tag, d);
                plist_free(d);
            }
            else if (mt == "Listen")
            {
                replyResult(c, tag, 0);
            }
            else if (mt == "ReadPairRecord")
            {
                std::string id = plistStr(req, "PairRecordID");
                if (id.empty())
                    id = _serial;
                std::string rec = readPairRecord(id);
                if (rec.empty())
                    replyResult(c, tag, 2);
                else
                {
                    plist_t d = plist_new_dict();
                    plist_dict_set_item(d, "PairRecordData", plist_new_data(rec.data(), rec.size()));
                    reply(c, tag, d);
                    plist_free(d);
                }
            }
            else if (mt == "SavePairRecord")
            {
                std::string id = plistStr(req, "PairRecordID");
                if (id.empty())
                    id = _serial;
                plist_t pd = plist_dict_get_item(req, "PairRecordData");
                if (pd)
                {
                    char *data = nullptr;
                    uint64_t dlen = 0;
                    plist_get_data_val(pd, &data, &dlen);
                    savePairRecord(id, std::string(data, data + dlen));
                    free(data);
                }
                replyResult(c, tag, 0);
            }
            else if (mt == "Connect")
            {
                uint16_t port = ntohs((uint16_t)plistUint(req, "PortNumber"));
                plist_free(req);
                auto conn = _host->connect(port);
                if (!conn)
                {
                    replyResult(c, tag, 3);
                    break;
                }
                replyResult(c, tag, 0);
                relay(c, conn);
                break; // the connection becomes a raw relay; no more mux requests
            }
            else
            {
                replyResult(c, tag, 0);
            }
            if (req)
                plist_free(req);
        }
        ::close(c);
    }

    void relay(int c, std::shared_ptr<MuxConn> conn)
    {
        std::thread up([&] {
            while (!conn->closed())
            {
                Bytes data = conn->recv();
                if (data.empty())
                    break;
                if (::send(c, data.data(), data.size(), MSG_NOSIGNAL) < 0)
                    break;
            }
            shutdown(c, SHUT_RDWR);
        });
        std::vector<uint8_t> buf(MAX_PAYLOAD);
        for (;;)
        {
            ssize_t n = ::recv(c, buf.data(), buf.size(), 0);
            if (n <= 0)
                break;
            conn->send(buf.data(), (size_t)n);
        }
        conn->close();
        up.join();
        _host->dropConn(conn->sport());
    }

    MuxHost *_host;
    std::string _sockPath, _serial;
    int _srv = -1;
    std::atomic<bool> _run{false};
    std::thread _accept;
};

// ── Usbmux orchestrator ────────────────────────────────────────────────────
Usbmux::Usbmux() = default;
Usbmux::~Usbmux() { stop(); }

bool Usbmux::configCarplay()
{
    int bus = 0, dev = 0;
    std::string sysdir;
    if (!findDev(_serial, bus, dev, sysdir))
    {
        log_e("cp-usbmux: iPhone %s not on USB", _serial.c_str());
        return false;
    }
    // Reveal the hidden CarPlay configs (vendor 0xC0/0x52) if still 4 configs.
    if (numConfigs(sysdir) < 6)
    {
        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", bus, dev);
        int fd = ::open(node, O_RDWR);
        if (fd < 0)
        {
            log_e("cp-usbmux: open %s: %s (root?)", node, strerror(errno));
            return false;
        }
        uint8_t reply = 0;
        usbControl(fd, 0xC0, 0x52, 0x0000, 0x0004, 1, &reply);
        ::close(fd);
        bool ok = false;
        for (int i = 0; i < 30; i++)
        {
            usleep(200000);
            if (findDev(_serial, bus, dev, sysdir) && numConfigs(sysdir) >= 6)
            {
                ok = true;
                break;
            }
        }
        if (!ok)
        {
            log_e("cp-usbmux: iPhone did not expose CarPlay configs (still %d)", numConfigs(sysdir));
            return false;
        }
    }
    // Let the just-revealed device finish re-enumerating before switching.
    usleep(500000);
    findDev(_serial, bus, dev, sysdir);
    // Select config 6. Prefer the usbfs USBDEVFS_SETCONFIGURATION ioctl on the
    // device node: with the fastcarplay udev rule granting the node, a NON-root
    // user can switch config this way (the kernel detaches the config-4 drivers)
    // -- so the app needs no root. Fall back to a raw sysfs write (needs root;
    // std::ofstream silently no-ops on sysfs, so use write()). Retry: the just-
    // revealed device can briefly EBUSY.
    for (int i = 0; i < 20; i++)
    {
        std::string cur = readSys(sysdir + "bConfigurationValue");
        if (cur == "6")
            return true;
        char node[64];
        snprintf(node, sizeof(node), "/dev/bus/usb/%03d/%03d", bus, dev);
        int ir = -1, ierr = 0;
        int ufd = ::open(node, O_RDWR | O_CLOEXEC);
        if (ufd >= 0)
        {
            int cfg6 = 6;
            ir = ioctl(ufd, USBDEVFS_SETCONFIGURATION, &cfg6);
            ierr = errno;
            ::close(ufd);
        }
        else
            ierr = errno;
        int wrote = -1;
        if (ir < 0) // ioctl unavailable (no node access) -> sysfs (root)
        {
            int cfd = ::open((sysdir + "bConfigurationValue").c_str(), O_WRONLY | O_CLOEXEC);
            if (cfd >= 0)
            {
                wrote = (int)::write(cfd, "6", 1);
                ::close(cfd);
            }
        }
        log_d("cp-usbmux: config->6 attempt %d cur=%s ioctl=%d(%s) sysfs-write=%d", i, cur.c_str(), ir,
              ir < 0 ? strerror(ierr) : "ok", wrote);
        usleep(400000);
        findDev(_serial, bus, dev, sysdir);
    }
    log_e("cp-usbmux: could not select config 6 (still %s) -- run the setup script (udev rule) or as root",
          readSys(sysdir + "bConfigurationValue").c_str());
    return false;
}

bool Usbmux::start(const std::string &serial)
{
    if (_running.load())
        return true;
    _serial = serial;
    _socketPath = "/tmp/fcp-usbmux-" + serial.substr(0, 8) + ".sock";

    if (!configCarplay())
        return false;

    _host = std::make_unique<MuxHost>();
    if (!_host->open(serial))
    {
        log_e("cp-usbmux: mux host open failed");
        _host.reset();
        return false;
    }
    _server = std::make_unique<MuxServer>(_host.get(), _socketPath, serial);
    if (!_server->start())
    {
        _host->close();
        _host.reset();
        _server.reset();
        return false;
    }
    _running.store(true);
    log_i("cp-usbmux: usbmux up for %s (config 6), socket %s", serial.substr(0, 8).c_str(),
          _socketPath.c_str());
    return true;
}

void Usbmux::stop()
{
    if (!_running.exchange(false) && !_server && !_host)
        return;
    if (_server)
        _server->stop();
    if (_host)
        _host->close();
    _server.reset();
    _host.reset();
}
} // namespace cp_usbmux

#endif /* USE_CP_WIRED */
