#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "abs_api.h"

// All ui_* calls must hold the LVGL port lock (they are LVGL calls).
void ui_init(void);
void ui_show_message(const char *msg);
// Play/pause from outside the UI (the BOOT button). Call with the LVGL lock held.
void ui_toggle_play(void);
// The UI keeps using `books` until the next ui_set_books(); the caller frees the old array after.
void ui_set_books(const abs_book_t *books, int count);

// Whether the books last passed to ui_set_books() came from the SD cache (vs. the server).
void ui_set_source(bool from_cache);
// When the library was last loaded (lv_tick), and whether it came from the cache.
void ui_get_source(uint32_t *loaded_tick, bool *from_cache);

// Like ui_show_message(), but takes the LVGL lock itself (for other tasks).
void ui_show_message_locked(const char *msg);
// A large notice over a dimmed screen (e.g. "Release to sleep"); NULL text hides it. Takes the
// LVGL lock itself.
void ui_show_notice(const char *icon, const char *text);

// The server's libraries and which one is selected (for Settings). The UI keeps its own copy.
void ui_set_libraries(const abs_library_t *libs, int count, const char *selected_id);
// The user picked another library: the main loop collects it and loads that library.
void ui_request_library(const char *library_id);
bool ui_take_library_request(char *library_id, size_t len);

// Opens the setup screen and portal (Wi-Fi, server, sign-in) for a phone or laptop.
void ui_setup_show(void);

// Ask the main loop to reload the library.
void ui_request_refresh(void);
// True once after a reload was requested. Lock not needed.
bool ui_take_refresh_request(void);

// For the remote control (network/remote.c). Call with the LVGL lock held.
// The books shown on the device (valid until the lock is released).
int ui_book_list(const abs_book_t **books);
// Plays a book from the library as if it was tapped. False if it isn't in the shown library.
bool ui_play_item(const char *item_id);
// The server's libraries and the selected one; switching goes through ui_request_library().
const abs_library_t *ui_libraries(int *count, const char **selected_id);
// Saves and applies the screen rotation, as the Settings switch does.
void ui_set_rotation(bool rotate180);
// Redraws Settings after a setting was changed from outside the UI.
void ui_settings_changed(void);
