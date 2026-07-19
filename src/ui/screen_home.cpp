#include "screen_home.h"

#ifdef USE_LVGL

#include "settings.h"
#include "ui_bridge.h"
#include "ui_screens.h"

namespace
{
lv_obj_t *g_header = nullptr;
lv_obj_t *g_resume = nullptr;
lv_obj_t *g_resumeLabel = nullptr;
ui_style::Metrics g_metrics{};

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

namespace screen_home
{
lv_obj_t *build(const ui_style::Metrics &m)
{
    g_metrics = m;
    lv_obj_t *screen = ui_style::listScreen(m);
    g_header = ui_style::header(screen, m, "");

    // The hero: twice a normal row, so it is unmistakable at a glance and an
    // easy target on a moving vehicle. Focused below, so one encoder click
    // does the common thing.
    g_resume = ui_style::row(screen, m, icons::ICON_VIDEO, "Resume");
    lv_obj_set_height(g_resume, m.rowHeight * 2);
    lv_obj_add_event_cb(g_resume, onResumeClicked, LV_EVENT_CLICKED, nullptr);
    g_resumeLabel = g_resume;

    lv_obj_t *settings = ui_style::row(screen, m, icons::ICON_SETTINGS, "Settings");
    lv_obj_add_event_cb(settings, onSettingsClicked, LV_EVENT_CLICKED, nullptr);

    update();

    // Default focus on whatever the driver most likely wants: the session if
    // one is waiting, otherwise Settings (the only thing left to do).
    if (m.group != nullptr)
        lv_group_focus_obj(ui_bridge::backgroundedSession() ? g_resume : settings);

    return screen;
}

void update()
{
    const bool waiting = ui_bridge::backgroundedSession();

    // Hidden rather than disabled: a control that would do nothing is just
    // something else to read.
    if (g_resume != nullptr)
    {
        if (waiting)
            lv_obj_remove_flag(g_resume, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_resume, LV_OBJ_FLAG_HIDDEN);
        ui_style::rowSelected(g_resume, waiting);
    }

    if (g_header != nullptr)
    {
        static char line[64];
        const char *status = get_var_status();
        snprintf(line, sizeof(line), "%s  -  %s", ui_bridge::protocolName(),
                 (status != nullptr && *status != '\0') ? status : "ready");
        lv_label_set_text(g_header, line);
    }
}
} // namespace screen_home

#endif /* USE_LVGL */
