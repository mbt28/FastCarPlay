#ifndef SRC_DISPLAY_GEOMETRY
#define SRC_DISPLAY_GEOMETRY

// The size of the surface the user actually looks at -- and therefore the size a
// phone should be asked to stream. Asking for anything else means every frame is
// encoded, decoded and then rescaled for nothing, which on a 408 MHz ARM926 is
// most of the frame budget.
//
// Where it comes from depends on how we ended up displaying, not on a per-board
// setting (same reasoning as video_path: ask the kernel, don't configure it):
//
//   Drm       the panel, from the DRM connector/CRTC -- nothing to configure
//   Sdl       the real window size *after* creation: a window manager is free
//             to ignore the size we asked for, and the window is resizable
//   Headless  width/height from settings -- there is no display to ask
//
// Physical millimetres come from the DRM connector when it reports them (EDID or
// a panel description in DT). CarPlay uses them for UI scaling; 0 means unknown
// and the consumer should fall back rather than invent a number.
struct DisplayGeometry
{
    int width = 0;
    int height = 0;
    int widthMm = 0;
    int heightMm = 0;
    const char *source = "unset"; // for logging: which rule above applied

    bool valid() const { return width > 0 && height > 0; }
};

#endif /* SRC_DISPLAY_GEOMETRY */
