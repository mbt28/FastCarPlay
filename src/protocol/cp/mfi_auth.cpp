#include "mfi_auth.h"

#ifdef __linux__

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

// Apple Authentication Coprocessor 2.0C / 3.0 register map. Cross-check with
// the datasheet for the exact part before relying on the challenge/cert flow.
namespace
{
constexpr uint8_t REG_DEVICE_VERSION = 0x00;
constexpr uint8_t REG_FIRMWARE_VERSION = 0x01;
constexpr uint8_t REG_PROTOCOL_MAJOR = 0x02;
constexpr uint8_t REG_PROTOCOL_MINOR = 0x03;
constexpr uint8_t REG_ERROR_CODE = 0x05;

constexpr uint8_t REG_AUTH_CONTROL = 0x10; // write to start, read for status
constexpr uint8_t REG_SIGNATURE_LENGTH = 0x11; // 2 bytes BE
constexpr uint8_t REG_SIGNATURE_DATA = 0x12;   // up to 128 bytes
constexpr uint8_t REG_CHALLENGE_LENGTH = 0x20; // 2 bytes BE
constexpr uint8_t REG_CHALLENGE_DATA = 0x21;   // up to 32 bytes
constexpr uint8_t REG_CERT_LENGTH = 0x30;      // 2 bytes BE
constexpr uint8_t REG_CERT_DATA_BASE = 0x31;   // paged: 0x31, 0x32, ... 128 B each
constexpr uint8_t REG_SELF_TEST = 0x40;

constexpr uint8_t AUTH_START_SIGNATURE = 0x01; // control: generate signature
constexpr int CERT_PAGE = 128;
constexpr int MAX_SIGNATURE = 128;
} // namespace

MfiAuth::~MfiAuth()
{
    close();
}

// This coprocessor does NOT accept a repeated-start read (write reg pointer +
// Sr + read in one I2C_RDWR): that returns Remote I/O errors / garbage.
// It needs two SEPARATE transactions -- write the register pointer (STOP),
// a short settle, then read (START) -- which is what plain write()/read() in
// I2C_SLAVE mode do. Measured 10/10 reliable this way, ~0/20 with Sr.
namespace
{
constexpr useconds_t REG_SETTLE_US = 1000; // gap after pointer write before read
// The chip NAKs the first transaction after idle, and clock-stretches through
// signing (tens of ms). Retry on a ~1s deadline like the reference driver, so
// one code path covers both wake-up and the sign wait.
constexpr int MFI_IO_RETRIES = 500; // x ~2ms ~= 1s
} // namespace

bool MfiAuth::readReg(uint8_t reg, uint8_t *data, size_t len)
{
    // The chip auto-increments across separate 1-byte reads but NAKs a
    // multi-byte read in one transaction, AND it NAKs the first read after
    // idle to wake. The pattern that works: set the pointer ONCE, then keep
    // retrying the byte reads (each byte its own transaction) WITHOUT
    // re-writing the pointer -- a re-write restarts the wake and it never
    // catches up. The whole thing is retried on the ~1s budget for the cold
    // NAK and the sign clock-stretch.
    for (int outer = 0; outer < 8; outer++)
    {
        // Set the register pointer (retry until it ACKs).
        bool pointerSet = false;
        for (int i = 0; i < MFI_IO_RETRIES && !pointerSet; i++)
        {
            if (::write(_fd, &reg, 1) == 1)
                pointerSet = true;
            else
                usleep(REG_SETTLE_US);
        }
        if (!pointerSet)
            continue;
        usleep(REG_SETTLE_US);

        // Read each byte, retrying that byte in place (pointer only advances on
        // a successful read, so a NAK leaves us aligned for the next attempt).
        size_t got = 0;
        for (; got < len; got++)
        {
            bool byteRead = false;
            for (int i = 0; i < MFI_IO_RETRIES && !byteRead; i++)
            {
                if (::read(_fd, data + got, 1) == 1)
                    byteRead = true;
                else
                    usleep(REG_SETTLE_US);
            }
            if (!byteRead)
                break;
        }
        if (got == len)
            return true;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "read reg 0x%02x > %s", reg, strerror(errno));
    _lastError = buf;
    return false;
}

bool MfiAuth::writeReg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buffer[1 + 256];
    if (len > sizeof(buffer) - 1)
    {
        _lastError = "write too large";
        return false;
    }
    buffer[0] = reg;
    memcpy(buffer + 1, data, len);

    for (int attempt = 0; attempt < MFI_IO_RETRIES; attempt++)
    {
        if (::write(_fd, buffer, 1 + len) == (ssize_t)(1 + len))
        {
            usleep(REG_SETTLE_US);
            return true;
        }
        usleep(REG_SETTLE_US);
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "write reg 0x%02x > %s", reg, strerror(errno));
    _lastError = buf;
    return false;
}

bool MfiAuth::tryAddress(uint8_t addr)
{
    _addr = addr;
    if (ioctl(_fd, I2C_SLAVE, addr) < 0)
    {
        _lastError = std::string("select 0x") + std::to_string((int)addr) + " > " + strerror(errno);
        return false;
    }
    // A device-version read that returns a known value (0x05=2.0C, 0x07=3.0)
    // is the cheapest proof the chip is there, awake, and read correctly.
    uint8_t v = 0;
    return readReg(REG_DEVICE_VERSION, &v, 1) && (v == 0x05 || v == 0x07);
}

bool MfiAuth::open(const std::string &busPath, uint8_t addr)
{
    close();
    _fd = ::open(busPath.c_str(), O_RDWR);
    if (_fd < 0)
    {
        _lastError = std::string("open ") + busPath + " > " + strerror(errno);
        return false;
    }

    if (addr != 0)
    {
        if (tryAddress(addr))
            return true;
        _lastError = "no MFi coprocessor at 0x" + std::to_string(addr) +
                     " on " + busPath + " (check RESET_N / power / wiring)";
        close();
        return false;
    }

    // Auto: the two documented addresses.
    for (uint8_t candidate : {0x10, 0x11})
        if (tryAddress(candidate))
            return true;

    _lastError = "no MFi coprocessor at 0x10/0x11 on " + busPath +
                 " (chip not responding -- check RESET_N is released, power, SDA/SCL)";
    close();
    return false;
}

void MfiAuth::close()
{
    if (_fd >= 0)
        ::close(_fd);
    _fd = -1;
}

bool MfiAuth::identify(Info &out)
{
    // Each register is read individually -- the chip does not auto-increment
    // across arbitrary registers (only the cert/response streaming registers).
    if (!readReg(REG_DEVICE_VERSION, &out.deviceVersion, 1) ||
        !readReg(REG_FIRMWARE_VERSION, &out.firmwareVersion, 1) ||
        !readReg(REG_PROTOCOL_MAJOR, &out.protocolMajor, 1) ||
        !readReg(REG_PROTOCOL_MINOR, &out.protocolMinor, 1) ||
        !readReg(REG_ERROR_CODE, &out.errorCode, 1))
        return false;

    _protoMajor = out.protocolMajor;
    // Signature sizes are fixed by protocol version: 2.0C = SHA-1(20) -> RSA(128),
    // 3.0 = SHA-256(32) -> ECDSA P-256(64).
    out.digestLen = (out.protocolMajor == 3) ? 32 : 20;
    out.signatureLen = (out.protocolMajor == 3) ? 64 : 128;
    return true;
}

bool MfiAuth::readCertificate(std::vector<uint8_t> &out)
{
    // The reference driver reads the self-test status before touching the
    // certificate, and empirically the cert-length read NAKs until it has.
    // 3.0 self-test is read-only (top two bits set = passed); 2.0C starts it
    // by writing 1 first.
    if (_protoMajor == 2)
    {
        uint8_t start = 1;
        writeReg(REG_SELF_TEST, &start, 1);
    }
    uint8_t selfTest = 0;
    if (!readReg(REG_SELF_TEST, &selfTest, 1))
        return false;
    if ((selfTest >> 6) != 3)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "self-test not passed (0x%02x)", selfTest);
        _lastError = buf;
        return false;
    }

    uint8_t lenBuf[2] = {0};
    if (!readReg(REG_CERT_LENGTH, lenBuf, 2))
        return false;
    const size_t certLen = ((size_t)lenBuf[0] << 8) | lenBuf[1];
    // Sanity per protocol version (2.0C < 1280, 3.0 = 607..609).
    const bool ok = (_protoMajor == 3) ? (certLen >= 607 && certLen <= 609)
                                       : (certLen > 0 && certLen < 1280);
    if (!ok)
    {
        _lastError = "implausible certificate length " + std::to_string(certLen);
        return false;
    }

    out.clear();
    out.reserve(certLen);
    // The cert spans sequential page registers, CERT_PAGE bytes each.
    for (size_t offset = 0; offset < certLen;)
    {
        const size_t chunk = std::min((size_t)CERT_PAGE, certLen - offset);
        const uint8_t reg = (uint8_t)(REG_CERT_DATA_BASE + offset / CERT_PAGE);
        uint8_t page[CERT_PAGE];
        if (!readReg(reg, page, chunk))
            return false;
        out.insert(out.end(), page, page + chunk);
        offset += chunk;
    }
    return true;
}

bool MfiAuth::sign(const std::vector<uint8_t> &digest, std::vector<uint8_t> &signature)
{
    // `digest` is the hash the phone wants signed: SHA-256 (32B) on 3.0,
    // SHA-1 (20B) on 2.0C. The chip returns an ECDSA-P256 (64B) / RSA (128B)
    // signature over it.
    const size_t expect = (_protoMajor == 3) ? 32 : 20;
    if (digest.size() != expect)
    {
        _lastError = "digest must be " + std::to_string(expect) + " bytes for protocol " +
                     std::to_string(_protoMajor);
        return false;
    }

    // 2.0C needs the challenge + response lengths written first; 3.0 does not.
    if (_protoMajor == 2)
    {
        uint8_t inLen[2] = {(uint8_t)(digest.size() >> 8), (uint8_t)(digest.size() & 0xff)};
        uint8_t outLen[2] = {0x00, 0x80}; // 128
        if (!writeReg(REG_CHALLENGE_LENGTH, inLen, 2) ||
            !writeReg(REG_SIGNATURE_LENGTH, outLen, 2))
            return false;
    }

    if (!writeReg(REG_CHALLENGE_DATA, digest.data(), digest.size()))
        return false;

    uint8_t start = AUTH_START_SIGNATURE;
    if (!writeReg(REG_AUTH_CONTROL, &start, 1))
        return false;

    // One status read: the chip clock-stretches through signing, so the read
    // (which retries on a ~1s deadline) returns only once it is done. 0x10 =
    // success; anything else is an error code.
    uint8_t status = 0;
    if (!readReg(REG_AUTH_CONTROL, &status, 1))
        return false;
    if (status != 0x10)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "signing failed, status 0x%02x", status);
        _lastError = buf;
        return false;
    }

    uint8_t sigLenBuf[2] = {0};
    if (!readReg(REG_SIGNATURE_LENGTH, sigLenBuf, 2))
        return false;
    const size_t sigLen = ((size_t)sigLenBuf[0] << 8) | sigLenBuf[1];
    if (sigLen == 0 || sigLen > MAX_SIGNATURE)
    {
        _lastError = "implausible signature length " + std::to_string(sigLen);
        return false;
    }

    signature.resize(sigLen);
    return readReg(REG_SIGNATURE_DATA, signature.data(), sigLen);
}

#endif /* __linux__ */
