// Now Playing: the loaded book with a chapter-progress ring around the edge. When nothing is loaded
// it offers the most recently listened book so playback can be resumed with one tap.

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "cover.h"
#include "player.h"
#include "ui_priv.h"

static lv_obj_t *s_backdrop, *s_arc, *s_vol_arc, *s_title, *s_chapter, *s_state, *s_play_label, *s_time, *s_remaining;
static const lv_image_dsc_t *s_backdrop_src;
static lv_obj_t *s_skip_lbl[2];  // -N / +N on the skip buttons
static lv_obj_t *s_sleep_lbl;     // sleep timer button: "Zz", or the minutes left
static bool s_arc_dragging, s_vol_dragging;
static uint32_t s_state_override_until;
static int s_resume = -1;  // book offered for resume while the player is idle

static void fmt_time(char *buf, size_t len, double secs)
{
    int s = secs < 0 ? 0 : (int)secs;
    if (s >= 3600) {
        snprintf(buf, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(buf, len, "%d:%02d", s / 60, s % 60);
    }
}

static void flash_state(const char *text)
{
    lv_label_set_text(s_state, text);
    s_state_override_until = lv_tick_get() + 1500;
}

// "11h 21m left  •  34%", with "<1%" for a book that has only just been started.
static void fmt_remaining(char *buf, size_t len, double position, double duration)
{
    int left = (int)(duration - position);
    double frac = duration > 0 ? position / duration : 0;
    char pct[8];
    if (frac > 0 && frac < 0.01) snprintf(pct, sizeof(pct), "<1%%");
    else snprintf(pct, sizeof(pct), "%d%%", (int)(100 * frac));
    snprintf(buf, len, "%dh %02dm left  " LV_SYMBOL_BULLET "  %s", left / 3600, (left / 60) % 60, pct);
}

static bool player_loaded(const player_status_t *st)
{
    return st->state != PLAYER_IDLE && st->item_id[0];
}

/* ---------- events ---------- */

// Play/pause; with nothing loaded, resumes the most recent book. (Also the BOOT button.)
void ui_toggle_play(void)
{
    player_status_t st;
    player_get_status(&st);
    if (!player_loaded(&st)) {
        // The page sets s_resume when it's shown; from the BOOT button it may not have been yet.
        const int i = s_resume >= 0 ? s_resume : (g_continue.count ? g_continue.idx[0] : -1);
        if (i >= 0) ui_play_book(i);
        return;
    }
    player_toggle();
}

static void on_toggle(lv_event_t *e)
{
    ui_toggle_play();
}

// Tapping the title opens the details sheet (download controls) for the book shown.
static void on_title(lv_event_t *e)
{
    player_status_t st;
    player_get_status(&st);
    int i = player_loaded(&st) ? ui_find_book(st.item_id) : s_resume;
    if (i >= 0) ui_sheet_show(i);
}

// Skip buttons; how far they jump is a setting (config.h).
// Skip lengths come from the settings at tap time (Settings > Device can change them).
static int skip_seconds(int dir)
{
    return dir < 0 ? -config_get()->skip_back_s : config_get()->skip_fwd_s;
}

static void set_skip_labels(void)
{
    char lbl[8];
    snprintf(lbl, sizeof(lbl), "%+d", skip_seconds(-1));
    if (strcmp(lv_label_get_text(s_skip_lbl[0]), lbl) != 0) lv_label_set_text(s_skip_lbl[0], lbl);
    snprintf(lbl, sizeof(lbl), "%+d", skip_seconds(1));
    if (strcmp(lv_label_get_text(s_skip_lbl[1]), lbl) != 0) lv_label_set_text(s_skip_lbl[1], lbl);
}

static void on_skip(lv_event_t *e)
{
    const int s = skip_seconds((int)(intptr_t)lv_event_get_user_data(e));
    player_seek_relative(s);
    char buf[16];
    snprintf(buf, sizeof(buf), "%+d s", s);
    flash_state(buf);
}
// Each tap moves to the next sleep timer choice: off, 15, 30, 45, 60 min, end of chapter, off.
// The step comes from the player's state, so a timer set from the remote carries on from there:
// a running timer moves to the first choice longer than the minutes left.
static int next_sleep_choice(const player_status_t *st)
{
    static const int MINUTES[] = {15, 30, 45, 60};
    if (st->sleep_mode == PLAYER_SLEEP_CHAPTER) return 0;
    if (st->sleep_mode == PLAYER_SLEEP_OFF) return MINUTES[0];
    const int left = (int)(st->sleep_left + 59) / 60;
    for (int i = 0; i < sizeof(MINUTES) / sizeof(MINUTES[0]); i++) {
        if (MINUTES[i] > left) return MINUTES[i];
    }
    return PLAYER_SLEEP_END_OF_CHAPTER;
}

static void on_sleep(lv_event_t *e)
{
    player_status_t st;
    player_get_status(&st);
    const int m = next_sleep_choice(&st);
    player_set_sleep(m);
    char buf[40];
    if (m == 0) snprintf(buf, sizeof(buf), "Sleep timer off");
    else if (m < 0) snprintf(buf, sizeof(buf), "Sleep at the end of the chapter");
    else snprintf(buf, sizeof(buf), "Sleep in %d min", m);
    flash_state(buf);
}

static void on_prev_ch(lv_event_t *e) { player_chapter_step(-1); flash_state("Previous chapter"); }
static void on_next_ch(lv_event_t *e) { player_chapter_step(1); flash_state("Next chapter"); }

// Left arc: volume, louder towards the top. Applied live while dragging.
static void on_volume_arc(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_vol_dragging = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_vol_dragging = false;
    } else if (code != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    int v = lv_arc_get_value(s_vol_arc);
    player_set_volume(v);
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " %d%%", v);
    flash_state(buf);
}

// Dragging the ring scrubs within the current chapter.
static void on_arc_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST) {
        return;  // layout/draw events arrive too, some before the player exists
    }
    player_status_t st;
    player_get_status(&st);
    double len = st.chapter_end - st.chapter_start;
    double target = st.chapter_start + len * lv_arc_get_value(s_arc) / 1000.0;
    if (code == LV_EVENT_PRESSED) {
        s_arc_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && s_arc_dragging) {
        char buf[16];
        fmt_time(buf, sizeof(buf), target - st.chapter_start);
        flash_state(buf);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_arc_dragging = false;
        if (len > 0 && player_loaded(&st)) {
            player_seek_to(target);
        }
    }
}

/* ---------- refresh ---------- */

static void set_backdrop(const char *item_id)
{
    const lv_image_dsc_t *bd = cover_get(item_id, COVER_BACKDROP);
    if (bd == s_backdrop_src) return;
    if (bd) {
        lv_image_set_src(s_backdrop, bd);
        lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    s_backdrop_src = bd;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void refresh_idle(void)
{
    s_resume = g_continue.count ? g_continue.idx[0] : -1;
    lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
    if (s_resume < 0) {
        set_backdrop(NULL);
        set_text(s_title, "Nothing playing");
        set_text(s_chapter, "");
        set_text(s_state, "");
        set_text(s_time, "");
        set_text(s_remaining, "");
        lv_arc_set_value(s_arc, 0);
        return;
    }
    const abs_book_t *b = &g_books[s_resume];
    set_backdrop(b->id);
    set_text(s_title, b->title);
    set_text(s_chapter, b->author);
    if (lv_tick_get() > s_state_override_until) set_text(s_state, "Tap " LV_SYMBOL_PLAY " to resume");
    char buf[48];
    if (b->podcast) {
        // Shows have no length of their own: describe the episode that will resume.
        snprintf(buf, sizeof(buf), "Latest episode  " LV_SYMBOL_BULLET "  %d%%", (int)(b->progress * 100) ?: 1);
    } else {
        fmt_remaining(buf, sizeof(buf), b->current_time, b->duration);
    }
    set_text(s_remaining, buf);
    set_text(s_time, "");
    lv_arc_set_value(s_arc, (int)(1000 * b->progress));
}

void playing_refresh(void)
{
    player_status_t st;
    player_get_status(&st);
    if (!s_vol_dragging) lv_arc_set_value(s_vol_arc, st.volume);
    set_skip_labels();
    if (!player_loaded(&st)) {
        refresh_idle();
        return;
    }
    s_resume = -1;
    set_backdrop(st.item_id);

    if (st.title[0]) set_text(s_title, st.title);
    set_text(s_chapter, st.chapter[0] ? st.chapter : st.author);

    bool playing = st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING || st.state == PLAYER_LOADING;
    lv_label_set_text(s_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (lv_tick_get() > s_state_override_until) {
        const char *msg = "";
        switch (st.state) {
        case PLAYER_LOADING:   msg = "Opening..."; break;
        case PLAYER_BUFFERING: msg = "Buffering..."; break;
        case PLAYER_PAUSED:    msg = "Paused"; break;
        case PLAYER_FINISHED:  msg = "Finished"; break;
        case PLAYER_ERROR:     msg = "Stream error - tap " LV_SYMBOL_PLAY " to retry"; break;
        default: break;
        }
        char sleep_msg[40];
        if (!msg[0] && st.sleep_mode == PLAYER_SLEEP_TIMER) {
            snprintf(sleep_msg, sizeof(sleep_msg), "Sleep in %d min", (int)(st.sleep_left + 59) / 60);
            msg = sleep_msg;
        } else if (!msg[0] && st.sleep_mode == PLAYER_SLEEP_CHAPTER) {
            msg = "Sleep at the end of the chapter";
        }
        set_text(s_state, msg);
    }

    char sl[16];
    if (st.sleep_mode == PLAYER_SLEEP_TIMER) snprintf(sl, sizeof(sl), "%dm", (int)(st.sleep_left + 59) / 60);
    else if (st.sleep_mode == PLAYER_SLEEP_CHAPTER) snprintf(sl, sizeof(sl), "Ch");
    else snprintf(sl, sizeof(sl), "Zz");
    set_text(s_sleep_lbl, sl);
    lv_obj_set_style_text_color(s_sleep_lbl, st.sleep_mode != PLAYER_SLEEP_OFF ? COLOR_ACCENT : COLOR_TEXT, 0);

    if (st.duration > 0) {
        double ch_len = st.chapter_end - st.chapter_start;
        double in_ch = st.position - st.chapter_start;
        if (!s_arc_dragging && ch_len > 0) {
            lv_arc_set_value(s_arc, (int)(1000 * in_ch / ch_len));
        }
        char a[16], b[16], buf[48];
        fmt_time(a, sizeof(a), in_ch);
        fmt_time(b, sizeof(b), ch_len);
        snprintf(buf, sizeof(buf), "%s / %s", a, b);
        set_text(s_time, buf);

        fmt_remaining(buf, sizeof(buf), st.position, st.duration);
        set_text(s_remaining, buf);
    }
}

void playing_prepare(int book_index)
{
    const abs_book_t *b = &g_books[book_index];
    s_backdrop_src = NULL;
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, b->title);
    lv_label_set_text(s_chapter, b->author);
    lv_label_set_text(s_state, "Opening...");
    lv_label_set_text(s_time, "");
    lv_label_set_text(s_remaining, "");
    lv_arc_set_value(s_arc, 0);
}

/* ---------- build ---------- */

static lv_obj_t *side_arc(lv_obj_t *page, int start, int end, int range, lv_event_cb_t cb)
{
    lv_obj_t *a = lv_arc_create(page);
    lv_obj_set_size(a, 348, 348);
    lv_obj_center(a);
    lv_arc_set_bg_angles(a, start, end);
    lv_arc_set_range(a, 0, range);
    lv_arc_set_value(a, 0);  // until set, LVGL draws a default indicator ignoring the angles above
    lv_obj_set_style_arc_width(a, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(a, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(a, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(a, cb, LV_EVENT_ALL, NULL);
    // Only the arc should grab touches, not the whole square it sits in. A thin arc at the rim
    // leaves a touch band only ~12 px wide where the panel is least sensitive, so widen it inwards
    // (the zone then starts ~150 px from the centre; controls stay inside that, see ui_priv.h).
    lv_obj_add_flag(a, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_set_ext_click_area(a, 18);
    return a;
}

void playing_build(lv_obj_t *page)
{
    // Cover art fills the round screen behind everything else (pre-dimmed by the loader).
    s_backdrop = lv_image_create(page);
    lv_obj_center(s_backdrop);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);

    // Two side arcs, sized like the Library's A-Z ring: progress on the right (fills from the top,
    // drag to scrub the chapter) and volume on the left (fills from the bottom).
    // The top ends stop short of 1 and 11 o'clock, clear of the dock's outer buttons.
    s_arc = side_arc(page, 312, 55, 1000, on_arc_event);
    s_vol_arc = side_arc(page, 125, 228, 100, on_volume_arc);
    lv_obj_t *vol_icon = ui_label(page, &ui_font_14, COLOR_MUTED, 0);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(vol_icon, LV_ALIGN_CENTER, -146, 0);

    // Everything tappable stays inside radius ~150 so it never sits under the ring (ui_priv.h).
    s_title = ui_label(page, &ui_font_20, COLOR_TEXT, 200);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -90);
    lv_obj_add_flag(s_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_title, on_title, LV_EVENT_CLICKED, NULL);

    s_chapter = ui_label(page, &ui_font_14, COLOR_MUTED, 230);
    lv_label_set_long_mode(s_chapter, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_chapter, LV_ALIGN_CENTER, 0, -67);

    s_state = ui_label(page, &ui_font_14, COLOR_ACCENT, 240);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, -48);

    lv_obj_t *play = ui_round_button(page, 80, LV_SYMBOL_PLAY, &lv_font_montserrat_40, on_toggle, NULL);
    lv_obj_set_style_bg_color(play, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xC07818), LV_STATE_PRESSED);
    s_play_label = lv_obj_get_child(play, 0);
    lv_obj_set_style_text_color(s_play_label, lv_color_black(), 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *b30 = ui_round_button(page, 60, "", &ui_font_20, on_skip, (void *)(intptr_t)-1);
    lv_obj_align(b30, LV_ALIGN_CENTER, -94, 4);
    lv_obj_t *f30 = ui_round_button(page, 60, "", &ui_font_20, on_skip, (void *)(intptr_t)1);
    lv_obj_align(f30, LV_ALIGN_CENTER, 94, 4);
    s_skip_lbl[0] = lv_obj_get_child(b30, 0);
    s_skip_lbl[1] = lv_obj_get_child(f30, 0);
    set_skip_labels();

    s_time = ui_label(page, &ui_font_16, COLOR_TEXT, 200);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 58);
    s_remaining = ui_label(page, &ui_font_14, COLOR_MUTED, 220);
    lv_obj_align(s_remaining, LV_ALIGN_CENTER, 0, 79);

    // Bottom row: previous chapter, sleep timer, next chapter (outer edges stay within r ~150).
    lv_obj_t *prev = ui_round_button(page, 38, LV_SYMBOL_PREV, &ui_font_16, on_prev_ch, NULL);
    lv_obj_align(prev, LV_ALIGN_CENTER, -52, 114);
    lv_obj_t *sleep = ui_round_button(page, 38, "Zz", &ui_font_14, on_sleep, NULL);
    lv_obj_align(sleep, LV_ALIGN_CENTER, 0, 114);
    s_sleep_lbl = lv_obj_get_child(sleep, 0);
    lv_obj_t *next = ui_round_button(page, 38, LV_SYMBOL_NEXT, &ui_font_16, on_next_ch, NULL);
    lv_obj_align(next, LV_ALIGN_CENTER, 52, 114);
}
