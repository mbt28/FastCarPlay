#include "screen_source.h"

#ifdef USE_LVGL

#include "ui_bridge.h"
#include "ui_screens.h"

namespace
{
struct Source
{
    int protocol;
    icons::Id icon;
    const char *label;
};

const Source SOURCES[] = {
    {ui_bridge::PROTOCOL_AA_USB, icons::ICON_USB, "AA Wired"},
    {ui_bridge::PROTOCOL_AA_WIRELESS, icons::ICON_WIRELESS, "AA Wireless"},
    {ui_bridge::PROTOCOL_CARPLAY_WIRED, icons::ICON_USB, "CarPlay Wired"},
    {ui_bridge::PROTOCOL_CARLINKIT, icons::ICON_DONGLE, "Dongle"},
};
constexpr int SOURCE_COUNT = (int)(sizeof(SOURCES) / sizeof(SOURCES[0]));

lv_obj_t *g_rows[SOURCE_COUNT] = {nullptr};
lv_obj_t *g_ticks[SOURCE_COUNT] = {nullptr};

void onSourceClicked(lv_event_t *e)
{
    set_var_protocol((int32_t)(intptr_t)lv_event_get_user_data(e));
    screen_source::update();
}

void onBackClicked(lv_event_t *e)
{
    (void)e;
    ui_screens::show(ui_screens::SCREEN_SETTINGS);
}
} // namespace

namespace screen_source
{
lv_obj_t *build(const ui_style::Metrics &m)
{
    lv_obj_t *screen = ui_style::listScreen(m);
    ui_style::header(screen, m, "Source");

    for (int i = 0; i < SOURCE_COUNT; i++)
    {
        lv_obj_t *row = ui_style::row(screen, m, SOURCES[i].icon, SOURCES[i].label);
        lv_obj_add_event_cb(row, onSourceClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SOURCES[i].protocol);
        g_rows[i] = row;
        g_ticks[i] = ui_style::rowValue(row, m, "");
    }

    lv_obj_t *back = ui_style::row(screen, m, icons::ICON_BACK, "Back");
    lv_obj_add_event_cb(back, onBackClicked, LV_EVENT_CLICKED, nullptr);

    update();
    return screen;
}

void update()
{
    const int active = get_var_protocol();
    for (int i = 0; i < SOURCE_COUNT; i++)
    {
        const bool selected = SOURCES[i].protocol == active;
        if (g_rows[i] != nullptr)
            ui_style::rowSelected(g_rows[i], selected);
        if (g_ticks[i] != nullptr)
            lv_label_set_text(g_ticks[i], selected ? LV_SYMBOL_OK : "");
    }
}
} // namespace screen_source

#endif /* USE_LVGL */
