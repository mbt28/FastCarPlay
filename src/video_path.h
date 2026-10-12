#ifndef SRC_VIDEO_PATH
#define SRC_VIDEO_PATH

// Decides once, at startup, HOW video is decoded and shown -- so the same
// binary does the right thing on any board without per-board settings.
//
// The board build owns the screen through KMS or runs headless:
//
//   Drm      V4L2-stateless decode -> DRM_PRIME -> DRM video plane (zero copy),
//            UI rendered into an RGB overlay plane above it
//   Headless no display at all (sessions still run; video is not presented)
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
    Headless,
};

struct Caps
{
    Mode mode = Mode::Headless;
    bool hwH264 = false;   // the hardware decodes H.264
    bool hwHevc = false;   // the hardware decodes HEVC
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
