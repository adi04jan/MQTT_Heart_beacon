// WiFi station for hb_link: join and stop commands in, HB_EV_WIFI out. Nothing here waits on the link: hb_link
// puts a deadline on every attempt (HB_LINK_FAST_MS, HB_LINK_SCAN_MS) and the driver reports failures as events.
// Up to HB_WIFI_NETS networks live in NVS (ssid0..3 / psk0..3); they arrive only over BLE (wifi.set).
#include "hb_port.h"

#include <string.h>
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";
static RTC_DATA_ATTR uint8_t s_bssid[6], s_channel, s_slot;  // the last AP joined, kept through deep sleep
static volatile bool s_started, s_join_pending, s_scan_join, s_on;
static volatile int s_reason;

static void act(void);

static void post(bool up)
{
    hb_ev_t ev = {.type = HB_EV_WIFI, .up = up};
    hb_port_post(&ev);
}

static void key(char *out, const char *base, int i)
{
    strcpy(out, base);
    out[strlen(base)] = (char)('0' + i);
    out[strlen(base) + 1] = 0;
}

static void net(int i, char ssid[33], char psk[64])
{
    char k[8];
    key(k, "ssid", i);
    hb_store_get_str(k, ssid, 33);
    if (psk) {
        key(k, "psk", i);
        hb_store_get_str(k, psk, 64);
    }
}

int hb_wifi_nets(char ssids[HB_WIFI_NETS][33])
{
    int n = 0;
    for (int i = 0; i < HB_WIFI_NETS; i++) {
        net(i, ssids[n], NULL);
        n += ssids[n][0] != 0;
    }
    return n;
}

static int find(const char *ssid)
{
    for (int i = 0; i < HB_WIFI_NETS; i++) {
        char s[33];
        net(i, s, NULL);
        if (s[0] && strcmp(s, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

bool hb_wifi_room(const char *ssid)
{
    char s[HB_WIFI_NETS][33];
    return find(ssid) >= 0 || hb_wifi_nets(s) < HB_WIFI_NETS;
}

esp_err_t hb_wifi_add(const char *ssid, const char *psk)
{
    int i = find(ssid);
    for (int j = 0; i < 0 && j < HB_WIFI_NETS; j++) {
        char s[33];
        net(j, s, NULL);
        i = s[0] ? -1 : j;
    }
    if (i < 0) {
        return ESP_ERR_NO_MEM;
    }
    char k[8];
    key(k, "psk", i);
    esp_err_t err = hb_store_set_str(k, psk[0] ? psk : "");
    key(k, "ssid", i);
    return err ? err : hb_store_set_str(k, ssid);
}

esp_err_t hb_wifi_remove(const char *ssid)
{
    int i = find(ssid);
    if (i < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (i == s_slot) {
        s_channel = 0;  // the cached AP belonged to it
    }
    char k[8];
    key(k, "ssid", i);
    esp_err_t err = hb_store_set_str(k, "");
    key(k, "psk", i);
    return err ? err : hb_store_set_str(k, "");
}

void hb_wifi_clear(void)
{
    for (int i = 0; i < HB_WIFI_NETS; i++) {
        char k[8];
        key(k, "ssid", i);
        hb_store_set_str(k, "");
        key(k, "psk", i);
        hb_store_set_str(k, "");
    }
    s_channel = 0;
}

static void connect_to(const char *ssid, const char *psk, const uint8_t *bssid, uint8_t channel)
{
    wifi_config_t wc = {.sta = {
        .scan_method = WIFI_ALL_CHANNEL_SCAN,
        .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,  // no RSSI floor: the bench AP sits near -85 dBm (v1)
        .threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        .pmf_cfg.capable = true,
        .listen_interval = 3,
    }};
    memcpy(wc.sta.ssid, ssid, strlen(ssid));
    memcpy(wc.sta.password, psk, strlen(psk));
    if (bssid) {
        wc.sta.scan_method = WIFI_FAST_SCAN;
        wc.sta.bssid_set = true;
        memcpy(wc.sta.bssid, bssid, 6);
        wc.sta.channel = channel;
    }
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    err = err ? err : esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        s_reason = WIFI_REASON_UNSPECIFIED;
        post(false);
    }
}

// The link's scan: the strongest AP of any stored network.
static void scan_done(void)
{
    s_scan_join = false;
    uint16_t n = 20;
    static wifi_ap_record_t ap[20];
    if (esp_wifi_scan_get_ap_records(&n, ap) != ESP_OK) {
        n = 0;
    }
    for (uint16_t i = 0; i < n; i++) {  // sorted strongest first
        int slot = find((const char *)ap[i].ssid);
        if (slot >= 0) {
            char ssid[33], psk[64];
            net(slot, ssid, psk);
            s_slot = (uint8_t)slot;
            ESP_LOGI(TAG, "join %s, ch %d, %d dBm", ssid, ap[i].primary, ap[i].rssi);
            connect_to(ssid, psk, ap[i].bssid, ap[i].primary);
            return;
        }
    }
    ESP_LOGW(TAG, "no stored network in range (%u APs)", n);
    s_reason = WIFI_REASON_NO_AP_FOUND;
    post(false);
}

static void scan_start(void)
{
    s_scan_join = true;
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        s_scan_join = false;
        s_reason = WIFI_REASON_UNSPECIFIED;
        post(false);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_started = true;
        if (s_join_pending) {
            s_join_pending = false;
            act();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_started = false;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_scan_join) {
            scan_done();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        if (d->reason == WIFI_REASON_ASSOC_LEAVE) {
            return;  // our own disconnect or stop: not a link event
        }
        ESP_LOGW(TAG, "down: reason %d, %d dBm", d->reason, d->rssi);
        s_reason = d->reason;
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

// What to do once the driver is up: scan for the stored networks, or connect to one network.
static struct {
    enum { P_NONE, P_SCAN, P_CONNECT } what;
    char ssid[33], psk[64];
    uint8_t bssid[6], channel;  // channel 0: let the driver find the AP
} s_pend;

static void act(void)
{
    if (s_pend.what == P_SCAN) {
        scan_start();
    } else if (s_pend.what == P_CONNECT) {
        connect_to(s_pend.ssid, s_pend.psk, s_pend.channel ? s_pend.bssid : NULL, s_pend.channel);
    }
    memset(&s_pend, 0, sizeof s_pend);  // the PSK does not linger
}

// Now if the driver is up (abandoning an attempt in progress), else once it reports STA_START.
static void go(void)
{
    s_on = true;
    s_scan_join = false;
    if (s_started) {
        esp_wifi_disconnect();  // ASSOC_LEAVE: not reported
        act();
        return;
    }
    s_join_pending = true;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "start: %s", esp_err_to_name(err));
        s_join_pending = false;
        memset(&s_pend, 0, sizeof s_pend);
        s_reason = WIFI_REASON_UNSPECIFIED;
        post(false);
    }
}

void hb_wifi_join(bool fast)
{
    s_pend.what = P_SCAN;
    if (fast && s_channel) {
        net(s_slot, s_pend.ssid, s_pend.psk);
        if (s_pend.ssid[0]) {
            s_pend.what = P_CONNECT;
            memcpy(s_pend.bssid, s_bssid, sizeof s_bssid);
            s_pend.channel = s_channel;
        }
    }
    ESP_LOGI(TAG, "join %s", s_pend.what == P_CONNECT ? s_pend.ssid : "(scan)");
    go();
}

void hb_wifi_try(const char *ssid, const char *psk)
{
    s_pend.what = P_CONNECT;
    strlcpy(s_pend.ssid, ssid, sizeof s_pend.ssid);
    strlcpy(s_pend.psk, psk, sizeof s_pend.psk);
    ESP_LOGI(TAG, "try %s", ssid);
    go();
}

void hb_wifi_stop(void)
{
    s_on = false;
    s_join_pending = s_scan_join = false;
    memset(&s_pend, 0, sizeof s_pend);
    if (s_started) {
        esp_wifi_stop();  // returns with the driver stopped; STA_STOP follows as an event
        s_started = false;
    }
}

int hb_wifi_scan(hb_net_t *out, int max)
{
    bool was = s_started;
    if (!was && esp_wifi_start() != ESP_OK) {
        return -1;
    }
    wifi_scan_config_t sc = {.show_hidden = false};
    int n = -1;
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {  // blocks ~2 s (all channels, active)
        static wifi_ap_record_t ap[20];
        uint16_t got = 20;
        n = 0;
        if (esp_wifi_scan_get_ap_records(&got, ap) == ESP_OK) {
            for (uint16_t i = 0; i < got && n < max; i++) {  // strongest first; one row per name
                bool dup = !ap[i].ssid[0];
                for (int j = 0; j < n && !dup; j++) {
                    dup = strcmp(out[j].ssid, (const char *)ap[i].ssid) == 0;
                }
                if (!dup) {
                    memcpy(out[n].ssid, ap[i].ssid, sizeof out[n].ssid);
                    out[n].rssi = ap[i].rssi;
                    out[n].sec = ap[i].authmode != WIFI_AUTH_OPEN;
                    n++;
                }
            }
        }
    }
    if (!was && !s_on) {
        esp_wifi_stop();
        s_started = false;
    }
    return n;
}

const char *hb_wifi_why(void)
{
    switch (s_reason) {
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
        return "wrong password";
    default:
        return "no answer";
    }
}

int hb_wifi_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}
