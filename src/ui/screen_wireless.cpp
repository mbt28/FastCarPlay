#include "screen_wireless.h"

#ifdef USE_LVGL

#include <cstring>
#include <string>

#include "common/logger.h"
#include "settings.h"
#include "ui_bridge.h"
#include "ui_screens.h"

namespace
{
enum RowId
{
    ROW_SSID = 0,
    ROW_PASSPHRASE,
    ROW_BT_NAME,
    ROW_BACK,
    ROW_COUNT,
};

struct Field
{
    const char *key;   // settings key
    const char *label; // row text
    icons::Id icon;
    int minLength;
    int maxLength;
};

// WPA2 fixes the passphrase at 8-63 characters and the SSID at 1-32; hostapd
// refuses to start outside those, so they are enforced before saving.
const Field FIELDS[] = {
    {"wifi-ssid", "Name", icons::ICON_WIRELESS, 1, 32},
    {"wifi-passphrase", "Password", icons::ICON_WIRELESS, 8, 63},
    {"bluetooth-name", "Bluetooth", icons::ICON_DONGLE, 1, 32},
};

lv_obj_t *g_rows[ROW_COUNT] = {nullptr};
lv_obj_t *g_values[ROW_COUNT] = {nullptr};
lv_obj_t *g_header = nullptr;
lv_obj_t *g_editor = nullptr;
lv_obj_t *g_textarea = nullptr;
lv_obj_t *g_hint = nullptr;
ui_style::Metrics g_metrics{};
int g_editing = -1;


const char *fieldValue(int id)
{
    switch (id)
    {
    case ROW_SSID:
        return Settings::wifiSsid.value.c_str();
    case ROW_PASSPHRASE:
        return Settings::wifiPass.value.c_str();
    case ROW_BT_NAME:
        return Settings::btName.value.c_str();
    default:
        return "";
    }
}

void closeEditor()
{
    g_editing = -1;
    g_editor = nullptr;
    g_textarea = nullptr;
    g_hint = nullptr;
    // Rebuild: the editor took the focus group over, so this puts the rows
    // back in it and restores encoder navigation.
    ui_screens::rebuild();
}

void onCancel(lv_event_t *e)
{
    (void)e;
    closeEditor(); // discard: nothing is written until the value validates
}

void onKeyboard(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CANCEL)
    {
        closeEditor();
        return;
    }
    if (code != LV_EVENT_READY || g_editing < 0 || g_textarea == nullptr)
        return;

    const Field &field = FIELDS[g_editing];
    const char *text = lv_textarea_get_text(g_textarea);
    const int length = text != nullptr ? (int)strlen(text) : 0;

    if (length < field.minLength || length > field.maxLength)
    {
        if (g_hint != nullptr)
        {
            static char message[64];
            snprintf(message, sizeof(message), "Needs %d-%d characters",
                     field.minLength, field.maxLength);
            lv_label_set_text(g_hint, message);
            lv_obj_set_style_text_color(g_hint, lv_color_hex(0xEF5350), LV_PART_MAIN);
        }
        return; // keep the editor open rather than saving something unusable
    }

    if (Settings::setUser(field.key, text))
        ui_bridge::setRestartNeeded(); // the AP and BT name are set up at start-up
    else
        log_e("Could not save %s", field.key);

    closeEditor();
}

void openEditor(int id)
{
    if (id < 0 || id >= (int)(sizeof(FIELDS) / sizeof(FIELDS[0])))
        return;

    g_editing = id;
    lv_obj_t *screen = lv_screen_active();

    g_editor = lv_obj_create(screen);
    // The screen is a flex column, which would place the editor *after* the
    // rows; opt out of the layout so it covers them instead.
    lv_obj_add_flag(g_editor, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(g_editor, 0, 0);
    lv_obj_set_size(g_editor, g_metrics.width, g_metrics.height);
    lv_obj_set_style_bg_color(g_editor, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g_editor, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(g_editor, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(g_editor, g_metrics.gap, LV_PART_MAIN);
    lv_obj_set_flex_flow(g_editor, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(g_editor, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(g_editor);

    // Title row: what is being edited, and an explicit way out. Text entry
    // must always be abandonable -- the keyboard's own close key is far too
    // easy to miss, and there is no other way back on a touch-only unit.
    lv_obj_t *titleRow = lv_obj_create(g_editor);
    lv_obj_set_size(titleRow, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(titleRow, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(titleRow, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(titleRow, 0, LV_PART_MAIN);
    lv_obj_set_flex_flow(titleRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(titleRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(titleRow, LV_OBJ_FLAG_SCROLLABLE);

    g_hint = lv_label_create(titleRow);
    lv_obj_set_style_text_font(g_hint, g_metrics.font, LV_PART_MAIN);
    lv_obj_set_style_text_color(g_hint, lv_color_hex(0x8FA3B0), LV_PART_MAIN);
    lv_label_set_text(g_hint, FIELDS[id].label);
    lv_obj_set_flex_grow(g_hint, 1);

    lv_obj_t *cancel = lv_button_create(titleRow);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x37474F), LV_PART_MAIN);
    lv_obj_set_style_pad_all(cancel, g_metrics.gap, LV_PART_MAIN);
    lv_obj_add_event_cb(cancel, onCancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *cancelLabel = lv_label_create(cancel);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_set_style_text_font(cancelLabel, g_metrics.font, LV_PART_MAIN);

    g_textarea = lv_textarea_create(g_editor);
    lv_obj_set_width(g_textarea, lv_pct(100));
    lv_textarea_set_one_line(g_textarea, true);
    lv_textarea_set_max_length(g_textarea, FIELDS[id].maxLength);
    lv_textarea_set_text(g_textarea, fieldValue(id));
    lv_obj_set_style_text_font(g_textarea, g_metrics.font, LV_PART_MAIN);

    lv_obj_t *keyboard = lv_keyboard_create(g_editor);
    lv_obj_set_width(keyboard, lv_pct(100));
    lv_obj_set_flex_grow(keyboard, 1);
    lv_keyboard_set_textarea(keyboard, g_textarea);
    lv_obj_add_event_cb(keyboard, onKeyboard, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(keyboard, onKeyboard, LV_EVENT_CANCEL, nullptr);

    // Encoder: the group holds only what the editor offers, so rotating can
    // never reach the rows hidden behind it. Deliberately *not* forced into
    // edit mode -- pressing enters the keyboard, and its close/accept keys
    // leave again, so the encoder can always get back out.
    lv_group_t *group = ui_screens::group();
    if (group != nullptr)
    {
        lv_group_remove_all_objs(group);
        lv_group_set_editing(group, false);
        lv_group_add_obj(group, keyboard);
        lv_group_add_obj(group, cancel);
        lv_group_focus_obj(keyboard);
    }
}

void onRowClicked(lv_event_t *e)
{
    const RowId id = (RowId)(intptr_t)lv_event_get_user_data(e);
    if (id == ROW_BACK)
    {
        ui_screens::show(ui_screens::SCREEN_SETTINGS);
        return;
    }
    openEditor((int)id);
}
} // namespace

namespace screen_wireless
{
lv_obj_t *build(const ui_style::Metrics &m)
{
    g_metrics = m;
    g_editing = -1;
    g_editor = nullptr;
    g_textarea = nullptr;
    g_hint = nullptr;
    for (int i = 0; i < ROW_COUNT; i++)
    {
        g_rows[i] = nullptr;
        g_values[i] = nullptr;
    }

    lv_obj_t *screen = ui_style::listScreen(m);
    g_header = ui_style::header(screen, m, "Wireless");

    for (int i = 0; i < (int)(sizeof(FIELDS) / sizeof(FIELDS[0])); i++)
    {
        lv_obj_t *row = ui_style::row(screen, m, FIELDS[i].icon, FIELDS[i].label);
        lv_obj_add_event_cb(row, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        g_rows[i] = row;
        g_values[i] = ui_style::rowValue(row, m, "");
    }

    lv_obj_t *back = ui_style::row(screen, m, icons::ICON_BACK, "Back");
    lv_obj_add_event_cb(back, onRowClicked, LV_EVENT_CLICKED, (void *)(intptr_t)ROW_BACK);
    g_rows[ROW_BACK] = back;

    update();
    return screen;
}

void update()
{
    for (int i = 0; i < (int)(sizeof(FIELDS) / sizeof(FIELDS[0])); i++)
        if (g_values[i] != nullptr)
            lv_label_set_text(g_values[i], fieldValue(i));

    if (g_header != nullptr)
        lv_label_set_text(g_header, ui_bridge::restartNeeded() ? "Wireless  -  restart to apply"
                                                   : "Wireless");
}
} // namespace screen_wireless

#endif /* USE_LVGL */
