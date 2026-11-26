// M1 logic: hb_button, hb_fx, hb_batt, schedule and power states through hb_core_boot / hb_core_step.
#include "unity.h"
#include "hb_core.h"
#include "hb_policy.h"
#include "hb_wire.h"

#include <stdio.h>

#include <string.h>

static bool g_wifi, g_cached;  // hb_boot_t.wifi / .ap_cached for the next boot()
static bool g_quiet_phone;     // the NEAR phone stops its `time` every 4 min (Doze)
static uint32_t g_hello_at;
static bool g_emit;            // run every step's actions through hb_wire_emit into g_pubs

void setUp(void)
{
    g_wifi = g_cached = g_emit = g_quiet_phone = false;
}
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

    const edge_t tap_then_hold[] = {{0, 1}, {100, 0}, {300, 1}, {3200, 0}};  // review I6: the custom mood
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, tap_then_hold, 4, 5000, got, at));  // and no TOGETHER on its release
    TEST_ASSERT_EQUAL_INT(HB_BTN_CUSTOM, got[0]);

    const edge_t together[] = {{0, 1}, {3000, 0}};  // review I3: a hold let go between 2 s and 5 s
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(2, feed(&b, together, 2, 5000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_HOLD, got[0]);
    TEST_ASSERT_EQUAL_INT(HB_BTN_TOGETHER, got[1]);
    const edge_t short_hold[] = {{0, 1}, {1500, 0}};  // under 2 s: just the battery bar
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(1, feed(&b, short_hold, 2, 4000, got, at));

    const edge_t reset[] = {{0, 1}, {16000, 0}};  // review R8: 15 s is a factory reset
    hb_btn_init(&b, false, 0);
    TEST_ASSERT_EQUAL_INT(3, feed(&b, reset, 2, 18000, got, at));
    TEST_ASSERT_EQUAL_INT(HB_BTN_RESET, got[2]);
    TEST_ASSERT_UINT32_WITHIN(10, HB_BTN_DEBOUNCE_MS + HB_BTN_RESET_MS, at[2]);
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

static void test_failed_adc_read_never_means_empty(void)
{
    const hb_cfg_t *cfg = &hb_cfg_default;
    hb_batt_t b = {0};
    hb_batt_seed(&b, 0, cfg);
    TEST_ASSERT_FALSE(b.off);
    TEST_ASSERT_EQUAL_UINT32(0, hb_batt_mv(&b));
    hb_batt_add(&b, 3900, cfg);  // first good read seeds, not 1/4 of it
    TEST_ASSERT_EQUAL_UINT32(3900, hb_batt_mv(&b));
    hb_batt_add(&b, 0, cfg);
    TEST_ASSERT_EQUAL_UINT32(3900, hb_batt_mv(&b));
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
static hb_mood_t g_mood;  // last mood sent
static hb_up_t g_ups[64];  // every uplink since boot()
static int g_n_ups;
static int g_measured, g_measured_lit, g_low_starts;
static struct {
    uint8_t cmd;  // hb_link_cmd_t
    uint32_t t;
} g_links[64];  // every link command since boot()
static int g_n_links, g_teles, g_reports, g_factory;

static struct {
    char topic[HB_TOPIC_MAX];
    char json[HB_MSG_MAX_BEACON + 1];
    bool retain, mood;
    hb_path_t path;
    uint64_t id;
} g_pubs[32];
static int g_n_pubs;
static const hb_wire_ctx_t g_x = {.did = "HB-3FA2C1", .fw = "2.0.0-test", .hw = "A", .rst = "poweron", .rssi = -61,
                                  .heap = 1234};

static void cap(void *arg, const hb_pub_t *p)
{
    (void)arg;
    TEST_ASSERT_EQUAL_size_t(strlen(p->json), (size_t)p->len);
    if (g_n_pubs < 32) {
        snprintf(g_pubs[g_n_pubs].topic, HB_TOPIC_MAX, "%s", p->topic);
        snprintf(g_pubs[g_n_pubs].json, sizeof g_pubs[0].json, "%s", p->json);
        g_pubs[g_n_pubs].retain = p->retain;
        g_pubs[g_n_pubs].mood = p->mood;
        g_pubs[g_n_pubs].path = p->path;
        g_pubs[g_n_pubs++].id = p->id;
    }
}

static void rec(void)
{
    if (g_emit) {
        hb_wire_emit(&core, &act, &g_x, now, cap, NULL);
    }
    if (act.link && g_n_links < 64) {
        g_links[g_n_links].cmd = act.link;
        g_links[g_n_links++].t = now;
    }
    g_teles += act.tele;
    g_factory += act.factory_reset;
    g_reports += act.state_changed;
}

static void boot(hb_wake_t wake, bool claimed, bool pressed, uint32_t mv, uint32_t epoch)
{
    now = 300;  // ms since reset when app_main gets going
    g_mv = mv;
    g_mood = HB_MOOD_NONE;
    g_n_ups = 0;
    g_n_links = g_teles = g_reports = g_n_pubs = g_factory = 0;
    g_hello_at = now;
    hb_boot_t b = {.wake = wake, .claimed = claimed, .pressed = pressed, .mv = mv, .epoch = epoch, .seed = 42,
                   .wifi = g_wifi, .ap_cached = g_cached};
    hb_core_boot(&core, &g_cfg, &b, now, &act);
    rec();
}

static void step(const hb_ev_t *ev)
{
    hb_fx_t before = core.fx;
    hb_core_step(&core, ev, now, &act);
    for (int i = 0; i < act.n_up; i++) {
        if (g_n_ups < 64) {
            g_ups[g_n_ups++] = act.up[i];
        }
        if (act.up[i].k) {
            g_mood = act.up[i].k;
        }
    }
    g_low_starts += core.fx.kind == HB_FX_LOW && (before.kind != HB_FX_LOW || before.t0 != core.fx.t0);
    rec();
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
        if (!g_quiet_phone && core.ble && core.phone && now - g_hello_at >= 240000) {
            g_hello_at = now;  // the app's `time` every 4 min, which keeps a NEAR phone counted as alive
            hb_ev_t h = {.type = HB_EV_HELLO};
            step(&h);
            continue;
        }
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

static void wifi(bool up);
static void mqtt(bool up);

// A path report the way the radios make one: BLE = an encrypted link whose phone has its server link;
// WIFI = the WiFi link comes up (provisioned on the spot); NONE = both gone.
static void path(hb_path_t p)
{
    hb_ev_t e = {.type = HB_EV_BLE, .up = p == HB_PATH_BLE};
    step(&e);
    if (p == HB_PATH_BLE) {
        e.type = HB_EV_PHONE;
        step(&e);
    } else if (p == HB_PATH_WIFI && core.link.st != HB_LS_UP) {
        e = (hb_ev_t){.type = HB_EV_PROV, .up = true};
        step(&e);
        wifi(true);
        mqtt(true);
    } else if (p == HB_PATH_NONE && core.link.st == HB_LS_UP) {
        wifi(false);
    }
}

static void ack(uint64_t id)
{
    hb_ev_t e = {.type = HB_EV_ACK, .id = id};
    step(&e);
}

static void show(hb_mood_t k, uint64_t id)
{
    hb_ev_t e = {.type = HB_EV_SHOW, .k = k, .id = id};
    step(&e);
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
    path(HB_PATH_WIFI);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(HB_MOOD_HEART, g_mood);
    TEST_ASSERT_EQUAL_INT(HB_PATH_WIFI, g_ups[0].path);
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
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);  // a hold never sends
    path(HB_PATH_BLE);
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    hb_ev_t e = {.type = HB_EV_CLAIMED};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    TEST_ASSERT_TRUE(act.state_changed);
    TEST_ASSERT_TRUE(core.claimed);
}

static void test_path_picks_near_away_offline(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    path(HB_PATH_WIFI);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    path(HB_PATH_BLE);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    hb_ev_t e = {.type = HB_EV_BLE, .up = false};
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);  // the BLE link went, WiFi is still up
    path(HB_PATH_NONE);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
}

static void test_nap_after_idle_and_tap_restarts_the_count(void)
{
    g_cfg = hb_cfg_default;
    g_cfg.idle_nap_min = 90;  // nap_wake_min 15
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_FALSE(run(89 * 60000, false));
    tap(1);
    TEST_ASSERT_FALSE(run(89 * 60000, false));
    TEST_ASSERT_TRUE(run(2 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);
    TEST_ASSERT_EQUAL_UINT32(15 * 60, act.sleep_s);
    TEST_ASSERT_EQUAL_UINT8(0, core.led[0][0] | core.led[0][1] | core.led[0][2]);  // dark before sleep
}

static void test_nap_is_off_by_default_and_never_happens_near(void)
{
    g_cfg = hb_cfg_default;
    TEST_ASSERT_EQUAL_UINT16(0, g_cfg.idle_nap_min);
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_FALSE(run(24 * 3600000u, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);

    g_cfg.idle_nap_min = 90;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    path(HB_PATH_BLE);
    TEST_ASSERT_FALSE(run(3 * 3600000u, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
}

static void test_nap_timer_wake_checks_in_then_naps_again(void)
{
    g_cfg = hb_cfg_default;
    g_cfg.idle_nap_min = 90;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_TRUE(run(91 * 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);

    boot(HB_WAKE_TIMER, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);  // quiet
    uint32_t t0 = now;
    TEST_ASSERT_TRUE(run(60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);
    TEST_ASSERT_UINT32_WITHIN(HB_TICK_MS, HB_NAP_LINK_MS, now - t0);  // no link came: gives up after 45 s

    g_wifi = true;  // review R4: a slow link (15 s to CONNACK) still gets its 10 s for queued hearts
    boot(HB_WAKE_TIMER, true, false, 3900, 0);
    t0 = now;
    wifi(true);
    TEST_ASSERT_FALSE(run(15000, false));
    mqtt(true);
    TEST_ASSERT_FALSE(run(HB_NAP_CHECKIN_MS - 500, false));
    TEST_ASSERT_TRUE(run(60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NAP, act.state);
    TEST_ASSERT_UINT32_WITHIN(HB_TICK_MS + 1, 15000 + HB_NAP_CHECKIN_MS, now - t0);
    g_wifi = false;

    boot(HB_WAKE_BUTTON, true, true, 3900, 0);  // button wake: awake again for the full idle time
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BATT, core.fx.kind);
    TEST_ASSERT_FALSE(run(60 * 60000, false));
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);  // the wake press sent nothing
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

// Review 2026-11-26 R2: on USB power the beacon never sleeps, so now_ms runs past 2^31 (24.86 days) and wraps at
// 49.7 days. A deadline left in the past used to wrap into the future: PAIRING (open to any phone) came back.
static void test_months_of_uptime_never_reopen_pairing_or_lose_the_clock(void)
{
    g_cfg = hb_cfg_default;
    const uint32_t epoch = 1795400000u;
    boot(HB_WAKE_POWERON, true, false, 3900, epoch);
    uint32_t t0 = now;
    for (int day = 1; day <= 60; day++) {
        TEST_ASSERT_FALSE(run(86400000u, false));
        TEST_ASSERT_TRUE(act.state != HB_ST_PAIRING);
        TEST_ASSERT_TRUE(core.fx.kind != HB_FX_PAIR);
    }
    uint32_t want = epoch + 60u * 86400u + (now - t0 - 60u * 86400000u) / 1000u;
    TEST_ASSERT_UINT32_WITHIN(1, want, hb_core_epoch(&core, now));
    run(1000, false);
    run(6000, true);  // and a 5 s hold still opens the window when asked
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
}

// Review R5: a crash, watchdog or brownout reset keeps the RTC core (port: RTC_NOINIT + magic) and starts
// quietly: the queued heart survives, and no battery bar loads a weak cell into another brownout.
static void test_reset_wake_keeps_the_outbox_and_starts_quietly(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_UINT8(1, core.n_out);
    uint64_t id = core.out[0].id;
    boot(HB_WAKE_RESET, true, false, 3600, 0);
    TEST_ASSERT_EQUAL_UINT8(1, core.n_out);
    TEST_ASSERT_TRUE(core.out[0].id == id);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);
    path(HB_PATH_BLE);
    TEST_ASSERT_TRUE(g_ups[g_n_ups - 1].id == id);
}

// Review R8: a 15 s hold flashes white and, once the button is up, asks the port for a factory reset.
static void test_fifteen_second_hold_flashes_white_then_factory_resets(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    run(15100, true);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_RESET, core.fx.kind);
    TEST_ASSERT_EQUAL_INT(0, g_factory);  // not while it is still held
    run(2000, false);
    TEST_ASSERT_EQUAL_INT(1, g_factory);
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
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);

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

// ---- uplink acks and show dedupe (PROTOCOL.md 4) -----------------------------------------------------

static void test_mood_waits_for_a_path_then_resends_backing_off_never_on_a_dead_path(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(0, g_n_ups);  // offline: held
    TEST_ASSERT_EQUAL_UINT8(1, core.n_out);
    uint64_t id = core.out[0].id;
    path(HB_PATH_BLE);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[0].k == HB_MOOD_HEART && g_ups[0].path == HB_PATH_BLE && g_ups[0].id == id);
    run(HB_ACK_MS - 100, false);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    run(200, false);  // no `resp` in 5 s and no WiFi (none provisioned): the same id on BLE again, not on a dead path
    TEST_ASSERT_EQUAL_INT(2, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[1].path == HB_PATH_BLE && g_ups[1].id == id);
    run(9800, false);
    TEST_ASSERT_EQUAL_INT(2, g_n_ups);
    run(300, false);  // then after 10 s, 20 s, 40 s, then every 60 s
    TEST_ASSERT_EQUAL_INT(3, g_n_ups);
    run(20000 + 40000 + 60000, false);
    TEST_ASSERT_EQUAL_INT(6, g_n_ups);
    path(HB_PATH_WIFI);  // a path report sends it at once on the new path
    TEST_ASSERT_EQUAL_INT(7, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[6].path == HB_PATH_WIFI && g_ups[6].id == id);
    ack(id);
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);
    run(5 * 60000, false);
    TEST_ASSERT_EQUAL_INT(7, g_n_ups);
}

// Review 2026-11-26 R1: the WiFi session dropped before the PUBACK and was back within 5 s. The mood used to
// fail over to a BLE path that was not there and then sit parked for good on a healthy link.
static void test_mood_whose_session_dropped_is_resent_on_the_healthy_link(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    wifi(true);
    mqtt(true);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    uint64_t id = g_ups[0].id;
    mqtt(false);  // the session drops before the PUBACK
    run(HB_LINK_QUICK_MS, false);
    wifi(true);
    mqtt(true);  // and is back within the 5 s
    run(60 * 60000, false);  // an hour on a healthy link
    int resent = 0;
    for (int i = 1; i < g_n_ups; i++) {
        TEST_ASSERT_TRUE(g_ups[i].id == id && g_ups[i].path == HB_PATH_WIFI);
        resent++;
    }
    TEST_ASSERT_TRUE(resent >= 1 && resent <= 70);  // resent, backing off to once a minute
    ack(id);
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);
}


static void test_ack_clears_only_its_own_mood(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    path(HB_PATH_BLE);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    uint64_t id = g_ups[0].id;
    ack(id ^ 1);  // an ack for something else (the phone acks every relayed message)
    TEST_ASSERT_EQUAL_UINT8(1, core.n_out);
    ack(id);  // the phone's resp{re: id, ok: true}
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);
    run(60000, false);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);  // nothing resent
}

static void test_outbox_keeps_the_newest_four_with_distinct_ids(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    tap(1);
    uint64_t oldest = core.out[0].id;
    for (int i = 0; i < HB_OUTBOX; i++) {
        run(4000, false);
        tap(i % 2 + 1);
    }
    TEST_ASSERT_EQUAL_UINT8(HB_OUTBOX, core.n_out);
    path(HB_PATH_WIFI);
    TEST_ASSERT_EQUAL_INT(HB_OUTBOX, g_n_ups);
    for (int i = 0; i < HB_OUTBOX; i++) {
        TEST_ASSERT_TRUE(g_ups[i].id != oldest && g_ups[i].id == core.out[i].id);
        for (int j = 0; j < i; j++) {
            TEST_ASSERT_TRUE(g_ups[i].id != g_ups[j].id);
        }
    }
}

static void test_unacked_mood_survives_deep_sleep_and_goes_out_on_the_first_path(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    path(HB_PATH_BLE);
    run(3000, false);
    tap(1);
    uint64_t id = g_ups[0].id;
    hb_ev_t e = {.type = HB_EV_SLEEP, .for_s = 600};
    step(&e);
    TEST_ASSERT_TRUE(run(10000, false));

    boot(HB_WAKE_TIMER, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_UINT8(1, core.n_out);
    TEST_ASSERT_EQUAL_INT(0, g_n_ups);
    path(HB_PATH_WIFI);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[0].id == id && g_ups[0].path == HB_PATH_WIFI);
}

static void test_duplicate_show_answers_shown_without_replaying(void)
{
    g_cfg = hb_cfg_default;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    path(HB_PATH_WIFI);
    run(5000, false);  // the boot battery bar is over
    show(HB_MOOD_HEART, 0xaa);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_HEART, core.fx.kind);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[0].k == HB_MOOD_NONE && g_ups[0].id == 0xaa && g_ups[0].path == HB_PATH_WIFI);
    run(5000, false);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);

    show(HB_MOOD_HEART, 0xaa);  // the router's retry on the other path, or a re-flush after a wake
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);  // not replayed
    TEST_ASSERT_EQUAL_INT(2, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[1].k == HB_MOOD_NONE && g_ups[1].id == 0xaa);

    for (uint64_t i = 1; i <= HB_SEEN; i++) {  // 16 newer ids push it out of the ring
        show(HB_MOOD_SORRY, 0x100 + i);
    }
    run(5000, false);
    show(HB_MOOD_HEART, 0xaa);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_HEART, core.fx.kind);

    int before = g_n_ups;
    show(HB_MOOD_NONE, 0xbb);  // no kind: dropped, unanswered
    path(HB_PATH_NONE);
    run(5000, false);
    show(HB_MOOD_ANNOY, 0xcc);  // no path to answer on: plays; the router retries for the receipt
    TEST_ASSERT_EQUAL_UINT8(HB_FX_ANNOY, core.fx.kind);
    TEST_ASSERT_EQUAL_INT(before, g_n_ups);
}

// ---- hb_link: WiFi + MQTT through hb_core ---------------------------------------------------------

static void wifi(bool up)
{
    hb_ev_t e = {.type = HB_EV_WIFI, .up = up};
    step(&e);
}

static void mqtt(bool up)
{
    hb_ev_t e = {.type = HB_EV_MQTT, .up = up};
    step(&e);
}

static uint8_t last_link(void)
{
    return g_n_links ? g_links[g_n_links - 1].cmd : HB_LINK_NONE;
}

static void test_link_backoff_5s_15s_60s_5m_15m_then_30m_and_never_sleeps(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_SCAN, last_link());  // nothing cached yet: scan
    static const uint32_t want[] = {5000, 15000, 60000, 300000, 900000, 1800000, 1800000, 1800000};
    for (int i = 0; i < 8; i++) {
        wifi(false);  // no known AP answered
        TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
        uint32_t t0 = now;
        int n = g_n_links;
        TEST_ASSERT_FALSE(run(want[i] + 200, false));  // light sleep in OFFLINE: no deep sleep, no reboot
        TEST_ASSERT_EQUAL_INT(n + 1, g_n_links);
        TEST_ASSERT_EQUAL_UINT8(HB_LINK_SCAN, last_link());
        TEST_ASSERT_EQUAL_UINT32(t0 + want[i], g_links[g_n_links - 1].t);
        TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    }
}

static void test_link_cached_ap_first_then_scan_at_once_then_mqtt(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = g_cached = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
    wifi(false);  // the cached BSSID is gone
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_SCAN, last_link());
    TEST_ASSERT_EQUAL_UINT32(g_links[0].t, g_links[1].t);  // no wait before the scan
    wifi(true);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_MQTT, last_link());
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);  // an IP alone is no path to the server
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    TEST_ASSERT_TRUE(act.state_changed && act.tele);

    boot(HB_WAKE_POWERON, true, false, 3900, 0);  // a cached AP that never answers: scan after 5 s
    uint32_t t0 = now;
    run(HB_LINK_FAST_MS + 100, false);
    TEST_ASSERT_EQUAL_INT(2, g_n_links);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_SCAN, last_link());
    TEST_ASSERT_EQUAL_UINT32(t0 + HB_LINK_FAST_MS, g_links[1].t);
}

static void test_link_drop_retries_after_1s_then_backs_off(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    wifi(true);
    mqtt(true);
    run(2000, false);
    mqtt(false);  // broker or AP gone
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    uint32_t t0 = now;
    run(HB_LINK_QUICK_MS + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());  // the join cached the AP
    TEST_ASSERT_EQUAL_UINT32(t0 + HB_LINK_QUICK_MS, g_links[g_n_links - 1].t);
    wifi(false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_SCAN, last_link());
    wifi(false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    t0 = now;
    run(5100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
    TEST_ASSERT_EQUAL_UINT32(t0 + 5000, g_links[g_n_links - 1].t);
    wifi(true);
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    mqtt(false);  // a success reset the count: the next failed round waits 5 s again
    run(HB_LINK_QUICK_MS, false);
    wifi(false);
    wifi(false);
    t0 = now;
    run(5100, false);
    TEST_ASSERT_EQUAL_UINT32(t0 + 5000, g_links[g_n_links - 1].t);
}

static void test_link_mqtt_without_connack_is_a_failed_round(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    wifi(true);
    uint32_t t0 = now;
    run(HB_LINK_MQTT_MS + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    TEST_ASSERT_EQUAL_UINT32(t0 + HB_LINK_MQTT_MS, g_links[g_n_links - 1].t);
    wifi(false);  // a late disconnect from the teardown is ignored while waiting
    mqtt(false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    run(5000, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
}

static void test_every_connect_reports_state_then_tele_every_tele_min(void)
{
    g_cfg = hb_cfg_default;
    g_cfg.tele_min = 2;
    g_wifi = true;
    boot(HB_WAKE_POWERON, false, false, 3900, 0);  // unclaimed: PAIRING, the state does not change on connect
    wifi(true);
    int reports = g_reports;
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(HB_ST_PAIRING, act.state);
    TEST_ASSERT_TRUE(act.state_changed);
    TEST_ASSERT_EQUAL_INT(reports + 1, g_reports);
    TEST_ASSERT_EQUAL_INT(1, g_teles);
    run(2 * 60000 + 100, false);
    TEST_ASSERT_EQUAL_INT(2, g_teles);
    run(2 * 60000, false);
    TEST_ASSERT_EQUAL_INT(3, g_teles);
    wifi(false);
    run(10 * 60000, false);  // link down: no tele, even after reconnect attempts fail
    TEST_ASSERT_EQUAL_INT(3, g_teles);
}

static void test_heart_tapped_offline_goes_out_on_connect_and_again_after_a_reconnect(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(0, g_n_ups);  // no path yet: queued
    wifi(true);
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    uint64_t id = g_ups[0].id;
    TEST_ASSERT_TRUE(g_ups[0].k == HB_MOOD_HEART && g_ups[0].path == HB_PATH_WIFI);
    run(HB_ACK_MS + 100, false);  // no PUBACK and no BLE path: the same id on WiFi again, next in 10 s
    TEST_ASSERT_EQUAL_INT(2, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[1].id == id && g_ups[1].path == HB_PATH_WIFI);
    run(HB_ACK_MS + 100, false);
    TEST_ASSERT_EQUAL_INT(2, g_n_ups);
    mqtt(false);
    run(HB_LINK_QUICK_MS, false);
    wifi(true);
    mqtt(true);  // reconnected: resent with the same id (the router dedupes on did + id)
    TEST_ASSERT_EQUAL_INT(3, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[2].id == id && g_ups[2].path == HB_PATH_WIFI);
    ack(id);
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);
}

static void test_config_use_wifi_stops_and_restarts_the_link(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    wifi(true);
    mqtt(true);
    hb_cfg_t c = hb_cfg_default;
    c.n = 2;
    c.use_wifi = false;
    hb_ev_t e = {.type = HB_EV_CFG, .cfg = &c};
    step(&e);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_EQUAL_UINT32(2, core.cfg.n);
    int n = g_n_links;
    run(60000, false);
    TEST_ASSERT_EQUAL_INT(n, g_n_links);  // stays off
    c.use_wifi = true;
    step(&e);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());

    g_wifi = false;  // nothing provisioned: the link never starts
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    run(60000, false);
    TEST_ASSERT_EQUAL_INT(0, g_n_links);
}

// ---- hb_wire: what goes on the wire ----------------------------------------------------------------

#define EPOCH0 1795400000u  // 2026-11-23 06:33:20 UTC

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(j) ? j->valuestring : "(none)";
}

static double jnum(const cJSON *o, const char *k)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(j) ? j->valuedouble : -1;
}

static void connected(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = g_emit = true;
    boot(HB_WAKE_POWERON, true, false, 3900, EPOCH0);
    wifi(true);
    mqtt(true);
}

static void test_wire_connect_publishes_state_and_tele_then_a_tap_publishes_the_mood(void)
{
    connected();
    TEST_ASSERT_EQUAL_INT(2, g_n_pubs);
    TEST_ASSERT_EQUAL_STRING("hb/v1/d/HB-3FA2C1/state", g_pubs[0].topic);
    TEST_ASSERT_TRUE(g_pubs[0].retain && !g_pubs[0].mood && g_pubs[0].path == HB_PATH_WIFI);
    hb_msg_t m;
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[0].json, strlen(g_pubs[0].json), &m));
    TEST_ASSERT_EQUAL_STRING("state", m.t);
    TEST_ASSERT_EQUAL_STRING("away", jstr(m.root, "st"));
    TEST_ASSERT_EQUAL_STRING("wifi", jstr(m.root, "path"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(m.root, "wake"));
    TEST_ASSERT_EQUAL_UINT32(EPOCH0, m.ts);
    hb_msg_free(&m);

    TEST_ASSERT_EQUAL_STRING("hb/v1/d/HB-3FA2C1/up", g_pubs[1].topic);
    TEST_ASSERT_FALSE(g_pubs[1].retain);
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[1].json, strlen(g_pubs[1].json), &m));
    TEST_ASSERT_EQUAL_STRING("tele", m.t);
    TEST_ASSERT_EQUAL_INT(3900, (int)jnum(m.root, "mv"));
    TEST_ASSERT_EQUAL_INT(-61, (int)jnum(m.root, "rssi"));
    TEST_ASSERT_EQUAL_INT(0, (int)jnum(m.root, "cfg"));
    TEST_ASSERT_EQUAL_STRING("2.0.0-test", jstr(m.root, "fw"));
    TEST_ASSERT_EQUAL_STRING("A", jstr(m.root, "hw"));
    TEST_ASSERT_EQUAL_STRING("poweron", jstr(m.root, "rst"));
    TEST_ASSERT_EQUAL_INT(1234, (int)jnum(m.root, "heap"));
    TEST_ASSERT_TRUE(jnum(m.root, "pct") >= 0 && jnum(m.root, "up_s") >= 0);
    hb_msg_free(&m);

    run(3000, false);
    tap(1);
    TEST_ASSERT_EQUAL_INT(3, g_n_pubs);
    TEST_ASSERT_TRUE(g_pubs[2].mood && !g_pubs[2].retain && g_pubs[2].id == g_ups[0].id);
    TEST_ASSERT_EQUAL_STRING("hb/v1/d/HB-3FA2C1/up", g_pubs[2].topic);
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[2].json, strlen(g_pubs[2].json), &m));
    char want[128], id[HB_ID_LEN + 1];
    hb_id_format(g_ups[0].id, id);
    snprintf(want, sizeof want, "{\"v\":1,\"t\":\"mood\",\"id\":\"%s\",\"ts\":%lu,\"k\":\"heart\"}", id,
             (unsigned long)m.ts);
    TEST_ASSERT_EQUAL_STRING(want, g_pubs[2].json);
    TEST_ASSERT_UINT32_WITHIN(10, EPOCH0 + 5, m.ts);
    hb_msg_free(&m);

    char will[HB_MSG_MAX_BEACON + 1];
    TEST_ASSERT_TRUE(hb_wire_will(&core, now, will, sizeof will) > 0);
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(will, strlen(will), &m));
    TEST_ASSERT_EQUAL_STRING("state", m.t);
    TEST_ASSERT_EQUAL_STRING("lost", jstr(m.root, "st"));
    TEST_ASSERT_EQUAL_INT(5, cJSON_GetArraySize(m.root));  // v, t, id, ts, st
    hb_msg_free(&m);
}

static hb_rx_t g_rx;

static int rx(const char *topic, const char *json)
{
    int before = g_n_pubs;
    hb_wire_rx(&core, &g_x, topic, json, strlen(json), now, &g_rx, cap, NULL);
    if (g_rx.has_ev) {
        step(&g_rx.ev);
    }
    return g_n_pubs - before;
}

static void test_wire_downlinks_become_events_and_replies(void)
{
    const char *down = "hb/v1/d/HB-3FA2C1/down";
    connected();
    run(5000, false);  // the boot battery bar is over
    g_n_pubs = 0;

    TEST_ASSERT_EQUAL_INT(1, rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000aa\",\"ts\":0,\"k\":\"sorry\"}"));
    TEST_ASSERT_EQUAL_UINT8(HB_FX_SORRY, core.fx.kind);
    char want[128];
    snprintf(want, sizeof want, "{\"v\":1,\"t\":\"shown\",\"id\":\"00000000000000aa\",\"ts\":%lu}",
             (unsigned long)hb_core_epoch(&core, now));
    TEST_ASSERT_EQUAL_STRING(want, g_pubs[0].json);
    TEST_ASSERT_FALSE(g_pubs[0].mood);

    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"time\",\"id\":\"00000000000000ab\",\"ts\":1800000000,\"tz\":330}"));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, hb_core_epoch(&core, now));
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"time\",\"id\":\"00000000000000ac\",\"ts\":0,\"tz\":330}"));
    TEST_ASSERT_EQUAL_UINT32(1800000000u, hb_core_epoch(&core, now));  // ts 0 carries no time

    hb_msg_t m;
    TEST_ASSERT_EQUAL_INT(1, rx(down, "{\"v\":1,\"t\":\"get\",\"id\":\"00000000000000ad\",\"ts\":0,\"what\":\"info\"}"));
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[1].json, strlen(g_pubs[1].json), &m));
    TEST_ASSERT_EQUAL_STRING("resp", m.t);
    TEST_ASSERT_EQUAL_STRING("00000000000000ad", jstr(m.root, "re"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m.root, "ok")));
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(m.root, "d");
    TEST_ASSERT_EQUAL_STRING("HB-3FA2C1", jstr(d, "did"));
    TEST_ASSERT_EQUAL_STRING("2.0.0-test", jstr(d, "fw"));
    TEST_ASSERT_EQUAL_INT(1, (int)jnum(d, "pv"));
    TEST_ASSERT_EQUAL_INT(1, (int)jnum(d, "min_pv"));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(d, "claimed")));
    hb_msg_free(&m);

    TEST_ASSERT_EQUAL_INT(1, rx(down, "{\"v\":1,\"t\":\"dance\",\"id\":\"00000000000000ae\",\"ts\":0}"));
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[2].json, strlen(g_pubs[2].json), &m));
    TEST_ASSERT_EQUAL_STRING("resp", m.t);
    TEST_ASSERT_EQUAL_STRING("00000000000000ae", jstr(m.root, "re"));
    TEST_ASSERT_EQUAL_STRING("unknown_t", jstr(m.root, "err"));
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(m.root, "ok")));
    hb_msg_free(&m);

    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"dance\",\"id\":\"BAD\",\"ts\":0}"));  // fails 2.2: silent
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{"));
    TEST_ASSERT_EQUAL_INT(0, rx("hb/v1/d/HB-000001/down",
                                "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000af\",\"ts\":0,\"k\":\"heart\"}"));
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000b0\",\"ts\":0,\"k\":\"hug\"}"));
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"sleep\",\"id\":\"00000000000000b1\",\"ts\":0,\"for_s\":-1}"));
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);

    run(5000, false);
    TEST_ASSERT_EQUAL_INT(1, rx(down, "{\"v\":1,\"t\":\"sleep\",\"id\":\"00000000000000b2\",\"ts\":0,\"for_s\":600}"));
    TEST_ASSERT_EQUAL_INT(HB_ST_SLEEP, act.state);  // retained state before the deep sleep, with its wake time
    TEST_ASSERT_EQUAL_STRING("hb/v1/d/HB-3FA2C1/state", g_pubs[3].topic);
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[3].json, strlen(g_pubs[3].json), &m));
    TEST_ASSERT_EQUAL_STRING("sleep", jstr(m.root, "st"));
    TEST_ASSERT_EQUAL_UINT32(hb_core_epoch(&core, now) + 600, (uint32_t)jnum(m.root, "wake"));
    hb_msg_free(&m);
    TEST_ASSERT_TRUE(run(1000, false));
    TEST_ASSERT_EQUAL_UINT32(600, act.sleep_s);
}

// ---- M4: handover (PROTOCOL.md 4) and the BLE requests ---------------------------------------------

static void ble_link(bool up)
{
    hb_ev_t e = {.type = HB_EV_BLE, .up = up};
    step(&e);
}

static void phone(bool up)
{
    hb_ev_t e = {.type = HB_EV_PHONE, .up = up};
    step(&e);
}

// Claimed, WiFi up, then the phone comes near and holds the BLE path past net.near_hold_s.
static void near_with_wifi_stopped(void)
{
    g_cfg = hb_cfg_default;
    g_wifi = true;
    boot(HB_WAKE_POWERON, true, false, 3900, 0);
    wifi(true);
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    path(HB_PATH_BLE);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    int n = g_n_links;
    run(g_cfg.near_hold_s * 1000u - 100, false);
    TEST_ASSERT_EQUAL_INT(n, g_n_links);  // WiFi stays up through the hold
    run(200, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
    TEST_ASSERT_TRUE(core.near_off);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
}

static void test_near_held_stops_wifi_and_away_restarts_it_after_the_grace(void)
{
    near_with_wifi_stopped();
    run(10 * 60000, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());  // stays off while near
    ble_link(false);
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    int n = g_n_links;
    run(g_cfg.away_grace_s * 1000u - 100, false);
    TEST_ASSERT_EQUAL_INT(n, g_n_links);  // the phone may just be out of range for a moment
    run(200, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());  // the cached AP first
    TEST_ASSERT_FALSE(core.near_off);
    wifi(true);
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);

    n = g_n_links;
    path(HB_PATH_BLE);  // back near before the hold: WiFi keeps running
    run(5000, false);
    ble_link(false);
    run(60000, false);
    TEST_ASSERT_EQUAL_INT(n, g_n_links);
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
}

static void test_pending_uplink_skips_the_grace(void)
{
    near_with_wifi_stopped();
    ble_link(false);
    run(3000, false);
    tap(1);  // a heart with no path: WiFi now, not in a minute
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
    TEST_ASSERT_TRUE(now - g_links[g_n_links - 1].t < 1000);
    wifi(true);
    mqtt(true);
    TEST_ASSERT_EQUAL_INT(1, g_n_ups);
    TEST_ASSERT_TRUE(g_ups[0].k == HB_MOOD_HEART && g_ups[0].path == HB_PATH_WIFI);
}

static void test_phone_upstream_down_starts_wifi_after_the_grace_keeping_ble(void)
{
    near_with_wifi_stopped();
    phone(false);  // the phone lost its server: no BLE path, but the link stays
    TEST_ASSERT_EQUAL_INT(HB_ST_OFFLINE, act.state);
    TEST_ASSERT_TRUE(core.ble);
    run(g_cfg.away_grace_s * 1000u + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
    phone(true);  // its server is back: NEAR again at once, WiFi off after another hold
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    run(g_cfg.near_hold_s * 1000u + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
}

// Review R12 / A1: a phone in Doze keeps its BLE link but its app stops talking (and its MQTT session may be gone
// without a `link up:false`). After HB_PHONE_SILENT_MS of silence the beacon stops counting it as its path.
static void test_silent_phone_in_doze_hands_back_to_wifi(void)
{
    near_with_wifi_stopped();
    g_quiet_phone = true;
    TEST_ASSERT_FALSE(run(HB_PHONE_SILENT_MS - 60000, false));
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
    TEST_ASSERT_FALSE(run(60000 + 1000, false));
    TEST_ASSERT_TRUE(core.ble);  // the link itself stays
    TEST_ASSERT_TRUE(act.state != HB_ST_NEAR);
    TEST_ASSERT_FALSE(run(g_cfg.away_grace_s * 1000u + 1000, false));
    TEST_ASSERT_FALSE(core.near_off);  // WiFi takes over
    TEST_ASSERT_TRUE(core.link.st != HB_LS_OFF);
    hb_ev_t e = {.type = HB_EV_PHONE, .up = true};  // the phone wakes and says hello: NEAR again
    step(&e);
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);
}

static void test_a_wifi_change_is_tried_at_once_even_near(void)
{
    near_with_wifi_stopped();
    hb_ev_t e = {.type = HB_EV_PROV, .up = true};
    step(&e);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());
    TEST_ASSERT_FALSE(core.near_off);
    run(g_cfg.near_hold_s * 1000u + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());  // near: off again after the hold

    e.up = false;  // the last network forgotten: no WiFi to start, ever
    step(&e);
    ble_link(false);
    run(10 * 60000, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_DOWN, last_link());
}

// A NEAR phone that stops acking (its server link died without saying so): WiFi, stopped by NEAR, comes back
// for the mood.
static void test_unacked_ble_mood_brings_back_the_wifi_near_stopped(void)
{
    near_with_wifi_stopped();
    tap(1);
    int n = g_n_ups;
    TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_ups[n - 1].path);
    run(HB_ACK_MS + 100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_LINK_FAST, last_link());  // WiFi started for it
    wifi(true);
    mqtt(true);
    run(HB_ACK_MS * 2 + 100, false);  // the next retry goes on the WiFi that is now live
    TEST_ASSERT_EQUAL_INT(HB_PATH_WIFI, g_ups[g_n_ups - 1].path);
}

static int ble_rx(const char *json)
{
    int before = g_n_pubs;
    hb_wire_ble_rx(&core, &g_x, json, strlen(json), now, &g_rx, cap, NULL);
    if (g_rx.has_ev) {
        step(&g_rx.ev);
    }
    return g_n_pubs - before;
}

// The last publish as a resp to re: returns its err ("" when ok).
static const char *resp_to(const char *re)
{
    static char err[32];
    hb_msg_t m;
    TEST_ASSERT_EQUAL_INT(HB_MSG_OK, hb_msg_decode(g_pubs[g_n_pubs - 1].json, strlen(g_pubs[g_n_pubs - 1].json), &m));
    TEST_ASSERT_EQUAL_STRING("resp", m.t);
    TEST_ASSERT_EQUAL_STRING(re, jstr(m.root, "re"));
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m.root, "ok"));
    snprintf(err, sizeof err, "%s", ok ? "" : jstr(m.root, "err"));
    hb_msg_free(&m);
    return err;
}

static void test_wire_ble_answers_on_ble_and_the_phone_drives_the_path(void)
{
    connected();
    run(5000, false);
    ble_link(true);
    g_n_pubs = 0;
    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"get\",\"id\":\"00000000000000c1\",\"ts\":0,\"what\":\"info\"}"));
    TEST_ASSERT_EQUAL_STRING("", resp_to("00000000000000c1"));
    TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_pubs[0].path);  // answered where it was asked
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);         // no `link up` yet

    TEST_ASSERT_EQUAL_INT(2, ble_rx("{\"v\":1,\"t\":\"link\",\"id\":\"00000000000000c2\",\"ts\":0,\"up\":true}"));
    TEST_ASSERT_EQUAL_INT(HB_ST_NEAR, act.state);  // state and tele go out on the new path, as on a connect
    TEST_ASSERT_TRUE(strstr(g_pubs[1].json, "\"t\":\"state\"") && strstr(g_pubs[1].json, "\"path\":\"ble\""));
    TEST_ASSERT_TRUE(strstr(g_pubs[2].json, "\"t\":\"tele\"") != NULL);
    TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_pubs[1].path);
    TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_pubs[2].path);
    // a new app session on the same connection says `link up` again: it gets the state and tele too
    TEST_ASSERT_EQUAL_INT(2, ble_rx("{\"v\":1,\"t\":\"link\",\"id\":\"00000000000000c7\",\"ts\":0,\"up\":true}"));

    run(3000, false);
    tap(1);
    uint64_t id = g_ups[g_n_ups - 1].id;
    TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_ups[g_n_ups - 1].path);
    char ack[160];
    char ids[HB_ID_LEN + 1];
    hb_id_format(id, ids);
    snprintf(ack, sizeof ack, "{\"v\":1,\"t\":\"resp\",\"id\":\"00000000000000c3\",\"ts\":0,\"re\":\"%s\",\"ok\":true}", ids);
    TEST_ASSERT_EQUAL_INT(0, ble_rx(ack));  // the phone's ack is not answered
    TEST_ASSERT_EQUAL_UINT8(0, core.n_out);

    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"link\",\"id\":\"00000000000000c4\",\"ts\":0,\"up\":false}"));
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    TEST_ASSERT_EQUAL_INT(HB_PATH_WIFI, g_pubs[g_n_pubs - 1].path);

    // the BLE-only requests do nothing over MQTT
    const char *down = "hb/v1/d/HB-3FA2C1/down";
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"link\",\"id\":\"00000000000000c5\",\"ts\":0,\"up\":true}"));
    TEST_ASSERT_EQUAL_INT(HB_ST_AWAY, act.state);
    TEST_ASSERT_EQUAL_INT(0, rx(down, "{\"v\":1,\"t\":\"reset\",\"id\":\"00000000000000c6\",\"ts\":0,\"what\":\"factory\"}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_NONE, g_rx.cmd.type);
}

static void test_wire_ble_requests_are_checked_before_the_port_sees_them(void)
{
    g_cfg = hb_cfg_default;
    g_emit = true;
    boot(HB_WAKE_POWERON, false, false, 3900, 0);  // unclaimed
    ble_link(true);
    g_n_pubs = 0;

    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"claim\",\"id\":\"00000000000000d1\",\"ts\":0,\"host\":\"203.0.113.10\","
                                    "\"mport\":70000,\"aport\":8443,\"user\":\"HB-3FA2C1\",\"pass\":\"p\"}"));
    TEST_ASSERT_EQUAL_STRING("bad_mport", resp_to("00000000000000d1"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_NONE, g_rx.cmd.type);

    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"claim\",\"id\":\"00000000000000d2\",\"ts\":0,\"host\":\"203.0.113.10\","
                                    "\"mport\":8883,\"aport\":8443,\"user\":\"HB-3FA2C1\",\"pass\":\"secret\"}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_CLAIM, g_rx.cmd.type);  // the port stores it and answers
    TEST_ASSERT_EQUAL_STRING("00000000000000d2", g_rx.cmd.re);
    TEST_ASSERT_EQUAL_STRING("203.0.113.10", g_rx.cmd.host);
    TEST_ASSERT_EQUAL_UINT16(8883, g_rx.cmd.mport);
    TEST_ASSERT_EQUAL_STRING("secret", g_rx.cmd.pass);
    hb_ev_t e = {.type = HB_EV_CLAIMED};
    step(&e);
    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"claim\",\"id\":\"00000000000000d3\",\"ts\":0,\"host\":\"203.0.113.10\","
                                    "\"mport\":8883,\"aport\":8443,\"user\":\"HB-3FA2C1\",\"pass\":\"other\"}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_CLAIM, g_rx.cmd.type);  // a bonded phone may claim again (new credentials)
    TEST_ASSERT_EQUAL_STRING("other", g_rx.cmd.pass);

    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"wifi.set\",\"id\":\"00000000000000d4\",\"ts\":0,\"ssid\":\"Home\",\"psk\":\"short\"}"));
    TEST_ASSERT_EQUAL_STRING("bad_psk", resp_to("00000000000000d4"));
    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"wifi.set\",\"id\":\"00000000000000d5\",\"ts\":0,\"ssid\":\"\",\"psk\":\"\"}"));
    TEST_ASSERT_EQUAL_STRING("bad_ssid", resp_to("00000000000000d5"));
    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"wifi.set\",\"id\":\"00000000000000d6\",\"ts\":0,\"ssid\":\"Open\",\"psk\":\"\"}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_WIFI_SET, g_rx.cmd.type);
    TEST_ASSERT_EQUAL_STRING("Open", g_rx.cmd.ssid);
    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"wifi.set\",\"id\":\"00000000000000d7\",\"ts\":0,\"ssid\":\"Home\",\"psk\":\"brass-heart-9\"}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_WIFI_SET, g_rx.cmd.type);
    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"wifi.scan\",\"id\":\"00000000000000d8\",\"ts\":0}"));
    TEST_ASSERT_EQUAL_INT(HB_CMD_WIFI_SCAN, g_rx.cmd.type);
    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"reset\",\"id\":\"00000000000000d9\",\"ts\":0,\"what\":\"all\"}"));
    TEST_ASSERT_EQUAL_STRING("bad_what", resp_to("00000000000000d9"));
    TEST_ASSERT_EQUAL_INT(0, ble_rx("{\"v\":1,\"t\":\"reset\",\"id\":\"00000000000000da\",\"ts\":0,\"what\":\"factory\"}"));
    TEST_ASSERT_TRUE(g_rx.cmd.type == HB_CMD_RESET && g_rx.cmd.factory);
    TEST_ASSERT_EQUAL_INT(1, ble_rx("{\"v\":1,\"t\":\"cfg\",\"id\":\"00000000000000db\",\"ts\":0,\"n\":5,\"b\":\"e30\",\"s\":\"AA\"}"));
    TEST_ASSERT_TRUE(resp_to("00000000000000db")[0] != 0);  // refused, and told why
    TEST_ASSERT_FALSE(g_rx.has_ev);
    for (int i = 0; i < g_n_pubs; i++) {
        TEST_ASSERT_EQUAL_INT(HB_PATH_BLE, g_pubs[i].path);
    }
}

// ---- review ideas (I2-I7): custom and together, echo and celebrate, streak ember, presence, good morning ----

static void test_custom_and_together_gestures_send_and_glow(void)
{
    connected();
    run(5000, false);
    g_n_pubs = 0;
    run(100, true);  // tap
    run(150, false);
    run(900, true);  // then hold: the custom mood
    run(800, false);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_CUSTOM, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(hb_cfg_default.mood_rgb, core.fx.rgb, 3);
    TEST_ASSERT_TRUE(strstr(g_pubs[g_n_pubs - 1].json, "\"k\":\"custom\",\"rgb\":\"#ff4fa3\",\"pat\":0") != NULL);
    run(8000, false);
    run(3000, true);  // a 2-5 s hold: together
    run(100, false);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BREATH, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8(3, core.fx.arg);  // an invitation: 3 breaths
    TEST_ASSERT_TRUE(strstr(g_pubs[g_n_pubs - 1].json, "\"k\":\"together\"") != NULL);
}

static void test_new_show_kinds_play_and_together_is_in_step(void)
{
    const char *down = "hb/v1/d/HB-3FA2C1/down";
    connected();
    run(5000, false);
    rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000e1\",\"ts\":0,\"k\":\"custom\",\"rgb\":\"#0a14c8\",\"pat\":1}");
    TEST_ASSERT_EQUAL_UINT8(HB_FX_CUSTOM, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8(1, core.fx.arg);
    TEST_ASSERT_TRUE(core.fx.rgb[0] == 10 && core.fx.rgb[1] == 20 && core.fx.rgb[2] == 200);
    rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000e2\",\"ts\":0,\"k\":\"echo\"}");
    TEST_ASSERT_EQUAL_UINT8(HB_FX_ECHO, core.fx.kind);
    rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000e3\",\"ts\":0,\"k\":\"celebrate\"}");
    TEST_ASSERT_EQUAL_UINT8(HB_FX_CELEBRATE, core.fx.kind);
    // synced together: the breath phase is set by the shared `at`, so two beacons with the same clock agree
    char msg[200];
    uint32_t at = hb_core_epoch(&core, now) - 7;
    snprintf(msg, sizeof msg, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000e4\",\"ts\":0,\"k\":\"together\",\"at\":%lu}",
             (unsigned long)at);
    rx(down, msg);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BREATH, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_BREATHS, core.fx.arg);
    uint32_t phase = ((hb_core_epoch(&core, now) - at) * 1000u + (now - core.ms0) % 1000u) % HB_FX_BREATH_MS;
    TEST_ASSERT_EQUAL_UINT32(phase, now - core.fx.t0);
}

static void test_streak_ember_and_partner_presence_light_idle_pixels(void)
{
    const char *down = "hb/v1/d/HB-3FA2C1/down";
    connected();
    run(6000, false);  // the boot bar is over: dark
    TEST_ASSERT_EQUAL_UINT8(0, core.led[0][0]);
    rx(down, "{\"v\":1,\"t\":\"streak\",\"id\":\"00000000000000f1\",\"ts\":0,\"n\":4}");
    run(100, false);
    TEST_ASSERT_EQUAL_UINT16(4, core.streak);
    TEST_ASSERT_TRUE(core.led[0][0] > 0 && core.led[4][0] == 0);  // the ember at the tip
    rx(down, "{\"v\":1,\"t\":\"presence\",\"id\":\"00000000000000f2\",\"ts\":0,\"uid\":\"bob\",\"near\":true}");
    run(100, false);
    TEST_ASSERT_EQUAL_UINT8(0, core.led[4][0]);  // led.presence is off by default
    core.cfg.presence = true;
    run(100, false);
    TEST_ASSERT_TRUE(core.led[4][0] > 0);
    hb_ev_t e = {.type = HB_EV_SLEEP, .for_s = 600};  // never into a deep sleep lit
    step(&e);
    TEST_ASSERT_TRUE(run(10000, false));
    TEST_ASSERT_EQUAL_UINT8(0, core.led[0][0] | core.led[4][0]);
}

static void test_good_morning_replays_last_evening_heart_once_softly(void)
{
    const char *down = "hb/v1/d/HB-3FA2C1/down";
    connected();
    run(5000, false);
    rx(down, "{\"v\":1,\"t\":\"show\",\"id\":\"00000000000000a7\",\"ts\":0,\"k\":\"sorry\"}");
    run(5000, false);
    hb_ev_t e = {.type = HB_EV_SLEEP, .for_s = 3600};
    step(&e);
    TEST_ASSERT_TRUE(run(10000, false));
    boot(HB_WAKE_TIMER, true, false, 3900, EPOCH0);
    TEST_ASSERT_EQUAL_UINT8(HB_FX_SORRY, core.fx.kind);
    TEST_ASSERT_EQUAL_UINT8(HB_GOOD_MORNING_LEVEL, core.fx.level);
    e = (hb_ev_t){.type = HB_EV_SLEEP, .for_s = 3600};
    run(5000, false);
    step(&e);
    TEST_ASSERT_TRUE(run(10000, false));
    boot(HB_WAKE_TIMER, true, false, 3900, EPOCH0);  // once only
    TEST_ASSERT_EQUAL_UINT8(HB_FX_NONE, core.fx.kind);
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
    RUN_TEST(test_failed_adc_read_never_means_empty);
    RUN_TEST(test_schedule_office_week);
    RUN_TEST(test_schedule_unknown_clock_or_off_is_in_hours);
    RUN_TEST(test_schedule_midnight_wrap);
    RUN_TEST(test_unclaimed_power_on_pairs_for_two_minutes);
    RUN_TEST(test_gestures_send_and_play_moods);
    RUN_TEST(test_five_second_hold_opens_pairing_and_claim_closes_it);
    RUN_TEST(test_path_picks_near_away_offline);
    RUN_TEST(test_nap_after_idle_and_tap_restarts_the_count);
    RUN_TEST(test_nap_is_off_by_default_and_never_happens_near);
    RUN_TEST(test_nap_timer_wake_checks_in_then_naps_again);
    RUN_TEST(test_schedule_off_sleeps_until_next_start);
    RUN_TEST(test_button_wake_out_of_hours_stays_up_five_minutes);
    RUN_TEST(test_months_of_uptime_never_reopen_pairing_or_lose_the_clock);
    RUN_TEST(test_reset_wake_keeps_the_outbox_and_starts_quietly);
    RUN_TEST(test_fifteen_second_hold_flashes_white_then_factory_resets);
    RUN_TEST(test_unknown_clock_never_sleeps_on_schedule);
    RUN_TEST(test_sleep_command_and_tap_cancels_pending_sleep);
    RUN_TEST(test_lowbatt_sleeps_button_only_and_rechecks_on_wake);
    RUN_TEST(test_draining_battery_warns_once_then_lowbatt);
    RUN_TEST(test_battery_is_never_sampled_while_leds_are_lit);
    RUN_TEST(test_mood_waits_for_a_path_then_resends_backing_off_never_on_a_dead_path);
    RUN_TEST(test_mood_whose_session_dropped_is_resent_on_the_healthy_link);
    RUN_TEST(test_ack_clears_only_its_own_mood);
    RUN_TEST(test_outbox_keeps_the_newest_four_with_distinct_ids);
    RUN_TEST(test_unacked_mood_survives_deep_sleep_and_goes_out_on_the_first_path);
    RUN_TEST(test_duplicate_show_answers_shown_without_replaying);
    RUN_TEST(test_link_backoff_5s_15s_60s_5m_15m_then_30m_and_never_sleeps);
    RUN_TEST(test_link_cached_ap_first_then_scan_at_once_then_mqtt);
    RUN_TEST(test_link_drop_retries_after_1s_then_backs_off);
    RUN_TEST(test_link_mqtt_without_connack_is_a_failed_round);
    RUN_TEST(test_every_connect_reports_state_then_tele_every_tele_min);
    RUN_TEST(test_heart_tapped_offline_goes_out_on_connect_and_again_after_a_reconnect);
    RUN_TEST(test_config_use_wifi_stops_and_restarts_the_link);
    RUN_TEST(test_wire_connect_publishes_state_and_tele_then_a_tap_publishes_the_mood);
    RUN_TEST(test_wire_downlinks_become_events_and_replies);
    RUN_TEST(test_near_held_stops_wifi_and_away_restarts_it_after_the_grace);
    RUN_TEST(test_pending_uplink_skips_the_grace);
    RUN_TEST(test_phone_upstream_down_starts_wifi_after_the_grace_keeping_ble);
    RUN_TEST(test_a_wifi_change_is_tried_at_once_even_near);
    RUN_TEST(test_silent_phone_in_doze_hands_back_to_wifi);
    RUN_TEST(test_unacked_ble_mood_brings_back_the_wifi_near_stopped);
    RUN_TEST(test_wire_ble_answers_on_ble_and_the_phone_drives_the_path);
    RUN_TEST(test_wire_ble_requests_are_checked_before_the_port_sees_them);
    RUN_TEST(test_custom_and_together_gestures_send_and_glow);
    RUN_TEST(test_new_show_kinds_play_and_together_is_in_step);
    RUN_TEST(test_streak_ember_and_partner_presence_light_idle_pixels);
    RUN_TEST(test_good_morning_replays_last_evening_heart_once_softly);
    return UNITY_END();
}
