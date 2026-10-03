#include "cp_control_cipher.h"

#include "cp_crypto.h"

namespace cp_control_cipher
{
namespace
{
constexpr size_t MAX_PAYLOAD = 0x4000; // 16 KiB
}

bool ControlCipher::decrypt(Bytes &buf, Bytes &plaintext)
{
    plaintext.clear();
    size_t off = 0;
    while (buf.size() - off >= 2)
    {
        const uint16_t len = (uint16_t)(buf[off] | (buf[off + 1] << 8)); // LE
        const size_t frameEnd = off + 2 + len + 16;
        if (buf.size() < frameEnd)
            break; // frame not fully received

        // Decrypt straight from `buf` onto the end of `plaintext`: the AAD is
        // the 2-byte length prefix, the ciphertext+tag follow it. No per-frame
        // temporaries.
        uint8_t nonce[12];
        cp_crypto::nonce64(_readCtr, nonce);
        const size_t pos = plaintext.size();
        plaintext.resize(pos + len);
        if (!cp_crypto::chachaOpen(_readKey.data(), nonce, buf.data() + off + 2, len + 16,
                                   buf.data() + off, 2, plaintext.data() + pos))
        {
            plaintext.resize(pos);
            return false; // auth failure: fatal
        }
        _readCtr++;
        off = frameEnd;
    }
    buf.erase(buf.begin(), buf.begin() + off);
    return true;
}

Bytes ControlCipher::encrypt(const Bytes &plain)
{
    // Size the whole output up front (2-byte header + 16-byte tag per chunk)
    // and seal each chunk straight from `plain` into it -- no per-chunk copy
    // of the plaintext and no temporary sealed buffer.
    const size_t chunks = plain.empty() ? 1 : (plain.size() + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
    Bytes out(plain.size() + chunks * (2 + 16));
    size_t i = 0, o = 0;
    uint8_t nonce[12];
    do
    {
        const size_t len = std::min(MAX_PAYLOAD, plain.size() - i);
        uint8_t header[2] = {(uint8_t)(len & 0xff), (uint8_t)(len >> 8)}; // LE
        out[o] = header[0];
        out[o + 1] = header[1];
        cp_crypto::nonce64(_writeCtr, nonce);
        cp_crypto::chachaSeal(_writeKey.data(), nonce, plain.data() + i, len,
                              header, 2, out.data() + o + 2);
        _writeCtr++;
        o += 2 + len + 16;
        i += MAX_PAYLOAD;
    } while (i < plain.size());
    out.resize(o);
    return out;
}
} // namespace cp_control_cipher
