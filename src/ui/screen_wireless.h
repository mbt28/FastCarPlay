#ifndef SRC_UI_SCREEN_WIRELESS
#define SRC_UI_SCREEN_WIRELESS

// Wireless Android Auto settings: the Wi-Fi AP the head unit runs (name and
// passphrase) and the Bluetooth name the phone pairs with.
//
// Editing happens through an on-screen keyboard, which is the first screen
// here that has to be operable with a 3-way encoder as well as by touch --
// the keyboard joins the focus group so rotating steps through its keys.
//
// Values are validated before they are saved: hostapd simply refuses to start
// on a passphrase shorter than 8 characters, and a head unit that silently
// stops advertising is far harder to diagnose than a rejected edit.

#ifdef USE_LVGL

#include "ui_style.h"

namespace screen_wireless
{
lv_obj_t *build(const ui_style::Metrics &m);
void update();
} // namespace screen_wireless

#endif /* USE_LVGL */
#endif /* SRC_UI_SCREEN_WIRELESS */
