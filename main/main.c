#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "abs_api.h"
#include "board.h"
#include "cover.h"
#include "player.h"
#include "storage.h"
#include "battery.h"
#include "power.h"
#include "catalog.h"
#include "config.h"
#include "portal.h"
#include "download.h"
#include "esp_timer.h"
#include "ui.h"
#include "wifi.h"

static void show_message(const char *msg)
{
    if (portal_active()) return;  // the setup screen is showing
    lvgl_port_lock(0);
    ui_show_message(msg);
    lvgl_port_unlock();
}

#define RETRY_US (30 * 1000000LL)


void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    config_init();  // before anything reads settings (the UI does)
    ESP_LOGI("main", "reset reason %d", (int)esp_reset_reason());
    ESP_ERROR_CHECK(board_display_init(NULL));
    lvgl_port_lock(0);
    ui_init();
    ui_show_message("Connecting to Wi-Fi...");
    lvgl_port_unlock();

    if (board_audio_init() != ESP_OK) {
        // No sound, but the library, downloads, settings and setup still work: don't reboot-loop.
        ESP_LOGE("main", "audio failed to start; carrying on without sound");
        show_message("Audio didn't start, so there's no sound.\nRestart to try again.");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    storage_init();
    battery_init();
    power_init();
    abs_api_init();
    cover_init();
    player_init();
    download_init();
    wifi_start();

    catalog_init();
    if (!config_complete()) {
        // First run (or settings cleared): straight into setup.
        lvgl_port_lock(0);
        ui_setup_show();
        lvgl_port_unlock();
    }
    bool cached = catalog_load_cached();
    bool online = false;
    int64_t last_try = -RETRY_US, started = esp_timer_get_time();
    bool first = true;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (!online && wifi_is_connected() && now - last_try >= RETRY_US) {
            if (!cached) show_message("Loading library...");
            online = catalog_load_network(cached);
            last_try = now;
            if (online && first) {
                first = false;
                ESP_LOGI("main", "free after library load: internal %u, PSRAM %u",
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#ifdef UI_CAPTURE
                void ui_capture_start(void);
                ui_capture_start();
#endif
#if defined(PORTAL_TEST) && !defined(UI_CAPTURE)
                lvgl_port_lock(0);
                ui_setup_show();
                lvgl_port_unlock();
#endif
            }
        } else if (!online && !cached && !wifi_is_connected() && now - started > 15000000LL) {
            show_message("Still waiting for Wi-Fi...\nSettings " LV_SYMBOL_RIGHT " Wi-Fi & login to change it.");
        }

        char library[40];
        if (ui_take_library_request(library, sizeof(library))) {
            // Switch library: show its cache at once (if any), then refresh from the server.
            char previous[40];
            strlcpy(previous, catalog_library_id(), sizeof(previous));
            catalog_select(library);
            cached = catalog_load_cached();
            if (wifi_is_connected()) {
                online = catalog_load_network(cached);
                last_try = now;
            } else if (!cached) {
                catalog_select(previous);  // nothing to show offline: stay where we were
                show_message("That library isn't saved on this device yet.\nConnect to Wi-Fi first.");
                vTaskDelay(pdMS_TO_TICKS(2500));
                show_message(NULL);
            }
        }

        if (ui_take_refresh_request()) {
            if (wifi_is_connected()) {
                online = catalog_load_network(false);
                last_try = now;
            } else {
                show_message("Offline - showing the saved library.");
                vTaskDelay(pdMS_TO_TICKS(2000));
                show_message(NULL);
            }
        }
        static bool told;
        if (abs_signed_out() && !told) {
            told = true;
            show_message("Signed out by the server.\nSettings " LV_SYMBOL_RIGHT " Wi-Fi & login to sign in again.");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
