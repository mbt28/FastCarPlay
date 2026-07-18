#ifndef SRC_UI_SCREEN_SETTINGS
#define SRC_UI_SCREEN_SETTINGS

// Basic settings list: night mode, video frame rate, debug overlay and icon
// theme, plus Back.
//
// Every change is persisted immediately to $HOME/.fastcarplay/usersettings.txt
// (never to a shipped preset). Some settings only reach the phone when the
// session is negotiated, so those are marked as needing a restart and the
// screen offers one rather than restarting under the user.

#ifdef USE_LVGL

#include "ui_style.h"

namespace screen_settings
{
lv_obj_t *build(const ui_style::Metrics &m);
void update();
} // namespace screen_settings

#endif /* USE_LVGL */
#endif /* SRC_UI_SCREEN_SETTINGS */
