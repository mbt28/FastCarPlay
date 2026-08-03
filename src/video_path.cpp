#include "video_path.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "common/logger.h"
#include "settings.h"

#if defined(USE_CEDAR) || defined(USE_CEDRUS)
#include "drm_display.h"
#endif

namespace video_path
{
namespace
{
// Coded formats a stateless (request-API) or stateful (M2M) decoder can accept.
// Spelled out rather than taken from videodev2.h: the *_SLICE fourccs are recent
// additions, and the F1C buildroot toolchain may predate them.
constexpr uint32_t FCC_H264_SLICE = 'S' | ('2' << 8) | ('6' << 16) | ((uint32_t)'4' << 24);
constexpr uint32_t FCC_HEVC_SLICE = 'S' | ('2' << 8) | ('6' << 16) | ((uint32_t)'5' << 24);
constexpr uint32_t FCC_H264 = 'H' | ('2' << 8) | ('6' << 16) | ((uint32_t)'4' << 24);
constexpr uint32_t FCC_HEVC = 'H' | ('E' << 8) | ('V' << 16) | ((uint32_t)'C' << 24);

// Ask one /dev/videoN which coded formats it takes on its OUTPUT queue (that is
// the queue you feed a decoder). Returns false if it is not a decoder at all.
bool queryDecoder(const char *node, bool &h264, bool &hevc, std::string &driver)
{
    int fd = ::open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    bool found = false;
    if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0)
    {
        const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                       : cap.capabilities;
        // Only mem2mem devices decode; cameras and ISPs are not interesting.
        const bool m2m = (caps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE)) != 0;
        if (m2m)
        {
            const uint32_t types[] = {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                      V4L2_BUF_TYPE_VIDEO_OUTPUT};
            for (uint32_t type : types)
            {
                for (uint32_t i = 0;; i++)
                {
                    struct v4l2_fmtdesc fmt;
                    memset(&fmt, 0, sizeof(fmt));
                    fmt.index = i;
                    fmt.type = type;
                    if (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) != 0)
                        break;
                    if (fmt.pixelformat == FCC_H264_SLICE || fmt.pixelformat == FCC_H264)
                    {
                        h264 = true;
                        found = true;
                    }
                    else if (fmt.pixelformat == FCC_HEVC_SLICE || fmt.pixelformat == FCC_HEVC)
                    {
                        hevc = true;
                        found = true;
                    }
                }
            }
            if (found)
                driver = (const char *)cap.driver;
        }
    }
    ::close(fd);
    return found;
}

// Walk /dev/video* and collect what the hardware can decode.
void scanDecoders(Caps &caps)
{
    DIR *d = opendir("/dev");
    if (!d)
        return;
    std::vector<std::string> nodes;
    for (struct dirent *e; (e = readdir(d));)
        if (strncmp(e->d_name, "video", 5) == 0)
            nodes.push_back(std::string("/dev/") + e->d_name);
    closedir(d);

    for (const std::string &n : nodes)
    {
        bool h264 = false, hevc = false;
        std::string drv;
        if (!queryDecoder(n.c_str(), h264, hevc, drv))
            continue;
        caps.hwH264 = caps.hwH264 || h264;
        caps.hwHevc = caps.hwHevc || hevc;
        if (caps.decoder.empty())
            caps.decoder = drv;
        log_d("video-path: %s (%s) decodes%s%s", n.c_str(), drv.c_str(), h264 ? " h264" : "",
              hevc ? " hevc" : "");
    }
}

// ffmpeg also has to be able to drive it: without the V4L2-request hwaccel
// compiled in (or the HW decoder class built), the kernel capability is moot.
bool ffmpegCanHw(AVCodecID id)
{
#ifdef USE_CEDRUS
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec)
        return false;
    for (int i = 0;; i++)
    {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(codec, i);
        if (!cfg)
            break;
        if (cfg->pix_fmt == AV_PIX_FMT_DRM_PRIME || cfg->device_type == AV_HWDEVICE_TYPE_DRM)
            return true;
    }
    return false;
#else
    (void)id;
    return false; // no hardware decoder compiled in
#endif
}

bool haveDesktop()
{
    const char *w = getenv("WAYLAND_DISPLAY");
    const char *x = getenv("DISPLAY");
    return (w && *w) || (x && *x);
}

// Can we become DRM master? Open and immediately release: this only answers the
// question, the real session is opened later by the render loop.
bool canDriveDrm()
{
#if defined(USE_CEDAR) || defined(USE_CEDRUS)
    if (!drm_display::open("probe"))
        return false;
    drm_display::close();
    return true;
#else
    return false;
#endif
}

Caps probe()
{
    Caps caps;
    caps.desktop = haveDesktop();

    scanDecoders(caps);
    caps.hwH264 = caps.hwH264 && ffmpegCanHw(AV_CODEC_ID_H264);
    caps.hwHevc = caps.hwHevc && ffmpegCanHw(AV_CODEC_ID_HEVC);
    const bool anyHw = caps.hwH264 || caps.hwHevc;

    // An explicit choice always wins -- for bring-up and for boards that lie.
    const std::string &forced = Settings::videoPath.value;
    if (forced == "sw")
    {
        caps.mode = Mode::Sdl;
        caps.why = "forced by video-path=sw";
        return caps;
    }
    if (forced == "headless")
    {
        caps.mode = Mode::Headless;
        caps.why = "forced by video-path=headless";
        return caps;
    }
    if (forced == "hw")
    {
        caps.drm = canDriveDrm();
        caps.mode = caps.drm ? Mode::Drm : Mode::Sdl;
        caps.why = caps.drm ? "forced by video-path=hw" : "video-path=hw but no DRM master";
        return caps;
    }

    // Auto. A desktop session owns the screen, so draw into a window there and
    // leave KMS alone -- that is also the developer's expectation.
    if (caps.desktop)
    {
        caps.mode = Mode::Sdl;
        caps.why = "desktop session present";
        return caps;
    }

    caps.drm = canDriveDrm();
    if (caps.drm)
    {
        // We own the screen, so video goes on a DRM plane either way -- whether
        // the frames come from hardware or software decode does not change that.
        caps.mode = Mode::Drm;
        caps.why = anyHw ? "DRM master + hardware decoder" : "DRM master, software decode";
    }
    else
    {
        caps.mode = Mode::Headless;
        caps.why = "no desktop and no DRM master";
    }
    return caps;
}
} // namespace

const Caps &detect()
{
    static const Caps caps = [] {
        Caps c = probe();
        std::string hw;
        if (c.hwH264)
            hw += " hw-h264";
        if (c.hwHevc)
            hw += " hw-hevc";
        if (!hw.empty() && !c.decoder.empty())
            hw += " via " + c.decoder;
        else if (hw.empty())
            hw = " software decode";
        log_i("video path: %s (%s)%s", modeName(c.mode), c.why.c_str(), hw.c_str());
        return c;
    }();
    return caps;
}

bool hwAvailable(AVCodecID codec)
{
    const Caps &c = detect();
    if (c.mode != Mode::Drm)
        return false;
    if (codec == AV_CODEC_ID_H264)
        return c.hwH264;
    if (codec == AV_CODEC_ID_HEVC)
        return c.hwHevc;
    return false;
}

AVCodecID preferredCodec()
{
    return detect().hwHevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264;
}

const char *modeName(Mode m)
{
    switch (m)
    {
    case Mode::Drm: return "drm";
    case Mode::Sdl: return "sdl";
    default: return "headless";
    }
}
} // namespace video_path
