#include "hb_core.h"
#include "hb_policy.h"

#include <string.h>

const hb_cfg_t hb_cfg_default = {
    .n_mqtt = 1,
    .mqtt = {8883},
    .api = 8443,
    .bright = 60,
    .presence = false,
    .mood_rgb = {0xff, 0x4f, 0xa3},  // "#ff4fa3"
    .mood_pat = 0,
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
    .adv_fast_ms = 100,
    .adv_fast_s = 30,
    .adv_slow_ms = 500,
    .conn_ms = 180,  // review P3: a phone tap glows the beacon within ~0.2 s; the LED floor dwarfs the cost
    .latency = 0,
    .sup_ms = 6000,
    .use_wifi = true,
    .near_hold_s = 30,
    .away_grace_s = 60,
    .tele_min = 30,
    .fw_min_mv = 3700,
};

static const hb_fx_kind_t k_mood_fx[] = {
    [HB_MOOD_HEART] = HB_FX_HEART,
    [HB_MOOD_SORRY] = HB_FX_SORRY,
    [HB_MOOD_ANNOY] = HB_FX_ANNOY,
    [HB_MOOD_CUSTOM] = HB_FX_CUSTOM,
    [HB_MOOD_TOGETHER] = HB_FX_BREATH,
    [HB_MOOD_ECHO] = HB_FX_ECHO,
    [HB_MOOD_CELEBRATE] = HB_FX_CELEBRATE,
};

// Plays a mood or show kind. A custom one takes its colour from rgb/pat (the sender's, or ours for our own press);
// together breathes 3 times as an invitation, 15 times when it is the shared one, in phase with `at`.
static void play(hb_core_t *c, hb_mood_t k, const uint8_t rgb[3], uint8_t pat, uint32_t at, uint32_t now_ms)
{
    uint8_t arg = k == HB_MOOD_CUSTOM ? pat : k == HB_MOOD_TOGETHER ? (at ? HB_FX_BREATHS : 3) : 0;
    uint32_t t0 = now_ms;
    uint32_t epoch = hb_core_epoch(c, now_ms);
    if (k == HB_MOOD_TOGETHER && at && epoch) {  // both beacons count breaths from the same epoch second
        uint32_t ms = (epoch - at) * 1000u + (now_ms - c->ms0) % 1000u;
        t0 = now_ms - ms % HB_FX_BREATH_MS;
    }
    hb_fx_start(&c->fx, k_mood_fx[k], arg, t0);
    if (k == HB_MOOD_CUSTOM) {
        memcpy(c->fx.rgb, rgb, 3);
    }
    if (k <= HB_MOOD_CUSTOM) {  // a heart, sorry, annoy or custom: the one the good-morning replay repeats
        c->morning = c->fx;
    }
}

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

uint64_t hb_core_new_id(hb_core_t *c)  // splitmix64
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
    c->out[c->n_out++] = (hb_pend_t){.id = hb_core_new_id(c), .k = (uint8_t)k, .path = HB_PATH_NONE};
}

static bool ble_live(const hb_core_t *c);
static void link_in(hb_core_t *c, hb_link_in_t in, uint32_t now_ms, hb_actions_t *out);

static bool path_live(const hb_core_t *c, hb_path_t p)
{
    return p == HB_PATH_BLE ? ble_live(c) : p == HB_PATH_WIFI && c->link.st == HB_LS_UP;
}

// Resend spacing for a mood with no ack: 5 s (PROTOCOL.md 4), then 10, 20, 40, then every 60 s.
static uint32_t ack_wait(uint8_t sends)
{
    return sends >= 5 ? 60000u : (uint32_t)HB_ACK_MS << (sends ? sends - 1 : 0);
}

// PROTOCOL.md 4: a waiting mood goes out on the live path. No ack in time: the other path if it is live (a
// WiFi that NEAR stopped is started for it), else the same path again, backing off; nothing live: it waits
// and goes out on the next path that comes up. It is never failed over to a path that is down.
static void pump(hb_core_t *c, uint32_t now_ms, hb_actions_t *out)
{
    for (uint8_t i = 0; i < c->n_out; i++) {
        hb_pend_t *p = &c->out[i];
        if (p->path != HB_PATH_NONE && !hb_due(now_ms, p->due)) {
            continue;  // in flight
        }
        hb_path_t to = c->path;
        if (p->path != HB_PATH_NONE) {
            hb_path_t other = p->path == HB_PATH_BLE ? HB_PATH_WIFI : HB_PATH_BLE;
            if (path_live(c, other)) {
                to = other;
            } else if (other == HB_PATH_WIFI && c->near_off && c->wifi_known && c->cfg.use_wifi) {
                c->near_off = false;  // the phone is not acking: bring WiFi back for this mood
                c->live_at = now_ms;  // and keep it up a full hold before NEAR may stop it again
                link_in(c, HB_LI_START, now_ms, out);
            }
        }
        if (to == HB_PATH_NONE) {
            p->path = HB_PATH_NONE;
            continue;
        }
        p->sends += p->sends < UINT8_MAX;
        p->path = (uint8_t)to;
        p->due = now_ms + ack_wait(p->sends);
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

// A path report: moods already backing off go out on it at once instead of at their next retry.
static void set_path(hb_core_t *c, hb_path_t p, uint32_t now_ms)
{
    c->path = p;
    for (uint8_t i = 0; p != HB_PATH_NONE && i < c->n_out; i++) {
        if (c->out[i].sends >= 2) {
            c->out[i].due = now_ms;
        }
    }
}

static bool wifi_wanted(const hb_core_t *c)
{
    return c->wifi_known && c->cfg.use_wifi && !c->near_off;
}

static bool ble_live(const hb_core_t *c)
{
    return c->ble && c->phone;
}

// Feeds hb_link; the WiFi path is live exactly while the link is up (BLE, when live, stays the path).
static void link_in(hb_core_t *c, hb_link_in_t in, uint32_t now_ms, hb_actions_t *out)
{
    bool was = c->link.st == HB_LS_UP;
    hb_link_cmd_t cmd = hb_link_ev(&c->link, in, now_ms);
    if (cmd != HB_LINK_NONE) {
        out->link = (uint8_t)cmd;
    }
    bool up = c->link.st == HB_LS_UP;
    if (up && !was) {
        if (c->path != HB_PATH_BLE) {
            set_path(c, HB_PATH_WIFI, now_ms);
        }
        c->report = true;  // every connect reports state and tele (PROTOCOL.md 3), the router answers `time`
        c->tele_due = now_ms;
        if (c->checkin) {  // NAP check-in: up now, stay long enough for the router to flush queued hearts
            c->checkin = false;
            c->nap_due = now_ms + HB_NAP_CHECKIN_MS;
        }
    } else if (was && !up && c->path == HB_PATH_WIFI) {
        set_path(c, HB_PATH_NONE, now_ms);
    }
}

// The BLE path is an encrypted link whose phone reports its server link up. Losing it (link gone, or the
// phone's upstream down) falls back to WiFi if that is up, and arms the grace before a stopped WiFi restarts.
static void ble_in(hb_core_t *c, bool ble, bool phone, uint32_t now_ms)
{
    bool was = ble_live(c);
    c->ble = ble;
    c->phone = ble && phone;
    bool live = ble_live(c);
    if (live == was) {
        return;
    }
    if (live) {
        c->live_at = now_ms;
        set_path(c, HB_PATH_BLE, now_ms);
        c->report = true;  // like a WiFi connect: state and tele for the phone to relay
        c->tele_due = now_ms;
    } else {
        c->wifi_due = now_ms + c->cfg.away_grace_s * 1000u;
        set_path(c, c->link.st == HB_LS_UP ? HB_PATH_WIFI : HB_PATH_NONE, now_ms);
    }
}

// PROTOCOL.md 4: the BLE path held net.near_hold_s stops WiFi (NEAR); once it is gone, WiFi comes back after
// net.away_grace_s, at once if a mood is waiting for its ack.
static void handover(hb_core_t *c, uint32_t now_ms, hb_actions_t *out)
{
    if (ble_live(c) && hb_due(now_ms, c->phone_seen + HB_PHONE_SILENT_MS)) {
        ble_in(c, c->ble, false, now_ms);  // a silent phone (Doze): as if it said its server link was down
    }
    if (ble_live(c)) {
        if (!c->near_off && hb_due(now_ms, c->live_at + c->cfg.near_hold_s * 1000u)) {
            link_in(c, HB_LI_STOP, now_ms, out);
            c->near_off = true;
        }
    } else if (c->near_off && (hb_due(now_ms, c->wifi_due) || c->n_out)) {
        c->near_off = false;
        if (wifi_wanted(c)) {
            link_in(c, HB_LI_START, now_ms, out);
        }
    }
}

// `show`: plays once per id; every accepted show, a repeat included, is answered with `shown` on the live
// path (none: the router retries). An empty battery accepts nothing new.
static void on_show(hb_core_t *c, const hb_ev_t *ev, uint32_t now_ms, hb_actions_t *out)
{
    if (ev->k <= HB_MOOD_NONE || ev->k >= HB_MOODS) {
        return;
    }
    if (!seen(c, ev->id)) {
        if (c->state == HB_ST_LOWBATT) {
            return;
        }
        remember(c, ev->id);
        activity(c, now_ms);
        play(c, ev->k, ev->rgb, ev->pat, ev->at, now_ms);
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
    case HB_BTN_CUSTOM:  // tap then hold: our own colour and pattern from the config
        queue_mood(c, HB_MOOD_CUSTOM);
        hb_fx_start(&c->fx, HB_FX_CUSTOM, c->cfg.mood_pat, now_ms);
        memcpy(c->fx.rgb, c->cfg.mood_rgb, 3);
        break;
    case HB_BTN_TOGETHER:  // a 2-5 s hold: an invitation to breathe together
        queue_mood(c, HB_MOOD_TOGETHER);
        hb_fx_start(&c->fx, HB_FX_BREATH, 3, now_ms);
        break;
    case HB_BTN_HOLD:
        battery_bar(c, now_ms);
        break;
    case HB_BTN_PAIR:
        c->pair_until = now_ms + HB_PAIR_WINDOW_MS;  // the blue pulse starts once the bar has finished
        break;
    case HB_BTN_RESET:  // review R8: recovery without USB
        hb_fx_start(&c->fx, HB_FX_RESET, 0, now_ms);
        c->reset_pending = true;
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
// now_ms wraps every 49.7 days and hb_due compares within +-24.86 days: a deadline left in the past would
// wrap back into the future (PAIRING reopening, schedule sleep blocked). Past deadlines are kept at now, and the
// wall clock is rebased daily so epoch0 + (now - ms0) never wraps either.
static void keep_time(hb_core_t *c, uint32_t now_ms)
{
    uint32_t *past[] = {&c->pair_until, &c->grace_until, &c->nap_due, &c->batt_due, &c->tele_due, &c->wifi_due};
    for (size_t i = 0; i < sizeof past / sizeof past[0]; i++) {
        if (hb_due(now_ms, *past[i])) {
            *past[i] = now_ms;
        }
    }
    if (c->epoch0 && now_ms - c->ms0 >= 86400000u) {
        uint32_t s = (now_ms - c->ms0) / 1000;
        c->epoch0 += s;
        c->ms0 += s * 1000;
    }
}

static void finish(hb_core_t *c, hb_state_t before, uint32_t now_ms, hb_actions_t *out)
{
    keep_time(c, now_ms);
    handover(c, now_ms, out);
    if (out->link == HB_LINK_NONE) {
        link_in(c, HB_LI_POLL, now_ms, out);
    }
    hb_policy_eval(c, now_ms);
    pump(c, now_ms, out);
    if (c->state == HB_ST_PAIRING && c->fx.kind == HB_FX_NONE) {
        hb_fx_start(&c->fx, HB_FX_PAIR, 0, now_ms);
    } else if (c->state != HB_ST_PAIRING && c->fx.kind == HB_FX_PAIR) {
        c->fx.kind = HB_FX_NONE;
    }

    uint8_t frame[HB_PIXELS][3];
    bool busy = hb_fx_frame(&c->fx, now_ms, c->cfg.bright, frame);
    if (!busy && !c->sleep_pending) {  // never left lit into a deep sleep: the pixels latch their last colour
        hb_fx_idle(c->streak > 0, c->partner_near && c->cfg.presence, c->cfg.bright, frame);
    }
    if (memcmp(frame, c->led, sizeof frame) != 0) {
        memcpy(c->led, frame, sizeof frame);
        memcpy(out->led, frame, sizeof frame);
        out->led_dirty = true;
    }
    busy = busy || hb_btn_busy(&c->btn);
    bool live = c->path != HB_PATH_NONE;  // `tele` goes out on whichever path is live
    if (live && hb_due(now_ms, c->tele_due)) {
        out->tele = true;
        c->tele_due = now_ms + c->cfg.tele_min * 60000u;
    }

    if (c->reset_pending && !busy) {
        out->factory_reset = true;  // the port erases and restarts; nothing after this matters
        c->reset_pending = false;
    }
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
        if (c->link.st != HB_LS_OFF && c->link.st != HB_LS_UP) {
            next = sooner(next, now_ms, c->link.due);
        }
        if (live) {
            next = sooner(next, now_ms, c->tele_due);
        }
        if (ble_live(c) && !c->near_off) {
            next = sooner(next, now_ms, c->live_at + c->cfg.near_hold_s * 1000u);
        } else if (!ble_live(c) && c->near_off) {
            next = sooner(next, now_ms, c->wifi_due);
        }
    }
    out->next_ms = next;
    out->state = c->state;
    out->state_changed = c->state != before || c->report;
    c->report = false;
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
    c->wifi_known = boot->wifi;
    c->ble = c->phone = c->near_off = c->checkin = false;  // the BLE stack starts fresh on every boot
    c->link = (hb_link_t){.cached = boot->ap_cached};
    c->report = false;
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
            c->nap_due = now_ms + HB_NAP_LINK_MS;  // the 10 s count starts when the link is up (link_in)
            c->checkin = true;
        }
        if (slept == HB_ST_SLEEP && c->morning.kind != HB_FX_NONE) {  // good morning: last evening's mood, softly
            c->fx = c->morning;
            c->fx.t0 = now_ms;
            c->fx.level = HB_GOOD_MORNING_LEVEL;
            c->morning.kind = HB_FX_NONE;
        }
        break;
    case HB_WAKE_RESET:
        break;
    }
    if (wifi_wanted(c)) {
        link_in(c, HB_LI_START, now_ms, out);
    }
    if (boot->wake != HB_WAKE_TIMER && boot->wake != HB_WAKE_RESET) {  // timer and reset wakes are quiet
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
    case HB_EV_BLE:
        ble_in(c, ev->up, c->phone, now_ms);
        break;
    case HB_EV_PHONE:
        c->phone_seen = now_ms;
        ble_in(c, c->ble, ev->up, now_ms);
        if (ble_live(c)) {  // the phone says hello on every (re)connect: it may be a new app session
            c->report = true;
            c->tele_due = now_ms;
        }
        break;
    case HB_EV_PROV:
        c->wifi_known = ev->up;
        if (ev->up && ble_live(c)) {  // a WiFi change is tried now; NEAR stops WiFi again after the hold
            c->near_off = false;
            c->live_at = now_ms;
        }
        link_in(c, HB_LI_STOP, now_ms, out);  // a fresh round on the new networks
        if (wifi_wanted(c)) {
            link_in(c, HB_LI_START, now_ms, out);
        }
        break;
    case HB_EV_CLAIMED:
        c->claimed = true;
        c->pair_until = now_ms;
        break;
    case HB_EV_ACK:
        acked(c, ev->id);
        break;
    case HB_EV_WIFI:
        link_in(c, ev->up ? HB_LI_WIFI_UP : HB_LI_WIFI_DOWN, now_ms, out);
        break;
    case HB_EV_MQTT:
        link_in(c, ev->up ? HB_LI_MQTT_UP : HB_LI_MQTT_DOWN, now_ms, out);
        break;
    case HB_EV_HELLO:
        c->phone_seen = now_ms;
        break;
    case HB_EV_STREAK:
        c->streak = (uint16_t)(ev->mv > 9999 ? 9999 : ev->mv);
        break;
    case HB_EV_PRESENCE:
        c->partner_near = ev->up;
        break;
    case HB_EV_CFG:
        c->cfg = *ev->cfg;
        link_in(c, wifi_wanted(c) ? HB_LI_START : HB_LI_STOP, now_ms, out);
        break;
    }
    finish(c, before, now_ms, out);
}
