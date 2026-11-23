// hb_batt: the one battery filter (v1 shared a moving window between callers). Raw samples are battery
// mV already scaled by the divider; the config's ppm correction is applied here.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hb_cfg.h"

#define HB_BATT_HYST_MV 50    // a flag set at <= threshold clears only above threshold + this
#define HB_BATT_FULL_MV 4150  // 100 % (v1 MAX_MV); 0 % is batt_off_mv

typedef struct {
    uint32_t f_q4;  // filtered mV, 4 fractional bits
    bool low;       // <= batt_low_mv: warn
    bool off;       // <= batt_off_mv: LOWBATT
} hb_batt_t;

// Restart the filter at this sample (boot). The low/off flags keep their state, so hysteresis holds
// across deep sleep; zero the struct first on power-on.
void hb_batt_seed(hb_batt_t *b, uint32_t raw_mv, const hb_cfg_t *cfg);
// One sample into the filter (exponential, alpha 1/4), then the thresholds.
void hb_batt_add(hb_batt_t *b, uint32_t raw_mv, const hb_cfg_t *cfg);
uint32_t hb_batt_mv(const hb_batt_t *b);
// 0..100, linear from batt_off_mv to HB_BATT_FULL_MV.
uint8_t hb_batt_pct(uint32_t mv, const hb_cfg_t *cfg);
