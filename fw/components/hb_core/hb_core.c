#include "hb_core.h"
#include "hb_policy.h"

#include <string.h>

const hb_cfg_t hb_cfg_default = {
    .bright = 60,
    .sched_on = false,
    .tz_min = 0,
    .days = 0x1f,  // "1111100": Monday to Friday
    .start_min = 9 * 60,
    .stop_min = 19 * 60,
    .idle_nap_min = 90,
    .nap_wake_min = 15,
    .batt_low_mv = 3450,
    .batt_off_mv = 3300,
    .batt_cal_ppm = 0,
};

static const hb_fx_kind_t k_mood_fx[] = {
    [HB_MOOD_HEART] = HB_FX_HEART,
    [HB_MOOD_SORRY] = HB_FX_SORRY,
    [HB_MOOD_ANNOY] = HB_FX_ANNOY,
};

uint32_t hb_core_epoch(const hb_core_t *c, uint32_t now_ms)
{
    return c->epoch0 ? c->epoch0 + (now_ms - c->ms0) / 1000 : 0;
}

static void battery_bar(hb_core_t *c, uint32_t now_ms)
{
    uint8_t lit = (uint8_t)(hb_batt_pct(hb_batt_mv(&c->batt), &c->cfg) * HB_PIXELS / 100);
    hb_fx_start(&c->fx, HB_FX_BATT, lit, now_ms);
}

// Something worth staying up for: restarts the nap countdown and cancels a pending NAP or SLEEP
// (hb_policy_eval decides again). An empty battery is never cancelled.
static void activity(hb_core_t *c, uint32_t now_ms)
{
    c->nap_due = now_ms + c->cfg.idle_nap_min * 60000u;
    if (c->state != HB_ST_LOWBATT) {
        c->sleep_pending = false;
    }
}

static void on_gesture(hb_core_t *c, hb_btn_ev_t g, uint32_t now_ms, hb_actions_t *out)
{
    if (g == HB_BTN_NONE || c->state == HB_ST_LOWBATT) {
        return;
    }
    activity(c, now_ms);
    c->grace_until = now_ms + HB_BTN_GRACE_MS;
    switch (g) {
    case HB_BTN_TAP1:
    case HB_BTN_TAP2:
    case HB_BTN_TAP3:
        out->mood = (hb_mood_t)(HB_MOOD_HEART + (g - HB_BTN_TAP1));
        hb_fx_start(&c->fx, k_mood_fx[out->mood], 0, now_ms);  // the presser's own beacon plays it too
        break;
    case HB_BTN_HOLD:
        battery_bar(c, now_ms);
        break;
    case HB_BTN_PAIR:
        c->pair_until = now_ms + HB_PAIR_WINDOW_MS;  // the blue pulse starts once the bar has finished
        break;
    default:
        break;
    }
}

static uint32_t sooner(uint32_t cur, uint32_t now_ms, uint32_t t)
{
    int32_t d = (int32_t)(t - now_ms);
    return d > 0 && (uint32_t)d < cur ? (uint32_t)d : cur;
}

// Common tail of boot and step: state, frame, and when to come back.
static void finish(hb_core_t *c, hb_state_t before, uint32_t now_ms, hb_actions_t *out)
{
    hb_policy_eval(c, now_ms);
    if (c->state == HB_ST_PAIRING && c->fx.kind == HB_FX_NONE) {
        hb_fx_start(&c->fx, HB_FX_PAIR, 0, now_ms);
    } else if (c->state != HB_ST_PAIRING && c->fx.kind == HB_FX_PAIR) {
        c->fx.kind = HB_FX_NONE;
    }

    uint8_t frame[HB_PIXELS][3];
    bool busy = hb_fx_frame(&c->fx, now_ms, c->cfg.bright, frame);
    if (memcmp(frame, c->led, sizeof frame) != 0) {
        memcpy(c->led, frame, sizeof frame);
        memcpy(out->led, frame, sizeof frame);
        out->led_dirty = true;
    }
    busy = busy || hb_btn_busy(&c->btn);

    if (c->sleep_pending && !busy) {
        out->sleep = true;
        out->sleep_s = c->sleep_s;
    } else if (!busy && hb_due(now_ms, c->batt_due)) {
        out->measure_batt = true;  // never under LED load: the pixels pull the cell down
        c->batt_due = now_ms + HB_BATT_PERIOD_MS;
    }

    uint32_t next = HB_BATT_PERIOD_MS;
    if (busy || c->sleep_pending) {
        next = HB_TICK_MS;
    } else {
        next = sooner(next, now_ms, c->batt_due);
        next = sooner(next, now_ms, c->pair_until);
        next = sooner(next, now_ms, c->grace_until);
        if (c->cfg.idle_nap_min) {
            next = sooner(next, now_ms, c->nap_due);
        }
    }
    out->next_ms = next;
    out->state = c->state;
    out->state_changed = c->state != before;
}

void hb_core_boot(hb_core_t *c, const hb_cfg_t *cfg, const hb_boot_t *boot, uint32_t now_ms, hb_actions_t *out)
{
    if (boot->wake == HB_WAKE_POWERON) {
        memset(c, 0, sizeof *c);
        c->state = HB_ST_OFFLINE;
    }
    hb_state_t slept = c->state;
    memset(out, 0, sizeof *out);
    c->cfg = *cfg;
    c->state = HB_ST_OFFLINE;
    c->path = HB_PATH_NONE;
    c->claimed = boot->claimed;
    c->sleep_pending = false;
    c->epoch0 = boot->epoch;
    c->ms0 = now_ms;
    c->pair_until = c->grace_until = now_ms;
    c->batt_due = now_ms + HB_BATT_PERIOD_MS;
    c->nap_due = now_ms + cfg->idle_nap_min * 60000u;
    memset(c->led, 0, sizeof c->led);  // the pixels come up dark, and were dark when we slept
    hb_fx_start(&c->fx, HB_FX_NONE, 0, now_ms);
    hb_btn_init(&c->btn, boot->pressed, now_ms);
    hb_batt_seed(&c->batt, boot->mv, cfg);

    switch (boot->wake) {
    case HB_WAKE_POWERON:
        if (!boot->claimed) {
            c->pair_until = now_ms + HB_PAIR_WINDOW_MS;
        }
        break;
    case HB_WAKE_BUTTON:
        c->grace_until = now_ms + HB_BTN_GRACE_MS;  // a button wake out of hours stays up 5 min
        break;
    case HB_WAKE_TIMER:
        if (slept == HB_ST_NAP) {
            c->nap_due = now_ms + HB_NAP_CHECKIN_MS;
        }
        break;
    }
    if (boot->wake != HB_WAKE_TIMER) {  // a timer wake is quiet
        if (c->batt.low) {
            hb_fx_start(&c->fx, HB_FX_LOW, 0, now_ms);
        } else {
            battery_bar(c, now_ms);
        }
    }
    finish(c, slept, now_ms, out);
    out->state_changed = true;  // every boot reports its state
}

void hb_core_step(hb_core_t *c, const hb_ev_t *ev, uint32_t now_ms, hb_actions_t *out)
{
    hb_state_t before = c->state;
    memset(out, 0, sizeof *out);
    switch (ev->type) {
    case HB_EV_TICK:
        on_gesture(c, hb_btn_update(&c->btn, ev->pressed, now_ms), now_ms, out);
        break;
    case HB_EV_BATT: {
        bool was_low = c->batt.low;
        hb_batt_add(&c->batt, ev->mv, &c->cfg);
        if (c->batt.low && !was_low && !c->batt.off) {
            hb_fx_start(&c->fx, HB_FX_LOW, 0, now_ms);
        }
        break;
    }
    case HB_EV_TIME:
        c->epoch0 = ev->ts;
        c->ms0 = now_ms;
        break;
    case HB_EV_SHOW:
        if (ev->k > HB_MOOD_NONE && ev->k <= HB_MOOD_ANNOY && c->state != HB_ST_LOWBATT) {
            activity(c, now_ms);
            hb_fx_start(&c->fx, k_mood_fx[ev->k], 0, now_ms);
        }
        break;
    case HB_EV_SLEEP:
        if (c->state != HB_ST_LOWBATT) {
            uint32_t epoch = hb_core_epoch(c, now_ms), s = ev->for_s;
            if (s == 0 && c->cfg.sched_on && epoch) {
                s = hb_sched_secs_to_start(&c->cfg, epoch);
            }
            hb_policy_sleep(c, HB_ST_SLEEP, s);
        }
        break;
    case HB_EV_PATH:
        c->path = ev->path;
        break;
    case HB_EV_CLAIMED:
        c->claimed = true;
        c->pair_until = now_ms;
        break;
    }
    finish(c, before, now_ms, out);
}
