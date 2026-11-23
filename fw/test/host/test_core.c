// M1 logic: hb_button, hb_fx, hb_batt, schedule and power states through hb_core_boot / hb_core_step.
#include "unity.h"
#include "hb_core.h"
#include "hb_policy.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

// ---- hb_button -------------------------------------------------------------------------------------

// Feeds level edges (t, pressed) and samples every 5 ms up to end_ms; returns the gestures in order.
typedef struct {
    uint32_t t;
    bool pressed;
} edge_t;

static int feed(hb_btn_t *b, const edge_t *edges, int n, uint32_t end_ms, hb_btn_ev_t *got, uint32_t *at)
{
    int k = 0, e = 0;
    bool level = b->raw;
    for (uint32_t t = 0; t <= end_ms; t += 5) {
        while (e < n && edges[e].t <= t) {
            level = edges[e++].pressed;
        }
        hb_btn_ev_t ev = hb_btn_update(b, level, t);
        if (ev != HB_BTN_NONE) {
            at[k] = t;
            got[k++] = ev;
        }
    }
    return k;
}

static void test_tap_with_bounce_is_one_heart_after_the_gap(void)
{
    hb_btn_t b;
    hb_btn_init(&b, false, 0);
    const edge_t e[] = {{100, 1}, {104, 0}, {108, 1}, {112, 0}, {115, 1},   // press bounces for 15 ms
                        {250, 0}, {253, 1}, {256, 0}};                      // release bounces too
    hb_btn_ev_t got[8];
    uint32_t at[8];
    TEST_ASSERT_EQUAL_INT(1, feed(&b, e, 8, 2000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP1, got[0]);
    TEST_ASSERT_UINT32_WITHIN(20, 256 + HB_BTN_DEBOUNCE_MS + HB_BTN_GAP_MS, at[0]);
    TEST_ASSERT_FALSE(hb_btn_busy(&b));
}

static void test_double_and_triple_tap(void)
{
    hb_btn_t b;
    hb_btn_ev_t got[8];
    uint32_t at[8];
    const edge_t two[] = {{0, 1}, {100, 0}, {300, 1}, {400, 0}};
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, two, 4, 2000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP2, got[0]);

    const edge_t three[] = {{0, 1}, {100, 0}, {300, 1}, {400, 0}, {600, 1}, {700, 0}};
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, three, 6, 2000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP3, got[0]);

    const edge_t five[] = {{0, 1}, {80, 0}, {200, 1}, {280, 0}, {400, 1}, {480, 0}, {600, 1}, {680, 0}, {800, 1}, {880, 0}};
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, five, 10, 2500, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP3, got[0]);  // more than three is still annoy
}

static void test_taps_further_apart_than_the_gap_are_separate(void)
{
    hb_btn_t b;
    hb_btn_init(&b, false, 0);
    const edge_t e[] = {{0, 1}, {100, 0}, {700, 1}, {800, 0}};
    hb_btn_ev_t got[8];
    uint32_t at[8];
    TEST_ASSERT_EQUAL_INT(2, feed(&b, e, 4, 2000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP1, got[0]);
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP1, got[1]);
}

static void test_glitches_shorter_than_debounce_are_ignored(void)
{
    hb_btn_t b;
    hb_btn_init(&b, false, 0);
    const edge_t e[] = {{100, 1}, {110, 0}, {300, 1}, {325, 0}};
    hb_btn_ev_t got[8];
    uint32_t at[8];
    TEST_ASSERT_EQUAL_INT(0, feed(&b, e, 4, 2000, got, at));
}

static void test_holds_fire_while_held_and_release_is_not_a_tap(void)
{
    hb_btn_t b;
    hb_btn_init(&b, false, 0);
    const edge_t e[] = {{0, 1}, {6000, 0}};
    hb_btn_ev_t got[8];
    uint32_t at[8];
    TEST_ASSERT_EQUAL_INT(2, feed(&b, e, 2, 8000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_HOLD, got[0]);
    TEST_ASSERT_UINT32_WITHIN(10, HB_BTN_DEBOUNCE_MS + HB_BTN_HOLD_MS, at[0]);
    TEST_ASSERT_EQUAL_INT(HB_BTN_PAIR, got[1]);
    TEST_ASSERT_UINT32_WITHIN(10, HB_BTN_DEBOUNCE_MS + HB_BTN_PAIR_MS, at[1]);

    const edge_t tap_then_hold[] = {{0, 1}, {100, 0}, {300, 1}, {1200, 0}};
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, tap_then_hold, 4, 3000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_HOLD, got[0]);
}

static void test_wake_press_is_swallowed_but_can_still_hold(void)
{
    hb_btn_t b;
    hb_btn_ev_t got[8];
    uint32_t at[8];
    const edge_t short_wake[] = {{200, 0}, {1000, 1}, {1100, 0}};
    hb_btn_init(&b, true, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, short_wake, 3, 2500, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_TAP1, got[0]);  // only the second press counts
    TEST_ASSERT_TRUE(at[0] > 1100);

    const edge_t held_wake[] = {{5500, 0}};
    hb_btn_init(&b, true, 0);
    TEST_ASSERT_EQUAL_INT(2, feed(&b, held_wake, 1, 7000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_HOLD, got[0]);
    TEST_ASSERT_EQUAL_INT(HB_BTN_PAIR, got[1]);
}

// ---- hb_fx -----------------------------------------------------------------------------------------

static uint8_t px[HB_PIXELS][3];

static bool frame_at(hb_fx_t *fx, uint32_t t, uint8_t bright)
{
    return hb_fx_frame(fx, t, bright, px);
}

static void test_heart_pulses_three_times_at_config_brightness(void)
{
    hb_fx_t fx;
    hb_fx_start(&fx, HB_FX_HEART, 0, 1000);
    TEST_ASSERT_TRUE(frame_at(&fx, 1000, 60));
    TEST_ASSERT_EQUAL_UINT8(0, px[0][0]);
    TEST_ASSERT_TRUE(frame_at(&fx, 1000 + 520, 60));  // peak: 250 * 60 %
    for (int i = 0; i < HB_PIXELS; i++) {
        TEST_ASSERT_EQUAL_UINT8(150, px[i][0]);
        TEST_ASSERT_EQUAL_UINT8(0, px[i][1]);
    }
    TEST_ASSERT_TRUE(frame_at(&fx, 1000 + 260, 60));
    TEST_ASSERT_EQUAL_UINT8(74, px[4][0]);  // half way up
    TEST_ASSERT_TRUE(frame_at(&fx, 1000 + 1100, 60));
    TEST_ASSERT_EQUAL_UINT8(0, px[0][0]);  // gap between beats
    TEST_ASSERT_TRUE(frame_at(&fx, 1000 + 2 * 1140 + 520, 100));
    TEST_ASSERT_EQUAL_UINT8(250, px[8][0]);  // third beat, full brightness
    TEST_ASSERT_FALSE(frame_at(&fx, 1000 + 3 * 1140, 60));
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, fx.kind);
}

static void test_mood_colours_and_zero_brightness(void)
{
    hb_fx_t fx;
    hb_fx_start(&fx, HB_FX_SORRY, 0, 0);
    frame_at(&fx, 520, 100);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){235, 161, 23}), px[0], 3);
    hb_fx_start(&fx, HB_FX_ANNOY, 0, 0);
    frame_at(&fx, 520, 100);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){23, 231, 235}), px[0], 3);
    TEST_ASSERT_TRUE(frame_at(&fx, 520, 0));  // still running, just dark
    TEST_ASSERT_EQUAL_UINT8(0, px[0][0] | px[0][1] | px[0][2]);
}

static void test_pair_pulse_repeats_until_replaced(void)
{
    hb_fx_t fx;
    hb_fx_start(&fx, HB_FX_PAIR, 0, 0);
    TEST_ASSERT_TRUE(frame_at(&fx, 100 * 1140 + 520, 100));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){0, 0, 250}), px[3], 3);
}

static int lit(void)
{
    int n = 0;
    for (int i = 0; i < HB_PIXELS; i++) {
        n += (px[i][0] | px[i][1] | px[i][2]) != 0;
    }
    return n;
}

static void test_battery_bar_fills_holds_and_wipes(void)
{
    hb_fx_t fx;
    hb_fx_start(&fx, HB_FX_BATT, 5, 0);
    frame_at(&fx, 0, 100);
    TEST_ASSERT_EQUAL_INT(1, lit());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){255, 0, 0}), px[0], 3);  // red end
    frame_at(&fx, 80, 100);
    TEST_ASSERT_EQUAL_INT(2, lit());
    frame_at(&fx, 899, 100);
    TEST_ASSERT_EQUAL_INT(5, lit());  // filled at 400, held to 900
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){255, 255, 0}), px[4], 3);  // yellow middle
    frame_at(&fx, 900, 100);
    TEST_ASSERT_EQUAL_INT(4, lit());
    TEST_ASSERT_EQUAL_UINT8(0, px[0][0]);  // wipe starts at pixel 0
    TEST_ASSERT_TRUE(frame_at(&fx, 1700, 100));
    TEST_ASSERT_EQUAL_INT(0, lit());
    TEST_ASSERT_FALSE(frame_at(&fx, 900 + 900, 100));

    hb_fx_start(&fx, HB_FX_BATT, 9, 0);
    frame_at(&fx, 9 * 80, 100);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((uint8_t[]){0, 255, 0}), px[8], 3);  // green end
}

static void test_low_battery_blinks_pixel_zero_three_times(void)
{
    hb_fx_t fx;
    hb_fx_start(&fx, HB_FX_LOW, 0, 0);
    int on = 0;
    for (uint32_t t = 0; t < 3000; t += 250) {
        TEST_ASSERT_TRUE(frame_at(&fx, t, 100));
        TEST_ASSERT_EQUAL_UINT8(0, px[1][0]);
        on += px[0][0] == 200;
    }
    TEST_ASSERT_EQUAL_INT(6, on);  // 3 blinks x 2 samples
    TEST_ASSERT_FALSE(frame_at(&fx, 3000, 100));
}

// ---- hb_batt ---------------------------------------------------------------------------------------

static void test_batt_filter_calibration_and_pct(void)
{
    hb_cfg_t cfg = hb_cfg_default;
    hb_batt_t b = {0};
    hb_batt_seed(&b, 4000, &cfg);
    TEST_ASSERT_EQUAL_UINT32(4000, hb_batt_mv(&b));
    hb_batt_add(&b, 3000, &cfg);
    TEST_ASSERT_EQUAL_UINT32(3750, hb_batt_mv(&b));  // alpha 1/4
    for (int i = 0; i < 60; i++) {
        hb_batt_add(&b, 3000, &cfg);
    }
    TEST_ASSERT_UINT32_WITHIN(1, 3000, hb_batt_mv(&b));

    cfg.batt_cal_ppm = 10000;  // +1 %
    hb_batt_seed(&b, 3800, &cfg);
    TEST_ASSERT_EQUAL_UINT32(3838, hb_batt_mv(&b));

    cfg = hb_cfg_default;
    TEST_ASSERT_EQUAL_UINT8(0, hb_batt_pct(3100, &cfg));
    TEST_ASSERT_EQUAL_UINT8(0, hb_batt_pct(3300, &cfg));
    TEST_ASSERT_EQUAL_UINT8(50, hb_batt_pct(3725, &cfg));
    TEST_ASSERT_EQUAL_UINT8(100, hb_batt_pct(4150, &cfg));
    TEST_ASSERT_EQUAL_UINT8(100, hb_batt_pct(4300, &cfg));
}

static void test_batt_thresholds_have_hysteresis(void)
{
    const hb_cfg_t *cfg = &hb_cfg_default;  // low 3450, off 3300
    hb_batt_t b = {0};
    hb_batt_seed(&b, 3451, cfg);
    TEST_ASSERT_FALSE(b.low);
    hb_batt_seed(&b, 3450, cfg);
    TEST_ASSERT_TRUE(b.low);
    hb_batt_seed(&b, 3500, cfg);
    TEST_ASSERT_TRUE(b.low);  // still within 50 mV
    hb_batt_seed(&b, 3501, cfg);
    TEST_ASSERT_FALSE(b.low);

    hb_batt_seed(&b, 3300, cfg);
    TEST_ASSERT_TRUE(b.off);
    TEST_ASSERT_TRUE(b.low);
    hb_batt_seed(&b, 3350, cfg);
    TEST_ASSERT_TRUE(b.off);
    hb_batt_seed(&b, 3351, cfg);
    TEST_ASSERT_FALSE(b.off);

    hb_batt_seed(&b, 3500, cfg);  // one sag of 300 mV does not reach off through the filter
    hb_batt_add(&b, 3200, cfg);
    TEST_ASSERT_FALSE(b.off);
}

// ---- schedule --------------------------------------------------------------------------------------

// Epochs in IST (+05:30), 2026-11-23 is a Monday.
#define MON_0859_59 1795404599u
#define MON_0900 1795404600u
#define SAT_1859_59 1795872599u
#define SAT_1900 1795872600u
#define SUN_1200 1795933800u
#define MON_NEXT_0900 1796009400u
#define FRI_2159 1795796940u
#define FRI_2200 1795797000u
#define SAT_0559 1795825740u
#define SAT_0600 1795825800u
#define SUN_2330 1795975200u
#define MON_NEXT_0500 1795995000u

static hb_cfg_t office(void)  // the seed schedule: Mon-Sat 09:00-19:00 IST
{
    hb_cfg_t c = hb_cfg_default;
    c.sched_on = true;
    c.tz_min = 330;
    c.days = 0x3f;
    return c;
}

static void test_schedule_office_week(void)
{
    hb_cfg_t c = office();
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, MON_0859_59));
    TEST_ASSERT_EQUAL_UINT32(1, hb_sched_secs_to_start(&c, MON_0859_59));
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, MON_0900));
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, SAT_1859_59));
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, SAT_1900));
    TEST_ASSERT_EQUAL_UINT32(MON_NEXT_0900 - SAT_1900, hb_sched_secs_to_start(&c, SAT_1900));  // Sat evening -> Mon
    TEST_ASSERT_EQUAL_UINT32(38 * 3600, hb_sched_secs_to_start(&c, SAT_1900));
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, SUN_1200));
    TEST_ASSERT_EQUAL_UINT32(21 * 3600, hb_sched_secs_to_start(&c, SUN_1200));
    TEST_ASSERT_EQUAL_UINT32(24 * 3600, hb_sched_secs_to_start(&c, MON_0900));  // strictly after now
}

static void test_schedule_unknown_clock_or_off_is_in_hours(void)
{
    hb_cfg_t c = office();
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, 0));
    c.sched_on = false;
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, SUN_1200));
}

static void test_schedule_midnight_wrap(void)
{
    hb_cfg_t c = office();
    c.days = 1 << 4;  // Friday only, 22:00-06:00: the window spills into Saturday morning
    c.start_min = 22 * 60;
    c.stop_min = 6 * 60;
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, FRI_2159));
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, FRI_2200));
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, SAT_0559));
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, SAT_0600));
    TEST_ASSERT_EQUAL_UINT32(6 * 86400 + 16 * 3600, hb_sched_secs_to_start(&c, SAT_0600));

    c.days = 1 << 6;  // Sunday only: spills into Monday, the week wraps
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, SUN_2330));
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, MON_NEXT_0500));
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, SAT_0559));

    c.start_min = c.stop_min = 9 * 60;  // equal = 24 h from start
    TEST_ASSERT_TRUE(hb_sched_in_hours(&c, MON_NEXT_0500));
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, MON_NEXT_0900));

    c.days = 0;
    TEST_ASSERT_FALSE(hb_sched_in_hours(&c, MON_0900));
    TEST_ASSERT_EQUAL_UINT32(0, hb_sched_secs_to_start(&c, MON_0900));
}

// ---- power states through hb_core ------------------------------------------------------------------

static hb_core_t core;  // persists across boots like RTC memory
static hb_actions_t act;
static uint32_t now, g_mv;
static hb_cfg_t g_cfg;
static hb_mood_t g_mood;
static int g_measured, g_measured_lit, g_low_starts;

static void boot(hb_wake_t wake, bool claimed, bool pressed, uint32_t mv, uint32_t epoch)
{
    now = 300;  // ms since reset when app_main gets going
    g_mv = mv;
    g_mood = HB_MOOD_NONE;
    hb_boot_t b = {.wake = wake, .claimed = claimed, .pressed = pressed, .mv = mv, .epoch = epoch};
    hb_core_boot(&core, &g_cfg, &b, now, &act);
}

static void step(const hb_ev_t *ev)
{
    hb_fx_t before = core.fx;
    hb_core_step(&core, ev, now, &act);
    if (act.mood) {
        g_mood = act.mood;
    }
    g_low_starts += core.fx.kind == HB_FX_LOW && (before.kind != HB_FX_LOW || before.t0 != core.fx.t0);
}

// Runs like the port: waits act.next_ms, answers measure_batt with g_mv, holds the button level
// (a level change steps at once, as the button ISR wakes the port). Stops at a sleep action or after
// ms. Returns true if it stopped on sleep.
static bool run(uint32_t ms, bool pressed)
{
    uint32_t end = now + ms;
    if (pressed != core.btn.raw && !act.sleep) {
        hb_ev_t e = {.type = HB_EV_TICK, .pressed = pressed};
        step(&e);
    }
    while (!act.sleep && (int32_t)(end - now) > 0) {
        if (act.measure_batt) {
            for (int i = 0; i < HB_PIXELS; i++) {
                g_measured_lit += (core.led[i][0] | core.led[i][1] | core.led[i][2]) != 0;
            }
            g_measured++;
            hb_ev_t e = {.type = HB_EV_BATT, .mv = g_mv};
            step(&e);
            continue;
        }
        uint32_t dt = act.next_ms < end - now ? act.next_ms : end - now;
        TEST_ASSERT_TRUE(dt > 0 && dt <= HB_BATT_PERIOD_MS);
        now += dt;
        hb_ev_t e = {.type = HB_EV_TICK, .pressed = pressed};
        step(&e);
    }
    return act.sleep;
}

static void tap(int n)
{
    for (int i = 0; i < n; i++) {
        run(100, true);
        run(100, false);
    }
    run(600, false);
}

static void test_unclaimed_power_on_pairs_for_two_minutes(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, false, false, 3900, 0);
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    TEST_ASSERT_TRUE(act.state_changed);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BATT, core.fx.kind);  // battery bar first
    run(5000, false);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_PAIR, core.fx.kind);  // then the blue pulse
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    run(HB_PAIR_WINDOW_MS, false);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8(0, core.led[0][2]);

    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
}

static void test_gestures_send_and_play_moods(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(HB_MOOD_HEART, g_mood);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_HEART, core.fx.kind);
    run(4000, false);
    tap(2);
    TEST_ASSERT_EQUAL_INT(HB_MOOD_SORRY, g_mood);
    run(4000, false);
    tap(3);
    TEST_ASSERT_EQUAL_INT(HB_MOOD_ANNOY, g_mood);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_ANNOY, core.fx.kind);
}

static void test_five_second_hold_opens_pairing_and_claim_closes_it(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    run(1000, true);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BATT, core.fx.kind);  // the 600 ms hold shows the battery bar
    run(4200, true);
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    run(100, false);
    TEST_ASSERT_EQUAL_INT(HB_MOOD_NONE, g_mood);  // a hold never sends
    hb_ev_t e = {.type = HB_EV_PATH, .path = HB_PATH_BLE};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    e = (hb_ev_t){.type = HB_EV_CLAIMED};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    TEST_ASSERT_TRUE(act.state_changed);
    TEST_ASSERT_TRUE(core.claimed);
}

static void test_path_picks_near_away_offline(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    hb_ev_t e = {.type = HB_EV_PATH, .path = HB_PATH_WIFI};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    e.path = HB_PATH_BLE;
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    e.path = HB_PATH_NONE;
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
}

static void test_nap_after_idle_and_tap_restarts_the_count(void)
{
    g_cfg = hb_cfg_default;  // idle_nap_min 90, nap_wake_min 15
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_FALSE(run(89 * 60000, false));
    tap(1);
    TEST_ASSERT_FALSE(run(89 * 60000, false));
    TEST_ASSERT_TRUE(run(2 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);
    TEST_ASSERT_EQUAL_UINT32(15 * 60, act.sleep_s);
    TEST_ASSERT_EQUAL_UINT8(0, core.led[0][0] | core.led[0][1] | core.led[0][2]);  // dark before sleep
}

static void test_nap_can_be_disabled_and_never_happens_near(void)
{
    g_cfg = hb_cfg_default;
    g_cfg.idle_nap_min = 0;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_FALSE(run(24 * 3600000u, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);

    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    hb_ev_t e = {.type = HB_EV_PATH, .path = HB_PATH_BLE};
    step(&e);
    TEST_ASSERT_FALSE(run(3 * 3600000u, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
}

static void test_nap_timer_wake_checks_in_then_naps_again(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_TRUE(run(91 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);

    boot(HB_WAKE_TIMER, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);  // quiet
    uint32_t t0 = now;
    TEST_ASSERT_TRUE(run(60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);
    TEST_ASSERT_UINT32_WITHIN(HB_TICK_MS, HB_NAP_CHECKIN_MS, now - t0);

    boot(HB_WAKE_BUTTON, true, true, 3900, 0);  // button wake: awake again for the full idle time
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BATT, core.fx.kind);
    TEST_ASSERT_FALSE(run(60 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_MOOD_NONE, g_mood);  // the wake press sent nothing
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
}

static void test_schedule_off_sleeps_until_next_start(void)
{
    g_cfg = office();
    g_cfg.idle_nap_min = 0;
    boot(HB_WAKE_POWERON, true, false, 3900, SAT_1859_59 - 59);  // Sat 18:59:00
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_TRUE(run(10 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);
    uint32_t slept_at = hb_core_epoch(&core, now);
    TEST_ASSERT_UINT32_WITHIN(HB_BATT_PERIOD_MS / 1000, SAT_1900, slept_at);  // re-checked at least every 30 s
    TEST_ASSERT_EQUAL_UINT32(MON_NEXT_0900 - slept_at, act.sleep_s);

    boot(HB_WAKE_TIMER, true, false, 3900, MON_NEXT_0900);
    TEST_ASSERT_FALSE(run(60 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
}

static void test_button_wake_out_of_hours_stays_up_five_minutes(void)
{
    g_cfg = office();
    g_cfg.idle_nap_min = 0;
    boot(HB_WAKE_BUTTON, true, true, 3900, SUN_1200);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    uint32_t t0 = now;
    TEST_ASSERT_TRUE(run(10 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);
    TEST_ASSERT_UINT32_WITHIN(HB_TICK_MS, HB_BTN_GRACE_MS, now - t0);
    TEST_ASSERT_EQUAL_UINT32(MON_NEXT_0900 - hb_core_epoch(&core, now), act.sleep_s);

    boot(HB_WAKE_TIMER, true, false, 3900, SUN_1200);  // early timer wake (RC drift): back to sleep at once
    TEST_ASSERT_TRUE(act.sleep);
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);
}

static void test_unknown_clock_never_sleeps_on_schedule(void)
{
    g_cfg = office();
    g_cfg.idle_nap_min = 0;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_FALSE(run(7 * 24 * 3600000u, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);

    hb_ev_t e = {.type = HB_EV_TIME, .ts = SUN_1200};  // a clock arrives: out of hours
    step(&e);
    TEST_ASSERT_TRUE(run(60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);
}

static void test_sleep_command_and_tap_cancels_pending_sleep(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    hb_ev_t e = {.type = HB_EV_SHOW, .k = HB_MOOD_HEART};
    step(&e);
    e = (hb_ev_t){.type = HB_EV_SLEEP, .for_s = 600};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);
    TEST_ASSERT_FALSE(act.sleep);  // the heart finishes first
    tap(1);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);  // a press while the heart plays keeps it up

    e = (hb_ev_t){.type = HB_EV_SLEEP, .for_s = 600};
    step(&e);
    TEST_ASSERT_TRUE(run(5000, false));
    TEST_ASSERT_EQUAL_UINT32(600, act.sleep_s);
}

static void test_lowbatt_sleeps_button_only_and_rechecks_on_wake(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3250, 0);
    TEST_ASSERT_EQUAL_INT(HB_ST_LOWBATT, act.state);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_LOW, core.fx.kind);
    TEST_ASSERT_FALSE(act.sleep);
    TEST_ASSERT_TRUE(run(5000, false));
    TEST_ASSERT_EQUAL_UINT32(0, act.sleep_s);  // button only

    boot(HB_WAKE_BUTTON, true, true, 3340, 0);  // recovered a little: inside the hysteresis band
    TEST_ASSERT_EQUAL_INT(HB_ST_LOWBATT, act.state);
    TEST_ASSERT_FALSE(run(5000, true));  // never sleeps with the button down
    run(10, false);
    TEST_ASSERT_TRUE(run(5000, false));
    TEST_ASSERT_EQUAL_INT(HB_MOOD_NONE, g_mood);

    boot(HB_WAKE_BUTTON, true, false, 3700, 0);  // charged
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_FALSE(run(60000, false));
}

static void test_draining_battery_warns_once_then_lowbatt(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3600, 0);
    g_low_starts = 0;
    g_mv = 3440;
    TEST_ASSERT_FALSE(run(20 * HB_BATT_PERIOD_MS, false));
    TEST_ASSERT_TRUE(core.batt.low);
    TEST_ASSERT_EQUAL_INT(1, g_low_starts);  // once, not on every sample
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);

    g_mv = 3200;
    TEST_ASSERT_TRUE(run(10 * HB_BATT_PERIOD_MS, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_LOWBATT, act.state);
}

static void test_battery_is_never_sampled_while_leds_are_lit(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, false, false, 3900, 0);  // pairing: 2 min of pulse
    g_measured = g_measured_lit = 0;
    for (int i = 0; i < 40; i++) {
        tap(1);
        run(5000, false);
    }
    TEST_ASSERT_TRUE(g_measured > 0);
    TEST_ASSERT_EQUAL_INT(0, g_measured_lit);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tap_with_bounce_is_one_heart_after_the_gap);
    RUN_TEST(test_double_and_triple_tap);
    RUN_TEST(test_taps_further_apart_than_the_gap_are_separate);
    RUN_TEST(test_glitches_shorter_than_debounce_are_ignored);
    RUN_TEST(test_holds_fire_while_held_and_release_is_not_a_tap);
    RUN_TEST(test_wake_press_is_swallowed_but_can_still_hold);
    RUN_TEST(test_heart_pulses_three_times_at_config_brightness);
    RUN_TEST(test_mood_colours_and_zero_brightness);
    RUN_TEST(test_pair_pulse_repeats_until_replaced);
    RUN_TEST(test_battery_bar_fills_holds_and_wipes);
    RUN_TEST(test_low_battery_blinks_pixel_zero_three_times);
    RUN_TEST(test_batt_filter_calibration_and_pct);
    RUN_TEST(test_batt_thresholds_have_hysteresis);
    RUN_TEST(test_schedule_office_week);
    RUN_TEST(test_schedule_unknown_clock_or_off_is_in_hours);
    RUN_TEST(test_schedule_midnight_wrap);
    RUN_TEST(test_unclaimed_power_on_pairs_for_two_minutes);
    RUN_TEST(test_gestures_send_and_play_moods);
    RUN_TEST(test_five_second_hold_opens_pairing_and_claim_closes_it);
    RUN_TEST(test_path_picks_near_away_offline);
    RUN_TEST(test_nap_after_idle_and_tap_restarts_the_count);
    RUN_TEST(test_nap_can_be_disabled_and_never_happens_near);
    RUN_TEST(test_nap_timer_wake_checks_in_then_naps_again);
    RUN_TEST(test_schedule_off_sleeps_until_next_start);
    RUN_TEST(test_button_wake_out_of_hours_stays_up_five_minutes);
    RUN_TEST(test_unknown_clock_never_sleeps_on_schedule);
    RUN_TEST(test_sleep_command_and_tap_cancels_pending_sleep);
    RUN_TEST(test_lowbatt_sleeps_button_only_and_rechecks_on_wake);
    RUN_TEST(test_draining_battery_warns_once_then_lowbatt);
    RUN_TEST(test_battery_is_never_sampled_while_leds_are_lit);
    return UNITY_END();
}
