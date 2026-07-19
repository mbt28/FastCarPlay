#include "screen_picker.h"

#ifdef USE_LVGL

#include "common/logger.h"
#include "settings.h"
#include "ui_bridge.h"
#include "ui_screens.h"
#include "ui_style.h"

namespace
{
struct Source
{
    int protocol;
    icons::Id icon;
    const char *label;
};

const Source SOURCES[] = {
    {ui_bridge::PROTOCOL_AA_USB, icons::ICON_USB, "Android Auto  -  USB"},
    {ui_bridge::PROTOCOL_AA_WIRELESS, icons::ICON_WIRELESS, "Android Auto  -  Wireless"},
    {ui_bridge::PROTOCOL_CARLINKIT, icons::ICON_DONGLE, "Carlinkit Dongle"},
};
constexpr int SOURCE_COUNT = (int)(sizeof(SOURCES) / sizeof(SOURCES[0]));

lv_obj_t *g_header = nullptr;
lv_obj_t *g_rows[SOURCE_COUNT] = {nullptr};
lv_obj_t *g_resume = nullptr;

void onSourceClicked(lv_event_t *e)
{
    switch ((int)(intptr_t)lv_event_get_user_data(e))
    {
    case ui_bridge::PROTOCOL_AA_USB:
        action_use_aa_usb(e);
        break;
    case ui_bridge::PROTOCOL_AA_WIRELESS:
        action_use_aa_wireless(e);
        break;
    default:
        action_use_dongle(e);
        break;
    }
    screen_picker::update();
}

void onResumeClicked(lv_event_t *e)
{
    (void)e;
    ui_bridge::requestResume();
}

void onSettingsClicked(lv_event_t *e)
{
    (void)e;
    ui_screens::show(ui_screens::SCREEN_SETTINGS);
}
} // namespace

namespace screen_picker
{
lv_obj_t *build(const ui_style::Metrics &m)
{
    for (int i = 0; i < SOURCE_COUNT; i++)
        g_rows[i] = nullptr;
    g_resume = nullptr;

    lv_obj_t *screen = ui_style::listScreen(m);
    g_header = ui_style::header(screen, m, "Select source");

    for (int i = 0; i < SOURCE_COUNT; i++)
    {
        lv_obj_t *row = ui_style::row(screen, m, SOURCES[i].icon, SOURCES[i].label);
        lv_obj_add_event_cb(row, onSourceClicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)SOURCES[i].protocol);
        g_rows[i] = row;
    }

    // Only meaningful while a session is backgrounded; hidden otherwise so it
    // never invites a click that would do nothing.
    g_resume = ui_style::row(screen, m, icons::ICON_VIDEO, "Resume session");
    lv_obj_add_event_cb(g_resume, onResumeClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *settings = ui_style::row(screen, m, icons::ICON_SETTINGS, "Settings");
    lv_obj_add_event_cb(settings, onSettingsClicked, LV_EVENT_CLICKED, nullptr);

    update();
    return screen;
}

void update()
{
    const int active = get_var_protocol();

    for (int i = 0; i < SOURCE_COUNT; i++)
        if (g_rows[i] != nullptr)
            ui_style::rowSelected(g_rows[i], SOURCES[i].protocol == active);

    if (g_resume != nullptr)
    {
        if (ui_bridge::backgroundedSession())
            lv_obj_remove_flag(g_resume, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_resume, LV_OBJ_FLAG_HIDDEN);
    }

    if (g_header != nullptr)
    {
        const char *status = get_var_status();
        lv_label_set_text(g_header,
                          (status != nullptr && *status != '\0') ? status : "Select source");
    }
}
} // namespace screen_picker

#endif /* USE_LVGL */
