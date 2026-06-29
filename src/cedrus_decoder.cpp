#include "cedrus_decoder.h"

#ifdef USE_CEDRUS

#include <cstring>
#include <cstdio>
#include <cerrno>

#include "common/logger.h"
#include "common/functions.h"
#include "settings.h"

extern "C"
{
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
}

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

// ---------------------------------------------------------------------------
// DEFE atomic display.  cedrus' CAPTURE buffer arrives as a drm_prime dma-buf
// (tiled NV12, ALLWINNER_TILED); we import it as a DRM_FORMAT_NV12 framebuffer
// and commit it via the atomic API so sun4i routes the plane through the DEFE
// front-end (HW de-tile + BT.601 CSC + scale). Requires the suniv DEFE kernel
// fix (EN bit31 cleared + FIR filter bypassed). Mirrors cedar_decoder.cpp but
// takes its planes from ffmpeg's AVDRMFrameDescriptor instead of two ION fds.
// ---------------------------------------------------------------------------
namespace
{
int drm_fd = -1;
uint32_t drm_crtc = 0, drm_plane = 0, drm_conn = 0, drm_cw = 0, drm_ch = 0;
uint32_t drm_prev_fb = 0, drm_mode_blob = 0;
drmModeModeInfo drm_mode;
bool drm_modeset_done = false;
uint32_t P_plane_fb, P_plane_crtc, P_plane_sx, P_plane_sy, P_plane_sw, P_plane_sh,
    P_plane_cx, P_plane_cy, P_plane_cw, P_plane_ch, P_crtc_mode, P_crtc_active, P_conn_crtc;

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
    if (drm_fd < 0) { perror("[Cedrus] open /dev/dri/card0"); return false; }
    drmSetMaster(drm_fd);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
    { fprintf(stderr, "[Cedrus] DRM atomic unavailable\n"); return false; }

    drmModeRes *res = drmModeGetResources(drm_fd);
    if (!res) return false;
    drmModeConnector *conn = nullptr;
    for (int i = 0; i < res->count_connectors; i++)
    {
        drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) { conn = c; break; }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "[Cedrus] no connected DRM connector\n"); return false; }
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
    if (!drm_plane) { fprintf(stderr, "[Cedrus] no NV12 plane found\n"); return false; }

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
    { fprintf(stderr, "[Cedrus] missing atomic properties\n"); return false; }
    if (drmModeCreatePropertyBlob(drm_fd, &drm_mode, sizeof(drm_mode), &drm_mode_blob))
    { perror("[Cedrus] CreatePropertyBlob"); return false; }

    fprintf(stderr, "[Cedrus] DRM crtc=%u %ux%u plane=%u (DEFE NV12 tiled, HW)\n",
            drm_crtc, drm_cw, drm_ch, drm_plane);
    return true;
}

// Present one decoded drm_prime frame. The dma-buf fd(s) are owned by ffmpeg's
// frame pool (cached, stable per CAPTURE buffer) -- do NOT close them; drmPrime
// caches a GEM handle per fd so handles stay bounded by the pool.
void drm_show(AVFrame *frame)
{
    AVDRMFrameDescriptor *d = (AVDRMFrameDescriptor *)frame->data[0];
    if (!d || d->nb_objects < 1 || d->nb_layers < 1) return;
    const AVDRMLayerDescriptor *layer = &d->layers[0];

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    int np = layer->nb_planes < 4 ? layer->nb_planes : 4;
    for (int i = 0; i < np; i++)
    {
        int oi = layer->planes[i].object_index;
        uint32_t h = 0;
        if (drmPrimeFDToHandle(drm_fd, d->objects[oi].fd, &h))
        { perror("[Cedrus] drmPrimeFDToHandle"); return; }
        handles[i] = h;
        pitches[i] = layer->planes[i].pitch;
        offsets[i] = layer->planes[i].offset;
        mods[i]    = d->objects[oi].format_modifier;
    }

    static bool logged = false;
    if (!logged)
    {
        logged = true;
        fprintf(stderr, "[Cedrus] frame %dx%d fmt=%.4s objs=%d planes=%d "
                "pitch0=%u pitch1=%u off1=%u mod=0x%llx\n",
                frame->width, frame->height, (const char *)&layer->format,
                d->nb_objects, layer->nb_planes, pitches[0], pitches[1], offsets[1],
                (unsigned long long)mods[0]);
    }

    uint32_t fb = 0;
    if (drmModeAddFB2WithModifiers(drm_fd, frame->width, frame->height, layer->format,
                                   handles, pitches, offsets, mods, &fb, DRM_MODE_FB_MODIFIERS))
    { perror("[Cedrus] AddFB2WithModifiers"); return; }

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
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sw,   (uint64_t)frame->width  << 16);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_sh,   (uint64_t)frame->height << 16);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cx,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cy,   0);
    drmModeAtomicAddProperty(req, drm_plane, P_plane_cw,   drm_cw); // scale -> panel
    drmModeAtomicAddProperty(req, drm_plane, P_plane_ch,   drm_ch);

    int crc = drmModeAtomicCommit(drm_fd, req, flags, nullptr);
    drmModeAtomicFree(req);
    if (crc)
    {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[Cedrus] atomic commit failed: %s\n", strerror(errno)); }
        drmModeRmFB(drm_fd, fb);
        return;
    }
    drm_modeset_done = true;
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
            fprintf(stderr, "[Cedrus] decode %.1f fps  %dx%d (DEFE)\n",
                    frames * 1000.0 / (now - t0), frame->width, frame->height);
        frames = 0;
        t0 = now;
    }
}

void drm_close()
{
    if (drm_prev_fb) { drmModeRmFB(drm_fd, drm_prev_fb); drm_prev_fb = 0; }
    if (drm_mode_blob) { drmModeDestroyPropertyBlob(drm_fd, drm_mode_blob); drm_mode_blob = 0; }
    if (drm_fd >= 0) { close(drm_fd); drm_fd = -1; }
    drm_modeset_done = false;
}

// get_format: select the V4L2-Request (DRM_PRIME) hwaccel when offered.
enum AVPixelFormat get_drm_format(AVCodecContext *, const enum AVPixelFormat *fmts)
{
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    return fmts[0];
}
} // namespace

CedrusDecoder::CedrusDecoder()
    : _active(false), _data(nullptr), _ctx(nullptr), _parser(nullptr), _hwdev(nullptr)
{
}

CedrusDecoder::~CedrusDecoder()
{
    stop();
}

void CedrusDecoder::start(AtomicQueue<Message> *data, AVCodecID codecId)
{
    if (_active)
        stop();
    buffer.reset();
    _data = data;
    _codecId = codecId;
    _active = true;
    _thread = std::thread(&CedrusDecoder::runner, this);
}

void CedrusDecoder::stop()
{
    if (!_active)
        return;
    _active = false;
    if (_data)
        _data->notify();
    if (_thread.joinable())
        _thread.join();
}

void CedrusDecoder::flush()
{
    if (_ctx)
        avcodec_flush_buffers(_ctx);
}

bool CedrusDecoder::setup(AVCodecID codecId)
{
    if (!drm_open())
        return false;

    const AVCodec *codec = avcodec_find_decoder(codecId);
    if (!codec)
    {
        log_e("[Cedrus] no decoder for codec %s", avcodec_get_name(codecId));
        return false;
    }

    _ctx = avcodec_alloc_context3(codec);
    if (!_ctx)
        return false;

    // V4L2 Request hwaccel is a DRM-typed hwdevice; get_format selects DRM_PRIME
    // so the in-kernel cedrus decoder is used and frames come back as dma-bufs.
    if (av_hwdevice_ctx_create(&_hwdev, AV_HWDEVICE_TYPE_DRM, nullptr, nullptr, 0) < 0)
    {
        log_e("[Cedrus] can't create DRM hwdevice (v4l2-request)");
        return false;
    }
    _ctx->hw_device_ctx = av_buffer_ref(_hwdev);
    _ctx->get_format = get_drm_format;
    if (Settings::codecLowDelay)
        _ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (Settings::codecFast)
        _ctx->flags2 |= AV_CODEC_FLAG2_FAST;

    int ret = avcodec_open2(_ctx, codec, nullptr);
    if (ret < 0)
    {
        log_e("[Cedrus] avcodec_open2 failed: %s", avErrorText(ret).c_str());
        return false;
    }

    _parser = av_parser_init(codecId);
    if (!_parser)
    {
        log_e("[Cedrus] can't init parser for %s", avcodec_get_name(codecId));
        return false;
    }

    log_i("[Cedrus] ffmpeg v4l2-request hwaccel ready (%s -> cedrus -> DEFE)", codec->name);
    return true;
}

void CedrusDecoder::teardown()
{
    if (_parser) { av_parser_close(_parser); _parser = nullptr; }
    if (_ctx) avcodec_free_context(&_ctx);
    if (_hwdev) av_buffer_unref(&_hwdev);
    drm_close();
}

void CedrusDecoder::runner()
{
    setThreadName("cedrus-decoder");

    if (!setup(_codecId))
    {
        teardown();
        return;
    }

    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    if (packet && frame)
        loop(packet, frame);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);

    teardown();
}

void CedrusDecoder::loop(AVPacket *packet, AVFrame *frame)
{
    while (_data->wait(_active))
    {
        std::unique_ptr<Message> segment = _data->pop();
        uint8_t *data_ptr = segment->data();
        int data_size = segment->length();

        while (_active && data_size > 0)
        {
            uint8_t *pk_data;
            int pk_size;
            int len = av_parser_parse2(_parser, _ctx, &pk_data, &pk_size,
                                       data_ptr, data_size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (len < 0)
                break;
            data_ptr += len;
            data_size -= len;
            if (pk_size <= 0)
                continue;

            av_packet_unref(packet);
            packet->data = pk_data;
            packet->size = pk_size;

            int send_ret = avcodec_send_packet(_ctx, packet);
            if (send_ret != 0)
            {
                log_w("[Cedrus] can't decode packet > %s", avErrorText(send_ret).c_str());
                continue;
            }
            while (avcodec_receive_frame(_ctx, frame) == 0 && _active)
            {
                if (frame->format == AV_PIX_FMT_DRM_PRIME)
                    drm_show(frame);
                av_frame_unref(frame);
            }
        }
    }
}

#endif /* USE_CEDRUS */
