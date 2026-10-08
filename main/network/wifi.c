#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1  // the last connection attempt ended in a disconnect

static volatile bool s_auto_reconnect = true;  // off while setup is scanning or testing
static esp_netif_t *s_sta;

static esp_err_t no_ip6_autoconfig(void *ctx)
{
    struct netif *lwip = esp_netif_get_netif_impl(s_sta);
    if (lwip) lwip->ip6_autoconfig_enabled = 0;
    return ESP_OK;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_reconnect && config_get()->wifi_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // A link-local IPv6 address lets mDNS answer AAAA queries for abspresso.local; without
        // one, phones and Macs wait out a ~5 s IPv6 lookup before falling back to IPv4. Only
        // link-local: routable addresses from the router's adverts may not be reachable on the
        // LAN, and browsers that pick one then fail to connect.
        esp_netif_tcpip_exec(no_ip6_autoconfig, NULL);  // lwIP state: change it on the TCP/IP task
        esp_netif_create_ip6_linklocal(s_sta);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_FAILED);
        if (s_auto_reconnect) {
            ESP_LOGW(TAG, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, "abspresso");  // how it shows in the router's device list
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, config_get()->wifi_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, config_get()->wifi_pass, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep between beacons saves a lot of power; the ~minute of buffered audio rides out
    // the added latency.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

bool wifi_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & BIT_CONNECTED;
}

bool wifi_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

esp_err_t wifi_start_ap(const char *ssid, const char *password)
{
    // Share the home network's channel when we're on it: in AP+STA mode the radio can only be on
    // one channel, and a setup network that hops channels drops the phone.
    uint8_t channel = 1;
    wifi_ap_record_t home;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&home) == ESP_OK) channel = home.primary;
    // Each reconnect attempt scans every channel (taking the setup network off the air), so only
    // keep reconnecting if we're already connected.
    s_auto_reconnect = wifi_is_connected();
    if (!s_auto_reconnect) esp_wifi_disconnect();
    wifi_config_t ap = {
        .ap = {
            .channel = channel,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    // Station stays up alongside, for scanning and for testing the new home network.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    // Full power while setting up: modem sleep makes the setup page sluggish.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "setup network '%s' up on channel %d: %s", ssid, channel, esp_err_to_name(err));
    return err;
}

void wifi_stop_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_auto_reconnect = true;
    // A failed test may have left the station set up for another network: go back to ours.
    wifi_config_t cur = {0}, want = {0};
    esp_wifi_get_config(WIFI_IF_STA, &cur);
    strlcpy((char *)want.sta.ssid, config_get()->wifi_ssid, sizeof(want.sta.ssid));
    strlcpy((char *)want.sta.password, config_get()->wifi_pass, sizeof(want.sta.password));
    if (strcmp((char *)cur.sta.ssid, (char *)want.sta.ssid) != 0 ||
        strcmp((char *)cur.sta.password, (char *)want.sta.password) != 0) {
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &want);
        xEventGroupClearBits(s_events, BIT_CONNECTED);
    }
    if (!wifi_is_connected() && want.sta.ssid[0]) esp_wifi_connect();
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    // A station that keeps retrying a network blocks scans, so stop that while scanning.
    const bool was = s_auto_reconnect;
    s_auto_reconnect = false;
    if (!wifi_is_connected()) esp_wifi_disconnect();
    const wifi_scan_config_t sc = {.show_hidden = false};
    uint16_t n = max;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) n = 0;
    s_auto_reconnect = was;
    if (was && !wifi_is_connected()) esp_wifi_connect();
    return n;
}

bool wifi_try_connect(const char *ssid, const char *password, int timeout_ms)
{
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    // Wait for an address; a disconnect along the way (wrong password, not found) is a failure,
    // but allow a couple of retries since the first attempt after switching can be refused.
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int failures = 0;
    while (xTaskGetTickCount() < end) {
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & BIT_CONNECTED) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);  // WaitBits cleared it
            return true;
        }
        if ((b & BIT_FAILED) && ++failures < 3) esp_wifi_connect();
        else if (b & BIT_FAILED) return false;
    }
    return false;
}
