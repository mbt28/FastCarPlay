#ifndef SRC_UI_UI_SCREENS
#define SRC_UI_UI_SCREENS

// Navigator for the built-in screens.
//
// Screens are built on demand and the previous one is deleted, so only the
// visible screen costs RAM. Navigation requests are deferred to LVGL's async
// queue: a row's click handler runs *on* the screen being replaced, so tearing
// it down inline would free the object mid-event.

#ifdef USE_LVGL

#include <lvgl/lvgl.h>

namespace ui_screens
{
enum Id
{
    SCREEN_HOME = 0,
    SCREEN_SETTINGS,
    SCREEN_WIRELESS,
    SCREEN_SOURCE,
};

// Builds and shows the first screen. Sizes come from the caller, never a
// compile-time constant.
void begin(int width, int height);

// Requests a screen change; takes effect on the next LVGL tick.
void show(Id id);

// Rebuilds the current screen -- used when something structural changes, such
// as the icon theme (different font, so the labels must be recreated).
void rebuild();

// Per-frame refresh of whatever is on screen.
void tick();

// Focus group for encoder/button navigation; null before begin().
lv_group_t *group();
} // namespace ui_screens

#endif /* USE_LVGL */
#endif /* SRC_UI_UI_SCREENS */
