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

        const Bytes aad(buf.begin() + off, buf.begin() + off + 2);
        const Bytes ctAndTag(buf.begin() + off + 2, buf.begin() + frameEnd);
        Bytes chunk;
        if (!cp_crypto::chachaOpen(_readKey, cp_crypto::nonce64(_readCtr), ctAndTag, aad, chunk))
            return false; // auth failure: fatal
        _readCtr++;
        plaintext.insert(plaintext.end(), chunk.begin(), chunk.end());
        off = frameEnd;
    }
    buf.erase(buf.begin(), buf.begin() + off);
    return true;
}

Bytes ControlCipher::encrypt(const Bytes &plain)
{
    Bytes out;
    size_t i = 0;
    do
    {
        const size_t len = std::min(MAX_PAYLOAD, plain.size() - i);
        const Bytes chunk(plain.begin() + i, plain.begin() + i + len);
        Bytes header{(uint8_t)(len & 0xff), (uint8_t)(len >> 8)}; // LE
        Bytes sealed = cp_crypto::chachaSeal(_writeKey, cp_crypto::nonce64(_writeCtr), chunk, header);
        _writeCtr++;
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), sealed.begin(), sealed.end());
        i += MAX_PAYLOAD;
    } while (i < plain.size());
    return out;
}
} // namespace cp_control_cipher
