#ifndef SRC_UI_OSD
#define SRC_UI_OSD

// The on-device UI toolkit behind one name. slint_osd flushes into
// drm_display's overlay dumb buffer; the application only ever sees `Osd`
// (beginDrm / end / active / pointer / tick / invalidate). USE_OSD_UI is
// defined only when a UI is built in, so a UI-less build compiles clean.

#if defined(USE_SLINT)
#include "slint_osd.h"
using Osd = SlintOsd;
#define USE_OSD_UI 1
#endif

#endif /* SRC_UI_OSD */
