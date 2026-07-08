#ifndef SRC_PROTOCOL_AA_AA_TRANSPORT
#define SRC_PROTOCOL_AA_AA_TRANSPORT

#include <atomic>
#include <cstdint>

// Byte transport under the Android Auto GAL session. AaConnection speaks the
// same protocol (framing, TLS, service discovery, channels) regardless of
// whether the bytes ride over USB/AOAP (wired) or a TCP socket on Wi-Fi
// (wireless), so the transport is the only piece that differs between them.
class AaTransport
{
public:
    virtual ~AaTransport() = default;

    // Establish a link to the phone: AOAP switch + claim for USB, or accept a
    // TCP connection for wireless. Blocks while trying; returns once `active`
    // goes false. Returns true when linked and ready for read()/write().
    virtual bool open(std::atomic<bool> &active) = 0;

    // Tear the link down and unblock any pending read().
    virtual void close() = 0;

    // True while the link is up. read()/write() only work when connected.
    virtual bool connected() const = 0;

    // Blocking read of exactly `len` bytes into `dst`. Returns false if the
    // link dropped (or was closed).
    virtual bool read(uint8_t *dst, uint32_t len) = 0;

    // Write all `len` bytes. Returns false on error (and drops the link).
    virtual bool write(const uint8_t *src, uint32_t len) = 0;

    // Short transport name for status()/logging (e.g. "usb", "tcp").
    virtual const char *name() const = 0;
};

#endif /* SRC_PROTOCOL_AA_AA_TRANSPORT */
