#ifndef SRC_DRM_DISPLAY
#define SRC_DRM_DISPLAY

// Shared DRM/KMS session for the F1C200s zero-copy display path (renderer =
// drm). One process-wide DRM master owns the connector/CRTC/mode and two
// planes:
//   - the video plane (primary, NV12 + ALLWINNER_TILED routed through the
//     DEFE front-end: HW de-tile + BT.601 CSC + scale) fed by CedarDecoder /
//     CedrusDecoder from their decode threads, and
//   - an ARGB8888 overlay plane above it for the UI (home screen, toasts,
//     debug), drawn through an SDL *software* renderer into an offscreen
//     surface and copied to double-buffered dumb framebuffers. Per-pixel
//     alpha is blended by the DE backend, so OSD over live video is free.
// The overlay plane is disabled whenever there is nothing to show, so it
// costs no scanout bandwidth during normal video playback.

#if defined(USE_CEDAR) || defined(USE_CEDRUS)

#include <cstdint>
#include <SDL2/SDL.h>

namespace drm_display
{
// Refcounted session: the UI and the decoder each open()/close() it.
// `tag` is only used for log prefixes.
bool open(const char *tag);
void close();

// Panel size discovered at open().
int width();
int height();

// Import the given dma-buf planes as a framebuffer and flip it fullscreen on
// the video plane (srcW/srcH crop the buffer before the DEFE scales it to the
// panel). Called from the decoder thread; serialised internally against the
// UI commits. Performs the initial modeset if the UI hasn't already.
bool showVideo(uint32_t fourcc, int w, int h, int srcW, int srcH,
               int nplanes, const int *dmabufFds, const uint32_t *pitches,
               const uint32_t *offsets, uint64_t modifier, const char *tag);

// Number of video frames presented so far (for "is video flowing" checks).
uint32_t videoFrames();

// UI overlay. uiRenderer() lazily creates the offscreen ARGB surface, the
// SDL software renderer targeting it, and the dumb framebuffers; it never
// needs an SDL video driver. uiPresent() copies the surface into the back
// dumb buffer and commits the overlay plane; uiHide() disables the plane.
SDL_Renderer *uiRenderer();
bool uiPresent();
void uiHide();
} // namespace drm_display

#endif /* USE_CEDAR || USE_CEDRUS */
#endif /* SRC_DRM_DISPLAY */
