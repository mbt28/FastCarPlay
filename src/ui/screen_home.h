#ifndef SRC_UI_SCREEN_HOME
#define SRC_UI_SCREEN_HOME

// Home: the "now" screen. One status line, one primary action, one way into
// Settings.
//
// This is the surface a driver sees most, so it does exactly one job: get
// back into the running session. Which source to use is set once and lives in
// Settings -- putting it here meant a rare decision occupying the most
// frequent screen, where a mis-tap could drop a live session.

#ifdef USE_LVGL

#include "ui_style.h"

namespace screen_home
{
lv_obj_t *build(const ui_style::Metrics &m);
void update();
} // namespace screen_home

#endif /* USE_LVGL */
#endif /* SRC_UI_SCREEN_HOME */
