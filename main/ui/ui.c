// UI shell for the 360x360 round panel: one screen with three pages (Home, Library, Now Playing)
// and a dock of page buttons across the top. Swipes belong to the pages (carousels and rows), so
// page switching is done from the dock. Player state and covers are polled on an LVGL timer, so
// nothing outside the LVGL task touches widgets.

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "download.h"
#include "player.h"
#include "ui_priv.h"
#include "src/core/lv_obj_event_private.h"  // lv_hit_test_info_t
#include "text.h"

#define RECENT_MAX 30
#define STALE_MS   (5 * 60 * 1000)  // reload the library when Home is opened after this long

const abs_book_t *g_books;
int g_book_count;
book_list_t g_alpha, g_continue, g_recent, g_downloaded;
author_t *g_authors;
int g_author_count;

static volatile bool s_refresh_requested;
static bool s_from_cache;
static uint32_t s_books_loaded_at;

static lv_obj_t *s_scr, *s_msg, *s_notice, *s_notice_icon, *s_notice_text;
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_dock[PAGE_COUNT];
static ui_page_t s_page;

/* ---------- helpers ---------- */

lv_obj_t *ui_round_button(lv_obj_t *parent, int size, const char *text, const lv_font_t *font, lv_event_cb_t cb,
                          void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(b, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COLOR_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int width)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) {
        lv_obj_set_width(l, width);
    }
    lv_label_set_text(l, "");
    return l;
}

void ui_one_line(lv_obj_t *label, const lv_font_t *font)
{
    // "..." truncation only happens when the height is fixed; otherwise the label wraps.
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

lv_obj_t *ui_page_container(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 360, 360);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

void ui_book_subtitle(const abs_book_t *b, char *buf, size_t len)
{
    // Downloaded (or downloading) books carry a marker after the progress.
    dl_state_t dl = download_state(b->id, NULL);
    const char *mark = dl == DL_DONE ? "  " LV_SYMBOL_SD_CARD
                       : (dl == DL_QUEUED || dl == DL_ACTIVE) ? "  " LV_SYMBOL_DOWNLOAD
                                                               : "";
    const size_t mark_len = strlen(mark);
    if (len <= mark_len) return ui_book_subtitle_plain(b, buf, len);
    ui_book_subtitle_plain(b, buf, len - mark_len);
    strcat(buf, mark);
}

void ui_book_subtitle_plain(const abs_book_t *b, char *buf, size_t len)
{
    if (b->podcast) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " %d episode%s", b->author, b->num_episodes,
                 b->num_episodes == 1 ? "" : "s");
    } else if (b->finished) {
        snprintf(buf, len, "%s  " LV_SYMBOL_OK, b->author);
    } else if (b->current_time > 0 && b->progress < 0.01f) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " <1%%", b->author);
    } else if (b->current_time > 0) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " %d%%", b->author, (int)(b->progress * 100));
    } else {
        snprintf(buf, len, "%s", b->author);
    }
}

bool ui_book_in_progress(const abs_book_t *b)
{
    return b->current_time > 0 && !b->finished;
}

static void on_book_row_clicked(lv_event_t *e)
{
    ui_open_book((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_book_row_long(lv_event_t *e)
{
    ui_sheet_show((int)(intptr_t)lv_event_get_user_data(e));
}

int ui_find_book(const char *item_id)
{
    for (int i = 0; item_id && item_id[0] && i < g_book_count; i++) {
        if (strcmp(g_books[i].id, item_id) == 0) return i;
    }
    return -1;
}

int ui_book_list(const abs_book_t **books)
{
    *books = g_books;
    return g_book_count;
}

bool ui_play_item(const char *item_id)
{
    const int i = ui_find_book(item_id);
    if (i < 0) return false;
    ui_play_book(i);
    return true;
}

lv_obj_t *ui_add_book_row(lv_obj_t *list, int book_index)
{
    const abs_book_t *b = &g_books[book_index];
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 10, 0);
    lv_obj_set_style_pad_row(btn, 2, 0);

    lv_obj_t *t = lv_label_create(btn);
    lv_label_set_text(t, b->title);
    ui_one_line(t, &ui_font_16);
    lv_obj_set_style_text_color(t, COLOR_TEXT, 0);

    char sub[160];
    ui_book_subtitle(b, sub, sizeof(sub));
    lv_obj_t *a = lv_label_create(btn);
    lv_label_set_text(a, sub);
    ui_one_line(a, &ui_font_14);
    lv_obj_set_style_text_color(a, ui_book_in_progress(b) ? COLOR_ACCENT : COLOR_MUTED, 0);

    // Short tap plays; long-press opens the details sheet (short-click isn't sent after a long press).
    lv_obj_add_event_cb(btn, on_book_row_clicked, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)book_index);
    lv_obj_add_event_cb(btn, on_book_row_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)book_index);
    return btn;
}

void ui_open_book(int book_index)
{
    if (book_index < 0 || book_index >= g_book_count) return;
    if (g_books[book_index].podcast) {
        ui_episodes_show(book_index);
        return;
    }
    ui_play_book(book_index);
}

void ui_play_book(int book_index)
{
    if (book_index < 0 || book_index >= g_book_count) return;
    const abs_book_t *b = &g_books[book_index];
    if (b->podcast && !b->resume_episode[0]) {
        ui_episodes_show(book_index);
        return;
    }
    player_open(b);
    playing_prepare(book_index);
    ui_show_page(PAGE_PLAYER);
}

/* ---------- pages and dock ---------- */

static bool __attribute__((unused)) is_page(const lv_obj_t *o)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (o == s_pages[i]) return true;
    }
    return false;
}

static void dock_highlight(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool on = i == (int)s_page;
        lv_obj_set_style_bg_color(s_dock[i], on ? COLOR_ACCENT : COLOR_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_dock[i], 0), on ? lv_color_black() : COLOR_TEXT, 0);
    }
}

static void refresh_page(void)
{
    switch (s_page) {
    case PAGE_HOME:     home_refresh(); break;
    case PAGE_LIBRARY:  library_refresh(); break;
    case PAGE_PLAYER:   playing_refresh(); break;
    case PAGE_SETTINGS: settings_refresh(); break;
    default: break;
    }
}

void ui_show_page(ui_page_t page)
{
    s_page = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)page) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    dock_highlight();
    refresh_page();  // straight away, not on the next tick: no flash of stale or empty values
    if (page == PAGE_HOME && g_book_count && lv_tick_elaps(s_books_loaded_at) > STALE_MS) {
        s_refresh_requested = true;  // pick up progress changes from this and other devices
    }
}

// Dock buttons take touches inside their circle only: the square around the outer buttons'
// circles reaches the side arcs and the A-Z ring.
static void round_hit_test(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_param(e);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    const int32_t r = lv_area_get_width(&a) / 2, dx = info->point->x - (a.x1 + r), dy = info->point->y - (a.y1 + r);
    info->res = dx * dx + dy * dy <= r * r;
}

static void on_dock(lv_event_t *e)
{
    ui_show_page((ui_page_t)(intptr_t)lv_event_get_user_data(e));
}

// Rebuilds g_downloaded; returns true if its membership changed (not just progress).
static bool rebuild_downloaded(void)
{
    static int *prev;
    static int prev_n;
    int n = 0;
    for (int i = 0; i < g_alpha.count; i++) {
        dl_state_t st = download_state(g_books[g_alpha.idx[i]].id, NULL);
        if (st != DL_NONE && st != DL_REMOVING) g_downloaded.idx[n++] = g_alpha.idx[i];
    }
    g_downloaded.count = n;
    bool changed = n != prev_n || (n && memcmp(prev, g_downloaded.idx, n * sizeof(int)) != 0);
    free(prev);
    prev = heap_caps_malloc((n ? n : 1) * sizeof(int), MALLOC_CAP_SPIRAM);
    memcpy(prev, g_downloaded.idx, n * sizeof(int));
    prev_n = n;
    return changed;
}

#ifdef LAYOUT_AUDIT
#include <math.h>
// Reports tappable objects whose (parent-clipped) bounds reach an arc's touch zone.
typedef struct { float r, lo, hi; } zone_t;  // radius from which the zone starts; angle range (deg)
static void audit(lv_obj_t *o, const char *page, const zone_t *z, int nz, const lv_area_t *clip)
{
    uint32_t n = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(o, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN) || lv_obj_check_type(c, &lv_arc_class)) continue;
        lv_area_t a, cl;
        lv_obj_get_coords(c, &a);
        cl.x1 = LV_MAX(a.x1, clip->x1);
        cl.y1 = LV_MAX(a.y1, clip->y1);
        cl.x2 = LV_MIN(a.x2, clip->x2);
        cl.y2 = LV_MIN(a.y2, clip->y2);
        if (cl.x1 > cl.x2 || cl.y1 > cl.y2) continue;
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && !is_page(c) && lv_obj_has_flag(c, LV_OBJ_FLAG_ADV_HITTEST)) {
            // Round buttons: test the circle's farthest point from the centre and its angular span.
            const float rad = lv_area_get_width(&a) / 2.0f, cx = a.x1 + rad - 180, cy = a.y1 + rad - 180;
            const float d = sqrtf(cx * cx + cy * cy), ang = atan2f(cy, cx) * 57.2958f;
            for (int zi = 0; zi < nz; zi++) {
                if (d + rad <= z[zi].r) continue;
                // Angular half-width of the part of the circle beyond the zone radius.
                const float cosv = (d * d + z[zi].r * z[zi].r - rad * rad) / (2 * d * z[zi].r);
                const float half = acosf(cosv > 1 ? 1 : cosv) * 57.2958f;
                float lo = z[zi].lo, hi = z[zi].hi, a2 = ang;
                if (lo > 180 && a2 < 0) a2 += 360;
                if (a2 + half >= lo && a2 - half <= hi) {
                    printf("AUDIT %s: round obj at (%.0f,%.0f) r=%.0f reaches %.0f..%.0f deg\n", page, cx + 180,
                           cy + 180, rad, a2 - half, a2 + half);
                }
            }
        } else if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE) && !is_page(c)) {
            int xs[2] = {cl.x1 - 180, cl.x2 - 180}, ys[2] = {cl.y1 - 180, cl.y2 - 180};
            for (int k = 0; k < 4; k++) {
                float x = xs[k & 1], y = ys[k >> 1], r = sqrtf(x * x + y * y), ang = atan2f(y, x) * 57.2958f;
                bool hit = false;
                for (int zi = 0; zi < nz; zi++) {
                    float lo = z[zi].lo, hi = z[zi].hi, a2 = ang;
                    if (lo > 180 && a2 < 0) a2 += 360;
                    if (r > z[zi].r && a2 >= lo && a2 <= hi) hit = true;
                }
                if (hit) {
                    printf("AUDIT %s: obj (%d,%d)-(%d,%d) corner r=%.0f ang=%.0f\n", page, (int)cl.x1, (int)cl.y1,
                           (int)cl.x2, (int)cl.y2, r, ang);
                    break;
                }
            }
        }
        audit(c, page, z, nz, &cl);
    }
}
#endif

static void refresh_timer(lv_timer_t *t)
{
    static int status_tick;
    if (++status_tick % 4 == 0) status_refresh();  // once a second
#ifdef LAYOUT_AUDIT
    static int tick;
    if (g_book_count && ++tick == 8) {
        const lv_area_t full = {0, 0, 359, 359};
        const zone_t lib[] = {{160, -66, 36}};
        const zone_t np[] = {{150, -54, 61}, {150, 119, 234}};
        for (int p = 0; p < PAGE_COUNT; p++) {
            ui_show_page(p);
            lv_obj_update_layout(s_scr);
            if (p == PAGE_LIBRARY) audit(s_scr, "library", lib, 1, &full);
            if (p == PAGE_PLAYER) audit(s_scr, "player", np, 2, &full);
        }
        ui_show_page(PAGE_HOME);
        printf("AUDIT done\n");
    }
#endif
    static uint32_t dl_gen;
    if (g_book_count && download_generation() != dl_gen) {
        dl_gen = download_generation();
        if (rebuild_downloaded()) home_lists_changed();
    }
    if (ui_sheet_visible()) {
        ui_sheet_refresh();
        return;
    }
    if (ui_episodes_visible()) {
        ui_episodes_refresh();
        return;
    }
    if (libpicker_visible()) return;
    if (ui_setup_visible()) {
        ui_setup_refresh();
        return;
    }

    refresh_page();
    // A subtle cue on the dock when something is playing.
    player_status_t st;
    player_get_status(&st);
    bool active = st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING || st.state == PLAYER_LOADING;
    lv_obj_set_style_border_width(s_dock[PAGE_PLAYER], active && s_page != PAGE_PLAYER ? 2 : 0, 0);
}

void ui_init(void)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_text_font(s_scr, &ui_font_16, 0);  // the default for labels that don't pick one
    lv_obj_set_style_bg_color(s_scr, COLOR_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    void (*builders[PAGE_COUNT])(lv_obj_t *) = {home_build, library_build, playing_build, settings_build};
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_pages[i] = ui_page_container(s_scr);
        builders[i](s_pages[i]);
    }

    // Dock last, so it is above the pages.
    static const char *icons[PAGE_COUNT] = {LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_PLAY,
                                                LV_SYMBOL_SETTINGS};
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dock[i] = ui_round_button(s_scr, 38, icons[i], &ui_font_16, on_dock, (void *)(intptr_t)i);
        // 44 px apart: the outer buttons stay inside the Library's A-Z ring.
        lv_obj_align(s_dock[i], LV_ALIGN_TOP_MID, (2 * i - (PAGE_COUNT - 1)) * 22, DOCK_Y);
        lv_obj_set_style_border_color(s_dock[i], COLOR_ACCENT, 0);
        lv_obj_add_flag(s_dock[i], LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_add_event_cb(s_dock[i], round_hit_test, LV_EVENT_HIT_TEST, NULL);
    }

    sheet_build(s_scr);
    episodes_build(s_scr);
    libpicker_build(s_scr);
    setup_build(s_scr);
    status_build(s_scr);  // last, so it stays above the pages and overlays

    s_msg = ui_label(s_scr, &ui_font_16, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_msg);

    // Notice: the screen dimmed behind a card with a big icon and text.
    s_notice = ui_page_container(s_scr);
    lv_obj_set_style_bg_color(s_notice, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_notice, LV_OPA_70, 0);
    lv_obj_t *card = lv_obj_create(s_notice);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 230, 150);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 24, 0);
    lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_notice_icon = ui_label(card, &lv_font_montserrat_40, COLOR_ACCENT, 0);
    lv_obj_align(s_notice_icon, LV_ALIGN_TOP_MID, 0, 24);
    s_notice_text = ui_label(card, &ui_font_20, COLOR_TEXT, 210);
    lv_obj_align(s_notice_text, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);

    ui_show_page(PAGE_HOME);
    lv_timer_create(refresh_timer, 250, NULL);
}

void ui_show_notice(const char *icon, const char *text)
{
    lvgl_port_lock(0);
    if (text) {
        lv_label_set_text(s_notice_icon, icon ? icon : "");
        lv_label_set_text(s_notice_text, text);
        lv_obj_remove_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_notice);
    } else {
        lv_obj_add_flag(s_notice, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void ui_show_message_locked(const char *msg)
{
    lvgl_port_lock(0);
    ui_show_message(msg);
    lvgl_port_unlock();
}

/* ---------- libraries ---------- */

static abs_library_t *s_libs;
static int s_lib_count;
static char s_lib_selected[40];
static char s_lib_request[40];
static volatile bool s_lib_requested;

void ui_set_libraries(const abs_library_t *libs, int count, const char *selected_id)
{
    free(s_libs);
    s_libs = heap_caps_malloc((count ? count : 1) * sizeof(abs_library_t), MALLOC_CAP_SPIRAM);
    memcpy(s_libs, libs, count * sizeof(abs_library_t));
    s_lib_count = count;
    strlcpy(s_lib_selected, selected_id, sizeof(s_lib_selected));
    library_names_changed();
    settings_libraries_changed();
}

const abs_library_t *ui_libraries(int *count, const char **selected_id)
{
    *count = s_lib_count;
    *selected_id = s_lib_selected;
    return s_libs;
}

bool ui_library_is_podcast(void)
{
    for (int i = 0; i < s_lib_count; i++) {
        if (strcmp(s_libs[i].id, s_lib_selected) == 0) return s_libs[i].podcast;
    }
    return false;
}

void ui_request_library(const char *library_id)
{
    strlcpy(s_lib_request, library_id, sizeof(s_lib_request));
    s_lib_requested = true;
}

bool ui_take_library_request(char *library_id, size_t len)
{
    if (!s_lib_requested) return false;
    s_lib_requested = false;
    strlcpy(library_id, s_lib_request, len);
    return true;
}

void ui_show_message(const char *msg)
{
    lv_label_set_text(s_msg, msg ? msg : "");
    if (msg && msg[0]) {
        lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_msg);
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- derived book lists ---------- */

static int cmp_last_update(const void *a, const void *b)
{
    double x = g_books[*(const int *)a].last_update, y = g_books[*(const int *)b].last_update;
    return x < y ? 1 : (x > y ? -1 : 0);
}

static int cmp_added(const void *a, const void *b)
{
    double x = g_books[*(const int *)a].added_at, y = g_books[*(const int *)b].added_at;
    return x < y ? 1 : (x > y ? -1 : 0);
}

static int cmp_author(const void *a, const void *b)
{
    const author_t *x = a, *y = b;
    int r = text_cmp(x->sort_key, y->sort_key);
    return r ? r : text_cmp(x->name, y->name);
}

static void *ps_alloc(size_t n)
{
    return heap_caps_calloc(1, n ? n : 1, MALLOC_CAP_SPIRAM);
}

static char *ps_strdup(const char *s, size_t n)
{
    char *d = ps_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}

static void free_lists(void)
{
    free(g_alpha.idx);
    free(g_continue.idx);
    free(g_recent.idx);
    free(g_downloaded.idx);
    for (int i = 0; i < g_author_count; i++) {
        free(g_authors[i].name);
        free(g_authors[i].sort_key);
        free(g_authors[i].books.idx);
    }
    free(g_authors);
    memset(&g_alpha, 0, sizeof(g_alpha));
    memset(&g_continue, 0, sizeof(g_continue));
    memset(&g_recent, 0, sizeof(g_recent));
    memset(&g_downloaded, 0, sizeof(g_downloaded));
    g_authors = NULL;
    g_author_count = 0;
}

// Books can credit several authors ("A, B"); each author gets an entry listing the book.
static void build_authors(void)
{
    int cap = g_book_count * 2 + 1, n = 0;
    author_t *authors = ps_alloc(cap * sizeof(author_t));
    for (int i = 0; i < g_book_count; i++) {
        const char *p = g_books[i].author;
        while (p && *p) {
            const char *end = strstr(p, ", ");
            size_t len = end ? (size_t)(end - p) : strlen(p);
            // Server data can carry stray spaces ("Joe White "), which would break surname sorting.
            const char *next = end ? end + 2 : NULL;
            while (len && *p == ' ') {
                p++;
                len--;
            }
            while (len && p[len - 1] == ' ') len--;
            if (len) {
                int a = 0;
                while (a < n && !(strlen(authors[a].name) == len && strncasecmp(authors[a].name, p, len) == 0)) a++;
                if (a == n && n < cap) {
                    authors[n].name = ps_strdup(p, len);
                    // Sort by surname: the last word of the name.
                    const char *sp = strrchr(authors[n].name, ' ');
                    const char *surname = sp ? sp + 1 : authors[n].name;
                    authors[n].sort_key = ps_strdup(surname, strlen(surname));
                    authors[n].books.idx = ps_alloc(8 * sizeof(int));
                    n++;
                }
                if (a < n) {
                    book_list_t *bl = &authors[a].books;
                    if (bl->count && bl->count % 8 == 0) {
                        bl->idx = heap_caps_realloc(bl->idx, (bl->count + 8) * sizeof(int), MALLOC_CAP_SPIRAM);
                    }
                    bl->idx[bl->count++] = i;
                }
            }
            p = next;
        }
    }
    qsort(authors, n, sizeof(author_t), cmp_author);
    g_authors = authors;
    g_author_count = n;
}

void ui_set_books(const abs_book_t *books, int count)
{
    free_lists();
    g_books = books;
    g_book_count = count;
    s_books_loaded_at = lv_tick_get();

    g_alpha.idx = ps_alloc(count * sizeof(int));
    g_continue.idx = ps_alloc(count * sizeof(int));
    g_recent.idx = ps_alloc(count * sizeof(int));
    g_downloaded.idx = ps_alloc(count * sizeof(int));
    for (int i = 0; i < count; i++) {
        g_alpha.idx[g_alpha.count++] = i;  // the server list is already A-Z
        g_recent.idx[g_recent.count++] = i;
        if (ui_book_in_progress(&books[i])) g_continue.idx[g_continue.count++] = i;
    }
    qsort(g_continue.idx, g_continue.count, sizeof(int), cmp_last_update);
    qsort(g_recent.idx, g_recent.count, sizeof(int), cmp_added);
    if (g_recent.count > RECENT_MAX) g_recent.count = RECENT_MAX;
    build_authors();
    rebuild_downloaded();

    home_set_books();
    library_set_books();
    ui_show_message(count ? NULL : "No books found");
}

void ui_set_source(bool from_cache)
{
    s_from_cache = from_cache;
}

void ui_get_source(uint32_t *loaded_tick, bool *from_cache)
{
    *loaded_tick = s_books_loaded_at;
    *from_cache = s_from_cache;
}

void ui_request_refresh(void)
{
    s_refresh_requested = true;
}

bool ui_take_refresh_request(void)
{
    bool r = s_refresh_requested;
    s_refresh_requested = false;
    return r;
}

