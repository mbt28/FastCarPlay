#include "cedar_decoder.h"

// The whole implementation is compiled only for the Cedar/F1C200s build; on
// other platforms this translation unit is empty and the avcodec Decoder is used.
#ifdef USE_CEDAR

#include <cstring>
#include <cstdio>
#include <cerrno>
#include <memory>

#include "common/logger.h"
#include "common/functions.h"
#include "settings.h"

extern "C"
{
#include "vdecoder.h"        // CreateVideoDecoder, RequestPicture, VideoPicture, ...
#include "memoryAdapter.h"   // MemAdapterGetOpsS
#include "sc_interface.h"    // CdcMemOpen / CdcMemClose
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
}

#include <drm_fourcc.h>

#include "drm_display.h"

// ---- direct /dev/fb0 output ----------------------------------------------
// This board has no GL/SDL display path, so decoded frames are de-tiled
// (MB32 YUV420 -> RGB) straight onto the framebuffer, centered. Same proven
// path as cedar-decode-test --fb (~60 fps for 320x240).
namespace {
int fb_fd = -1;
uint8_t *fb_mem = nullptr;
struct fb_var_screeninfo fb_var;
struct fb_fix_screeninfo fb_fix;
uint8_t rowY[2048], rowU[1024], rowV[1024];

inline uint8_t clip8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

bool fb_open()
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) return false;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &fb_var) ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &fb_fix))
        return false;
    fb_mem = (uint8_t *)mmap(nullptr, fb_fix.smem_len, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fb_fd, 0);
    if (fb_mem == MAP_FAILED) { fb_mem = nullptr; return false; }
    return true;
}

void fb_show(VideoPicture *p)
{
    if (!fb_mem) return;
    const int W = p->nWidth, H = p->nHeight;
    const uint8_t *Yp = (const uint8_t *)p->pData0;
    const uint8_t *Cp = (const uint8_t *)p->pData1;
    const int mbW = ((W + 31) & ~31) / 32;
    const int cw = (W + 1) / 2;
    const int dispW = (p->nRightOffset  > 0 && p->nRightOffset  <= W) ? p->nRightOffset  : W;
    const int dispH = (p->nBottomOffset > 0 && p->nBottomOffset <= H) ? p->nBottomOffset : H;
    const int bpp = fb_var.bits_per_pixel;
    int ox = ((int)fb_var.xres - dispW) / 2; if (ox < 0) ox = 0;
    int oy = ((int)fb_var.yres - dispH) / 2; if (oy < 0) oy = 0;
    int last_cy = -1;

    for (int y = 0; y < dispH && (oy + y) < (int)fb_var.yres; y++) {
        int my = y / 32, ly = y % 32;
        for (int mx = 0; mx < mbW; mx++)
            memcpy(rowY + mx * 32, Yp + ((long)(my * mbW + mx)) * 1024 + ly * 32, 32);
        int cy = y / 2;
        if (cy != last_cy) {
            int cmy = cy / 32, cly = cy % 32;
            for (int mx = 0; mx < mbW; mx++) {
                uint8_t t[32];
                memcpy(t, Cp + ((long)(cmy * mbW + mx)) * 1024 + cly * 32, 32);
                for (int k = 0; k < 16; k++) { rowU[mx * 16 + k] = t[2 * k]; rowV[mx * 16 + k] = t[2 * k + 1]; }
            }
            last_cy = cy;
        }
        uint8_t *row = fb_mem + (long)(oy + y) * fb_fix.line_length + (long)ox * (bpp / 8);
        for (int x = 0; x < dispW && (ox + x) < (int)fb_var.xres; x++) {
            int cidx = x >> 1; if (cidx >= cw) cidx = cw - 1;
            int Yv = rowY[x], D = rowU[cidx] - 128, E = rowV[cidx] - 128;
            uint8_t r = clip8(Yv + ((1436 * E) >> 10));
            uint8_t g = clip8(Yv - ((352 * D + 731 * E) >> 10));
            uint8_t b = clip8(Yv + ((1814 * D) >> 10));
            if (bpp == 16)
                ((uint16_t *)row)[x] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
            else { row[x * 4 + 0] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 0xff; }
        }
    }

    // decode/display frame-rate, logged ~every 2 s
    static int frames = 0;
    static int64_t t0 = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (t0 == 0) t0 = now;
    if (++frames >= 60 || now - t0 >= 2000)
    {
        if (now > t0)
            fprintf(stderr, "[Cedar] decode %.1f fps  %dx%d\n",
                    frames * 1000.0 / (now - t0), dispW, dispH);
        frames = 0;
        t0 = now;
    }
}

// ---- DEFE front-end NV12+tiled plane (renderer = drm) --------------------
// Zero-copy hardware path: Cedar's two ION buffers (Y + interleaved-UV, both
// MB32 tiled) are imported as a DRM_FORMAT_NV12 + DRM_FORMAT_MOD_ALLWINNER_TILED
// framebuffer and committed via the atomic API, which routes the plane through
// the DEFE front-end -> HW de-tile + BT.601 CSC + scale, no CPU per-pixel work.
// Requires the suniv DEFE kernel fix (EN bit31 cleared + FIR filter bypassed).
extern "C" int ion_alloc_get_dmabuf_fd(void *vir_addr);

bool use_drm = false;

// Present one MB32-tiled picture on the shared DRM video plane: Cedar's two
// ION buffers (Y + interleaved UV) import as one NV12 + ALLWINNER_TILED fb;
// drm_display routes it through the DEFE (HW de-tile + CSC + scale) and the
// UI overlay shares the same DRM session.
void drm_show(VideoPicture *p)
{
    int fds[2] = {ion_alloc_get_dmabuf_fd(p->pData0), ion_alloc_get_dmabuf_fd(p->pData1)};
    if (fds[0] < 0 || fds[1] < 0) return;

    const int W = p->nWidth, H = p->nHeight;
    uint32_t pitch = (W + 31) & ~31;
    uint32_t pitches[2] = {pitch, pitch};
    uint32_t offsets[2] = {0, 0};
    const int dispW = (p->nRightOffset  > 0 && p->nRightOffset  <= W) ? p->nRightOffset  : W;
    const int dispH = (p->nBottomOffset > 0 && p->nBottomOffset <= H) ? p->nBottomOffset : H;

    drm_display::showVideo(DRM_FORMAT_NV12, W, H, dispW, dispH, 2, fds,
                           pitches, offsets, DRM_FORMAT_MOD_ALLWINNER_TILED, "Cedar");
}
} // namespace

CedarDecoder::CedarDecoder()
    : _active(false),
      _data(nullptr),
      _dec(nullptr),
      _memops(nullptr)
{
}

CedarDecoder::~CedarDecoder()
{
    stop();
}

void CedarDecoder::start(AtomicQueue<Message> *data, AVCodecID codecId)
{
    (void)codecId; // always H.264 on this path
    if (_active)
        stop();

    buffer.reset();
    _data = data;
    _active = true;
    _thread = std::thread(&CedarDecoder::runner, this);
}

void CedarDecoder::stop()
{
    if (!_active)
        return;
    _active = false;
    _data->notify();
    if (_thread.joinable())
        _thread.join();
}

void CedarDecoder::flush()
{
    if (_dec)
        ResetVideoDecoder(static_cast<VideoDecoder *>(_dec));
}

bool CedarDecoder::setup()
{
    AddVDPlugin();
    VideoDecoder *dec = CreateVideoDecoder();
    if (!dec)
    {
        log_e("[Cedar] CreateVideoDecoder failed");
        return false;
    }
    _dec = dec;

    VideoStreamInfo si;
    memset(&si, 0, sizeof(si));
    si.eCodecFormat = VIDEO_CODEC_FORMAT_H264;

    VConfig vc;
    memset(&vc, 0, sizeof(vc));
    vc.eOutputPixelFormat = PIXEL_FORMAT_YUV_MB32_420; // the VE's only output here
    vc.bDispErrorFrame = 1;
    // Reserve extra frame buffers beyond the H.264 DPB minimum, otherwise the
    // decoder runs out (H264HoldFrameBuffer error) once fb_show holds one for
    // display: nDisplayHolding covers our held frame, nDecodeSmooth gives slack.
    vc.nDisplayHoldingFrameBufferNum = 2;
    vc.nDecodeSmoothFrameBufferNum = 2;
    vc.memops = MemAdapterGetOpsS();
    if (!vc.memops)
    {
        log_e("[Cedar] MemAdapterGetOpsS failed (is /dev/ion present?)");
        return false;
    }
    _memops = vc.memops;
    CdcMemOpen(vc.memops);

    if (InitializeVideoDecoder(dec, &si, &vc) != 0)
    {
        log_e("[Cedar] InitializeVideoDecoder failed");
        return false;
    }
    if (Settings::renderer.value == "drm")
    {
        use_drm = drm_display::open("Cedar");
        if (!use_drm)
            fprintf(stderr, "[Cedar] DRM DEFE plane unavailable; falling back to /dev/fb0\n");
    }
    if (!use_drm && !fb_open())
        fprintf(stderr, "[Cedar] /dev/fb0 open failed; decoding without display\n");
    log_i("[Cedar] HW H.264 decoder ready");
    return true;
}

void CedarDecoder::teardown()
{
    if (use_drm)
    {
        drm_display::close();
        use_drm = false;
    }
    if (_dec)
    {
        DestroyVideoDecoder(static_cast<VideoDecoder *>(_dec));
        _dec = nullptr;
    }
    if (_memops)
    {
        CdcMemClose(static_cast<struct ScMemOpsS *>(_memops));
        _memops = nullptr;
    }
}

// Submit one complete H.264 access unit and pump the decoder.
void CedarDecoder::feed(const uint8_t *data, int len)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(_dec);
    char *b0, *b1;
    int s0, s1;

    if (len <= 0)
        return;
    if (RequestVideoStreamBuffer(dec, len, &b0, &s0, &b1, &s1, 0) != 0 || (s0 + s1) < len)
        return;
    if (len <= s0)
        memcpy(b0, data, len);
    else
    {
        memcpy(b0, data, s0);
        memcpy(b1, data + s0, len - s0);
    }

    VideoStreamDataInfo di;
    memset(&di, 0, sizeof(di));
    di.pData = b0;
    di.nLength = len;
    di.bIsFirstPart = 1;
    di.bIsLastPart = 1;
    di.bValid = 1;
    SubmitVideoStreamData(dec, &di, 0);

    for (;;)
    {
        int r = DecodeVideoStream(dec, 0, 0, 0, 0);
        if (r == VDECODE_RESULT_FRAME_DECODED ||
            r == VDECODE_RESULT_KEYFRAME_DECODED ||
            r == VDECODE_RESULT_OK)
            break;
        if (r == VDECODE_RESULT_RESOLUTION_CHANGE)
            continue;
        break; // NO_FRAME_BUFFER / NO_BITSTREAM / UNSUPPORTED -> drain frees buffers
    }
}

// Paint every ready MB32 picture straight to /dev/fb0 (no SDL on this board).
void CedarDecoder::drain(uint32_t &counter)
{
    VideoDecoder *dec = static_cast<VideoDecoder *>(_dec);
    VideoPicture *p;

    while (_active && (p = RequestPicture(dec, 0)) != nullptr)
    {
        if (use_drm)
            drm_show(p);
        else
            fb_show(p);
        counter++;
        ReturnPicture(dec, p);
    }
}

void CedarDecoder::runner()
{
    setThreadName("cedar-decoder");

    if (!setup())
    {
        teardown();
        return;
    }

    // The CarPlay dongle delivers one complete H.264 access unit per video
    // message (Annex-B, with start codes), so each message is fed straight to the
    // Frame-SBM. This drops ffmpeg's H.264 parser entirely -- no per-byte parse /
    // slice-header scan on every frame.
    uint32_t counter = 0;
    while (_data->wait(_active))
    {
        std::unique_ptr<Message> segment = _data->pop();
        if (segment->length() > 0)
        {
            feed(segment->data(), segment->length());
            drain(counter);
        }
    }

    teardown();
}

#endif /* USE_CEDAR */
