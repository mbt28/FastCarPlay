#ifndef SRC_UI_OSD
#define SRC_UI_OSD

// Selects the on-device UI toolkit at build time behind one name. The
// implementations share the same seam -- beginDrm / end / active / pointer /
// tick / invalidate -- and flush into drm_display's overlay dumb buffer; the
// application only ever sees `Osd`.

#if defined(USE_LVGL)
#include "lvgl_osd.h"
using Osd = LvglOsd;
#define USE_OSD_UI 1
#elif defined(USE_SLINT)
#include "slint_osd.h"
using Osd = SlintOsd;
#define USE_OSD_UI 1
#endif

#endif /* SRC_UI_OSD */
