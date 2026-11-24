#include "hb_link.h"

#include "hb_core.h"

const uint32_t hb_link_backoff_ms[HB_LINK_STEPS] = {5000, 15000, 60000, 300000, 900000, 1800000};

static hb_link_cmd_t attempt(hb_link_t *l, hb_link_st_t st, uint32_t now_ms)
{
    l->st = (uint8_t)st;
    switch (st) {
    case HB_LS_FAST:
        l->due = now_ms + HB_LINK_FAST_MS;
        return HB_LINK_FAST;
    case HB_LS_SCAN:
        l->due = now_ms + HB_LINK_SCAN_MS;
        return HB_LINK_SCAN;
    default:
        l->due = now_ms + HB_LINK_MQTT_MS;
        return HB_LINK_MQTT;
    }
}

static hb_link_cmd_t wait(hb_link_t *l, uint32_t ms, uint32_t now_ms)
{
    l->st = HB_LS_WAIT;
    l->due = now_ms + ms;
    return HB_LINK_DOWN;
}

static hb_link_cmd_t fail(hb_link_t *l, uint32_t now_ms)
{
    uint8_t i = l->fails < HB_LINK_STEPS ? l->fails : HB_LINK_STEPS - 1;
    if (l->fails < UINT8_MAX) {
        l->fails++;
    }
    return wait(l, hb_link_backoff_ms[i], now_ms);
}

hb_link_cmd_t hb_link_ev(hb_link_t *l, hb_link_in_t in, uint32_t now_ms)
{
    bool due = in == HB_LI_POLL && hb_due(now_ms, l->due);
    if (in == HB_LI_STOP) {
        bool radio_on = l->st != HB_LS_OFF && l->st != HB_LS_WAIT;
        l->st = HB_LS_OFF;
        return radio_on ? HB_LINK_DOWN : HB_LINK_NONE;
    }
    if (in == HB_LI_START) {
        if (l->st != HB_LS_OFF) {
            return HB_LINK_NONE;
        }
        l->fails = 0;
        return attempt(l, l->cached ? HB_LS_FAST : HB_LS_SCAN, now_ms);
    }
    switch ((hb_link_st_t)l->st) {
    case HB_LS_FAST:
    case HB_LS_SCAN:
        if (in == HB_LI_WIFI_UP) {
            l->cached = true;
            return attempt(l, HB_LS_MQTT, now_ms);
        }
        if (in == HB_LI_WIFI_DOWN || due) {  // the cached AP failed: scan at once; a scan failed: back off
            return l->st == HB_LS_FAST ? attempt(l, HB_LS_SCAN, now_ms) : fail(l, now_ms);
        }
        break;
    case HB_LS_MQTT:
        if (in == HB_LI_MQTT_UP) {
            l->st = HB_LS_UP;
            l->fails = 0;
        } else if (in == HB_LI_WIFI_DOWN || in == HB_LI_MQTT_DOWN || due) {
            return fail(l, now_ms);
        }
        break;
    case HB_LS_UP:
        if (in == HB_LI_WIFI_DOWN || in == HB_LI_MQTT_DOWN) {
            return wait(l, HB_LINK_QUICK_MS, now_ms);
        }
        break;
    case HB_LS_WAIT:
        if (due) {
            return attempt(l, l->cached ? HB_LS_FAST : HB_LS_SCAN, now_ms);
        }
        break;
    case HB_LS_OFF:
        break;
    }
    return HB_LINK_NONE;
}
