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
    .idle_nap_min = 0,  // NAP off by default (PROTOCOL.md 7)
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

static uint64_t new_id(hb_core_t *c)  // splitmix64
{
    uint64_t z = (c->rng += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void send_up(hb_actions_t *out, hb_mood_t k, hb_path_t path, uint64_t id)
{
    if (out->n_up < sizeof out->up / sizeof out->up[0]) {
        out->up[out->n_up++] = (hb_up_t){.k = k, .path = path, .id = id};
    }
}

// A new mood goes to the back of the outbox (the oldest is dropped when full); pump() sends it.
static void queue_mood(hb_core_t *c, hb_mood_t k)
{
    if (c->n_out == HB_OUTBOX) {
        memmove(&c->out[0], &c->out[1], sizeof c->out[0] * (HB_OUTBOX - 1));
        c->n_out--;
    }
    c->out[c->n_out++] = (hb_pend_t){.id = new_id(c), .k = (uint8_t)k, .path = HB_PATH_NONE};
}

// PROTOCOL.md 4: send a waiting mood on the live path; no ack in 5 s, the same id on the other path; no
// ack there either, parked until the next path report (HB_EV_PATH).
static void pump(hb_core_t *c, uint32_t now_ms, hb_actions_t *out)
{
    for (uint8_t i = 0; i < c->n_out; i++) {
        hb_pend_t *p = &c->out[i];
        hb_path_t to;
        if (p->path == HB_PATH_NONE) {
            to = c->path;
        } else if (p->sends == 1 && hb_due(now_ms, p->due)) {
            to = p->path == HB_PATH_BLE ? HB_PATH_WIFI : HB_PATH_BLE;
        } else {
            continue;
        }
        if (to == HB_PATH_NONE) {
            continue;
        }
        p->sends = p->path == HB_PATH_NONE ? 1 : 2;
        p->path = (uint8_t)to;
        p->due = now_ms + HB_ACK_MS;
        send_up(out, (hb_mood_t)p->k, to, p->id);
    }
}

static void acked(hb_core_t *c, uint64_t id)
{
    for (uint8_t i = 0; i < c->n_out; i++) {
        if (c->out[i].id == id) {
            memmove(&c->out[i], &c->out[i + 1], sizeof c->out[0] * (size_t)(c->n_out - i - 1));
            c->n_out--;
            return;
        }
    }
}

static bool seen(const hb_core_t *c, uint64_t id)
{
    for (uint8_t i = 0; i < c->n_seen; i++) {
        if (c->seen[i] == id) {
            return true;
        }
    }
    return false;
}

static void remember(hb_core_t *c, uint64_t id)
{
    c->seen[c->seen_next] = id;
    c->seen_next = (uint8_t)((c->seen_next + 1) % HB_SEEN);
    if (c->n_seen < HB_SEEN) {
        c->n_seen++;
    }
}

// `show`: plays once per id; every accepted show, a repeat included, is answered with `shown` on the live
// path (none: the router retries). An empty battery accepts nothing new.
static void on_show(hb_core_t *c, const hb_ev_t *ev, uint32_t now_ms, hb_actions_t *out)
{
    if (ev->k <= HB_MOOD_NONE || ev->k > HB_MOOD_ANNOY) {
        return;
    }
    if (!seen(c, ev->id)) {
        if (c->state == HB_ST_LOWBATT) {
            return;
        }
        remember(c, ev->id);
        activity(c, now_ms);
        hb_fx_start(&c->fx, k_mood_fx[ev->k], 0, now_ms);
    }
    if (c->path != HB_PATH_NONE) {
        send_up(out, HB_MOOD_NONE, c->path, ev->id);
    }
}

static void on_gesture(hb_core_t *c, hb_btn_ev_t g, uint32_t now_ms)
{
    if (g == HB_BTN_NONE || c->state == HB_ST_LOWBATT) {
        return;
    }
    activity(c, now_ms);
    c->grace_until = now_ms + HB_BTN_GRACE_MS;
    switch (g) {
    case HB_BTN_TAP1:
    case HB_BTN_TAP2:
    case HB_BTN_TAP3: {
        hb_mood_t k = (hb_mood_t)(HB_MOOD_HEART + (g - HB_BTN_TAP1));
        queue_mood(c, k);
        hb_fx_start(&c->fx, k_mood_fx[k], 0, now_ms);  // the presser's own beacon plays it too
        break;
    }
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
    pump(c, now_ms, out);
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
        for (uint8_t i = 0; i < c->n_out; i++) {
            next = sooner(next, now_ms, c->out[i].due);  // past deadlines (parked) are ignored
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
    c->rng ^= boot->seed;
    for (uint8_t i = 0; i < c->n_out; i++) {  // moods from before the sleep: resent on the first path
        c->out[i].path = HB_PATH_NONE;
        c->out[i].sends = 0;
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
        on_gesture(c, hb_btn_update(&c->btn, ev->pressed, now_ms), now_ms);
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
        on_show(c, ev, now_ms, out);
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
        for (uint8_t i = 0; i < c->n_out; i++) {  // a path report retries parked moods
            hb_pend_t *p = &c->out[i];
            if (p->sends == 2 && hb_due(now_ms, p->due)) {
                p->path = HB_PATH_NONE;
                p->sends = 0;
            }
        }
        break;
    case HB_EV_CLAIMED:
        c->claimed = true;
        c->pair_until = now_ms;
        break;
    case HB_EV_ACK:
        acked(c, ev->id);
        break;
    }
    finish(c, before, now_ms, out);
}
