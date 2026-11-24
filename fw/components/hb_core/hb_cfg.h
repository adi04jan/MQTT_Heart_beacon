// hb_cfg: the config values hb_core runs on (PROTOCOL.md section 7). hb_config (M3) parses the signed
// config into this; until then the beacon runs on hb_cfg_default.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t bright;          // led.bright, percent 0..100
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
} hb_cfg_t;

// Compiled defaults, equal to vectors.json config.defaults.
extern const hb_cfg_t hb_cfg_default;
