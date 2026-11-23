#include "hb_batt.h"

static uint32_t calibrated(uint32_t raw_mv, const hb_cfg_t *cfg)
{
    return (uint32_t)((int64_t)raw_mv * (1000000 + cfg->batt_cal_ppm) / 1000000);
}

static bool hyst(bool flag, uint32_t mv, uint32_t threshold)
{
    return flag ? mv <= threshold + HB_BATT_HYST_MV : mv <= threshold;
}

static void thresholds(hb_batt_t *b, const hb_cfg_t *cfg)
{
    uint32_t mv = hb_batt_mv(b);
    b->low = hyst(b->low, mv, cfg->batt_low_mv);
    b->off = hyst(b->off, mv, cfg->batt_off_mv);
}

void hb_batt_seed(hb_batt_t *b, uint32_t raw_mv, const hb_cfg_t *cfg)
{
    b->f_q4 = calibrated(raw_mv, cfg) << 4;
    thresholds(b, cfg);
}

void hb_batt_add(hb_batt_t *b, uint32_t raw_mv, const hb_cfg_t *cfg)
{
    int32_t x = (int32_t)(calibrated(raw_mv, cfg) << 4);
    b->f_q4 = (uint32_t)((int32_t)b->f_q4 + (x - (int32_t)b->f_q4) / 4);
    thresholds(b, cfg);
}

uint32_t hb_batt_mv(const hb_batt_t *b)
{
    return (b->f_q4 + 8) >> 4;
}

uint8_t hb_batt_pct(uint32_t mv, const hb_cfg_t *cfg)
{
    // ponytail: linear like v1's battery bar; a LiPo discharge table if the app's % proves misleading.
    if (mv <= cfg->batt_off_mv) {
        return 0;
    }
    if (mv >= HB_BATT_FULL_MV) {
        return 100;
    }
    return (uint8_t)((mv - cfg->batt_off_mv) * 100 / (HB_BATT_FULL_MV - cfg->batt_off_mv));
}
