// hb_fx: LED effects as a pure function of time since start. The caller asks for one frame per tick;
// nothing blocks. Timings and colours follow v1 (Code/MQTT_Heart_Beacon.ino).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HB_PIXELS 9

#define HB_FX_PULSE_MS 1040  // one heartbeat: 520 ms fade in, 520 ms fade out
#define HB_FX_PULSE_GAP_MS 100
#define HB_FX_MOOD_PULSES 3
#define HB_FX_BAR_STEP_MS 80     // battery bar: one more pixel every 80 ms
#define HB_FX_BAR_HOLD_MS 500    // then held
#define HB_FX_BAR_WIPE_MS 100    // then switched off one pixel every 100 ms
#define HB_FX_LOW_BLINK_MS 500   // low battery: pixel 0 red 500 ms on, 500 ms off, 3 times

typedef enum {
    HB_FX_NONE,
    HB_FX_HEART,  // red, 3 pulses
    HB_FX_SORRY,  // orange, 3 pulses
    HB_FX_ANNOY,  // cyan, 3 pulses
    HB_FX_PAIR,   // blue pulse, repeats until replaced or stopped
    HB_FX_BATT,   // battery bar; arg = lit pixels 0..HB_PIXELS
    HB_FX_LOW,    // low-battery blink
} hb_fx_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t arg;
    uint32_t t0;
} hb_fx_t;

// Starts kind at now_ms, replacing whatever played.
void hb_fx_start(hb_fx_t *fx, hb_fx_kind_t kind, uint8_t arg, uint32_t now_ms);
// Writes the frame for now_ms (GRB not applied: rgb order) scaled by bright percent. Returns false, with a
// dark frame and kind reset to HB_FX_NONE, once the effect has ended.
bool hb_fx_frame(hb_fx_t *fx, uint32_t now_ms, uint8_t bright, uint8_t rgb[HB_PIXELS][3]);
