#include "cp_nalu.h"

#include <cstring>

namespace cp_nalu
{
namespace
{
const uint8_t START_CODE[4] = {0x00, 0x00, 0x00, 0x01};

void pushStart(Bytes &out) { out.insert(out.end(), START_CODE, START_CODE + 4); }
uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }

// Extract the parameter-set NALUs from an avcC record (H.264) as Annex-B.
Bytes avcCToAnnexB(const uint8_t *a, size_t n)
{
    Bytes out;
    size_t off = 5; // version, profile, compat, level, lengthSizeMinusOne
    if (off >= n)
        return out;
    int numSps = a[off++] & 0x1f;
    for (int i = 0; i < numSps && off + 2 <= n; i++)
    {
        size_t len = be16(a + off);
        off += 2;
        if (off + len > n)
            break;
        pushStart(out);
        out.insert(out.end(), a + off, a + off + len);
        off += len;
    }
    if (off >= n)
        return out;
    int numPps = a[off++];
    for (int i = 0; i < numPps && off + 2 <= n; i++)
    {
        size_t len = be16(a + off);
        off += 2;
        if (off + len > n)
            break;
        pushStart(out);
        out.insert(out.end(), a + off, a + off + len);
        off += len;
    }
    return out;
}

// Extract the VPS/SPS/PPS NALUs from an hvcC record (H.265) as Annex-B.
Bytes hvcCToAnnexB(const uint8_t *a, size_t n)
{
    Bytes out;
    size_t off = 22; // fixed profile/level block before the array count
    if (off >= n)
        return out;
    int numArrays = a[off++];
    for (int arr = 0; arr < numArrays && off + 3 <= n; arr++)
    {
        off++; // array_completeness + NAL_unit_type
        int numNalus = be16(a + off);
        off += 2;
        for (int i = 0; i < numNalus && off + 2 <= n; i++)
        {
            size_t len = be16(a + off);
            off += 2;
            if (off + len > n)
                break;
            pushStart(out);
            out.insert(out.end(), a + off, a + off + len);
            off += len;
        }
    }
    return out;
}

// An avcC record starts [1][profile][compat][level][fc|len][e0|numSPS][spsLen][SPS…].
bool looksLikeAvcC(const uint8_t *a, size_t n)
{
    if (n < 9)
        return false;
    if ((a[5] & 0x1f) < 1)
        return false; // numOfSPS
    size_t spsLen = be16(a + 6);
    if (8 + spsLen > n)
        return false;
    return (a[8] & 0x1f) == 7; // H.264 SPS NAL unit type
}
} // namespace

Bytes avccFrameToAnnexB(const uint8_t *frame, size_t len, int lengthSize)
{
    Bytes out;
    size_t off = 0;
    while (off + (size_t)lengthSize <= len)
    {
        size_t n = 0;
        for (int i = 0; i < lengthSize; i++)
            n = n * 256 + frame[off + i];
        off += lengthSize;
        if (n == 0 || off + n > len)
            break;
        pushStart(out);
        out.insert(out.end(), frame + off, frame + off + n);
        off += n;
    }
    return out;
}

Bytes configToAnnexB(const uint8_t *payload, size_t len, bool &hevc)
{
    for (size_t i = 4; i + 4 <= len; i++)
    {
        if (std::memcmp(payload + i, "hvcC", 4) == 0)
        {
            hevc = true;
            return hvcCToAnnexB(payload + i + 4, len - i - 4);
        }
        if (std::memcmp(payload + i, "avcC", 4) == 0)
        {
            hevc = false;
            return avcCToAnnexB(payload + i + 4, len - i - 4);
        }
    }
    // Bare record with no fourcc (some phones send avcC as raw bytes).
    if (looksLikeAvcC(payload, len))
    {
        hevc = false;
        return avcCToAnnexB(payload, len);
    }
    hevc = true;
    return hvcCToAnnexB(payload, len);
}
} // namespace cp_nalu
