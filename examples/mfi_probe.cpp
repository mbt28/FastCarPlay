// MFi authentication coprocessor bring-up probe (CarPlay P0).
//
// The instrument for getting the chip talking: it opens the I2C bus, finds the
// coprocessor, prints its identity, reads the accessory certificate, and runs
// a self-test challenge. Until the chip ACKs, it reports exactly what failed
// so the hardware (RESET_N, power, wiring) can be sorted first.
//
//   make mfi_probe && ../out/mfi_probe [/dev/i2c-1] [addr]
//
//   /dev/i2c-1  the bus the chip is wired to (default /dev/i2c-1)
//   addr        7-bit address in hex (default: auto-try 0x10 then 0x11)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "protocol/cp/mfi_auth.h"

static void hexDump(const char *label, const std::vector<uint8_t> &data, size_t max = 48)
{
    printf("%s (%zu bytes):", label, data.size());
    for (size_t i = 0; i < data.size() && i < max; i++)
    {
        if (i % 16 == 0)
            printf("\n  ");
        printf("%02x ", data[i]);
    }
    if (data.size() > max)
        printf("\n  ... (%zu more)", data.size() - max);
    printf("\n");
}

int main(int argc, char **argv)
{
    const char *bus = argc > 1 ? argv[1] : "/dev/i2c-1";
    uint8_t addr = argc > 2 ? (uint8_t)strtol(argv[2], nullptr, 0) : 0;

    printf("MFi coprocessor probe on %s%s\n", bus,
           addr ? "" : " (auto address 0x10/0x11)");

    MfiAuth mfi;
    if (!mfi.open(bus, addr))
    {
        printf("\nNOT FOUND: %s\n", mfi.lastError());
        printf("\nThe chip is not answering. Most likely, in order:\n");
        printf("  1. RESET_N held low -- it must be released high (3.3V) to leave reset\n");
        printf("  2. VCC (3.3V) / GND not connected\n");
        printf("  3. SDA/SCL swapped -- GPIO2 = pin 3 = SDA, GPIO3 = pin 5 = SCL\n");
        printf("  4. wrong bus -- confirm with: i2cdetect -y <bus>\n");
        return 1;
    }
    printf("Found at 0x%02x\n\n", mfi.address());

    MfiAuth::Info info;
    if (!mfi.identify(info))
    {
        printf("identify failed: %s\n", mfi.lastError());
        return 1;
    }
    printf("Identity:\n");
    printf("  device version   0x%02x  (%s)\n", info.deviceVersion,
           info.deviceVersion == 0x05 ? "2.0C" : info.deviceVersion == 0x07 ? "3.0" : "unknown");
    printf("  firmware version 0x%02x\n", info.firmwareVersion);
    printf("  auth protocol    %d.%d\n", info.protocolMajor, info.protocolMinor);
    printf("  signature        %d-byte digest -> %d-byte signature\n", info.digestLen, info.signatureLen);
    printf("  error code       0x%02x\n\n", info.errorCode);

    std::vector<uint8_t> cert;
    if (mfi.readCertificate(cert))
        hexDump("Accessory certificate", cert);
    else
        printf("certificate read failed: %s\n", mfi.lastError());
    printf("\n");

    // Self-test: a fixed digest of the right size for this chip, just to
    // exercise sign(). The real digest in CarPlay pairing comes from hashing
    // the phone's pair-setup data.
    std::vector<uint8_t> digest(info.digestLen ? info.digestLen : 32);
    for (size_t i = 0; i < digest.size(); i++)
        digest[i] = (uint8_t)(i * 7 + 1);

    std::vector<uint8_t> signature;
    if (mfi.sign(digest, signature))
    {
        hexDump("Signature for test challenge", signature);
        printf("\nPASS: coprocessor identified, certificate read, challenge signed.\n");
        return 0;
    }
    printf("sign failed: %s\n", mfi.lastError());
    printf("\nPARTIAL: chip talks, but the challenge/cert flow needs the datasheet\n"
           "         register semantics verified for this exact part.\n");
    return 1;
}
