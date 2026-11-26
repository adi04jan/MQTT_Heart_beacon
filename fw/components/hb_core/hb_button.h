// hb_button: one active-low button -> gestures. Feed the debounced-or-not level and an injected ms clock.
// Tap (press < HOLD) counts; the count is reported once no new press follows within GAP after a release:
// 1 = heart, 2 = sorry, 3 or more = annoy. Holds fire while still pressed: HOLD (battery bar), PAIR (5 s),
// RESET (15 s). A tap then a hold is CUSTOM instead of HOLD; a plain hold let go between 2 s and 5 s is TOGETHER.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HB_BTN_DEBOUNCE_MS 30
#define HB_BTN_GAP_MS 400    // longest release-to-press gap inside a double or triple tap
#define HB_BTN_HOLD_MS 600   // v1's long press
#define HB_BTN_PAIR_MS 5000  // opens the pairing window
#define HB_BTN_TOGETHER_MS 2000  // a hold let go after this (and before PAIR) is TOGETHER
#define HB_BTN_RESET_MS 15000    // factory reset

typedef enum {
    HB_BTN_NONE,
    HB_BTN_TAP1,
    HB_BTN_TAP2,
    HB_BTN_TAP3,
    HB_BTN_HOLD,
    HB_BTN_PAIR,
    HB_BTN_RESET,     // held 15 s
    HB_BTN_CUSTOM,    // a tap, then a hold (instead of HOLD)
    HB_BTN_TOGETHER,  // a plain hold let go between 2 s and 5 s
} hb_btn_ev_t;

typedef struct {
    bool raw, stable;
    bool swallow;   // the current press woke us: its release is not a tap
    uint8_t taps;   // short presses in the gesture so far
    uint8_t holds;  // hold events already fired for the current press
    bool custom;    // the current press is a tap-then-hold (CUSTOM): no TOGETHER on its release
    uint32_t raw_since, pressed_at, released_at;
} hb_btn_t;

// Call at boot with the level at that moment. A press already in progress (the wake press) never counts
// as a tap; holds still fire, timed from boot.
void hb_btn_init(hb_btn_t *b, bool pressed, uint32_t now_ms);
hb_btn_ev_t hb_btn_update(hb_btn_t *b, bool pressed, uint32_t now_ms);
// True while the button needs sampling: bouncing, held, or a tap count still open.
bool hb_btn_busy(const hb_btn_t *b);
