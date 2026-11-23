// hb_policy: power states, office schedule, nap (plan "Power / States"). Called by hb_core.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "hb_core.h"

// Schedule: on during [start, stop) local time, the window belonging to the day it starts on, so
// 22:00-06:00 on Friday covers Saturday 05:59. stop <= start runs past midnight (equal = 24 h).
// sched.on false or epoch 0 (clock unknown) = in hours.
bool hb_sched_in_hours(const hb_cfg_t *cfg, uint32_t epoch);
// Seconds from epoch to the next window start strictly after it; 0 if no day is enabled.
uint32_t hb_sched_secs_to_start(const hb_cfg_t *cfg, uint32_t epoch);

// Puts the core into deep-sleep state s (NAP, SLEEP, LOWBATT); the sleep action follows once the
// effect has finished and the button is released.
void hb_policy_sleep(hb_core_t *c, hb_state_t s, uint32_t secs);
// Re-derives the state after any input: LOWBATT, then schedule SLEEP, then NAP, else the awake state
// (PAIRING while the window is open, else NEAR / AWAY / OFFLINE from the path).
void hb_policy_eval(hb_core_t *c, uint32_t now_ms);

// "pairing", "near", "away", "offline", "nap", "sleep", "lowbatt" (the `state` message's st).
const char *hb_state_name(hb_state_t s);
