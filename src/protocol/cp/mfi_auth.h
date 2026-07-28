#ifndef SRC_PROTOCOL_CP_MFI_AUTH
#define SRC_PROTOCOL_CP_MFI_AUTH

// Driver for the Apple MFi authentication coprocessor (2.0C / 3.0), an I2C
// device the head unit uses to prove to an iPhone that it is a licensed
// CarPlay accessory. CarPlay pair-setup folds a challenge from the phone
// through this chip and returns its signature + the chip's certificate.
//
// This is CarPlay's only genuinely device-specific *hardware* dependency, so
// it is isolated behind this one class: probe/identify, read the certificate,
// sign a challenge. Different boards/chips drop in by changing the bus + addr.
//
// Register addresses below follow the published Apple Authentication
// Coprocessor 2.0C / 3.0 map. VERIFY THEM against the datasheet for the exact
// part in hand before trusting the challenge/cert paths -- the identify path
// (read-only) is the safe first contact.

#ifdef __linux__

#include <cstdint>
#include <string>
#include <vector>

class MfiAuth
{
public:
    // Read-only identity, the first thing to confirm the chip is alive.
    struct Info
    {
        uint8_t deviceVersion = 0;      // 0x05 = 2.0C, 0x07 = 3.0
        uint8_t firmwareVersion = 0;
        uint8_t protocolMajor = 0;      // 2 or 3
        uint8_t protocolMinor = 0;
        uint8_t errorCode = 0;
        uint16_t digestLen = 0;         // sign() input: 20 (2.0C) or 32 (3.0)
        uint16_t signatureLen = 0;      // sign() output: 128 (2.0C) or 64 (3.0)
    };

    MfiAuth() = default;
    ~MfiAuth();

    // Open the I2C bus (e.g. "/dev/i2c-1") at the chip's 7-bit address. If
    // addr is 0, tries the two documented addresses (0x10, 0x11) and keeps the
    // first that answers an identity read.
    bool open(const std::string &busPath, uint8_t addr = 0);
    void close();
    bool isOpen() const { return _fd >= 0; }
    uint8_t address() const { return _addr; }

    // Read the identity registers. False if the chip does not respond.
    bool identify(Info &out);

    // The accessory certificate the phone validates. Read once and cached by
    // the caller; it does not change.
    bool readCertificate(std::vector<uint8_t> &out);

    // Sign `challenge` (the phone's pair-setup challenge) and return the
    // coprocessor's signature. This is the operation on the CarPlay hot path
    // of pairing -- one-time per phone, not per frame.
    bool sign(const std::vector<uint8_t> &challenge, std::vector<uint8_t> &signature);

    const char *lastError() const { return _lastError.c_str(); }

private:
    bool writeReg(uint8_t reg, const uint8_t *data, size_t len);
    bool readReg(uint8_t reg, uint8_t *data, size_t len);
    bool tryAddress(uint8_t addr);

    int _fd = -1;
    uint8_t _addr = 0;
    uint8_t _protoMajor = 0; // set by identify(); selects the 2.0C vs 3.0 flow
    std::string _lastError;
};

#endif /* __linux__ */
#endif /* SRC_PROTOCOL_CP_MFI_AUTH */
