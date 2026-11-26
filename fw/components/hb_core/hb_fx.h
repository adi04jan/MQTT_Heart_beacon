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
#define HB_FX_FLASH_MS 150       // factory reset: white 150 ms on, 150 ms off, 3 times
#define HB_FX_BREATH_MS 4000     // one slow breath: 2 s in, 2 s out
#define HB_FX_BREATHS 15         // a `breath` with arg 0 lasts 15 breaths (one minute)
#define HB_FX_CELEBRATE_MS 12000 // rainbow chase
#define HB_FX_CELEBRATE_STEP 10  // ms per hue step (0..255 wheel)

typedef enum {
    HB_FX_NONE,
    HB_FX_HEART,  // red, 3 pulses
    HB_FX_SORRY,  // orange, 3 pulses
    HB_FX_ANNOY,  // cyan, 3 pulses
    HB_FX_PAIR,   // blue pulse, repeats until replaced or stopped
    HB_FX_BATT,   // battery bar; arg = lit pixels 0..HB_PIXELS
    HB_FX_LOW,    // low-battery blink
    HB_FX_RESET,  // white, 3 quick flashes (factory reset)
    HB_FX_ECHO,   // one soft brass pulse: your heart just glowed on the other desk
    HB_FX_BREATH, // rose, slow breaths; arg = breaths (0 = HB_FX_BREATHS). Start t0 on an epoch boundary to sync
    HB_FX_CELEBRATE,  // rainbow chase around the ring
    HB_FX_CUSTOM, // rgb: the sender's colour; arg 0 = 3 pulses, 1 = 3 breaths
} hb_fx_kind_t;

typedef struct {
    uint8_t kind;
    uint8_t arg;
    uint8_t level;   // percent of the brightness this effect plays at (100 = full; a dimmed replay is lower)
    uint8_t rgb[3];  // HB_FX_CUSTOM's colour
    uint32_t t0;
} hb_fx_t;

// Starts kind at now_ms, replacing whatever played, at level 100 (set fx->level / fx->rgb after if needed).
void hb_fx_start(hb_fx_t *fx, hb_fx_kind_t kind, uint8_t arg, uint32_t now_ms);
// Writes the frame for now_ms (GRB not applied: rgb order) scaled by bright percent. Returns false, with a
// dark frame and kind reset to HB_FX_NONE, once the effect has ended.
bool hb_fx_frame(hb_fx_t *fx, uint32_t now_ms, uint8_t bright, uint8_t rgb[HB_PIXELS][3]);

// Idle overlay, drawn only while no effect plays: a faint ember on pixel 0 (a shared streak) and a faint warm
// pixel 4 (the partner's phone is near their beacon). Scaled by bright like everything else.
void hb_fx_idle(bool ember, bool presence, uint8_t bright, uint8_t rgb[HB_PIXELS][3]);
