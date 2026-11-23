#include "hb_button.h"

void hb_btn_init(hb_btn_t *b, bool pressed, uint32_t now_ms)
{
    *b = (hb_btn_t){.raw = pressed, .stable = pressed, .swallow = pressed,
                    .raw_since = now_ms, .pressed_at = now_ms, .released_at = now_ms};
}

hb_btn_ev_t hb_btn_update(hb_btn_t *b, bool pressed, uint32_t now_ms)
{
    if (pressed != b->raw) {
        b->raw = pressed;
        b->raw_since = now_ms;
    }
    if (b->raw != b->stable && now_ms - b->raw_since >= HB_BTN_DEBOUNCE_MS) {
        b->stable = b->raw;
        if (b->stable) {
            b->pressed_at = now_ms;
            b->holds = 0;
        } else if (b->swallow) {
            b->swallow = false;
        } else if (b->holds == 0) {
            b->taps += b->taps < 3;
            b->released_at = now_ms;
        }
    }
    if (b->stable) {
        uint32_t held = now_ms - b->pressed_at;
        if (b->holds == 0 && held >= HB_BTN_HOLD_MS) {
            b->holds = 1;
            b->taps = 0;  // tap-then-hold is a hold
            return HB_BTN_HOLD;
        }
        if (b->holds == 1 && held >= HB_BTN_PAIR_MS) {
            b->holds = 2;
            return HB_BTN_PAIR;
        }
    } else if (b->taps && now_ms - b->released_at >= HB_BTN_GAP_MS) {
        uint8_t n = b->taps;
        b->taps = 0;
        return n == 1 ? HB_BTN_TAP1 : n == 2 ? HB_BTN_TAP2 : HB_BTN_TAP3;
    }
    return HB_BTN_NONE;
}

bool hb_btn_busy(const hb_btn_t *b)
{
    return b->raw != b->stable || b->stable || b->taps;
}
