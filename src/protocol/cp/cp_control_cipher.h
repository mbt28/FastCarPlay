#ifndef SRC_PROTOCOL_CP_CP_CONTROL_CIPHER
#define SRC_PROTOCOL_CP_CP_CONTROL_CIPHER

// ChaCha20-Poly1305 framing for the CarPlay control channel, active after
// pair-verify. Every RTSP message travels as one or more frames:
//   [2B length LE (ciphertext only)][ciphertext][16B tag]
// The 2-byte length header is the AEAD associated data; the nonce is a
// per-direction 8-byte little-endian counter that increments per frame. Read
// and write use separate keys. Mirrors LIVI controlCipher.ts.

#include <cstdint>
#include <vector>

namespace cp_control_cipher
{
using Bytes = std::vector<uint8_t>;

class ControlCipher
{
public:
    ControlCipher(const Bytes &readKey, const Bytes &writeKey)
        : _readKey(readKey), _writeKey(writeKey) {}

    // Decrypt as many whole frames as `buf` holds. `plaintext` gets the
    // decrypted RTSP bytes; `buf` is replaced with the unconsumed remainder.
    // Returns false if a frame fails authentication (fatal for the channel).
    bool decrypt(Bytes &buf, Bytes &plaintext);

    // Frame + encrypt one plaintext message, splitting at 16 KiB.
    Bytes encrypt(const Bytes &plain);

private:
    Bytes _readKey;
    Bytes _writeKey;
    uint64_t _readCtr = 0;
    uint64_t _writeCtr = 0;
};
} // namespace cp_control_cipher

#endif /* SRC_PROTOCOL_CP_CP_CONTROL_CIPHER */
