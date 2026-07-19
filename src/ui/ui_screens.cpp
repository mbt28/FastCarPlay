#include "ui_screens.h"

#ifdef USE_LVGL

#include <lvgl/lvgl.h>

#include "common/logger.h"
#include "icons.h"
#include "screen_home.h"
#include "screen_source.h"
#include "screen_settings.h"
#include "screen_wireless.h"
#include "settings.h"
#include "ui_style.h"

namespace
{
ui_style::Metrics g_metrics{};
ui_screens::Id g_current = ui_screens::SCREEN_HOME;
bool g_started = false;
lv_group_t *g_group = nullptr;

void build(ui_screens::Id id)
{
    // Rows join the group as they are built, so start from empty; the
    // outgoing screen's objects are about to be freed.
    if (g_group != nullptr)
    {
        lv_group_remove_all_objs(g_group);
        // Leave edit mode: a widget that captures the encoder (the keyboard)
        // would otherwise keep it after the screen is gone, and rotating
        // would no longer move between rows.
        lv_group_set_editing(g_group, false);
    }
    g_metrics.group = g_group;

    lv_obj_t *screen = nullptr;
    switch (id)
    {
    case ui_screens::SCREEN_SETTINGS:
        screen = screen_settings::build(g_metrics);
        break;
    case ui_screens::SCREEN_WIRELESS:
        screen = screen_wireless::build(g_metrics);
        break;
    case ui_screens::SCREEN_SOURCE:
        screen = screen_source::build(g_metrics);
        break;
    default:
        screen = screen_home::build(g_metrics);
        break;
    }

    if (screen == nullptr)
    {
        log_e("UI: screen %d failed to build", (int)id);
        return;
    }

    g_current = id;
    // auto_del: the outgoing screen is freed once it is off screen.
    lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

// Runs from LVGL's async queue, i.e. after the click handler that asked for
// the change has returned and it is safe to delete the old screen.
void onShow(void *param)
{
    build((ui_screens::Id)(intptr_t)param);
}
} // namespace

namespace ui_screens
{
void begin(int width, int height)
{
    icons::begin();
    g_metrics = ui_style::metrics(width, height);
    if (g_group == nullptr)
        g_group = lv_group_create();
    g_started = true;
    build(Settings::lvglStartScreen.value == "settings"  ? SCREEN_SETTINGS
          : Settings::lvglStartScreen.value == "source" ? SCREEN_SOURCE
                                                        : SCREEN_HOME);
    log_v("UI: %dx%d, row %d px", width, height, g_metrics.rowHeight);
}

void show(Id id)
{
    if (!g_started)
        return;
    lv_async_call(onShow, (void *)(intptr_t)id);
}

void rebuild()
{
    if (!g_started)
        return;
    // Re-read the icon theme first: it selects a different font, so every
    // label has to be recreated rather than just relabelled.
    icons::begin();
    g_metrics = ui_style::metrics(g_metrics.width, g_metrics.height);
    lv_async_call(onShow, (void *)(intptr_t)g_current);
}

lv_group_t *group()
{
    return g_group;
}

void tick()
{
    if (!g_started)
        return;

    switch (g_current)
    {
    case SCREEN_SETTINGS:
        screen_settings::update();
        break;
    case SCREEN_WIRELESS:
        screen_wireless::update();
        break;
    case SCREEN_SOURCE:
        screen_source::update();
        break;
    default:
        screen_home::update();
        break;
    }
}
} // namespace ui_screens

#endif /* USE_LVGL */
