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

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

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
int drm_fd = -1;
uint32_t drm_crtc = 0, drm_plane = 0, drm_conn = 0, drm_cw = 0, drm_ch = 0;
uint32_t drm_prev_fb = 0, drm_mode_blob = 0;
drmModeModeInfo drm_mode;
bool drm_modeset_done = false;
uint32_t P_plane_fb, P_plane_crtc, P_plane_sx, P_plane_sy, P_plane_sw, P_plane_sh,
    P_plane_cx, P_plane_cy, P_plane_cw, P_plane_ch, P_crtc_mode, P_crtc_active, P_conn_crtc;

// Resolve a property id by name on a DRM object (plane/crtc/connector).
uint32_t prop_id(uint32_t obj_id, uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, obj_id, obj_type);
    uint32_t id = 0;
    if (props)
    {
        for (uint32_t i = 0; i < props->count_props && !id; i++)
        {
            drmModePropertyRes *pr = drmModeGetProperty(drm_fd, props->props[i]);
            if (pr)
            {
                if (!strcmp(pr->name, name)) id = pr->prop_id;
                drmModeFreeProperty(pr);
            }
        }
        drmModeFreeObjectProperties(props);
    }
    return id;
}

uint64_t plane_type(uint32_t plane_id)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(drm_fd, plane_id, DRM_MODE_OBJECT_PLANE);
    uint64_t type = (uint64_t)-1;
    if (props)
    {
        for (uint32_t i = 0; i < props->count_props; i++)
        {
            drmModePropertyRes *pr = drmModeGetProperty(drm_fd, props->props[i]);
            if (pr)
            {
                if (!strcmp(pr->name, "type")) type = props->prop_values[i];
                drmModeFreeProperty(pr);
            }
        }
        drmModeFreeObjectProperties(props);
    }
    return type;
}

bool drm_open()
{
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("[Cedar] open /dev/dri/card0"); return false; }
    drmSetMaster(drm_fd);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    // The tiled plane only routes through the DEFE front-end via the atomic API
    // (sun4i decides in atomic_check); legacy SetPlane lands on the back-end.
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
    { fprintf(stderr, "[Cedar] DRM atomic unavailable\n"); return false; }

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) return false;
    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++)
    {
        drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "[Cedar] no connected DRM connector\n"); return false; }
    drm_conn = conn->connector_id;
    drm_mode = conn->modes[0];

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, conn->encoder_id);
    drm_crtc = enc ? enc->crtc_id : (res->count_crtcs ? res->crtcs[0] : 0);
    drmModeCrtc *crtc = drmModeGetCrtc(drm_fd, drm_crtc);
    drm_cw = (crtc && crtc->mode.hdisplay) ? crtc->mode.hdisplay : drm_mode.hdisplay;
    drm_ch = (crtc && crtc->mode.vdisplay) ? crtc->mode.vdisplay : drm_mode.vdisplay;

    int crtc_idx = 0;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == drm_crtc) { crtc_idx = i; break; }

    // Prefer the primary plane (clean full modeset) that can take NV12.
    uint32_t fallback = 0;
    drmModePlaneRes *prr = drmModeGetPlaneResources(drm_fd);
    for (uint32_t i = 0; i < prr->count_planes && !drm_plane; i++)
    {
        drmModePlane *pl = drmModeGetPlane(drm_fd, prr->planes[i]);
        if (pl && (pl->possible_crtcs & (1 << crtc_idx)))
        {
            bool ok = false;
            for (uint32_t f = 0; f < pl->count_formats; f++)
                if (pl->formats[f] == DRM_FORMAT_NV12) { ok = true; break; }
            if (ok)
            {
                if (!fallback) fallback = pl->plane_id;
                if (plane_type(pl->plane_id) == DRM_PLANE_TYPE_PRIMARY) drm_plane = pl->plane_id;
            }
        }
        if (pl) drmModeFreePlane(pl);
    }
    if (!drm_plane) drm_plane = fallback;
    if (!drm_plane) { fprintf(stderr, "[Cedar] no NV12 plane found\n"); return false; }

    P_plane_fb    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    P_plane_crtc  = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    P_plane_sx    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    P_plane_sy    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    P_plane_sw    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    P_plane_sh    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    P_plane_cx    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    P_plane_cy    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    P_plane_cw    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    P_plane_ch    = prop_id(drm_plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    P_crtc_mode   = prop_id(drm_crtc,  DRM_MODE_OBJECT_CRTC,  "MODE_ID");
    P_crtc_active = prop_id(drm_crtc,  DRM_MODE_OBJECT_CRTC,  "ACTIVE");
    P_conn_crtc   = prop_id(drm_conn,  DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    if (!P_plane_fb || !P_plane_crtc || !P_crtc_mode || !P_crtc_active || !P_conn_crtc)
    { fprintf(stderr, "[Cedar] missing atomic properties\n"); return false; }
    if (drmModeCreatePropertyBlob(drm_fd, &drm_mode, sizeof(drm_mode), &drm_mode_blob))
    { perror("[Cedar] CreatePropertyBlob"); return false; }

    fprintf(stderr, "[Cedar] DRM crtc=%u %ux%u plane=%u (DEFE NV12 tiled, HW)\n",
            drm_crtc, drm_cw, drm_ch, drm_plane);
    return true;
}

void drm_show(VideoPicture *p)
{
    // Import Cedar's Y and UV ION buffers (separate dmabufs) as one tiled NV12 fb.
    // ion_alloc_get_dmabuf_fd() returns each buffer's CACHED dma-buf fd (owned by
    // the ion allocator, one per pool buffer) -- it must NOT be closed. drmPrime
    // also caches a GEM handle per dmabuf, so both stay bounded to the frame pool.
    uint32_t hy = 0, hc = 0;
    int fy = ion_alloc_get_dmabuf_fd(p->pData0);
    int fc = ion_alloc_get_dmabuf_fd(p->pData1);
    if (fy < 0 || fc < 0) return;
    if (drmPrimeFDToHandle(drm_fd, fy, &hy) || drmPrimeFDToHandle(drm_fd, fc, &hc))
    { perror("[Cedar] drmPrimeFDToHandle"); return; }

    const int W = p->nWidth, H = p->nHeight;
    uint32_t pitch = (W + 31) & ~31;
    uint32_t handles[4] = {hy, hc, 0, 0};
    uint32_t pitches[4] = {pitch, pitch, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    uint64_t mods[4] = {DRM_FORMAT_MOD_ALLWINNER_TILED, DRM_FORMAT_MOD_ALLWINNER_TILED, 0, 0};
    uint32_t fb = 0;
    if (drmModeAddFB2WithModifiers(drm_fd, W, H, DRM_FORMAT_NV12, handles, pitches, offsets,
                                   mods, &fb, DRM_MODE_FB_MODIFIERS))
    { perror("[Cedar] AddFB2WithModifiers"); return; }

    const int dispW = (p->nRightOffset  > 0 && p->nRightOffset  <= W) ? p->nRightOffset  : W;
    const int dispH = (p->nBottomOffset > 0 && p->nBottomOffset <= H) ? p->nBottomOffset : H;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = 0;
    if (!drm_modeset_done)
    {
        drmModeAtomicAddProperty(req, drm_conn, P_conn_crtc,  drm_crtc);
        drmModeAtomicAddProperty(req, drm_crtc, P_crtc_mode,  drm_mode_blob);
        drmModeAtomicAddProperty(req, drm_crtc, P_crtc_active, 1);
        flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    drmModeAtomicAddProperty(req, drm_plane, P_plane_fb,   fb);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_crtc, drm_crtc);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sx,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sy,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sw,   (uint64_t)dispW << 16);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sh,   (uint64_t)dispH << 16);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cx,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cy,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cw,   drm_cw);  // scale crop -> panel
    drmModeAtomicAddProperty(req, drm_plane, P_plane_ch,   drm_ch);

    int crc = drmModeAtomicCommit(drm_fd, req, flags, nullptr);
    drmModeAtomicFree(req);
    if (crc)
    {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[Cedar] atomic commit failed: %s\n", strerror(errno)); }
        drmModeRmFB(drm_fd, fb);
        return;
    }
    drm_modeset_done = true;
    // Retire the previous frame's fb (a new fb is created each frame); the GEM
    // handles are cached per dmabuf by drmPrime, so they don't accumulate.
    if (drm_prev_fb) drmModeRmFB(drm_fd, drm_prev_fb);
    drm_prev_fb = fb;

    static int frames = 0;
    static int64_t t0 = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (t0 == 0) t0 = now;
    if (++frames >= 60 || now - t0 >= 2000)
    {
        if (now > t0)
            fprintf(stderr, "[Cedar] decode %.1f fps  %dx%d (DEFE)\n",
                    frames * 1000.0 / (now - t0), dispW, dispH);
        frames = 0;
        t0 = now;
    }
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
        use_drm = drm_open();
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
