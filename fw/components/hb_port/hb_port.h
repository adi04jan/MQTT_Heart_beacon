// hb_port: hb_core's hardware, carrier rev A (plan "Firmware layout"). Every wait has a timeout.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "hb_core.h"

#define HB_LED_GPIO 2   // 9 WS2812-class pixels, GRB
#define HB_BATT_GPIO 3  // ADC1, battery through a 1:2 divider

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

// store_nvs.c
esp_err_t hb_store_init(void);
uint8_t hb_store_get_u8(const char *key, uint8_t def);
