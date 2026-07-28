// CarPlay MFiSAP /auth-setup end-to-end test against the REAL MFi chip.
//
// Plays the phone: builds an /auth-setup request, runs the accessory responder
// (which drives the coprocessor), then does exactly what an iPhone does with
// the reply -- derive the shared key, decrypt the signature, and verify it
// against the public key in the accessory certificate. If that verify passes,
// the chip's signature is one a real phone would accept: the "MFi accepted"
// milestone, provable without a phone.
//
//   make cp_authsetup_test && ../out/cp_authsetup_test [/dev/i2c-1] [addr]

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "protocol/cp/cp_auth_setup.h"
#include "protocol/cp/cp_crypto.h"
#include "protocol/cp/mfi_auth.h"

using cp_crypto::Bytes;

// Adapts the P0 hardware driver to the abstract signer the responder needs.
class ChipSigner : public cp_auth_setup::MfiSigner
{
public:
    ChipSigner(MfiAuth &chip, int major) : _chip(chip), _major(major) {}
    bool certificate(Bytes &out) override { return _chip.readCertificate(out); }
    bool sign(const Bytes &digest, Bytes &sig) override { return _chip.sign(digest, sig); }
    int protocolMajor() override { return _major; }

private:
    MfiAuth &_chip;
    int _major;
};

static uint32_t readU32BE(const Bytes &b, size_t off)
{
    return ((uint32_t)b[off] << 24) | ((uint32_t)b[off + 1] << 16) |
           ((uint32_t)b[off + 2] << 8) | b[off + 3];
}

int main(int argc, char **argv)
{
    const char *bus = argc > 1 ? argv[1] : "/dev/i2c-1";
    uint8_t addr = argc > 2 ? (uint8_t)strtol(argv[2], nullptr, 0) : 0x10;

    MfiAuth chip;
    if (!chip.open(bus, addr))
    {
        printf("no MFi chip: %s\n", chip.lastError());
        return 1;
    }
    MfiAuth::Info info;
    if (!chip.identify(info))
    {
        printf("identify failed: %s\n", chip.lastError());
        return 1;
    }
    printf("MFi %s on %s @ 0x%02x\n\n", info.protocolMajor == 3 ? "3.0" : "2.0C", bus, chip.address());
    ChipSigner signer(chip, info.protocolMajor);

    // --- phone side: build the /auth-setup request -------------------------
    cp_crypto::X25519Pair controller = cp_crypto::x25519Generate();
    Bytes request;
    request.push_back(0x01); // MFiSAP version
    request.insert(request.end(), controller.pubRaw.begin(), controller.pubRaw.end());

    // --- accessory side: the responder (drives the chip) -------------------
    Bytes response = cp_auth_setup::handle(request, signer);
    if (response.empty())
    {
        printf("FAIL: responder returned nothing\n");
        return 1;
    }

    // --- phone side: parse + verify exactly like an iPhone would -----------
    if (response.size() < 40)
    {
        printf("FAIL: response too short (%zu bytes)\n", response.size());
        return 1;
    }
    Bytes ourPub(response.begin(), response.begin() + 32);
    size_t off = 32;
    uint32_t certLen = readU32BE(response, off);
    off += 4;
    if (off + certLen + 4 > response.size())
    {
        printf("FAIL: bad certificate length %u\n", certLen);
        return 1;
    }
    Bytes cert(response.begin() + off, response.begin() + off + certLen);
    off += certLen;
    uint32_t sigLen = readU32BE(response, off);
    off += 4;
    if (off + sigLen != response.size())
    {
        printf("FAIL: bad signature length %u\n", sigLen);
        return 1;
    }
    Bytes encSig(response.begin() + off, response.end());
    printf("response: ourPub 32B, cert %uB, encSig %uB\n", certLen, sigLen);

    // Derive the same AES key/iv from the shared secret.
    Bytes shared = cp_crypto::x25519Shared(controller.priv, ourPub);
    Bytes keyLabel{'A', 'E', 'S', '-', 'K', 'E', 'Y'};
    Bytes ivLabel{'A', 'E', 'S', '-', 'I', 'V'};
    Bytes aesKey = cp_crypto::sha1(cp_crypto::concat(keyLabel, shared));
    Bytes aesIv = cp_crypto::sha1(cp_crypto::concat(ivLabel, shared));
    aesKey.resize(16);
    aesIv.resize(16);

    Bytes sig = cp_crypto::aesCtr128(aesKey, aesIv, encSig); // CTR: decrypt == encrypt
    printf("decrypted signature: %u bytes\n", (unsigned)sig.size());

    // The phone verifies the signature over (ourPub || controllerPub) using
    // the certificate's public key.
    Bytes signedData = cp_crypto::concat(ourPub, controller.pubRaw);
    bool ok = cp_crypto::verifyCertSignature(cert, signedData, sig);

    printf("\n%s\n", ok ? "PASS: chip signature verifies against its own certificate\n"
                          "      -> a real iPhone would accept this accessory."
                        : "FAIL: signature does not verify against the certificate");
    return ok ? 0 : 1;
}
