// WiFi station for hb_link: join and stop commands in, HB_EV_WIFI out. Nothing here waits: hb_link puts a
// deadline on every attempt (HB_LINK_FAST_MS, HB_LINK_SCAN_MS) and the driver reports failures as events.
#include "hb_port.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";
static RTC_DATA_ATTR uint8_t s_bssid[6], s_channel;  // the last AP joined, kept through deep sleep
static volatile bool s_started, s_join_pending;

static void post(bool up)
{
    hb_ev_t ev = {.type = HB_EV_WIFI, .up = up};
    hb_port_post(&ev);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_join_pending) {
            s_join_pending = false;
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_started = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (d->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;  // our own disconnect or stop: not a link event
        }
        ESP_LOGW(TAG, "down: reason %d, %d dBm", d->reason, d->rssi);
        post(false);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_bssid, ap.bssid, sizeof s_bssid);
            s_channel = ap.primary;
            ESP_LOGI(TAG, "up: ch %d, %d dBm", ap.primary, ap.rssi);
        }
        post(true);
    }
}

esp_err_t hb_wifi_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_INVALID_STATE, err, TAG, "event loop");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "storage");  // never written to flash
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL), TAG, "wifi ev");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL), TAG, "ip ev");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    return esp_wifi_set_ps(WIFI_PS_MAX_MODEM);  // with listen_interval 3 (plan: AWAY)
}

bool hb_wifi_cached(void)
{
    return s_channel != 0;
}

void hb_wifi_forget(void)
{
    s_channel = 0;
}

void hb_wifi_join(bool fast)
{
    char ssid[33], psk[65];
    hb_store_get_str("ssid", ssid, sizeof ssid);
    hb_store_get_str("psk", psk, sizeof psk);
    wifi_config_t wc = {.sta = {
        .scan_method = WIFI_ALL_CHANNEL_SCAN,
        .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,  // no RSSI floor: the bench AP sits near -85 dBm (v1)
        .threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        .pmf_cfg.capable = true,
        .listen_interval = 3,
    }};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, psk, strlen(psk));
    if (fast && s_channel) {
        wc.sta.scan_method = WIFI_FAST_SCAN;
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, s_bssid, sizeof s_bssid);
        wc.sta.channel = s_channel;
    }
    ESP_LOGI(TAG, "join %s%s", ssid, fast && s_channel ? " (cached AP)" : " (scan)");
    if (s_started) {
        esp_wifi_disconnect();  // abandon an attempt in progress (ASSOC_LEAVE, not reported)
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK && s_started) {
        err = esp_wifi_connect();
    } else if (err == ESP_OK) {
        s_join_pending = true;  // connect once the driver reports STA_START
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "join: %s", esp_err_to_name(err));
        post(false);
    }
}

void hb_wifi_stop(void)
{
    s_join_pending = false;
    if (s_started) {
        esp_wifi_stop();  // returns with the driver stopped; STA_STOP follows as an event
        s_started = false;
    }
}

int hb_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
