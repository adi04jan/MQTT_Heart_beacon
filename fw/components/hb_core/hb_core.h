// hb_core: the beacon's whole behaviour as one pure step function (plan "Firmware layout").
// No IDF, no clock reads: the port passes now_ms (free-running, may wrap) and executes the actions.
// hb_core_t is meant to live in RTC memory so the power state survives deep sleep.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hb_batt.h"
#include "hb_button.h"
#include "hb_cfg.h"
#include "hb_fx.h"

#define HB_TICK_MS 20               // frame and button sampling period while something is moving
#define HB_BATT_PERIOD_MS 30000     // battery sample period, also the longest the core sleeps between steps
#define HB_PAIR_WINDOW_MS 120000    // pairing window after a 5 s hold or an unclaimed power-on
#define HB_BTN_GRACE_MS 300000      // no schedule SLEEP within 5 min of a button wake or gesture
#define HB_NAP_CHECKIN_MS 10000     // a NAP timer wake stays up this long
#define HB_SEEN 16                  // `show` ids remembered for dedupe (PROTOCOL.md 3: ring of 16)
#define HB_OUTBOX 4                 // moods waiting for an ack; a fifth drops the oldest
#define HB_ACK_MS 5000              // PROTOCOL.md 4: no ack in 5 s, resend the same id on the other path

typedef enum {  // plan "Power / States"; NAP, SLEEP and LOWBATT are deep sleep
    HB_ST_PAIRING,
    HB_ST_NEAR,
    HB_ST_AWAY,
    HB_ST_OFFLINE,
    HB_ST_NAP,
    HB_ST_SLEEP,
    HB_ST_LOWBATT,
} hb_state_t;

typedef enum { HB_MOOD_NONE, HB_MOOD_HEART, HB_MOOD_SORRY, HB_MOOD_ANNOY } hb_mood_t;
typedef enum { HB_PATH_NONE, HB_PATH_BLE, HB_PATH_WIFI } hb_path_t;
typedef enum { HB_WAKE_POWERON, HB_WAKE_BUTTON, HB_WAKE_TIMER } hb_wake_t;

typedef enum {
    HB_EV_TICK,      // pressed = button level now (the port samples it for every tick)
    HB_EV_BATT,      // mv = battery mV after the divider, answering actions.measure_batt
    HB_EV_TIME,      // ts = epoch seconds from the phone or router (`time` message)
    HB_EV_SHOW,      // k + id (`show` message): plays once per id, answers `shown` every time; radio stub until M3
    HB_EV_SLEEP,     // for_s (`sleep` message, 0 = until schedule on or button); radio stub until M3
    HB_EV_PATH,      // path = live server path, as decided by hb_link; radio stub until M3/M4
    HB_EV_CLAIMED,   // claim accepted (closes the pairing window); radio stub until M4
    HB_EV_ACK,       // id: an uplink was acked (WiFi: its PUBACK; BLE: the phone's `resp{re: id, ok: true}`)
} hb_ev_type_t;

typedef struct {
    hb_ev_type_t type;
    union {
        bool pressed;
        uint32_t mv;
        uint32_t ts;
        hb_mood_t k;
        uint32_t for_s;
        hb_path_t path;
    };
    uint64_t id;  // HB_EV_SHOW, HB_EV_ACK (hb_id_parse of the envelope id)
} hb_ev_t;

// One message for the port to send now (radio stub until M3).
typedef struct {
    hb_mood_t k;      // HB_MOOD_NONE: `shown` with this id; else `mood` k with this id
    hb_path_t path;   // the path to send it on (the port brings that path up if it has to)
    uint64_t id;
} hb_up_t;

// A mood waiting for its ack, kept in RTC memory through deep sleep.
typedef struct {
    uint64_t id;
    uint32_t due;     // ack deadline of the last send
    uint8_t k;        // hb_mood_t
    uint8_t path;     // hb_path_t of the last send; HB_PATH_NONE = waiting for a path
    uint8_t sends;    // 1: sent on one path; 2: on the other too, then parked until the next path report
} hb_pend_t;

typedef struct {
    hb_wake_t wake;
    bool claimed;
    bool pressed;     // button level at boot (the wake press)
    uint32_t mv;      // first battery sample, taken before any LED lights
    uint32_t epoch;   // wall clock kept by the RTC across deep sleep, 0 = unknown
    uint64_t seed;    // random bits (esp_random) mixed into the message id generator on every boot
} hb_boot_t;

// Apply in field order: LEDs, uplinks, measurement, then deep sleep.
typedef struct {
    bool led_dirty;                 // push led[] (only set when the frame changed)
    uint8_t led[HB_PIXELS][3];      // r, g, b
    hb_up_t up[HB_OUTBOX + 1];      // send these, in order: a `shown` and the moods due
    uint8_t n_up;
    bool measure_batt;              // read the battery and post HB_EV_BATT
    bool sleep;                     // enter deep sleep now; LEDs are dark, button wake on
    uint32_t sleep_s;               // with a timer wake after this many seconds, 0 = button only
    hb_state_t state;
    bool state_changed;             // publish `state` (M3) before acting on sleep
    uint32_t next_ms;               // step again (HB_EV_TICK) within this many ms
} hb_actions_t;

typedef struct {
    hb_cfg_t cfg;
    hb_state_t state;
    hb_path_t path;
    bool claimed;
    bool sleep_pending;     // state is a deep-sleep state, waiting for the effect and button to finish
    uint32_t sleep_s;
    uint32_t epoch0, ms0;   // wall clock: epoch0 at ms0; epoch0 == 0 = unknown
    uint32_t pair_until, nap_due, grace_until, batt_due;  // ms deadlines
    hb_btn_t btn;
    hb_fx_t fx;
    hb_batt_t batt;
    uint8_t led[HB_PIXELS][3];  // last frame handed out
    uint64_t rng;               // message id generator (splitmix64), seeded by hb_boot_t.seed
    uint64_t seen[HB_SEEN];     // `show` ids already played; the oldest is overwritten
    uint8_t n_seen, seen_next;
    hb_pend_t out[HB_OUTBOX];   // moods sent or waiting to be, oldest first
    uint8_t n_out;
} hb_core_t;

// Every boot, before any step. On HB_WAKE_POWERON the core is reset; otherwise c holds the state it
// slept in (RTC memory) and the wake is interpreted against it.
void hb_core_boot(hb_core_t *c, const hb_cfg_t *cfg, const hb_boot_t *boot, uint32_t now_ms, hb_actions_t *out);
void hb_core_step(hb_core_t *c, const hb_ev_t *ev, uint32_t now_ms, hb_actions_t *out);

// Wall clock at now_ms, 0 if unknown.
uint32_t hb_core_epoch(const hb_core_t *c, uint32_t now_ms);

// Deadline t has been reached at now (wrap-safe for deadlines under 24 days away).
static inline bool hb_due(uint32_t now_ms, uint32_t t)
{
    return (int32_t)(now_ms - t) >= 0;
}
