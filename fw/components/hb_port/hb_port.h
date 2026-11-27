// hb_port: hb_core's hardware, carrier rev A or B (Kconfig HB_HW, plan "Firmware layout"). Every wait has a timeout.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hb_core.h"

#include "sdkconfig.h"

// Button on CONFIG_HB_BTN_GPIO (0) on both. Rev B also has, not driven by any code yet: GPIO3 USB sense
// (68k/100k from VBUS, digital), GPIO1 LIS3DH INT1 (deep-sleep wake capable), GPIO8/9 LIS3DH SDA/SCL at 0x19
// (pull-ups on the breakout). GPIO2 is a strapping pin and stays free on rev B.
#if CONFIG_HB_HW_C
// Rev C (ESP32_heart_revC3): GPIO1 LIS3DH INT1, GPIO3 USB sense, GPIO2 SCL / GPIO8 SDA, GPIO5 on GND, GPIO20 LED
// switch (R8 4.7k keeps it off through the ROM's reset-time pull-up). GPIO21 is the ROM's UART TX: it chatters
// into the pixels' DIN at reset while their rail is off.
#define HB_HW "C"
#define HB_LED_GPIO 21  // 9 WS2812-class pixels, GRB, through 330 ohm
#define HB_BATT_GPIO 4  // ADC1_CH4, 1M/1M divider + 100 nF
#elif CONFIG_HB_HW_B
#define HB_HW "B"
#define HB_LED_GPIO 10  // 9 WS2812-class pixels, GRB, through 330 ohm
#define HB_BATT_GPIO 4  // ADC1_CH4, 1M/1M divider + 100 nF
#else
#define HB_HW "A"
#define HB_LED_GPIO 2   // 9 WS2812-class pixels, GRB
#define HB_BATT_GPIO 3  // ADC1, battery through a 1:2 divider
#endif

// main.c: the one event queue. Drivers post from their own tasks; hb_core runs on the main task only.
void hb_port_post(const hb_ev_t *ev);
void hb_port_post_rx(const char *topic, int topic_len, const char *data, int data_len);  // copies both
void hb_port_post_puback(int msg_id);
void hb_port_post_ble(const char *msg, size_t len);  // one whole message from the phone; copied

// led_rmt.c
esp_err_t hb_led_init(void);
esp_err_t hb_led_show(const uint8_t rgb[HB_PIXELS][3]);
void hb_led_park(void);  // rail off, data line low and held through deep sleep

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
// Networks (max HB_WIFI_NETS) are stored in NVS as ssid0..3 / psk0..3.
#define HB_WIFI_NETS 4
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool sec;
} hb_net_t;
esp_err_t hb_wifi_init(void);
bool hb_wifi_cached(void);       // a BSSID + channel from the last join is in RTC memory
void hb_wifi_forget(void);       // drop it (the network changed)
void hb_wifi_join(bool fast);    // fast: the cached BSSID on its channel; else scan, strongest AP of any stored network
void hb_wifi_try(const char *ssid, const char *psk);  // join this network now (wifi.set tries before it stores)
void hb_wifi_stop(void);
int hb_wifi_rssi(void);          // dBm, 0 when not associated
const char *hb_wifi_why(void);   // the last failure for a person: "wrong password", "network not found", ...
int hb_wifi_scan(hb_net_t *out, int max);  // blocking, ~2 s; strongest first, one per name; -1 when busy
int hb_wifi_nets(char ssids[HB_WIFI_NETS][33]);  // the stored networks, packed; returns how many
bool hb_wifi_room(const char *ssid);         // ssid is stored already, or a slot is free
esp_err_t hb_wifi_add(const char *ssid, const char *psk);  // replaces ssid's slot or takes a free one
esp_err_t hb_wifi_remove(const char *ssid);  // ESP_ERR_NOT_FOUND
void hb_wifi_clear(void);

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

// ble.c: NimBLE peripheral (PROTOCOL.md 6). Posts HB_EV_BLE and whole messages (hb_port_post_ble).
esp_err_t hb_ble_init(const char *did, const hb_cfg_t *cfg);
void hb_ble_params(const hb_cfg_t *cfg);      // advertising and connection parameters from a new config
void hb_ble_window(bool open);                // the pairing window: new bonds allowed, fast advertising
bool hb_ble_send(const char *json, int len);  // false without an encrypted link
void hb_ble_flush(uint32_t ms);               // before deep sleep: let the last notifies go out, then hang up
void hb_ble_forget_all(void);                 // factory reset: every bond

// console.c: bench view of the provisioning over the USB console (BLE is the way in).
void hb_console_start(void);
