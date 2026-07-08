#ifndef SRC_PROTOCOL_AA_AA_TCP_TRANSPORT
#define SRC_PROTOCOL_AA_AA_TCP_TRANSPORT

#include <atomic>
#include <cstdint>

#include "protocol/aa/aa_const.h"
#include "protocol/aa/aa_transport.h"

// TCP transport for wireless Android Auto. Listens on a port (5277) and, once
// the Bluetooth handshake has told the phone the head unit's IP + this port,
// accepts the phone's connection and streams frame bytes over it. The GAL
// session (framing, TLS, channels) is identical to the USB path.
class AaTcpTransport : public AaTransport
{
public:
    explicit AaTcpTransport(uint16_t port = AA_TCP_PORT);
    ~AaTcpTransport() override;

    bool open(std::atomic<bool> &active) override;
    void close() override;
    bool connected() const override { return _connected.load(); }
    bool read(uint8_t *dst, uint32_t len) override;
    bool write(const uint8_t *src, uint32_t len) override;
    const char *name() const override { return "tcp"; }

private:
    bool ensureListen();

    uint16_t _port;
    int _listenFd;
    int _clientFd;
    std::atomic<bool> _connected;
};

#endif /* SRC_PROTOCOL_AA_AA_TCP_TRANSPORT */
