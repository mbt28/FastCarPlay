#include "drm_display.h"

#if defined(USE_CEDAR) || defined(USE_CEDRUS)

#include <cstdio>
#include <cstring>
#include <cerrno>
#include <map>
#include <mutex>

extern "C"
{
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/mman.h>
}

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

namespace drm_display
{
namespace
{
std::mutex mtx;          // guards everything below + all atomic commits
int refcount = 0;

int fd = -1;
uint32_t conn = 0, crtc = 0, cw = 0, ch = 0;
uint32_t mode_blob = 0;
drmModeModeInfo mode;
bool modeset_done = false;

// video plane (primary, NV12 -> DEFE)
uint32_t vplane = 0;
// Decoded-frame DRM framebuffers, cached by dma-buf fd. The decoder's frame
// pool cycles a small, stable set of fds, so we build each framebuffer once and
// reuse it instead of AddFB2/RmFB every frame (costly on the ARM926).
std::map<int, uint32_t> vfbCache;
uint32_t vframes = 0;
struct PlaneProps
{
    uint32_t fb, crtc, sx, sy, sw, sh, cx, cy, cw, ch, zpos;
} vp{}, up{};
uint32_t P_crtc_mode = 0, P_crtc_active = 0, P_conn_crtc = 0;

// UI overlay plane (ARGB8888) + double-buffered dumb framebuffers
uint32_t uplane = 0;
struct DumbFb
{
    uint32_t handle = 0, pitch = 0, fb = 0;
    uint64_t size = 0;
    uint8_t *map = nullptr;
};
DumbFb ubuf[2];
int uback = 0;
bool ui_ready = false;
bool ui_visible = false;
SDL_Surface *usurface = nullptr;
SDL_Renderer *urenderer = nullptr;

// black fallback fb for the video plane: some drivers refuse a CRTC with no
// primary plane, so a UI-only modeset (home screen before any video) may need
// the primary enabled with something.
DumbFb blackfb;

uint32_t prop_id(uint32_t obj_id, uint32_t obj_type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    uint32_t id = 0;
    if (props)
    {
        for (uint32_t i = 0; i < props->count_props && !id; i++)
        {
            drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[i]);
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
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    uint64_t type = (uint64_t)-1;
    if (props)
    {
        for (uint32_t i = 0; i < props->count_props; i++)
        {
            drmModePropertyRes *pr = drmModeGetProperty(fd, props->props[i]);
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

bool plane_has_format(drmModePlane *pl, uint32_t fmt)
{
    for (uint32_t f = 0; f < pl->count_formats; f++)
        if (pl->formats[f] == fmt)
            return true;
    return false;
}

void get_plane_props(uint32_t plane, PlaneProps &p)
{
    p.fb   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p.crtc = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p.sx   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p.sy   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p.sw   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p.sh   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p.cx   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p.cy   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p.cw   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p.ch   = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    p.zpos = prop_id(plane, DRM_MODE_OBJECT_PLANE, "zpos"); // optional
}

// Append the initial connector/CRTC/mode setup if it hasn't happened yet.
// Returns the commit flags to use. Caller holds the mutex.
uint32_t add_modeset(drmModeAtomicReq *req)
{
    if (modeset_done)
        return 0;
    drmModeAtomicAddProperty(req, conn, P_conn_crtc, crtc);
    drmModeAtomicAddProperty(req, crtc, P_crtc_mode, mode_blob);
    drmModeAtomicAddProperty(req, crtc, P_crtc_active, 1);
    return DRM_MODE_ATOMIC_ALLOW_MODESET;
}

void add_plane_fullscreen(drmModeAtomicReq *req, uint32_t plane,
                          const PlaneProps &p, uint32_t fb, int srcW, int srcH)
{
    drmModeAtomicAddProperty(req, plane, p.fb, fb);
    drmModeAtomicAddProperty(req, plane, p.crtc, crtc);
    drmModeAtomicAddProperty(req, plane, p.sx, 0);
    drmModeAtomicAddProperty(req, plane, p.sy, 0);
    drmModeAtomicAddProperty(req, plane, p.sw, (uint64_t)srcW << 16);
    drmModeAtomicAddProperty(req, plane, p.sh, (uint64_t)srcH << 16);
    drmModeAtomicAddProperty(req, plane, p.cx, 0);
    drmModeAtomicAddProperty(req, plane, p.cy, 0);
    drmModeAtomicAddProperty(req, plane, p.cw, cw);
    drmModeAtomicAddProperty(req, plane, p.ch, ch);
}

bool create_dumb(DumbFb &b, uint32_t fmt)
{
    drm_mode_create_dumb creq{};
    creq.width = cw;
    creq.height = ch;
    creq.bpp = 32;
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq))
    { perror("[Drm] CREATE_DUMB"); return false; }
    b.handle = creq.handle;
    b.pitch = creq.pitch;
    b.size = creq.size;

    drm_mode_map_dumb mreq{};
    mreq.handle = b.handle;
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq))
    { perror("[Drm] MAP_DUMB"); return false; }
    b.map = (uint8_t *)mmap(nullptr, b.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mreq.offset);
    if (b.map == MAP_FAILED) { b.map = nullptr; perror("[Drm] mmap dumb"); return false; }
    memset(b.map, 0, b.size);

    uint32_t handles[4] = {b.handle, 0, 0, 0};
    uint32_t pitches[4] = {b.pitch, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(fd, cw, ch, fmt, handles, pitches, offsets, &b.fb, 0))
    { perror("[Drm] AddFB2 dumb"); return false; }
    return true;
}

void destroy_dumb(DumbFb &b)
{
    if (b.fb) { drmModeRmFB(fd, b.fb); b.fb = 0; }
    if (b.map) { munmap(b.map, b.size); b.map = nullptr; }
    if (b.handle)
    {
        drm_mode_destroy_dumb dreq{};
        dreq.handle = b.handle;
        drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        b.handle = 0;
    }
}

bool session_open(const char *tag)
{
    fd = ::open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "[%s] open /dev/dri/card0: %s\n", tag, strerror(errno)); return false; }
    drmSetMaster(fd);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    // The tiled plane only routes through the DEFE front-end via the atomic
    // API (sun4i decides in atomic_check); legacy SetPlane lands on the DEBE.
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
    { fprintf(stderr, "[%s] DRM atomic unavailable\n", tag); return false; }

    drmModeRes *res = drmModeGetResources(fd);
    if (!res) return false;
    drmModeConnector *c = nullptr;
    for (int i = 0; i < res->count_connectors && !c; i++)
    {
        drmModeConnector *ci = drmModeGetConnector(fd, res->connectors[i]);
        if (ci && ci->connection == DRM_MODE_CONNECTED && ci->count_modes > 0) { c = ci; break; }
        if (ci) drmModeFreeConnector(ci);
    }
    if (!c) { fprintf(stderr, "[%s] no connected DRM connector\n", tag); drmModeFreeResources(res); return false; }
    conn = c->connector_id;
    mode = c->modes[0];

    drmModeEncoder *enc = drmModeGetEncoder(fd, c->encoder_id);
    crtc = enc ? enc->crtc_id : (res->count_crtcs ? res->crtcs[0] : 0);
    if (enc) drmModeFreeEncoder(enc);
    drmModeCrtc *cr = drmModeGetCrtc(fd, crtc);
    cw = (cr && cr->mode.hdisplay) ? cr->mode.hdisplay : mode.hdisplay;
    ch = (cr && cr->mode.vdisplay) ? cr->mode.vdisplay : mode.vdisplay;
    if (cr) drmModeFreeCrtc(cr);

    int crtc_idx = 0;
    for (int i = 0; i < res->count_crtcs; i++)
        if (res->crtcs[i] == crtc) { crtc_idx = i; break; }
    drmModeFreeConnector(c);
    drmModeFreeResources(res);

    // Video plane: prefer the primary that takes NV12 (DEFE-routed).
    // UI plane: the first non-primary overlay that takes ARGB8888.
    uint32_t vfallback = 0;
    drmModePlaneRes *prr = drmModeGetPlaneResources(fd);
    if (!prr) return false;
    for (uint32_t i = 0; i < prr->count_planes; i++)
    {
        drmModePlane *pl = drmModeGetPlane(fd, prr->planes[i]);
        if (!pl) continue;
        if (pl->possible_crtcs & (1u << crtc_idx))
        {
            uint64_t type = plane_type(pl->plane_id);
            if (!vplane && plane_has_format(pl, DRM_FORMAT_NV12))
            {
                if (!vfallback) vfallback = pl->plane_id;
                if (type == DRM_PLANE_TYPE_PRIMARY) vplane = pl->plane_id;
            }
            if (!uplane && type == DRM_PLANE_TYPE_OVERLAY &&
                plane_has_format(pl, DRM_FORMAT_ARGB8888))
                uplane = pl->plane_id;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(prr);
    if (!vplane) vplane = vfallback;
    if (!vplane) { fprintf(stderr, "[%s] no NV12 plane found\n", tag); return false; }
    if (uplane == vplane) uplane = 0;

    get_plane_props(vplane, vp);
    if (uplane) get_plane_props(uplane, up);
    P_crtc_mode   = prop_id(crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    P_crtc_active = prop_id(crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    P_conn_crtc   = prop_id(conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    if (!vp.fb || !vp.crtc || !P_crtc_mode || !P_crtc_active || !P_conn_crtc)
    { fprintf(stderr, "[%s] missing atomic properties\n", tag); return false; }
    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob))
    { fprintf(stderr, "[%s] CreatePropertyBlob: %s\n", tag, strerror(errno)); return false; }

    fprintf(stderr, "[Drm] crtc=%u %ux%u video-plane=%u ui-plane=%u\n",
            crtc, cw, ch, vplane, uplane);
    if (!uplane)
        fprintf(stderr, "[Drm] no ARGB overlay plane: UI overlay disabled\n");
    return true;
}

void flush_video_fbs()
{
    for (auto &kv : vfbCache)
        drmModeRmFB(fd, kv.second);
    vfbCache.clear();
}

void session_close()
{
    if (urenderer) { SDL_DestroyRenderer(urenderer); urenderer = nullptr; }
    if (usurface) { SDL_FreeSurface(usurface); usurface = nullptr; }
    destroy_dumb(ubuf[0]);
    destroy_dumb(ubuf[1]);
    destroy_dumb(blackfb);
    ui_ready = false;
    ui_visible = false;
    flush_video_fbs();
    if (mode_blob) { drmModeDestroyPropertyBlob(fd, mode_blob); mode_blob = 0; }
    if (fd >= 0) { ::close(fd); fd = -1; }
    conn = crtc = cw = ch = 0;
    vplane = uplane = 0;
    vframes = 0;
    modeset_done = false;
}
} // namespace

bool open(const char *tag)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (refcount > 0) { refcount++; return true; }
    if (!session_open(tag)) { session_close(); return false; }
    refcount = 1;
    return true;
}

void close()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (refcount == 0) return;
    if (--refcount == 0) session_close();
}

int width()  { return (int)cw; }
int height() { return (int)ch; }

bool showVideo(uint32_t fourcc, int w, int h, int srcW, int srcH,
               int nplanes, const int *dmabufFds, const uint32_t *pitches,
               const uint32_t *offsets, uint64_t modifier, const char *tag)
{
    std::lock_guard<std::mutex> lock(mtx);
    if (fd < 0) return false;

    int np = nplanes < 4 ? nplanes : 4;

    // Reuse the framebuffer for this pool buffer if we've already built one.
    // Keyed by the primary plane's fd, which is stable per pool buffer. Only on
    // a cache miss do we import the dma-buf handles + create the framebuffer.
    uint32_t fb = 0;
    auto cached = vfbCache.find(dmabufFds[0]);
    if (cached != vfbCache.end())
    {
        fb = cached->second;
    }
    else
    {
        uint32_t handles[4] = {0}, pit[4] = {0}, off[4] = {0};
        uint64_t mods[4] = {0};
        for (int i = 0; i < np; i++)
        {
            uint32_t hn = 0;
            // The dma-buf fds are owned by the decoder's frame pool (cached,
            // stable per buffer) -- do NOT close them; drmPrime caches one GEM
            // handle per fd so handles stay bounded by the pool.
            if (drmPrimeFDToHandle(fd, dmabufFds[i], &hn))
            { fprintf(stderr, "[%s] drmPrimeFDToHandle: %s\n", tag, strerror(errno)); return false; }
            handles[i] = hn;
            pit[i] = pitches[i];
            off[i] = offsets[i];
            mods[i] = modifier;
        }
        if (drmModeAddFB2WithModifiers(fd, w, h, fourcc, handles, pit, off, mods,
                                       &fb, DRM_MODE_FB_MODIFIERS))
        { fprintf(stderr, "[%s] AddFB2WithModifiers: %s\n", tag, strerror(errno)); return false; }
        vfbCache[dmabufFds[0]] = fb;
    }

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = add_modeset(req);
    add_plane_fullscreen(req, vplane, vp, fb, srcW, srcH);

    int crc = drmModeAtomicCommit(fd, req, flags, nullptr);
    drmModeAtomicFree(req);
    if (crc)
    {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[%s] atomic commit failed: %s\n", tag, strerror(errno)); }
        return false; // fb stays cached; a transient commit failure won't drop it
    }
    modeset_done = true;
    vframes++;

    static int frames = 0;
    static int64_t t0 = 0;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (t0 == 0) t0 = now;
    if (++frames >= 60 || now - t0 >= 2000)
    {
        if (now > t0)
            fprintf(stderr, "[%s] decode %.1f fps  %dx%d (DEFE)\n",
                    tag, frames * 1000.0 / (now - t0), w, h);
        frames = 0;
        t0 = now;
    }
    return true;
}

uint32_t videoFrames()
{
    std::lock_guard<std::mutex> lock(mtx);
    return vframes;
}

SDL_Renderer *uiRenderer()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (fd < 0 || !uplane)
        return nullptr;
    if (ui_ready)
        return urenderer;

    if (!create_dumb(ubuf[0], DRM_FORMAT_ARGB8888) ||
        !create_dumb(ubuf[1], DRM_FORMAT_ARGB8888))
        return nullptr;

    // Offscreen composition surface; SDL's ARGB8888 matches DRM's (both are
    // the same little-endian 32-bit word order).
    usurface = SDL_CreateRGBSurfaceWithFormat(0, cw, ch, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!usurface) { fprintf(stderr, "[Drm] ui surface: %s\n", SDL_GetError()); return nullptr; }
    urenderer = SDL_CreateSoftwareRenderer(usurface);
    if (!urenderer) { fprintf(stderr, "[Drm] ui renderer: %s\n", SDL_GetError()); return nullptr; }
    SDL_SetRenderDrawBlendMode(urenderer, SDL_BLENDMODE_BLEND);

    ui_ready = true;
    return urenderer;
}

bool uiPresent()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!ui_ready)
        return false;

    // Copy the composed surface into the back dumb buffer (row-wise: pitches
    // may differ). UI updates are rare, so the copy cost is irrelevant.
    DumbFb &b = ubuf[uback];
    const uint8_t *src = (const uint8_t *)usurface->pixels;
    for (uint32_t y = 0; y < ch; y++)
        memcpy(b.map + y * b.pitch, src + y * usurface->pitch, cw * 4);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = add_modeset(req);
    add_plane_fullscreen(req, uplane, up, b.fb, cw, ch);
    if (up.zpos)
        drmModeAtomicAddProperty(req, uplane, up.zpos, 1);

    int crc = drmModeAtomicCommit(fd, req, flags, nullptr);
    if (crc && !modeset_done && blackfb.fb == 0)
    {
        // Some drivers refuse to enable the CRTC without the primary plane;
        // retry the UI-only modeset with a black fb on the video plane. The
        // first real video frame simply replaces it.
        if (create_dumb(blackfb, DRM_FORMAT_XRGB8888))
        {
            drmModeAtomicFree(req);
            req = drmModeAtomicAlloc();
            flags = add_modeset(req);
            add_plane_fullscreen(req, uplane, up, b.fb, cw, ch);
            if (up.zpos)
                drmModeAtomicAddProperty(req, uplane, up.zpos, 1);
            add_plane_fullscreen(req, vplane, vp, blackfb.fb, cw, ch);
            crc = drmModeAtomicCommit(fd, req, flags, nullptr);
        }
    }
    drmModeAtomicFree(req);
    if (crc)
    {
        static bool warned = false;
        if (!warned) { warned = true; fprintf(stderr, "[Drm] ui commit failed: %s\n", strerror(errno)); }
        return false;
    }
    modeset_done = true;
    ui_visible = true;
    uback ^= 1;
    return true;
}

void uiHide()
{
    std::lock_guard<std::mutex> lock(mtx);
    if (!ui_ready || !ui_visible || !modeset_done)
        return;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, uplane, up.fb, 0);
    drmModeAtomicAddProperty(req, uplane, up.crtc, 0);
    if (drmModeAtomicCommit(fd, req, 0, nullptr) == 0)
        ui_visible = false;
    drmModeAtomicFree(req);
}
} // namespace drm_display

#endif /* USE_CEDAR || USE_CEDRUS */
