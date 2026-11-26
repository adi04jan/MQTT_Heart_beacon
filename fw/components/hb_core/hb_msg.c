#include "hb_msg.h"

#include <stdio.h>
#include <string.h>

static const char *const k_types[] = {
    "mood", "show", "shown", "rcpt", "tele", "state", "cfg", "time", "sleep", "get", "resp",
    "claim", "wifi.scan", "wifi.list", "wifi.set", "wifi.forget", "wifi.use", "link", "relay", "reset",
    "streak", "presence", "batt",
};

bool hb_id_parse(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    size_t i = 0;
    for (; s[i] && i < HB_ID_LEN; i++) {
        char ch = s[i];
        if (ch >= '0' && ch <= '9') {
            v = v << 4 | (uint64_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            v = v << 4 | (uint64_t)(ch - 'a' + 10);
        } else {
            return false;
        }
    }
    if (i != HB_ID_LEN || s[i]) {
        return false;
    }
    *out = v;
    return true;
}

void hb_id_format(uint64_t id, char out[HB_ID_LEN + 1])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = HB_ID_LEN - 1; i >= 0; i--, id >>= 4) {
        out[i] = hex[id & 0xf];
    }
    out[HB_ID_LEN] = 0;
}

static bool is_id(const cJSON *j)
{
    uint64_t ignored;
    return cJSON_IsString(j) && hb_id_parse(j->valuestring, &ignored);
}

hb_msg_err_t hb_msg_decode(const char *buf, size_t len, hb_msg_t *out)
{
    memset(out, 0, sizeof *out);
    if (len > HB_MSG_MAX) {
        return HB_MSG_TOO_BIG;
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(buf, len, &end, false);
    hb_msg_err_t err = HB_MSG_OK;
    if (!cJSON_IsObject(root)) {
        err = HB_MSG_BAD_JSON;
        goto done;
    }
    for (; end < buf + len; end++) {  // only JSON whitespace may follow the object
        if (*end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
            err = HB_MSG_BAD_JSON;
            goto done;
        }
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "v");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (!cJSON_IsNumber(v) || v->valuedouble != 1.0) {
        err = HB_MSG_BAD_V;
    } else if (!cJSON_IsString(t) || t->valuestring[0] == '\0') {
        err = HB_MSG_BAD_T;
    } else if (!is_id(id)) {
        err = HB_MSG_BAD_ID;
    } else if (!cJSON_IsNumber(ts) || !(ts->valuedouble >= 0.0 && ts->valuedouble <= 4294967295.0) ||
               (double)(uint32_t)ts->valuedouble != ts->valuedouble) {
        err = HB_MSG_BAD_TS;
    } else {
        out->root = root;
        out->t = t->valuestring;
        out->id = id->valuestring;
        out->ts = (uint32_t)ts->valuedouble;
        return HB_MSG_OK;
    }
done:
    cJSON_Delete(root);
    return err;
}

void hb_msg_free(hb_msg_t *m)
{
    cJSON_Delete(m->root);
    memset(m, 0, sizeof *m);
}

const char *hb_msg_err_str(hb_msg_err_t e)
{
    static const char *const names[] = {"ok", "too_big", "bad_json", "bad_v", "bad_t", "bad_id", "bad_ts"};
    return (unsigned)e < sizeof names / sizeof names[0] ? names[e] : "?";
}

bool hb_msg_known_type(const char *t)
{
    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        if (strcmp(t, k_types[i]) == 0) {
            return true;
        }
    }
    return false;
}

cJSON *hb_msg_new(const char *t, const char *id, uint32_t ts)
{
    cJSON *m = cJSON_CreateObject();
    if (m && cJSON_AddNumberToObject(m, "v", 1) && cJSON_AddStringToObject(m, "t", t) &&
        cJSON_AddStringToObject(m, "id", id) && cJSON_AddNumberToObject(m, "ts", ts)) {
        return m;
    }
    cJSON_Delete(m);
    return NULL;
}

cJSON *hb_msg_resp_error(const char *re, const char *err, const char *id, uint32_t ts)
{
    cJSON *m = hb_msg_new("resp", id, ts);
    if (m && cJSON_AddStringToObject(m, "re", re) && cJSON_AddFalseToObject(m, "ok") &&
        cJSON_AddStringToObject(m, "err", err)) {
        return m;
    }
    cJSON_Delete(m);
    return NULL;
}

int hb_msg_print(const cJSON *msg, char *buf, size_t cap)
{
    if (cap == 0 || cap > 0x7fffffff || !cJSON_PrintPreallocated((cJSON *)msg, buf, (int)cap, false)) {
        return -1;
    }
    size_t n = strlen(buf);
    return n <= HB_MSG_MAX_BEACON ? (int)n : -1;
}

void hb_did_from_mac(const uint8_t mac[6], char out[HB_DID_LEN + 1])
{
    snprintf(out, HB_DID_LEN + 1, "HB-%02X%02X%02X", mac[3], mac[4], mac[5]);
}
