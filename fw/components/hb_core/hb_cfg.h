// hb_cfg: the config the beacon runs on (PROTOCOL.md section 7). hb_config parses the signed config into
// this; without one the beacon runs on hb_cfg_default.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HB_CFG_HOSTS 4
#define HB_CFG_PORTS 4

typedef struct {
    uint32_t n;              // config number, 0 = compiled defaults (nothing accepted yet)
    char did[10];            // "HB-XXXXXX" the config was issued for, "" for the defaults
    uint8_t n_hosts;         // srv.hosts, 0 = use the provisioned host
    char hosts[HB_CFG_HOSTS][65];
    uint8_t n_mqtt;          // srv.mqtt
    uint16_t mqtt[HB_CFG_PORTS];
    uint16_t api;            // srv.api
    char name[33];           // name, UTF-8
    uint8_t bright;          // led.bright, percent 0..100
    bool presence;           // led.presence: a faint pixel while another member's phone is near their beacon
    uint8_t mood_rgb[3];     // mood.rgb: the custom mood's colour
    uint8_t mood_pat;        // mood.pat: 0 = 3 pulses, 1 = 3 slow breaths
    bool sched_on;           // sched.on
    int16_t tz_min;          // sched.tz, minutes east of UTC
    uint8_t days;            // sched.days, bit 0 = Monday .. bit 6 = Sunday
    uint16_t start_min;      // sched.start, minutes after local midnight
    uint16_t stop_min;       // sched.stop; stop < start runs past midnight, stop == start is 24 h (PROTOCOL.md 7.2)
    uint16_t idle_nap_min;   // pwr.idle_nap_min, 0 = never nap (the default)
    uint16_t nap_wake_min;   // pwr.nap_wake_min
    uint16_t batt_low_mv;    // pwr.batt_low_mv
    uint16_t batt_off_mv;    // pwr.batt_off_mv
    int32_t batt_cal_ppm;    // pwr.batt_cal_ppm, divider correction
    uint16_t adv_fast_ms, adv_fast_s, adv_slow_ms, conn_ms, latency, sup_ms;  // ble.*
    bool use_wifi;           // net.use_wifi
    uint16_t near_hold_s, away_grace_s, tele_min;                            // net.*
    char fw_ver[17], fw_path[97], fw_sha256[65];                             // fw.*
    uint16_t fw_min_mv;
} hb_cfg_t;

// Compiled defaults, equal to vectors.json config.defaults.
extern const hb_cfg_t hb_cfg_default;
