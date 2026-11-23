#include "hb_policy.h"

static bool day_on(const hb_cfg_t *cfg, uint32_t wday)
{
    return (cfg->days >> wday) & 1;
}

// Local weekday (0 = Monday) and second of the day.
static void local_time(const hb_cfg_t *cfg, uint32_t epoch, uint32_t *wday, uint32_t *sec)
{
    int64_t t = (int64_t)epoch + cfg->tz_min * 60;
    if (t < 0) {
        t = 0;
    }
    *wday = (uint32_t)((t / 86400 + 3) % 7);  // 1970-01-01 was a Thursday
    *sec = (uint32_t)(t % 86400);
}

bool hb_sched_in_hours(const hb_cfg_t *cfg, uint32_t epoch)
{
    if (!cfg->sched_on || epoch == 0) {
        return true;
    }
    uint32_t wday, sec;
    local_time(cfg, epoch, &wday, &sec);
    uint32_t m = sec / 60;
    bool today = day_on(cfg, wday), yesterday = day_on(cfg, (wday + 6) % 7);
    if (cfg->start_min < cfg->stop_min) {
        return today && m >= cfg->start_min && m < cfg->stop_min;
    }
    return (today && m >= cfg->start_min) || (yesterday && m < cfg->stop_min);
}

uint32_t hb_sched_secs_to_start(const hb_cfg_t *cfg, uint32_t epoch)
{
    uint32_t wday, sec;
    local_time(cfg, epoch, &wday, &sec);
    for (uint32_t ahead = 0; ahead <= 7; ahead++) {
        if (!day_on(cfg, (wday + ahead) % 7)) {
            continue;
        }
        int64_t d = (int64_t)ahead * 86400 + cfg->start_min * 60 - sec;
        if (d > 0) {
            return (uint32_t)d;
        }
    }
    return 0;
}

void hb_policy_sleep(hb_core_t *c, hb_state_t s, uint32_t secs)
{
    c->state = s;
    c->sleep_pending = true;
    c->sleep_s = secs;
}

void hb_policy_eval(hb_core_t *c, uint32_t now_ms)
{
    if (c->sleep_pending) {
        return;
    }
    if (c->batt.off) {
        hb_policy_sleep(c, HB_ST_LOWBATT, 0);
        hb_fx_start(&c->fx, HB_FX_LOW, 0, now_ms);
        return;
    }
    hb_state_t s = !hb_due(now_ms, c->pair_until) ? HB_ST_PAIRING
                 : c->path == HB_PATH_BLE      ? HB_ST_NEAR
                 : c->path == HB_PATH_WIFI     ? HB_ST_AWAY
                                               : HB_ST_OFFLINE;
    uint32_t epoch = hb_core_epoch(c, now_ms);
    if (s != HB_ST_PAIRING && hb_due(now_ms, c->grace_until) && !hb_sched_in_hours(&c->cfg, epoch)) {
        hb_policy_sleep(c, HB_ST_SLEEP, hb_sched_secs_to_start(&c->cfg, epoch));
        return;
    }
    if ((s == HB_ST_AWAY || s == HB_ST_OFFLINE) && c->cfg.idle_nap_min && hb_due(now_ms, c->nap_due)) {
        hb_policy_sleep(c, HB_ST_NAP, c->cfg.nap_wake_min * 60u);
        return;
    }
    c->state = s;
}

const char *hb_state_name(hb_state_t s)
{
    static const char *const names[] = {"pairing", "near", "away", "offline", "nap", "sleep", "lowbatt"};
    return (unsigned)s < sizeof names / sizeof names[0] ? names[s] : "?";
}
