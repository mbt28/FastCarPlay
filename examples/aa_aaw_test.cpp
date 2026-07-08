// Exercise aa_aaw::runHandshake against a mock phone over a socketpair:
// confirm the head unit sends VersionRequest -> StartRequest{ip,port} ->
// InfoResponse{ssid,pass,bssid} and echoes pings as pongs.
// Build from examples/: make aa_aaw_test && ../out/aa_aaw_test
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include "protocol/aa/aa_aaw.h"

static int rc = 0;
#define CHECK(c, m) do { if(!(c)){ fprintf(stderr, "FAIL: %s\n", m); rc = 1; } } while(0)

static bool recvFrame(int fd, uint16_t &msgId, std::vector<uint8_t> &body)
{
    uint8_t h[4];
    size_t g = 0;
    while (g < 4) { ssize_t k = recv(fd, h + g, 4 - g, 0); if (k <= 0) return false; g += k; }
    uint16_t len = (h[0] << 8) | h[1];
    msgId = (h[2] << 8) | h[3];
    body.resize(len);
    g = 0;
    while (g < len) { ssize_t k = recv(fd, body.data() + g, len - g, 0); if (k <= 0) return false; g += k; }
    return true;
}

// Minimal protobuf field reader: pull a length-delimited string / varint.
static uint64_t varint(const uint8_t *p, size_t &i)
{
    uint64_t v = 0; int s = 0;
    while (p[i] & 0x80) { v |= (uint64_t)(p[i] & 0x7f) << s; s += 7; i++; }
    v |= (uint64_t)(p[i] & 0x7f) << s; i++;
    return v;
}

int main()
{
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    aa_aaw::Params params{"192.168.53.1", 5277, "FastCarPlay", "carplay1234", "aa:bb:cc:dd:ee:ff", 6};
    std::atomic<bool> active{true};

    std::thread hu([&] { aa_aaw::runHandshake(sv[0], params, active); });

    uint16_t id;
    std::vector<uint8_t> body;

    // 1) VersionRequest (msgId 4)
    CHECK(recvFrame(sv[1], id, body) && id == 4, "expected VersionRequest (4)");
    // reply VersionResponse (5)
    { uint8_t f[] = {0, 0, 0, 5}; send(sv[1], f, 4, 0); }

    // 2) StartRequest (msgId 1): field1 string ip, field2 varint port
    CHECK(recvFrame(sv[1], id, body) && id == 1, "expected StartRequest (1)");
    {
        size_t i = 0;
        CHECK(body[i++] == 0x0a, "start field1 tag");       // field1, len-delim
        uint64_t l = varint(body.data(), i);
        std::string ip((char *)&body[i], l); i += l;
        CHECK(ip == params.ip, "start ip mismatch");
        CHECK(body[i++] == 0x10, "start field2 tag");       // field2, varint
        uint64_t port = varint(body.data(), i);
        CHECK(port == params.port, "start port mismatch");
    }

    // reply InfoRequest (2)
    { uint8_t f[] = {0, 0, 0, 2}; send(sv[1], f, 4, 0); }

    // 3) InfoResponse (msgId 3): strings ssid/pass/bssid + security + aptype
    CHECK(recvFrame(sv[1], id, body) && id == 3, "expected InfoResponse (3)");
    {
        size_t i = 0;
        CHECK(body[i++] == 0x0a, "info ssid tag");
        uint64_t l = varint(body.data(), i);
        std::string ssid((char *)&body[i], l); i += l;
        CHECK(ssid == params.ssid, "info ssid mismatch");
    }

    // 4) ping (8) -> expect pong (9) echoing the body
    { uint8_t f[] = {0, 3, 0, 8, 1, 2, 3}; send(sv[1], f, 7, 0); }
    CHECK(recvFrame(sv[1], id, body) && id == 9 && body.size() == 3 && body[0] == 1, "expected pong (9)");

    close(sv[1]);
    active = false;
    hu.join();

    printf(rc ? "aa_aaw_test: FAILED\n" : "aa_aaw_test: all checks passed\n");
    return rc;
}
