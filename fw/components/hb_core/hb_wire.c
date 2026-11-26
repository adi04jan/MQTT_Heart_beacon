#include "hb_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hb_policy.h"

static const char *const k_moods[HB_MOODS] = {
    [HB_MOOD_HEART] = "heart",   [HB_MOOD_SORRY] = "sorry",       [HB_MOOD_ANNOY] = "annoy",
    [HB_MOOD_CUSTOM] = "custom", [HB_MOOD_TOGETHER] = "together", [HB_MOOD_ECHO] = "echo",
    [HB_MOOD_CELEBRATE] = "celebrate",
};

static void rgb_hex(const uint8_t c[3], char out[8])
{
    snprintf(out, 8, "#%02x%02x%02x", c[0], c[1], c[2]);
}

// "#rrggbb", lowercase hex (PROTOCOL.md 7 mood.rgb); false leaves out untouched.
static bool rgb_parse(const char *s, uint8_t out[3])
{
    if (!s || strlen(s) != 7 || s[0] != '#' || strspn(s + 1, "0123456789abcdef") != 6) {
        return false;
    }
    for (int k = 0; k < 3; k++) {
        char h[3] = {s[1 + 2 * k], s[2 + 2 * k], 0};
        out[k] = (uint8_t)strtoul(h, NULL, 16);
    }
    return true;
}

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
        if (x->dump) {
            cJSON_AddTrueToObject(m, "dump");
        }
        send(pub, arg, path, up, m, false, false, 0);
    }
    for (int i = 0; i < a->n_up; i++) {
        const hb_up_t *u = &a->up[i];
        cJSON *m = new_msg(c, u->k ? "mood" : "shown", u->id, now_ms);
        if (u->k) {
            cJSON_AddStringToObject(m, "k", k_moods[u->k]);
        }
        if (u->k == HB_MOOD_CUSTOM) {  // our own colour and pattern, from the config
            char rgb[8];
            rgb_hex(c->cfg.mood_rgb, rgb);
            cJSON_AddStringToObject(m, "rgb", rgb);
            cJSON_AddNumberToObject(m, "pat", c->cfg.mood_pat);
        }
        send(pub, arg, u->path, up, m, false, u->k != HB_MOOD_NONE, u->id);
    }
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

// A string of min..max bytes into out (max + 1 bytes of room).
static bool field(const cJSON *o, const char *k, size_t min, size_t max, char *out)
{
    const char *v = str(o, k);
    size_t n = v ? strlen(v) : 0;
    if (!v || n < min || n > max) {
        return false;
    }
    memcpy(out, v, n + 1);
    return true;
}

static bool port(const cJSON *o, const char *k, uint16_t *out)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsNumber(j) || j->valuedouble < 1 || j->valuedouble > 65535 ||
        (double)(int)j->valuedouble != j->valuedouble) {
        return false;
    }
    *out = (uint16_t)j->valuedouble;
    return true;
}

void hb_wire_resp(hb_core_t *c, const hb_wire_ctx_t *x, const char *re, bool ok, const char *err, cJSON *d,
                  uint32_t now_ms, hb_path_t path, hb_pub_fn pub, void *arg)
{
    cJSON *r = new_msg(c, "resp", hb_core_new_id(c), now_ms);
    if (!r) {
        cJSON_Delete(d);
        return;
    }
    cJSON_AddStringToObject(r, "re", re);
    cJSON_AddBoolToObject(r, "ok", ok);
    if (!ok && err) {
        cJSON_AddStringToObject(r, "err", err);
    }
    if (d) {
        cJSON_AddItemToObject(r, "d", d);
    }
    char up[HB_TOPIC_MAX];
    hb_wire_topic(up, x->did, "up");
    send(pub, arg, path, up, r, false, false, 0);
}

// The BLE-only requests: checked here, carried out by the port (rx->cmd), which answers.
static void ble_request(hb_core_t *c, const hb_wire_ctx_t *x, const hb_msg_t *m, uint32_t now_ms, hb_rx_t *rx,
                        hb_pub_fn pub, void *arg)
{
    hb_cmd_t *k = &rx->cmd;
    const char *t = m->t, *err = NULL;
    memcpy(k->re, m->id, sizeof k->re);
    if (strcmp(t, "claim") == 0) {
        // A bonded phone may claim again (the review's R3: a Forget whose factory reset never landed left the
        // beacon refusing the new credentials). The bond is the proof; it already allows a factory reset.
        err = !field(m->root, "host", 1, 64, k->host) ? "bad_host"
            : !port(m->root, "mport", &k->mport)      ? "bad_mport"
            : !port(m->root, "aport", &k->aport)      ? "bad_aport"
            : !field(m->root, "user", 1, 32, k->user) ? "bad_user"
            : !field(m->root, "pass", 1, 64, k->pass) ? "bad_pass"
                                                      : NULL;
        k->type = HB_CMD_CLAIM;
    } else if (strcmp(t, "wifi.scan") == 0) {
        k->type = HB_CMD_WIFI_SCAN;
    } else if (strcmp(t, "wifi.list") == 0) {
        k->type = HB_CMD_WIFI_LIST;
    } else if (strcmp(t, "wifi.set") == 0) {
        // a WPA2 passphrase is 8..63 bytes; none for an open network
        bool psk = field(m->root, "psk", 0, 63, k->psk) && (!k->psk[0] || strlen(k->psk) >= 8);
        err = !field(m->root, "ssid", 1, 32, k->ssid) ? "bad_ssid" : !psk ? "bad_psk" : NULL;
        k->type = HB_CMD_WIFI_SET;
    } else if (strcmp(t, "wifi.forget") == 0) {
        err = field(m->root, "ssid", 1, 32, k->ssid) ? NULL : "bad_ssid";
        k->type = HB_CMD_WIFI_FORGET;
    } else if (strcmp(t, "reset") == 0) {
        const char *what = str(m->root, "what");
        k->factory = what && strcmp(what, "factory") == 0;
        err = k->factory || (what && strcmp(what, "wifi") == 0) ? NULL : "bad_what";
        k->type = HB_CMD_RESET;
    } else {
        // wifi.use. ponytail: net.use_wifi lives in the signed config; add this if the app ever sends it
        err = "unsupported";
    }
    if (err) {
        k->type = HB_CMD_NONE;
        hb_wire_resp(c, x, m->id, false, err, NULL, now_ms, HB_PATH_BLE, pub, arg);
    }
}

// Both transports. Answers go back on the path the message came in on.
static void dispatch(hb_core_t *c, const hb_wire_ctx_t *x, const hb_msg_t *m, uint32_t now_ms, hb_rx_t *rx,
                     hb_path_t path, hb_pub_fn pub, void *arg)
{
    hb_ev_t *ev = &rx->ev;
    const char *t = m->t;
    if (!hb_msg_known_type(t)) {
        char id[HB_ID_LEN + 1], up[HB_TOPIC_MAX];
        hb_id_format(hb_core_new_id(c), id);
        hb_wire_topic(up, x->did, "up");
        send(pub, arg, path, up, hb_msg_resp_error(m->id, "unknown_t", id, hb_core_epoch(c, now_ms)), false, false,
             0);
    } else if (strcmp(t, "show") == 0) {
        const char *k = str(m->root, "k");
        for (int i = HB_MOOD_HEART; k && i < HB_MOODS; i++) {
            if (strcmp(k, k_moods[i]) == 0) {
                ev->type = HB_EV_SHOW;
                ev->k = (hb_mood_t)i;
                rx->has_ev = hb_id_parse(m->id, &ev->id);
            }
        }
        if (rx->has_ev && ev->k == HB_MOOD_CUSTOM) {
            const cJSON *pat = cJSON_GetObjectItemCaseSensitive(m->root, "pat");
            if (!rgb_parse(str(m->root, "rgb"), ev->rgb)) {
                memset(ev->rgb, 255, 3);  // a custom show without a colour plays white
            }
            ev->pat = cJSON_IsNumber(pat) && pat->valuedouble == 1 ? 1 : 0;
        }
        if (rx->has_ev && ev->k == HB_MOOD_TOGETHER) {
            const cJSON *at = cJSON_GetObjectItemCaseSensitive(m->root, "at");
            ev->at = cJSON_IsNumber(at) && at->valuedouble > 0 && at->valuedouble <= 4294967295.0 ? (uint32_t)at->valuedouble : 0;
        }
    } else if (strcmp(t, "streak") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(m->root, "n");
        if (cJSON_IsNumber(n) && n->valuedouble >= 0) {
            ev->type = HB_EV_STREAK;
            ev->mv = n->valuedouble > 9999 ? 9999u : (uint32_t)n->valuedouble;
            rx->has_ev = true;
        }
    } else if (strcmp(t, "presence") == 0) {
        const cJSON *near = cJSON_GetObjectItemCaseSensitive(m->root, "near");
        if (cJSON_IsBool(near)) {
            ev->type = HB_EV_PRESENCE;
            ev->up = cJSON_IsTrue(near);
            rx->has_ev = true;
        }
    } else if (strcmp(t, "time") == 0) {
        if (m->ts) {  // the envelope ts is the time; tz is informational (PROTOCOL.md 3)
            ev->type = HB_EV_TIME;
            ev->ts = m->ts;
            rx->has_ev = true;
        }
    } else if (strcmp(t, "sleep") == 0) {
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(m->root, "for_s");
        if (cJSON_IsNumber(s) && s->valuedouble >= 0 && s->valuedouble <= 4294967295.0 &&
            (double)(uint32_t)s->valuedouble == s->valuedouble) {
            ev->type = HB_EV_SLEEP;
            ev->for_s = (uint32_t)s->valuedouble;
            rx->has_ev = true;
        }
    } else if (strcmp(t, "get") == 0) {
        const char *what = str(m->root, "what");
        if (what && strcmp(what, "info") == 0) {
            cJSON *d = cJSON_CreateObject();
            cJSON_AddStringToObject(d, "did", x->did);
            cJSON_AddStringToObject(d, "fw", x->fw);
            cJSON_AddStringToObject(d, "hw", x->hw);
            cJSON_AddNumberToObject(d, "pv", 1);
            cJSON_AddNumberToObject(d, "min_pv", 1);
            cJSON_AddBoolToObject(d, "claimed", c->claimed);
            hb_wire_resp(c, x, m->id, true, NULL, d, now_ms, path, pub, arg);
        }
    } else if (path != HB_PATH_BLE) {
        // claim, wifi.*, reset, link and the phone's acks exist only on the bonded BLE link
    } else if (strcmp(t, "cfg") == 0) {  // the phone's copy of a signed config: answered, unlike the retained one
        rx->cfg_err = hb_config_accept(m->root, x->did, c->cfg.n, x->cfg_key, x->cfg_key_len, &rx->cfg, rx->raw,
                                       &rx->raw_len);
        if (!rx->cfg_err) {
            ev->type = HB_EV_CFG;
            ev->cfg = &rx->cfg;
            rx->has_ev = true;
        }
        hb_wire_resp(c, x, m->id, !rx->cfg_err, rx->cfg_err, NULL, now_ms, path, pub, arg);
    } else if (strcmp(t, "link") == 0) {
        const cJSON *up = cJSON_GetObjectItemCaseSensitive(m->root, "up");
        if (cJSON_IsBool(up)) {
            ev->type = HB_EV_PHONE;
            ev->up = cJSON_IsTrue(up);
            rx->has_ev = true;
        }
    } else if (strcmp(t, "resp") == 0) {
        const char *re = str(m->root, "re");
        if (re && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m->root, "ok"))) {
            ev->type = HB_EV_ACK;  // the phone's relay of that uplink was PUBACKed (PROTOCOL.md 4)
            rx->has_ev = hb_id_parse(re, &ev->id);
        }
    } else if (strcmp(t, "claim") == 0 || strncmp(t, "wifi.", 5) == 0 || strcmp(t, "reset") == 0) {
        ble_request(c, x, m, now_ms, rx, pub, arg);
    }
}

static void rx_reset(hb_rx_t *rx)
{
    memset(&rx->ev, 0, sizeof rx->ev);
    rx->has_ev = false;
    rx->cfg_err = NULL;
    rx->raw_len = 0;
    rx->cmd.type = HB_CMD_NONE;
}

void hb_wire_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *topic, const char *data, size_t len,
                uint32_t now_ms, hb_rx_t *rx, hb_pub_fn pub, void *arg)
{
    char down[HB_TOPIC_MAX], cfg[HB_TOPIC_MAX];
    hb_wire_topic(down, x->did, "down");
    hb_wire_topic(cfg, x->did, "cfg");
    rx_reset(rx);
    bool is_cfg = strcmp(topic, cfg) == 0;
    if (!is_cfg && strcmp(topic, down) != 0) {
        return;
    }
    hb_msg_t m;
    if (hb_msg_decode(data, len, &m) != HB_MSG_OK) {
        rx->cfg_err = is_cfg ? "envelope" : NULL;
        return;
    }
    if (is_cfg) {  // the retained config: never answered
        rx->cfg_err = strcmp(m.t, "cfg") != 0
                        ? "t"
                        : hb_config_accept(m.root, x->did, c->cfg.n, x->cfg_key, x->cfg_key_len, &rx->cfg, rx->raw,
                                           &rx->raw_len);
        if (!rx->cfg_err) {
            rx->ev.type = HB_EV_CFG;
            rx->ev.cfg = &rx->cfg;
            rx->has_ev = true;
        }
    } else {
        dispatch(c, x, &m, now_ms, rx, HB_PATH_WIFI, pub, arg);
    }
    hb_msg_free(&m);
}

void hb_wire_ble_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *data, size_t len, uint32_t now_ms,
                    hb_rx_t *rx, hb_pub_fn pub, void *arg)
{
    rx_reset(rx);
    hb_msg_t m;
    if (hb_msg_decode(data, len, &m) != HB_MSG_OK) {
        return;
    }
    dispatch(c, x, &m, now_ms, rx, HB_PATH_BLE, pub, arg);
    hb_msg_free(&m);
}
