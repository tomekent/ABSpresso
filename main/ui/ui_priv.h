#pragma once

// Shared between the UI shell (ui.c) and its pages. Everything here runs in the LVGL task.

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "abs_api.h"
#include "ui_fonts.h"

#define COLOR_BG     lv_color_hex(0x101418)
#define COLOR_CARD   lv_color_hex(0x1E252C)
#define COLOR_ACCENT lv_color_hex(0xF0A030)
#define COLOR_TEXT   lv_color_hex(0xF2F2F2)
#define COLOR_MUTED  lv_color_hex(0x8C96A0)

// Touch layout rule: the arcs claim touches from their (widened) inner edge outwards, within their
// angles: Now Playing's side arcs from ~150 px from the screen centre, the Library's A-Z ring from
// ~160 px. Keep tappable things' bounding boxes inside those radii where they share angles with
// an arc, and inside ~160 elsewhere.

// The dock spans y 39..77; pages start below it.
#define DOCK_Y   39
#define PAGE_TOP 84

// Shared page layout, so Home and Library line up: covers (and lists) in the middle, the switcher
// pill at the bottom.
#define PAGE_CAROUSEL_Y (PAGE_TOP + 2)
#define PAGE_SWITCHER_Y (PAGE_TOP + 196)

typedef enum {
    PAGE_HOME,
    PAGE_LIBRARY,
    PAGE_PLAYER,
    PAGE_SETTINGS,
    PAGE_COUNT,
} ui_page_t;

// Views of the book array as index lists into g_books.
typedef struct {
    int *idx;
    int count;
} book_list_t;

typedef struct {
    char *name;
    char *sort_key;  // surname first, for A-Z
    book_list_t books;
} author_t;

extern const abs_book_t *g_books;
extern int g_book_count;
extern book_list_t g_alpha;      // all books A-Z
extern book_list_t g_continue;   // started, not finished; most recently listened first
extern book_list_t g_recent;     // newest additions first
extern book_list_t g_downloaded; // on the SD card or downloading, A-Z
extern author_t *g_authors;      // A-Z by surname
extern int g_author_count;

/* helpers (ui.c) */
lv_obj_t *ui_round_button(lv_obj_t *parent, int size, const char *text, const lv_font_t *font, lv_event_cb_t cb,
                          void *user);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int width);
lv_obj_t *ui_page_container(lv_obj_t *parent);
// Makes a label a single full-width line that ends in "..." when too long.
void ui_one_line(lv_obj_t *label, const lv_font_t *font);
// "Author  •  34%", plus an SD/download marker for downloaded books.
void ui_book_subtitle(const abs_book_t *b, char *buf, size_t len);
void ui_book_subtitle_plain(const abs_book_t *b, char *buf, size_t len);
bool ui_book_in_progress(const abs_book_t *b);
lv_obj_t *ui_add_book_row(lv_obj_t *list, int book_index);
// Tap on a book: plays it; tap on a podcast show: opens its episode list.
void ui_open_book(int book_index);
// Plays straight away (a show resumes its latest episode).
void ui_play_book(int book_index);
void ui_show_page(ui_page_t page);

/* book details sheet (ui_sheet.c) */
void sheet_build(lv_obj_t *scr);
void ui_sheet_show(int book_index);
void ui_sheet_refresh(void);
void ui_sheet_hide(void);
bool ui_sheet_visible(void);
// Index into g_books of the book with this id, or -1.
int ui_find_book(const char *item_id);

/* libraries (ui.c) */
bool ui_library_is_podcast(void);

/* podcast episode list overlay (ui_episodes.c) */
void episodes_build(lv_obj_t *scr);
void ui_episodes_show(int book_index);
void ui_episodes_refresh(void);
void ui_episodes_hide(void);
bool ui_episodes_visible(void);

/* Settings page (ui_settings.c) */
void settings_build(lv_obj_t *page);
void settings_refresh(void);
// The server's library list changed (enables choosing between them).
void settings_libraries_changed(void);
void libpicker_build(lv_obj_t *scr);
bool libpicker_visible(void);

/* setup screen (ui_setup.c) */
void setup_build(lv_obj_t *scr);
void ui_setup_show(void);
void ui_setup_refresh(void);
bool ui_setup_visible(void);

/* status row above the dock (ui_status.c) */
void status_build(lv_obj_t *scr);
void status_refresh(void);

/* pages */
void home_build(lv_obj_t *page);
void home_set_books(void);
void home_refresh(void);
// The shelves' contents changed (e.g. a download was added or removed).
void home_lists_changed(void);

void library_build(lv_obj_t *page);
void library_set_books(void);
void library_refresh(void);
// The selected library changed type (books/podcasts): relabel views.
void library_names_changed(void);

void playing_build(lv_obj_t *page);
void playing_prepare(int book_index);
void playing_refresh(void);
