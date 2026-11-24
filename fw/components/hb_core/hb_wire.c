#include "hb_wire.h"

#include <stdio.h>
#include <string.h>
#include "hb_policy.h"

static const char *const k_moods[] = {[HB_MOOD_HEART] = "heart", [HB_MOOD_SORRY] = "sorry", [HB_MOOD_ANNOY] = "annoy"};

void hb_wire_topic(char out[HB_TOPIC_MAX], const char *did, const char *leaf)
{
    snprintf(out, HB_TOPIC_MAX, "hb/v1/d/%s/%s", did, leaf);
}

static cJSON *new_msg(hb_core_t *c, const char *t, uint64_t id, uint32_t now_ms)
{
    char ids[HB_ID_LEN + 1];
    hb_id_format(id, ids);
    return hb_msg_new(t, ids, hb_core_epoch(c, now_ms));
}

// Prints and hands over one message (and frees it); a message that does not print is not sent.
static void send(hb_pub_fn pub, void *arg, hb_path_t path, const char *topic, cJSON *m, bool retain, bool mood,
                 uint64_t id)
{
    char buf[HB_MSG_MAX_BEACON + 1];
    int n = m ? hb_msg_print(m, buf, sizeof buf) : -1;
    cJSON_Delete(m);
    if (n > 0) {
        hb_pub_t p = {.path = path, .topic = topic, .json = buf, .len = n, .retain = retain, .mood = mood, .id = id};
        pub(arg, &p);
    }
}

int hb_wire_will(hb_core_t *c, uint32_t now_ms, char *buf, size_t cap)
{
    cJSON *m = new_msg(c, "state", hb_core_new_id(c), now_ms);
    int n = m && cJSON_AddStringToObject(m, "st", "lost") ? hb_msg_print(m, buf, cap) : -1;
    cJSON_Delete(m);
    return n;
}

void hb_wire_emit(hb_core_t *c, const hb_actions_t *a, const hb_wire_ctx_t *x, uint32_t now_ms, hb_pub_fn pub,
                  void *arg)
{
    char up[HB_TOPIC_MAX], state[HB_TOPIC_MAX];
    hb_wire_topic(up, x->did, "up");
    hb_wire_topic(state, x->did, "state");
    hb_path_t path = c->path;
    if (path != HB_PATH_NONE && a->state_changed) {
        cJSON *m = new_msg(c, "state", hb_core_new_id(c), now_ms);
        cJSON_AddStringToObject(m, "st", hb_state_name(a->state));
        cJSON_AddStringToObject(m, "path", path == HB_PATH_BLE ? "ble" : "wifi");
        uint32_t epoch = hb_core_epoch(c, now_ms);
        if (a->state >= HB_ST_NAP && c->sleep_s && epoch) {  // deep sleep with a timer: when it is back
            cJSON_AddNumberToObject(m, "wake", (double)epoch + c->sleep_s);
        }
        send(pub, arg, path, state, m, true, false, 0);
    }
    if (path != HB_PATH_NONE && a->tele) {
        cJSON *m = new_msg(c, "tele", hb_core_new_id(c), now_ms);
        uint32_t mv = hb_batt_mv(&c->batt);
        cJSON_AddNumberToObject(m, "mv", mv);
        cJSON_AddNumberToObject(m, "pct", hb_batt_pct(mv, &c->cfg));
        cJSON_AddNumberToObject(m, "rssi", x->rssi);
        cJSON_AddStringToObject(m, "fw", x->fw);
        cJSON_AddStringToObject(m, "hw", x->hw);
        cJSON_AddNumberToObject(m, "cfg", c->cfg.n);
        cJSON_AddNumberToObject(m, "up_s", now_ms / 1000);
        cJSON_AddStringToObject(m, "rst", x->rst);
        cJSON_AddNumberToObject(m, "heap", x->heap);
        send(pub, arg, path, up, m, false, false, 0);
    }
    for (int i = 0; i < a->n_up; i++) {
        const hb_up_t *u = &a->up[i];
        cJSON *m = new_msg(c, u->k ? "mood" : "shown", u->id, now_ms);
        if (u->k) {
            cJSON_AddStringToObject(m, "k", k_moods[u->k]);
        }
        send(pub, arg, u->path, up, m, false, u->k != HB_MOOD_NONE, u->id);
    }
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

static void reply(const hb_wire_ctx_t *x, cJSON *m, hb_pub_fn pub, void *arg)
{
    char up[HB_TOPIC_MAX];
    hb_wire_topic(up, x->did, "up");
    send(pub, arg, HB_PATH_WIFI, up, m, false, false, 0);  // it came over MQTT: answer there
}

void hb_wire_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *topic, const char *data, size_t len,
                uint32_t now_ms, hb_rx_t *rx, hb_pub_fn pub, void *arg)
{
    char down[HB_TOPIC_MAX], cfg[HB_TOPIC_MAX];
    hb_wire_topic(down, x->did, "down");
    hb_wire_topic(cfg, x->did, "cfg");
    memset(&rx->ev, 0, sizeof rx->ev);
    rx->has_ev = false;
    rx->cfg_err = NULL;
    rx->raw_len = 0;
    bool is_cfg = strcmp(topic, cfg) == 0;
    if (!is_cfg && strcmp(topic, down) != 0) {
        return;
    }
    hb_msg_t m;
    if (hb_msg_decode(data, len, &m) != HB_MSG_OK) {
        rx->cfg_err = is_cfg ? "envelope" : NULL;
        return;
    }
    hb_ev_t *ev = &rx->ev;
    const char *t = m.t;
    if (is_cfg) {
        rx->cfg_err = strcmp(t, "cfg") != 0 ? "t"
                    : hb_config_accept(m.root, x->did, c->cfg.n, x->cfg_key, x->cfg_key_len, &rx->cfg, rx->raw,
                                       &rx->raw_len);
        if (!rx->cfg_err) {
            ev->type = HB_EV_CFG;
            ev->cfg = &rx->cfg;
            rx->has_ev = true;
        }
    } else if (!hb_msg_known_type(t)) {
        char id[HB_ID_LEN + 1];
        hb_id_format(hb_core_new_id(c), id);
        reply(x, hb_msg_resp_error(m.id, "unknown_t", id, hb_core_epoch(c, now_ms)), pub, arg);
    } else if (strcmp(t, "show") == 0) {
        const char *k = str(m.root, "k");
        for (int i = HB_MOOD_HEART; k && i <= HB_MOOD_ANNOY; i++) {
            if (strcmp(k, k_moods[i]) == 0) {
                ev->type = HB_EV_SHOW;
                ev->k = (hb_mood_t)i;
                rx->has_ev = hb_id_parse(m.id, &ev->id);
            }
        }
    } else if (strcmp(t, "time") == 0) {
        if (m.ts) {  // the envelope ts is the time; tz is informational (PROTOCOL.md 3)
            ev->type = HB_EV_TIME;
            ev->ts = m.ts;
            rx->has_ev = true;
        }
    } else if (strcmp(t, "sleep") == 0) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(m.root, "for_s");
        if (cJSON_IsNumber(s) && s->valuedouble >= 0 && s->valuedouble <= 4294967295.0 &&
            (double)(uint32_t)s->valuedouble == s->valuedouble) {
            ev->type = HB_EV_SLEEP;
            ev->for_s = (uint32_t)s->valuedouble;
            rx->has_ev = true;
        }
    } else if (strcmp(t, "get") == 0) {
        const char *what = str(m.root, "what");
        if (what && strcmp(what, "info") == 0) {
            cJSON *r = new_msg(c, "resp", hb_core_new_id(c), now_ms);
            cJSON_AddStringToObject(r, "re", m.id);
            cJSON_AddTrueToObject(r, "ok");
            cJSON *d = cJSON_AddObjectToObject(r, "d");
            cJSON_AddStringToObject(d, "did", x->did);
            cJSON_AddStringToObject(d, "fw", x->fw);
            cJSON_AddStringToObject(d, "hw", x->hw);
            cJSON_AddNumberToObject(d, "pv", 1);
            cJSON_AddNumberToObject(d, "min_pv", 1);
            cJSON_AddBoolToObject(d, "claimed", c->claimed);
            reply(x, r, pub, arg);
        }
    }
    hb_msg_free(&m);
}
