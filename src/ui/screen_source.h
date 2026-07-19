#ifndef SRC_UI_SCREEN_SOURCE
#define SRC_UI_SCREEN_SOURCE

// Source: which session to run. A radio group -- only one is ever held, so
// exactly one row carries the tick.
//
// Choosing stages the change and returns; it never restarts under the driver.
// Settings then shows "restart to apply" and offers "Restart now".

#ifdef USE_LVGL

#include "ui_style.h"

namespace screen_source
{
lv_obj_t *build(const ui_style::Metrics &m);
void update();
} // namespace screen_source

#endif /* USE_LVGL */
#endif /* SRC_UI_SCREEN_SOURCE */
