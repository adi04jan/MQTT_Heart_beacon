// hb_port: hb_core's hardware, carrier rev A (plan "Firmware layout"). Every wait has a timeout.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hb_core.h"

#define HB_LED_GPIO 2   // 9 WS2812-class pixels, GRB
#define HB_BATT_GPIO 3  // ADC1, battery through a 1:2 divider

// main.c: the one event queue. Drivers post from their own tasks; hb_core runs on the main task only.
void hb_port_post(const hb_ev_t *ev);
void hb_port_post_rx(const char *topic, int topic_len, const char *data, int data_len);  // copies both
void hb_port_post_puback(int msg_id);

// led_rmt.c
esp_err_t hb_led_init(void);
esp_err_t hb_led_show(const uint8_t rgb[HB_PIXELS][3]);
void hb_led_park(void);  // data line low, held through deep sleep

// adc.c
esp_err_t hb_adc_init(void);
uint32_t hb_adc_batt_mv(void);  // battery mV with the divider applied; 0 = read failed

// sleep.c
hb_wake_t hb_sleep_wake_cause(void);
esp_err_t hb_sleep_auto_light(void);  // frequency scaling + automatic light sleep
void hb_sleep_deep(uint32_t secs);    // does not return; button wake, plus a timer when secs > 0

// store_nvs.c: namespace "hb". Provisioning keys (console.c): did user pass host ssid psk; plus claimed (u8)
// and cfg (the accepted config's bytes).
esp_err_t hb_store_init(void);
uint8_t hb_store_get_u8(const char *key, uint8_t def);
esp_err_t hb_store_set_u8(const char *key, uint8_t v);
void hb_store_get_str(const char *key, char *out, size_t cap);  // "" when unset or too long
esp_err_t hb_store_set_str(const char *key, const char *v);     // "" erases the key
size_t hb_store_get_blob(const char *key, void *out, size_t cap);  // 0 when unset
esp_err_t hb_store_set_blob(const char *key, const void *v, size_t len);
esp_err_t hb_store_erase_all(void);

// wifi.c: station for hb_link. Posts HB_EV_WIFI; hb_link bounds every attempt, the driver never waits here.
esp_err_t hb_wifi_init(void);
bool hb_wifi_cached(void);       // a BSSID + channel from the last join is in RTC memory
void hb_wifi_forget(void);       // drop it (the network changed)
void hb_wifi_join(bool fast);    // fast: the cached BSSID on its channel; else scan all channels, strongest AP
void hb_wifi_stop(void);
int hb_wifi_rssi(void);          // dBm, 0 when not associated

// mqtt.c: MQTT 5 over TLS (esp-mqtt) for hb_link. Posts HB_EV_MQTT, puback and rx.
typedef struct {
    const char *host;   // IP or name; TLS checks it against the leaf's SAN
    uint16_t port;
    const char *did;    // client id d-{did}, topics hb/v1/d/{did}/...
    const char *user, *pass;
    const char *will;   // LWT payload, retained QoS 1 on .../state
    const char *ca;     // pinned CA, PEM, NUL-terminated
} hb_mqtt_cfg_t;
esp_err_t hb_mqtt_start(const hb_mqtt_cfg_t *cfg);
void hb_mqtt_stop(void);  // clean DISCONNECT when connected (no LWT); bounded by the network timeout
int hb_mqtt_publish(const char *topic, const char *json, int len, bool retain);  // QoS 1; msg id or -1
bool hb_mqtt_flush(uint32_t ms);  // waits until every QoS 1 publish is acked, at most ms

// console.c: temporary M3 provisioning over the USB console (M4 moves it to BLE).
void hb_console_start(void);
