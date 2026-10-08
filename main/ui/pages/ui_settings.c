// Settings page: two views, chosen with the switcher pill at the bottom (as on Home and Library).
// Server: the Audiobookshelf connection, the library (switch and refresh) and Wi-Fi & login setup.
// Device: screen, power and playback preferences, SD card and firmware info.

#include <stdio.h>
#include <string.h>
#include "abs_api.h"
#include "board.h"
#include "config.h"
#include "download.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "power.h"
#include "storage.h"
#include "switcher.h"
#include "ui.h"
#include "ui_priv.h"
#include "wifi.h"

typedef enum { VIEW_SERVER, VIEW_DEVICE, VIEW_COUNT } view_t;
static const char *const s_names[VIEW_COUNT] = {"Server", "Device"};

// Rows of both views. Device rows from ROW_BRIGHT on are tappable: values cycle, toggles flip.
enum {
    ROW_SERVER, ROW_USER, ROW_LIBRARY, ROW_CONTENTS, ROW_UPDATED, ROW_WIFI,  // Server
    ROW_BRIGHT, ROW_SCREEN, ROW_SLEEP, ROW_SKIP_BACK, ROW_SKIP_FWD,           // Device: cycle
    ROW_ROTATE, ROW_BLUETOOTH,                                               // Device: toggles
    ROW_SD, ROW_VERSION,                                                     // Device: info
    ROW_COUNT
};
static const char *const s_keys[ROW_COUNT] = {
    "Server", "Signed in", "Library", "Contents", "Updated", "Wi-Fi",
    "Brightness", "Screen off", "Sleep", "Skip back", "Skip forward",
    "Rotate 180\xc2\xb0", "Bluetooth",
    "SD card", "Firmware",
};
static const view_t s_row_view[ROW_COUNT] = {
    VIEW_SERVER, VIEW_SERVER, VIEW_SERVER, VIEW_SERVER, VIEW_SERVER, VIEW_SERVER,
    VIEW_DEVICE, VIEW_DEVICE, VIEW_DEVICE, VIEW_DEVICE, VIEW_DEVICE,
    VIEW_DEVICE, VIEW_DEVICE,
    VIEW_DEVICE, VIEW_DEVICE,
};

#define ROW_H   18
#define ROW_GAP 4
#define AREA_W  252
#define BTN_H   36

static lv_obj_t *s_views[VIEW_COUNT], *s_values[ROW_COUNT], *s_rotate_sw, *s_library_btn;
static switcher_t *s_switcher;
static view_t s_view;
static bool s_rotated = true;  // default: upside down (how this device is mounted)

static void apply_rotation(bool rotated, bool save)
{
    s_rotated = rotated;
    board_set_rotated(rotated);
    lv_obj_invalidate(lv_screen_active());
    if (save) config_set_rotate(rotated);
}

// Choices the cycling rows step through.
static const int BRIGHTNESS[] = {20, 40, 60, 80, 100};
static const int SCREEN_OFF_S[] = {30, 60, 120, 300, 0};
static const int SLEEP_MIN[] = {5, 10, 30, 0};
static const int SKIP_S[] = {10, 15, 30, 60};

static int next_choice(const int *choices, int n, int current)
{
    for (int i = 0; i < n; i++) {
        if (choices[i] == current) return choices[(i + 1) % n];
    }
    return choices[0];
}

static void on_cycle_row(lv_event_t *e)
{
    power_config_t c;
    power_get_config(&c);
    const app_config_t *cfg = config_get();
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case ROW_BRIGHT: c.brightness = next_choice(BRIGHTNESS, 5, c.brightness); break;
    case ROW_SCREEN: c.screen_off_s = next_choice(SCREEN_OFF_S, 5, c.screen_off_s); break;
    case ROW_SLEEP:  c.sleep_min = next_choice(SLEEP_MIN, 4, c.sleep_min); break;
    case ROW_SKIP_BACK:
        config_set_skip(next_choice(SKIP_S, 4, cfg->skip_back_s), cfg->skip_fwd_s);
        settings_refresh();
        return;
    case ROW_SKIP_FWD:
        config_set_skip(cfg->skip_back_s, next_choice(SKIP_S, 4, cfg->skip_fwd_s));
        settings_refresh();
        return;
    }
    power_set_config(&c);
    settings_refresh();
}

static void on_rotate(lv_event_t *e)
{
    apply_rotation(lv_obj_has_state(s_rotate_sw, LV_STATE_CHECKED), true);
}

/* ---------- library picker ---------- */

static lv_obj_t *s_picker, *s_picker_list;

static void picker_close(void)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void on_picker_close(lv_event_t *e) { picker_close(); }

static void on_pick(lv_event_t *e)
{
    int n;
    const char *sel;
    const abs_library_t *libs = ui_libraries(&n, &sel);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    picker_close();
    if (i < 0 || i >= n || strcmp(libs[i].id, sel) == 0) return;
    ui_request_library(libs[i].id);
    char msg[96];
    snprintf(msg, sizeof(msg), "Opening %s...", libs[i].name);
    ui_show_message(msg);
}

static void on_library_row(lv_event_t *e)
{
    int n;
    const char *sel;
    const abs_library_t *libs = ui_libraries(&n, &sel);
    if (n < 2) return;
    lv_obj_clean(s_picker_list);
    for (int i = 0; i < n; i++) {
        const bool current = strcmp(libs[i].id, sel) == 0;
        lv_obj_t *btn = lv_list_add_button(s_picker_list, NULL, NULL);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, current ? COLOR_ACCENT : COLOR_CARD, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 2, 0);
        lv_obj_t *t = lv_label_create(btn);
        lv_label_set_text(t, libs[i].name);
        ui_one_line(t, &ui_font_16);
        lv_obj_set_style_text_color(t, current ? lv_color_black() : COLOR_TEXT, 0);
        lv_obj_t *k = lv_label_create(btn);
        lv_label_set_text(k, libs[i].podcast ? "Podcast library" : "Audiobook library");
        ui_one_line(k, &ui_font_14);
        lv_obj_set_style_text_color(k, current ? lv_color_black() : COLOR_MUTED, 0);
        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker);
}

bool libpicker_visible(void)
{
    return s_picker && !lv_obj_has_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

void libpicker_build(lv_obj_t *scr)
{
    s_picker = ui_page_container(scr);
    lv_obj_set_style_bg_color(s_picker, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = ui_round_button(s_picker, 36, LV_SYMBOL_CLOSE, &ui_font_16, on_picker_close, NULL);
    lv_obj_align(close, LV_ALIGN_CENTER, 0, -128);
    lv_obj_t *title = ui_label(s_picker, &ui_font_16, COLOR_ACCENT, 200);
    lv_label_set_text(title, "Choose library");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -90);

    s_picker_list = lv_list_create(s_picker);
    lv_obj_set_size(s_picker_list, 240, 210);
    lv_obj_align(s_picker_list, LV_ALIGN_CENTER, 0, 36);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_list, 0, 0);
    lv_obj_set_style_pad_all(s_picker_list, 0, 0);
    lv_obj_set_style_pad_row(s_picker_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_picker_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void on_refresh(lv_event_t *e)
{
    ui_request_refresh();
    ui_show_message("Refreshing...");
}

static void on_setup(lv_event_t *e)
{
    ui_setup_show();
}

static void set_value(int row, const char *text)
{
    if (s_values[row] && strcmp(lv_label_get_text(s_values[row]), text) != 0) lv_label_set_text(s_values[row], text);
}

static void refresh_server(void)
{
    char buf[80];
    set_value(ROW_SERVER, abs_server());
    const app_config_t *c = config_get();
    if (c->username[0]) snprintf(buf, sizeof(buf), "%s", c->username);
    else snprintf(buf, sizeof(buf), "%s", c->access[0] ? "with API key" : "no");
    set_value(ROW_USER, buf);
    set_value(ROW_LIBRARY, abs_library_name()[0] ? abs_library_name() : "-");
    snprintf(buf, sizeof(buf), "%d %s, %d authors", g_book_count, ui_library_is_podcast() ? "shows" : "books",
             g_author_count);
    set_value(ROW_CONTENTS, buf);

    uint32_t loaded;
    bool cached;
    ui_get_source(&loaded, &cached);
    uint32_t mins = lv_tick_elaps(loaded) / 60000;
    if (!g_book_count) snprintf(buf, sizeof(buf), "not loaded");
    else if (cached) snprintf(buf, sizeof(buf), "from SD cache");
    else if (mins == 0) snprintf(buf, sizeof(buf), "just now");
    else snprintf(buf, sizeof(buf), "%lu min ago", (unsigned long)mins);
    set_value(ROW_UPDATED, buf);

    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%.32s", (const char *)ap.ssid);
    } else {
        snprintf(buf, sizeof(buf), "offline");
    }
    set_value(ROW_WIFI, buf);
}

void settings_libraries_changed(void)
{
    if (!s_library_btn) return;
    // Choosing a library only makes sense with more than one.
    int nlibs;
    const char *sel;
    ui_libraries(&nlibs, &sel);
    if (nlibs > 1) lv_obj_remove_state(s_library_btn, LV_STATE_DISABLED);
    else lv_obj_add_state(s_library_btn, LV_STATE_DISABLED);
}

static void refresh_device(void)
{
    char buf[80];
    power_config_t pc;
    power_get_config(&pc);
    snprintf(buf, sizeof(buf), "%d%%  " LV_SYMBOL_RIGHT, pc.brightness);
    set_value(ROW_BRIGHT, buf);
    if (!pc.screen_off_s) snprintf(buf, sizeof(buf), "Never  " LV_SYMBOL_RIGHT);
    else if (pc.screen_off_s < 60) snprintf(buf, sizeof(buf), "%d s  " LV_SYMBOL_RIGHT, pc.screen_off_s);
    else snprintf(buf, sizeof(buf), "%d min  " LV_SYMBOL_RIGHT, pc.screen_off_s / 60);
    set_value(ROW_SCREEN, buf);
    if (!pc.sleep_min) snprintf(buf, sizeof(buf), "Never  " LV_SYMBOL_RIGHT);
    else snprintf(buf, sizeof(buf), "after %d min  " LV_SYMBOL_RIGHT, pc.sleep_min);
    set_value(ROW_SLEEP, buf);
    snprintf(buf, sizeof(buf), "%d s  " LV_SYMBOL_RIGHT, config_get()->skip_back_s);
    set_value(ROW_SKIP_BACK, buf);
    snprintf(buf, sizeof(buf), "%d s  " LV_SYMBOL_RIGHT, config_get()->skip_fwd_s);
    set_value(ROW_SKIP_FWD, buf);
    if (lv_obj_has_state(s_rotate_sw, LV_STATE_CHECKED) != s_rotated) {
        if (s_rotated) lv_obj_add_state(s_rotate_sw, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_rotate_sw, LV_STATE_CHECKED);
    }

    if (storage_ready()) {
        uint64_t free_b, total_b;
        storage_space(&free_b, &total_b);
        snprintf(buf, sizeof(buf), "%.1f GB free, %d book%s", free_b / 1e9, g_downloaded.count,
                 g_downloaded.count == 1 ? "" : "s");
    } else {
        snprintf(buf, sizeof(buf), "none");
    }
    set_value(ROW_SD, buf);
}

void settings_refresh(void)
{
    if (s_view == VIEW_SERVER) refresh_server();
    else refresh_device();
}

void ui_settings_changed(void)
{
    settings_refresh();
}

void ui_set_rotation(bool rotate180)
{
    if (rotate180 != s_rotated) apply_rotation(rotate180, true);
    settings_refresh();
}

/* ---------- build ---------- */

static lv_obj_t *make_view(lv_obj_t *page)
{
    // Same area as the Library's lists; scrolls when the rows don't all fit.
    lv_obj_t *v = lv_obj_create(page);
    lv_obj_remove_style_all(v);
    lv_obj_set_size(v, AREA_W, PAGE_SWITCHER_Y - PAGE_TOP - 8);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, PAGE_TOP);
    lv_obj_remove_flag(v, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(v, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(v, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_top(v, 4, 0);
    lv_obj_set_style_pad_bottom(v, 8, 0);
    lv_obj_set_style_pad_row(v, ROW_GAP, 0);
    return v;
}

// A "key ... value" row; returns the row (the value label goes in s_values).
static lv_obj_t *make_row(lv_obj_t *view, int i)
{
    lv_obj_t *row = lv_obj_create(view);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *k = ui_label(row, &ui_font_14, COLOR_MUTED, 96);
    lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(k, LV_LABEL_LONG_CLIP);  // one line
    lv_label_set_text(k, s_keys[i]);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void add_value(lv_obj_t *row, int i)
{
    s_values[i] = ui_label(row, &ui_font_14, COLOR_TEXT, 150);
    lv_obj_set_style_text_align(s_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_values[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
}

static lv_obj_t *add_switch(lv_obj_t *row)
{
    lv_obj_set_height(row, 24);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 44, 22);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(sw, COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 6);
    return sw;
}

// A rounded button with an icon and text; `primary` is filled with the accent colour.
static lv_obj_t *make_button(lv_obj_t *parent, int w, const char *text, bool primary, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, BTN_H);
    lv_obj_set_style_radius(btn, BTN_H / 2, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, primary ? COLOR_ACCENT : COLOR_CARD, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
    // Disabled (e.g. only one library to choose from): the normal look, faded.
    lv_obj_set_style_bg_color(btn, primary ? COLOR_ACCENT : COLOR_CARD, LV_STATE_DISABLED);
    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_t *l = ui_label(btn, &ui_font_14, primary ? lv_color_black() : COLOR_TEXT, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void build_server(lv_obj_t *v)
{
    for (int i = 0; i < ROW_COUNT; i++) {
        if (s_row_view[i] == VIEW_SERVER) add_value(make_row(v, i), i);
    }
    // Library actions side by side, then setup.
    lv_obj_t *pair = lv_obj_create(v);
    lv_obj_remove_style_all(pair);
    lv_obj_set_size(pair, lv_pct(100), BTN_H);
    lv_obj_set_style_margin_top(pair, 6, 0);
    lv_obj_remove_flag(pair, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_library_btn = make_button(pair, 122, LV_SYMBOL_LIST "  Library", false, on_library_row);
    lv_obj_align(s_library_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *refresh = make_button(pair, 122, LV_SYMBOL_REFRESH "  Refresh", false, on_refresh);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, 0, 0);
    make_button(v, 200, LV_SYMBOL_WIFI "  Wi-Fi & login setup", true, on_setup);
}

static void build_device(lv_obj_t *v)
{
    for (int i = 0; i < ROW_COUNT; i++) {
        if (s_row_view[i] != VIEW_DEVICE) continue;
        lv_obj_t *row = make_row(v, i);
        if (i == ROW_ROTATE) {
            s_rotate_sw = add_switch(row);
            lv_obj_add_event_cb(s_rotate_sw, on_rotate, LV_EVENT_VALUE_CHANGED, NULL);
        } else if (i == ROW_BLUETOOTH) {
            // Shown for what's coming: there's no Bluetooth audio yet, so it stays off.
            lv_obj_t *sw = add_switch(row);
            lv_obj_add_state(sw, LV_STATE_DISABLED);
            lv_obj_t *note = ui_label(row, &ui_font_14, COLOR_MUTED, 0);
            lv_label_set_text(note, "coming soon");
            lv_obj_align_to(note, sw, LV_ALIGN_OUT_LEFT_MID, -8, 0);
        } else {
            add_value(row, i);
            if (i < ROW_ROTATE) {
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_ext_click_area(row, 2);
                lv_obj_add_event_cb(row, on_cycle_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            }
        }
    }
    set_value(ROW_VERSION, esp_app_get_description()->version);
}

static void show_view(view_t v)
{
    s_view = v;
    for (int i = 0; i < VIEW_COUNT; i++) {
        if (i == (int)v) lv_obj_remove_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_views[i], LV_OBJ_FLAG_HIDDEN);
    }
    switcher_select(s_switcher, v);
    settings_refresh();
}

static void on_switch(int index, int dir)
{
    show_view((view_t)index);
}

void settings_build(lv_obj_t *page)
{
    apply_rotation(config_get()->rotate180, false);
    s_views[VIEW_SERVER] = make_view(page);
    build_server(s_views[VIEW_SERVER]);
    s_views[VIEW_DEVICE] = make_view(page);
    build_device(s_views[VIEW_DEVICE]);
    if (s_rotated) lv_obj_add_state(s_rotate_sw, LV_STATE_CHECKED);
    s_switcher = switcher_create(page, PAGE_SWITCHER_Y, s_names, VIEW_COUNT, on_switch);
    settings_libraries_changed();
    show_view(VIEW_SERVER);
}

#ifdef UI_CAPTURE
void settings_debug_view(int view)
{
    show_view((view_t)view);
}

void settings_debug_open_picker(void)
{
    on_library_row(NULL);
}

void settings_debug_close_picker(void)
{
    picker_close();
}
#endif
