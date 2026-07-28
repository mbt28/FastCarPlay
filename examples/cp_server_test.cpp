// CarPlay TCP server integration test. Runs the control server in-process and
// connects a real TCP client that performs the full handshake over the socket:
// pair-verify (plaintext) -> encrypted framing -> /auth-setup (MFiSAP) against
// the real chip. Proves the transport path end to end.
//
//   make cp_server_test && ../out/cp_server_test [/dev/i2c-1] [addr]

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>
#include <poll.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "protocol/cp/cp_auth_setup.h"
#include "protocol/cp/cp_control_cipher.h"
#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/cp_identity.h"
#include "protocol/cp/cp_rtsp.h"
#include "protocol/cp/cp_server.h"
#include "protocol/cp/cp_tlv8.h"
#include "protocol/cp/mfi_auth.h"

using cp_crypto::Bytes;
static const uint16_t PORT = 17000;

static int failures = 0;
static void check(bool ok, const char *what)
{
    printf("  %-56s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

class ChipSigner : public cp_auth_setup::MfiSigner
{
public:
    ChipSigner(MfiAuth &c, int m) : _c(c), _m(m) {}
    bool certificate(Bytes &o) override { return _c.readCertificate(o); }
    bool sign(const Bytes &d, Bytes &s) override { return _c.sign(d, s); }
    int protocolMajor() override { return _m; }
private:
    MfiAuth &_c; int _m;
};

// A phone-side TCP client that speaks the control channel.
struct Client
{
    int fd;
    cp_control_cipher::ControlCipher *cipher = nullptr;
    Bytes raw, plain;

    void request(const std::string &path, const Bytes &body)
    {
        std::string head = "POST " + path + " RTSP/1.0\r\nCSeq: 1\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n";
        Bytes rtsp(head.begin(), head.end());
        rtsp.insert(rtsp.end(), body.begin(), body.end());
        Bytes wire = cipher ? cipher->encrypt(rtsp) : rtsp;
        send(fd, wire.data(), wire.size(), 0);
    }

    // Read until a complete RTSP response is buffered; return its body.
    Bytes responseBody()
    {
        for (int i = 0; i < 50; i++)
        {
            // Try to parse a full message already in `plain`.
            Bytes copy = plain;
            auto msgs = cp_rtsp::parse(copy);
            if (!msgs.empty())
            {
                plain = copy; // consume
                return msgs[0].body;
            }
            struct pollfd pfd{fd, POLLIN, 0};
            if (poll(&pfd, 1, 200) <= 0)
                continue;
            uint8_t buf[8192];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            raw.insert(raw.end(), buf, buf + n);
            if (cipher)
            {
                Bytes dec;
                cipher->decrypt(raw, dec);
                plain.insert(plain.end(), dec.begin(), dec.end());
            }
            else
            {
                plain.insert(plain.end(), raw.begin(), raw.end());
                raw.clear();
            }
        }
        return {};
    }
};

int main(int argc, char **argv)
{
    const char *bus = argc > 1 ? argv[1] : "/dev/i2c-1";
    uint8_t addr = argc > 2 ? (uint8_t)strtol(argv[2], nullptr, 0) : 0x10;

    char tmpl[] = "/tmp/fcp-srv-XXXXXX";
    cp_identity::setStorageDir(mkdtemp(tmpl));

    MfiAuth chip;
    bool haveChip = chip.open(bus, addr);
    MfiAuth::Info info{};
    if (haveChip) haveChip = chip.identify(info);
    ChipSigner signer(chip, info.protocolMajor);
    printf("CarPlay TCP server test (chip %s)\n\n", haveChip ? "present" : "absent");

    cp_crypto::Ed25519Pair ctrlLt = cp_crypto::ed25519Generate();
    const std::string ctrlId = "phone-tcp";
    cp_identity::savePairing(ctrlId, ctrlLt.pubRaw);
    const cp_identity::Identity &acc = cp_identity::loadOrCreateIdentity();

    cp_server::Server server;
    check(server.start(PORT, haveChip ? &signer : nullptr), "server listening");

    // Connect a real TCP client.
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    check(connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0, "client connected");
    Client cli{fd};

    // pair-verify over the socket.
    printf("\npair-verify over TCP:\n");
    cp_crypto::X25519Pair cEph = cp_crypto::x25519Generate();
    cli.request("/pair-verify", cp_tlv8::encode({{0x06, {1}}, {0x03, cEph.pubRaw}}));
    auto t2 = cp_tlv8::decode(cli.responseBody());
    Bytes accEphPub = t2[0x03];
    check(!accEphPub.empty(), "M2 received");

    Bytes shared = cp_crypto::x25519Shared(cEph.priv, accEphPub);
    Bytes encKey = cp_crypto::hkdfSha512(shared, "Pair-Verify-Encrypt-Salt", "Pair-Verify-Encrypt-Info", 32);
    Bytes plain;
    cp_crypto::chachaOpen(encKey, cp_crypto::nonceLabel("PV-Msg02"), t2[0x05], {}, plain);
    auto sub = cp_tlv8::decode(plain);
    Bytes proven = cp_crypto::concat(cp_crypto::concat(accEphPub, sub[0x01]), cEph.pubRaw);
    check(cp_crypto::ed25519Verify(acc.pubRaw, proven, sub[0x0a]), "accessory identity proven");

    Bytes idb(ctrlId.begin(), ctrlId.end());
    Bytes toSign = cp_crypto::concat(cp_crypto::concat(cEph.pubRaw, idb), accEphPub);
    Bytes sig = cp_crypto::ed25519Sign(ctrlLt.privRaw, toSign);
    Bytes inner = cp_tlv8::encode({{0x01, idb}, {0x0a, sig}});
    Bytes sealed = cp_crypto::chachaSeal(encKey, cp_crypto::nonceLabel("PV-Msg03"), inner);
    cli.request("/pair-verify", cp_tlv8::encode({{0x06, {3}}, {0x05, sealed}}));
    check(cp_tlv8::decode(cli.responseBody())[0x06] == Bytes{4}, "M4 state = 4 (paired)");

    // Switch the client to encrypted framing (matching keys) and run auth-setup.
    cp_control_cipher::ControlCipher phoneCipher(
        cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Read-Encryption-Key", 32),
        cp_crypto::hkdfSha512(shared, "Control-Salt", "Control-Write-Encryption-Key", 32));
    cli.cipher = &phoneCipher;

    if (haveChip)
    {
        printf("\n/auth-setup over the encrypted TCP channel:\n");
        cp_crypto::X25519Pair aEph = cp_crypto::x25519Generate();
        Bytes areq;
        areq.push_back(0x01);
        areq.insert(areq.end(), aEph.pubRaw.begin(), aEph.pubRaw.end());
        cli.request("/auth-setup", areq);
        Bytes body = cli.responseBody();
        check(!body.empty(), "MFiSAP response received (decrypted)");

        Bytes ourPub(body.begin(), body.begin() + 32);
        size_t off = 32;
        auto u32 = [&](size_t o){ return ((uint32_t)body[o]<<24)|((uint32_t)body[o+1]<<16)|((uint32_t)body[o+2]<<8)|body[o+3]; };
        uint32_t certLen = u32(off); off += 4;
        Bytes cert(body.begin()+off, body.begin()+off+certLen); off += certLen;
        uint32_t sigLen = u32(off); off += 4;
        Bytes encSig(body.begin()+off, body.begin()+off+sigLen);
        Bytes aShared = cp_crypto::x25519Shared(aEph.priv, ourPub);
        Bytes aKey = cp_crypto::sha1(cp_crypto::concat(Bytes{'A','E','S','-','K','E','Y'}, aShared));
        Bytes aIv = cp_crypto::sha1(cp_crypto::concat(Bytes{'A','E','S','-','I','V'}, aShared));
        aKey.resize(16); aIv.resize(16);
        Bytes s = cp_crypto::aesCtr128(aKey, aIv, encSig);
        check(cp_crypto::verifyCertSignature(cert, cp_crypto::concat(ourPub, aEph.pubRaw), s),
              "MFi signature verifies over the encrypted TCP channel");
    }

    close(fd);
    server.stop();
    printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
