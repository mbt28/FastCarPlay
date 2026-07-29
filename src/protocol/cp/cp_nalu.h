#ifndef SRC_PROTOCOL_CP_CP_NALU
#define SRC_PROTOCOL_CP_CP_NALU

// Convert CarPlay's length-prefixed H.264/H.265 video into an Annex-B byte
// stream (00 00 00 01 start codes) that a standard decoder/parser accepts.
// CarPlay screen frames carry NAL units each prefixed with a big-endian length
// (AVCC/HVCC style); the codec config arrives as an avcC/hvcC record, optionally
// wrapped in an avc1/hvc1 sample entry. Port of LIVI cp/stack/nalu.ts.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cp_nalu
{
using Bytes = std::vector<uint8_t>;

// Rewrite a run of length-prefixed NAL units to Annex-B. `lengthSize` is 1..4.
Bytes avccFrameToAnnexB(const uint8_t *frame, size_t len, int lengthSize = 4);

// Convert a VideoConfig payload (an avcC/hvcC atom, an avc1/hvc1 sample entry
// with the record nested inside, or a bare record) into Annex-B parameter sets,
// detecting the codec from the atom. Sets `hevc` true for H.265, false for H.264.
Bytes configToAnnexB(const uint8_t *payload, size_t len, bool &hevc);

} // namespace cp_nalu

#endif /* SRC_PROTOCOL_CP_CP_NALU */
