#ifndef SRC_DRM_DISPLAY
#define SRC_DRM_DISPLAY

// Shared DRM/KMS session for the F1C200s zero-copy display path (renderer =
// drm). One process-wide DRM master owns the connector/CRTC/mode and two
// planes:
//   - the video plane (primary, NV12 + ALLWINNER_TILED routed through the
//     DEFE front-end: HW de-tile + BT.601 CSC + scale) fed by V4l2DrmDecoder
//     from its decode thread, and
//   - an overlay plane above it for the UI (home screen, toasts, debug).
//     Its pixel format comes from what the plane actually supports -- RGB565
//     preferred (half the memory and copy bandwidth of 32-bit), ARGB8888 as
//     the fallback -- and the UI writes straight into a single dumb
//     framebuffer. The plane is committed only over the rows that carry
//     content, so a toast band never occludes the live video below it.
// The overlay plane is disabled whenever there is nothing to show, so it
// costs no scanout bandwidth during normal video playback.

#ifdef USE_CEDRUS

#include <cstdint>
#include <SDL2/SDL.h>

namespace drm_display
{
// Refcounted session: the UI and the decoder each open()/close() it.
// `tag` is only used for log prefixes.
bool open(const char *tag);
void close();

// Panel size discovered at open(). The millimetre pair is what the connector
// reports (EDID / DT panel description) and is 0 when it reports nothing --
// callers must treat 0 as "unknown" rather than as a zero-sized panel.
int width();
int height();
int widthMm();
int heightMm();

// Import the given dma-buf planes as a framebuffer and flip it fullscreen on
// the video plane (srcW/srcH crop the buffer before the DEFE scales it to the
// panel). Called from the decoder thread; serialised internally against the
// UI commits. Performs the initial modeset if the UI hasn't already.
// After it returns true the buffer may stay on scanout until the NEXT
// successful showVideo() -- the decoder must keep it alive until then.
bool showVideo(uint32_t fourcc, int w, int h, int srcW, int srcH,
               int nplanes, const int *dmabufFds, const uint32_t *pitches,
               const uint32_t *offsets, uint64_t modifier, const char *tag);

// Number of video frames presented so far (for "is video flowing" checks).
// Lock-free: safe to poll from the main loop while a commit is in flight.
uint32_t videoFrames();

// Drop the cached per-dmabuf framebuffers AND their GEM handles. Must be
// called from the decoder's teardown before close(): the UI keeps the session
// (and the DRM fd) open, so without this the handles would keep dma-buf
// references on the decoder's freed frame pool -- a CMA leak per rebuild.
void flushVideo();

// ── UI overlay ──────────────────────────────────────────────────────────
// The overlay's pixel format, chosen from the plane's format list at open()
// (DRM_FORMAT_RGB565 preferred, DRM_FORMAT_ARGB8888 fallback), and its bytes
// per pixel. Valid after open().
uint32_t uiFormat();
int uiBpp();

// Create the single dumb framebuffer for the overlay (lazy, idempotent).
bool uiBegin();
// Copy a rectangle of pixels (tightly packed rows of `pitch` bytes, in
// uiFormat()) into the dumb buffer. LVGL's flush callback lands here.
void uiWriteRect(int x, int y, int w, int h, const uint8_t *px, int pitch);
// Commit the overlay plane over rows [y0, y1) -- full screen for the home
// UI, just the toast band over live video. No-op when already showing the
// same band. uiHide() disables the plane.
bool uiShow(int y0, int y1);
void uiHide();

// Legacy SDL path for Interface (home screen fallback, toasts, debug): a
// software renderer targeting an offscreen surface in uiFormat().
// uiPresent()/uiPresentRows() copy the surface (or just rows [y0, y1)) into
// the dumb buffer and commit the plane over those rows.
SDL_Renderer *uiRenderer();
bool uiPresent();
bool uiPresentRows(int y0, int y1);
} // namespace drm_display

#endif /* USE_CEDRUS */
#endif /* SRC_DRM_DISPLAY */
