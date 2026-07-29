// cp_nalu_test -- offline validation of the CarPlay screen framing + NALU
// conversion (cp_nalu). Two parts:
//   1. A self-contained round-trip for avccFrameToAnnexB (synthetic NALUs).
//   2. If a real captured screen stream is present (default
//      /tmp/cpav/stream-screen.bin, or argv[1]), walk every 128-byte-header
//      message, confirm it parses cleanly to EOF, extract the VideoConfig atom
//      and convert it to Annex-B, checking the codec + parameter-set NAL types.
//
// The frame bodies are ChaCha20 encrypted (per-session key), so this test does
// NOT decrypt them -- it validates the framing + config path that needs no key.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "protocol/cp/cp_nalu.h"

using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define CHECK(cond, msg)                                                                            \
    do                                                                                              \
    {                                                                                               \
        if (!(cond))                                                                                \
        {                                                                                           \
            printf("  FAIL: %s\n", msg);                                                            \
            failures++;                                                                             \
        }                                                                                           \
    } while (0)

// Number of Annex-B NAL units (00 00 00 01 start codes) in a buffer.
static int countAnnexB(const Bytes &b)
{
    int n = 0;
    for (size_t i = 0; i + 4 <= b.size(); i++)
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1)
            n++;
    return n;
}

static void testAvccRoundTrip()
{
    printf("avccFrameToAnnexB round-trip:\n");
    // Two length-prefixed NALs (4-byte BE length): [00000003 61 62 63][00000002 de ad].
    Bytes frame = {0, 0, 0, 3, 0x61, 0x62, 0x63, 0, 0, 0, 2, 0xde, 0xad};
    Bytes ab = cp_nalu::avccFrameToAnnexB(frame.data(), frame.size(), 4);
    Bytes want = {0, 0, 0, 1, 0x61, 0x62, 0x63, 0, 0, 0, 1, 0xde, 0xad};
    CHECK(ab == want, "two NALs rewritten to Annex-B");
    CHECK(countAnnexB(ab) == 2, "two start codes emitted");

    // A truncated trailing length is ignored, not read out of bounds.
    Bytes trunc = {0, 0, 0, 5, 0x11, 0x22};
    Bytes abt = cp_nalu::avccFrameToAnnexB(trunc.data(), trunc.size(), 4);
    CHECK(abt.empty(), "truncated NAL dropped cleanly");
    printf("  %s\n", failures ? "(errors above)" : "ok");
}

static void testCapture(const char *path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        printf("capture walk: skipped (no %s)\n", path);
        return;
    }
    Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    printf("capture walk: %s (%zu bytes)\n", path, data.size());

    constexpr size_t HDR = 128;
    size_t off = 0, msgs = 0;
    int nConfig = 0, nFrame = 0, nOther = 0;
    Bytes configBody;
    while (off + HDR <= data.size())
    {
        const uint8_t *h = data.data() + off;
        uint32_t bodySize = h[0] | (h[1] << 8) | (h[2] << 16) | ((uint32_t)h[3] << 24);
        if (off + HDR + bodySize > data.size())
            break; // partial trailing message
        uint8_t opcode = h[4];
        if (opcode == 1)
        {
            nConfig++;
            if (configBody.empty())
                configBody.assign(data.begin() + off + HDR, data.begin() + off + HDR + bodySize);
        }
        else if (opcode == 0)
            nFrame++;
        else
            nOther++;
        off += HDR + bodySize;
        msgs++;
    }
    printf("  parsed %zu messages, consumed %zu/%zu bytes (remainder %zu)\n", msgs, off, data.size(),
           data.size() - off);
    CHECK(off == data.size(), "capture parses cleanly to EOF");
    CHECK(nConfig >= 1, "at least one VideoConfig");
    CHECK(nFrame > 0, "video frames present");
    printf("  opcodes: config=%d frame=%d other=%d\n", nConfig, nFrame, nOther);

    if (!configBody.empty())
    {
        bool hevc = false;
        Bytes ps = cp_nalu::configToAnnexB(configBody.data(), configBody.size(), hevc);
        int nals = countAnnexB(ps);
        printf("  config: codec=%s -> %zuB param-sets, %d NAL(s)\n", hevc ? "HEVC" : "h264",
               ps.size(), nals);
        CHECK(!ps.empty(), "config produced parameter sets");
        CHECK(nals >= 2, "at least SPS+PPS (HEVC: VPS+SPS+PPS)");
        // First NAL type: HEVC = (byte>>1)&0x3f, expect VPS(32)/SPS(33). H.264 = byte&0x1f, SPS(7).
        if (nals >= 1 && ps.size() >= 5)
        {
            uint8_t nb = ps[4];
            if (hevc)
            {
                int t = (nb >> 1) & 0x3f;
                printf("  first NAL type=%d (HEVC 32=VPS,33=SPS,34=PPS)\n", t);
                CHECK(t == 32 || t == 33, "HEVC first param-set is VPS/SPS");
            }
            else
            {
                int t = nb & 0x1f;
                printf("  first NAL type=%d (H.264 7=SPS)\n", t);
                CHECK(t == 7, "H.264 first param-set is SPS");
            }
        }
    }
}

int main(int argc, char **argv)
{
    printf("== cp_nalu_test ==\n");
    testAvccRoundTrip();
    testCapture(argc > 1 ? argv[1] : "/tmp/cpav/stream-screen.bin");
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
