#include "screen_settings.h"

#ifdef USE_LVGL

#include <cstdio>

#include "common/logger.h"
#include "settings.h"
#include "ui_bridge.h"
#include "ui_screens.h"

namespace
{
enum RowId
{
    ROW_SOURCE = 0,
    ROW_NIGHT,
    ROW_FPS,
    ROW_DEBUG,
    ROW_ICONS,
    ROW_WIRELESS,
    ROW_RESTART,
    ROW_BACK,
    ROW_COUNT,
};

lv_obj_t *g_rows[ROW_COUNT] = {nullptr};
lv_obj_t *g_values[ROW_COUNT] = {nullptr};
lv_obj_t *g_header = nullptr;


const char *nightModeName(int mode)
{
    switch (mode)
    {
    case 0:
        return "Day";
    case 1:
        return "Night";
    default:
        return "Auto";
    }
}

void persist(const char *key, const char *value, bool needsRestart)
{
    if (!Settings::setUser(key, value))
    {
        log_e("Could not save %s", key);
        return;
    }
    if (needsRestart)
        ui_bridge::setRestartNeeded();
}

void onRowClicked(lv_event_t *e)
{
    char buffer[16];

    switch ((RowId)(intptr_t)lv_event_get_user_data(e))
    {
    case ROW_NIGHT:
    {
        // 0 day -> 1 night -> 2 auto -> 0. Only reaches the phone in the
        // sensor batch at connect, so it needs a restart to take effect.
        const int next = (Settings::nightMode + 1) % 3;
        snprintf(buffer, sizeof(buffer), "%d", next);
        persist("night-mode", buffer, true);
        break;
    }
    case ROW_FPS:
    {
        const int next = Settings::aaFps >= 60 ? 30 : 60;
        snprintf(buffer, sizeof(buffer), "%d", next);
        persist("aa-video-fps", buffer, true); // negotiated at session setup
        break;
    }
    case ROW_DEBUG:
        // Read every frame by the render loop, so this one applies live.
        persist("debug-overlay", Settings::debugOverlay ? "false" : "true", false);
        break;

    case ROW_ICONS:
        persist("icon-theme",
                Settings::iconTheme.value == "material" ? "builtin" : "material", false);
        // A different icon set is a different font: rebuild rather than
        // relabel, which also previews the change immediately.
        ui_screens::rebuild();
        return;

    case ROW_SOURCE:
        ui_screens::show(ui_screens::SCREEN_SOURCE);
        return;

    case ROW_WIRELESS:
        ui_screens::show(ui_screens::SCREEN_WIRELESS);
        return;

    case ROW_RESTART:
        ui_bridge::requestRestart();
        return;

    case ROW_BACK:
        ui_screens::show(ui_screens::SCREEN_HOME);
        return;

    default:
        return;
    }

    screen_settings::update();
}

lv_obj_t *addRow(lv_obj_t *screen, const ui_style::Metrics &m, RowId id,
                 icons::Id icon, const char *text, bool withValue)
{
    lv_obj_t *row = ui_style::row(screen, m, icon, text);
    lv_obj_add_event_cb(row, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    g_rows[id] = row;
    g_values[id] = withValue ? ui_style::rowValue(row, m, "") : nullptr;
    return row;
}
} // namespace

namespace screen_settings
{
lv_obj_t *build(const ui_style::Metrics &m)
{
    for (int i = 0; i < ROW_COUNT; i++)
    {
        g_rows[i] = nullptr;
        g_values[i] = nullptr;
    }

    lv_obj_t *screen = ui_style::listScreen(m);
    g_header = ui_style::header(screen, m, "Settings");

    // Back at the top (below the header) -- easier to reach on a car screen.
    addRow(screen, m, ROW_BACK, icons::ICON_BACK, "Back", false);
    addRow(screen, m, ROW_SOURCE, icons::ICON_USB, "Source", true);
    addRow(screen, m, ROW_NIGHT, icons::ICON_NIGHT, "Night", true);
    addRow(screen, m, ROW_FPS, icons::ICON_VIDEO, "Video", true);
    addRow(screen, m, ROW_DEBUG, icons::ICON_DEBUG, "Debug", true);
    addRow(screen, m, ROW_ICONS, icons::ICON_THEME, "Icons", true);
    addRow(screen, m, ROW_WIRELESS, icons::ICON_WIRELESS, "Wireless", false);
    addRow(screen, m, ROW_RESTART, icons::ICON_RESTART, "Restart now", false);

    update();
    return screen;
}

void update()
{
    char buffer[24];

    if (g_values[ROW_SOURCE] != nullptr)
        lv_label_set_text(g_values[ROW_SOURCE], ui_bridge::protocolName());

    if (g_values[ROW_NIGHT] != nullptr)
        lv_label_set_text(g_values[ROW_NIGHT], nightModeName(Settings::nightMode));

    if (g_values[ROW_FPS] != nullptr)
    {
        snprintf(buffer, sizeof(buffer), "%d fps", (int)Settings::aaFps);
        lv_label_set_text(g_values[ROW_FPS], buffer);
    }

    if (g_values[ROW_DEBUG] != nullptr)
        lv_label_set_text(g_values[ROW_DEBUG], Settings::debugOverlay ? "On" : "Off");

    if (g_values[ROW_ICONS] != nullptr)
        lv_label_set_text(g_values[ROW_ICONS], Settings::iconTheme.value.c_str());

    // "Restart now" is only meaningful once something needs it, so it stays
    // hidden rather than inviting a pointless restart.
    if (g_rows[ROW_RESTART] != nullptr)
    {
        if (ui_bridge::restartNeeded())
            lv_obj_remove_flag(g_rows[ROW_RESTART], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(g_rows[ROW_RESTART], LV_OBJ_FLAG_HIDDEN);
    }

    if (g_header != nullptr)
        lv_label_set_text(g_header, ui_bridge::restartNeeded() ? "Settings  -  restart to apply"
                                                   : "Settings");
}
} // namespace screen_settings

#endif /* USE_LVGL */
