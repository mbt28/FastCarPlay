#include "ui_style.h"

#ifdef USE_LVGL

namespace
{
// Rows never shrink below this: a percentage-sized row would be ~28 px on a
// 400x234 panel, well under a usable touch target. Small panels therefore
// show fewer rows and scroll.
constexpr int ROW_MIN_HEIGHT = 44;

constexpr uint32_t COLOR_BG = 0x101418;
constexpr uint32_t COLOR_ROW = 0x1E262E;
constexpr uint32_t COLOR_ROW_ON = 0x1E88E5;
constexpr uint32_t COLOR_EDGE = 0x64B5F6;
constexpr uint32_t COLOR_DIM = 0x8FA3B0;
constexpr uint32_t COLOR_FOCUS = 0xFFB300;    // encoder focus ring
constexpr uint32_t COLOR_ROW_FOCUS = 0x2C3844; // focused row fill
} // namespace

namespace ui_style
{
Metrics metrics(int width, int height)
{
    Metrics m{};
    m.width = width;
    m.height = height;
    m.rowHeight = LV_MAX(ROW_MIN_HEIGHT, height / 6);
    m.gap = LV_MAX(2, height / 60);

    // Font tiers: LVGL fonts are bitmaps and cannot scale continuously, so
    // pick the nearest size rather than stretching one.
    if (height <= 272)
        m.font = &lv_font_montserrat_14;
    else if (height <= 480)
        m.font = &lv_font_montserrat_20;
    else
        m.font = &lv_font_montserrat_28;

    m.iconFont = icons::font(height);
    return m;
}

lv_obj_t *listScreen(const Metrics &m)
{
    lv_obj_t *screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, m.gap, LV_PART_MAIN);
    lv_obj_set_style_pad_row(screen, m.gap, LV_PART_MAIN);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(screen, LV_DIR_VER);
    return screen;
}

lv_obj_t *header(lv_obj_t *screen, const Metrics &m, const char *text)
{
    lv_obj_t *label = lv_label_create(screen);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_font(label, m.font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *row(lv_obj_t *parent, const Metrics &m, icons::Id icon, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_set_height(button, m.rowHeight);
    lv_obj_set_style_radius(button, m.gap, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_ROW), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_EDGE), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);

    // Icon and text are separate labels: a label carries one font, and a
    // dedicated icon set is a different font from the body text.
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(button, m.gap * 2, LV_PART_MAIN);

    lv_obj_t *glyph = lv_label_create(button);
    lv_label_set_text(glyph, icons::text(icon));
    lv_obj_set_style_text_font(glyph, m.iconFont, LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, m.font, LV_PART_MAIN);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(label, 1); // pushes any value label to the right

    // Encoder focus: a visible ring, distinct from the "this is the active
    // source" fill, so the two never read as the same thing.
    lv_obj_set_style_outline_color(button, lv_color_hex(COLOR_FOCUS), LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(button, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_opa(button, LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(COLOR_ROW_FOCUS), LV_STATE_FOCUSED);

    if (m.group != nullptr)
        lv_group_add_obj(m.group, button);

    return button;
}

lv_obj_t *rowValue(lv_obj_t *row, const Metrics &m, const char *text)
{
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_font(label, m.font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COLOR_DIM), LV_PART_MAIN);
    lv_label_set_text(label, text != nullptr ? text : "");
    return label;
}

void rowSelected(lv_obj_t *row, bool selected)
{
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COLOR_ROW_ON : COLOR_ROW),
                              LV_PART_MAIN);
    lv_obj_set_style_border_width(row, selected ? 2 : 0, LV_PART_MAIN);

    // The focused style would otherwise hide the "this is the active source"
    // fill on the row that also has encoder focus -- which is the common case,
    // since focus starts on the current source. Keep the fill and let the
    // outline ring carry focus on its own.
    lv_obj_set_style_bg_color(row, lv_color_hex(selected ? COLOR_ROW_ON : COLOR_ROW_FOCUS),
                              LV_STATE_FOCUSED);
}
} // namespace ui_style

#endif /* USE_LVGL */
