#ifndef SRC_UI_SCREEN_PICKER
#define SRC_UI_SCREEN_PICKER

// Source picker (Android Auto USB / wireless / Carlinkit dongle) plus a way
// into Settings.
//
// This is the hand-written reference screen: it works with no EEZ Studio
// project, and it is what `lvgl-screen = picker` renders. Once screens are
// designed in EEZ Studio, switch to `lvgl-screen = generated` -- both drive
// the *same* ui_bridge contract, so nothing else changes.

#ifdef USE_LVGL

#include "ui_style.h"

namespace screen_picker
{
lv_obj_t *build(const ui_style::Metrics &m);
void update();
} // namespace screen_picker

#endif /* USE_LVGL */
#endif /* SRC_UI_SCREEN_PICKER */
