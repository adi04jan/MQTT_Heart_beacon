#include "hb_fx.h"

#include <string.h>

static const uint8_t k_rgb[][3] = {
    [HB_FX_HEART] = {250, 0, 0},
    [HB_FX_SORRY] = {235, 161, 23},
    [HB_FX_ANNOY] = {23, 231, 235},
    [HB_FX_PAIR] = {0, 0, 250},
    [HB_FX_LOW] = {200, 0, 0},
    [HB_FX_RESET] = {255, 255, 255},
    [HB_FX_ECHO] = {230, 140, 50},
    [HB_FX_BREATH] = {255, 40, 90},
};

static const uint8_t k_ember[3] = {60, 18, 0};     // a coal: pixel 0, the heart's tip
static const uint8_t k_presence[3] = {40, 30, 20}; // warm white: pixel 4, top of the left lobe

#define PERIOD_MS (HB_FX_PULSE_MS + HB_FX_PULSE_GAP_MS)

void hb_fx_start(hb_fx_t *fx, hb_fx_kind_t kind, uint8_t arg, uint32_t now_ms)
{
    *fx = (hb_fx_t){.kind = (uint8_t)kind, .arg = arg, .level = 100, .t0 = now_ms};
}

// Heartbeat level 0..255, e ms into one period: linear up, linear down, dark gap.
static uint32_t pulse(uint32_t e)
{
    const uint32_t half = HB_FX_PULSE_MS / 2;
    if (e < half) {
        return e * 255 / half;
    }
    return e < HB_FX_PULSE_MS ? (HB_FX_PULSE_MS - e) * 255 / half : 0;
}

// Breath level 0..255, e ms into one breath: linear in over half, out over the other half.
static uint32_t breath(uint32_t e)
{
    const uint32_t half = HB_FX_BREATH_MS / 2, x = e % HB_FX_BREATH_MS;
    return x < half ? x * 255 / half : (HB_FX_BREATH_MS - x) * 255 / half;
}

static void set(uint8_t px[3], const uint8_t c[3], uint32_t level, uint8_t bright)
{
    for (int k = 0; k < 3; k++) {
        px[k] = (uint8_t)(c[k] * level * bright / (255u * 100u));
    }
}

static void fill(uint8_t rgb[HB_PIXELS][3], const uint8_t c[3], uint32_t level, uint8_t bright)
{
    for (int i = 0; i < HB_PIXELS; i++) {
        set(rgb[i], c, level, bright);
    }
}

// v1 battery_level_show(): red -> yellow over pixels 0..4, yellow -> green over 5..8.
static void bar_colour(int i, uint8_t c[3])
{
    c[0] = (uint8_t)(i <= 4 ? 255 : 255 - (i - 5) * 255 / 3);
    c[1] = (uint8_t)(i <= 4 ? i * 255 / 4 : 255);
    c[2] = 0;
}

// The classic NeoPixel colour wheel: 0..255 -> red, green, blue and back.
static void wheel(uint32_t pos, uint8_t c[3])
{
    pos = 255 - (pos & 255);
    if (pos < 85) {
        c[0] = (uint8_t)(255 - pos * 3), c[1] = 0, c[2] = (uint8_t)(pos * 3);
    } else if (pos < 170) {
        pos -= 85;
        c[0] = 0, c[1] = (uint8_t)(pos * 3), c[2] = (uint8_t)(255 - pos * 3);
    } else {
        pos -= 170;
        c[0] = (uint8_t)(pos * 3), c[1] = (uint8_t)(255 - pos * 3), c[2] = 0;
    }
}

bool hb_fx_frame(hb_fx_t *fx, uint32_t now_ms, uint8_t bright, uint8_t rgb[HB_PIXELS][3])
{
    memset(rgb, 0, HB_PIXELS * 3);
    const uint32_t e = now_ms - fx->t0;
    const uint8_t b = (uint8_t)(bright * (fx->level && fx->level < 100 ? fx->level : 100u) / 100);  // 0 = full
    switch (fx->kind) {
    case HB_FX_HEART:
    case HB_FX_SORRY:
    case HB_FX_ANNOY:
    case HB_FX_PAIR:
        if (fx->kind != HB_FX_PAIR && e >= HB_FX_MOOD_PULSES * PERIOD_MS) {
            break;
        }
        fill(rgb, k_rgb[fx->kind], pulse(e % PERIOD_MS), b);
        return true;
    case HB_FX_BATT: {
        const uint32_t fill_ms = fx->arg * HB_FX_BAR_STEP_MS, wipe = fill_ms + HB_FX_BAR_HOLD_MS;
        if (e >= wipe + HB_PIXELS * HB_FX_BAR_WIPE_MS) {
            break;
        }
        uint32_t on = e < fill_ms ? e / HB_FX_BAR_STEP_MS + 1 : fx->arg;
        uint32_t off = e < wipe ? 0 : (e - wipe) / HB_FX_BAR_WIPE_MS + 1;
        for (uint32_t i = off; i < on; i++) {
            uint8_t c[3];
            bar_colour((int)i, c);
            set(rgb[i], c, 255, b);
        }
        return true;
    }
    case HB_FX_LOW:
        if (e >= 6 * HB_FX_LOW_BLINK_MS) {
            break;
        }
        if ((e / HB_FX_LOW_BLINK_MS) % 2 == 0) {
            set(rgb[0], k_rgb[HB_FX_LOW], 255, b);
        }
        return true;
    case HB_FX_RESET:
        if (e >= 6 * HB_FX_FLASH_MS) {
            break;
        }
        if ((e / HB_FX_FLASH_MS) % 2 == 0) {
            fill(rgb, k_rgb[HB_FX_RESET], 255, b);
        }
        return true;
    case HB_FX_ECHO:
        if (e >= PERIOD_MS) {
            break;
        }
        fill(rgb, k_rgb[HB_FX_ECHO], pulse(e) * 2 / 5, b);  // soft: two fifths of a heartbeat
        return true;
    case HB_FX_BREATH:
        if (e >= (uint32_t)(fx->arg ? fx->arg : HB_FX_BREATHS) * HB_FX_BREATH_MS) {
            break;
        }
        fill(rgb, k_rgb[HB_FX_BREATH], breath(e), b);
        return true;
    case HB_FX_CELEBRATE:
        if (e >= HB_FX_CELEBRATE_MS) {
            break;
        }
        for (int i = 0; i < HB_PIXELS; i++) {
            uint8_t c[3];
            wheel(e / HB_FX_CELEBRATE_STEP + (uint32_t)i * 28, c);
            set(rgb[i], c, 255, b);
        }
        return true;
    case HB_FX_CUSTOM:
        if (fx->arg == 1) {
            if (e >= 3 * HB_FX_BREATH_MS) {
                break;
            }
            fill(rgb, fx->rgb, breath(e), b);
        } else {
            if (e >= HB_FX_MOOD_PULSES * PERIOD_MS) {
                break;
            }
            fill(rgb, fx->rgb, pulse(e % PERIOD_MS), b);
        }
        return true;
    default:
        break;
    }
    fx->kind = HB_FX_NONE;
    return false;
}

void hb_fx_idle(bool ember, bool presence, uint8_t bright, uint8_t rgb[HB_PIXELS][3])
{
    if (ember) {
        set(rgb[0], k_ember, 255, bright);
    }
    if (presence) {
        set(rgb[4], k_presence, 255, bright);
    }
}
