#ifndef SRC_COMMON_ANNEXB
#define SRC_COMMON_ANNEXB

#include <cstdint>

// NAL classification of one Annex-B access unit (H.264 or HEVC). Emulation
// prevention keeps 00 00 01 out of NAL payloads, so every match is a real
// start code; the scan stops as soon as both answers are known.
struct AnnexBKinds
{
    bool keyframe = false;  // H.264 IDR slice / HEVC IRAP (BLA/IDR/CRA)
    bool paramSets = false; // H.264 SPS/PPS, HEVC VPS/SPS/PPS
};

inline AnnexBKinds annexbKinds(const uint8_t *p, int n, bool hevc)
{
    AnnexBKinds k;
    for (int i = 0; i + 3 < n && !(k.keyframe && k.paramSets); i++)
    {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        const uint8_t h = p[i + 3];
        if (hevc)
        {
            const int type = (h >> 1) & 0x3F;
            k.keyframe |= type >= 16 && type <= 21;
            k.paramSets |= type >= 32 && type <= 34;
        }
        else
        {
            const int type = h & 0x1F;
            k.keyframe |= type == 5;
            k.paramSets |= type == 7 || type == 8;
        }
        i += 2;
    }
    return k;
}

// True if the access unit carries a picture (a VCL slice NAL). Stops at the
// first one, so a normal frame costs a few bytes of scanning.
inline bool annexbHasPicture(const uint8_t *p, int n, bool hevc)
{
    for (int i = 0; i + 3 < n; i++)
    {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
            continue;
        const uint8_t h = p[i + 3];
        const int type = hevc ? (h >> 1) & 0x3F : h & 0x1F;
        if (hevc ? type <= 31 : (type >= 1 && type <= 5))
            return true;
        i += 2;
    }
    return false;
}

#endif /* SRC_COMMON_ANNEXB */
