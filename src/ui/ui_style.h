#ifndef SRC_UI_UI_STYLE
#define SRC_UI_UI_STYLE

// Shared look and layout for the built-in screens, so the picker and the
// settings list cannot drift apart.
//
// Everything here follows the resolution rules in docs/lvgl-ui-blueprint.md
// §3: rows are a fixed height (never a fraction of the screen, which would
// shrink them below a usable touch target on a 400x234 panel), widths are
// percentages, and the font tier is chosen from the panel height.

#ifdef USE_LVGL

#include <lvgl/lvgl.h>

#include "icons.h"

namespace ui_style
{
struct Metrics
{
    int width;
    int height;
    int rowHeight;
    int gap;
    const lv_font_t *font;
    const lv_font_t *iconFont;
    // Focus group for encoder / button navigation. Every row joins it as it
    // is created, so rotating steps through them in visual order. Null when
    // only touch is in use.
    lv_group_t *group;
};

Metrics metrics(int width, int height);

// A full-screen scrollable flex column, styled and padded.
lv_obj_t *listScreen(const Metrics &m);

// Dim caption line at the top of a screen.
lv_obj_t *header(lv_obj_t *screen, const Metrics &m, const char *text);

// A touch row: icon + label, full width, fixed height.
lv_obj_t *row(lv_obj_t *parent, const Metrics &m, icons::Id icon, const char *text);

// Right-aligned value label for a settings row ("On", "60 fps"...).
lv_obj_t *rowValue(lv_obj_t *row, const Metrics &m, const char *text);

// Highlight a row as the active choice.
void rowSelected(lv_obj_t *row, bool selected);
} // namespace ui_style

#endif /* USE_LVGL */
#endif /* SRC_UI_UI_STYLE */
