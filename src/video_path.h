#ifndef SRC_VIDEO_PATH
#define SRC_VIDEO_PATH

// Decides once, at startup, HOW video is decoded and shown -- so the same
// binary does the right thing on any board without per-board settings.
//
// The decode and display choices are NOT independent, so they are one decision:
// a hardware decoder hands back an AV_PIX_FMT_DRM_PRIME dma-buf, which is only
// worth having if we can hand it straight to a DRM plane (the display engine
// then de-tiles, colour-converts and scales it for free). Showing it through an
// SDL texture instead would mean copying every frame out of (uncached) CMA and
// converting on the CPU -- which erases the point of hardware decode on a small
// SoC. So: Hw implies the DRM plane, Sw implies the SDL texture.
//
//   Hw       V4L2-stateless decode -> DRM_PRIME -> DRM video plane (zero copy),
//            UI drawn by SDL into an ARGB overlay plane above it
//   Sw       libavcodec software decode -> SDL texture in a window (desktop)
//   Headless decode with no display at all
//
// Hardware capability is read from the kernel, not guessed: each V4L2 decoder
// node is asked which coded formats it accepts (VIDIOC_ENUM_FMT on the OUTPUT
// queue). That is what distinguishes boards -- an F1C200s' cedrus reports H.264
// slices only, a Pi5's rpi-hevc-dec reports HEVC slices only -- and it is the
// one check ffmpeg's own tables cannot answer, because avcodec_get_hw_config()
// only reports what ffmpeg was COMPILED with, not what this chip contains.

extern "C"
{
#include <libavcodec/avcodec.h> // AVCodecID
}

#include <string>

namespace video_path
{
enum class Mode
{
    Drm,      // we own KMS: video on a DRM plane, UI on an overlay plane
    Sdl,      // a desktop session: video in an SDL window
    Headless,
};

struct Caps
{
    Mode mode = Mode::Sdl;
    bool hwH264 = false;   // the hardware decodes H.264
    bool hwHevc = false;   // the hardware decodes HEVC
    bool desktop = false;  // a compositor/X session owns the screen
    bool drm = false;      // we can become DRM master (so we can drive a plane)
    std::string decoder;   // V4L2 driver behind the hardware path, for logging
    std::string why;       // one line explaining the choice
};

// Probe once and cache. Safe to call from anywhere, any number of times.
const Caps &detect();

// Is there hardware decode for this codec on this board?
bool hwAvailable(AVCodecID codec);

// The codec to ask a phone for. HEVC only when the hardware actually decodes
// it; H.264 otherwise, being the universally cheap choice (it is also what a
// software decoder handles most easily).
AVCodecID preferredCodec();

const char *modeName(Mode m);
} // namespace video_path

#endif /* SRC_VIDEO_PATH */
