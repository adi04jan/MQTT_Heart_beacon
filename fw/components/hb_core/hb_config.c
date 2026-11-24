#include "hb_config.h"

#include <string.h>
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#if defined(MBEDTLS_USE_PSA_CRYPTO)
#include "psa/crypto.h"
#endif

static const cJSON *get(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);  // NULL for a NULL (absent) block
}

// A JSON integer (not a bool, no fraction) in [lo, hi].
static bool is_int(const cJSON *j, double lo, double hi)
{
    return cJSON_IsNumber(j) && j->valuedouble >= lo && j->valuedouble <= hi &&
           (double)(int64_t)j->valuedouble == j->valuedouble;
}

// Absent: keep the default. Present: must be an int in [lo, hi], else fail with path.
#define OPT_INT(blk, key, path, lo, hi, dst)                     \
    do {                                                          \
        const cJSON *j_ = get(blk, key);                          \
        if (j_) {                                                 \
            if (!is_int(j_, lo, hi)) {                            \
                return path;                                      \
            }                                                     \
            (dst) = (int32_t)j_->valuedouble;                     \
        }                                                         \
    } while (0)

#define OPT_BOOL(blk, key, path, dst)                             \
    do {                                                          \
        const cJSON *j_ = get(blk, key);                          \
        if (j_) {                                                 \
            if (!cJSON_IsBool(j_)) {                              \
                return path;                                      \
            }                                                     \
            (dst) = cJSON_IsTrue(j_);                             \
        }                                                         \
    } while (0)

// String of lo..hi UTF-8 bytes into dst (hi + 1 bytes); absent keeps the default.
#define OPT_STR(blk, key, path, lo, hi, dst)                      \
    do {                                                          \
        const cJSON *j_ = get(blk, key);                          \
        if (j_ && !str_in(j_, lo, hi, dst)) {                     \
            return path;                                          \
        }                                                         \
    } while (0)

// A present block must be an object; returns it (NULL when absent) or fails with its name.
#define BLOCK(var, name)                                          \
    const cJSON *var = get(root, name);                           \
    if (var && !cJSON_IsObject(var)) {                            \
        return name;                                              \
    }

static bool str_in(const cJSON *j, size_t lo, size_t hi, char *dst)
{
    if (!cJSON_IsString(j)) {
        return false;
    }
    size_t n = strlen(j->valuestring);
    if (n < lo || n > hi) {
        return false;
    }
    memcpy(dst, j->valuestring, n + 1);
    return true;
}

static bool hex_in(const char *s, size_t n, bool upper)
{
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (upper ? (c >= 'A' && c <= 'F') : (c >= 'a' && c <= 'f')))) {
            return false;
        }
    }
    return true;
}

// "HH:MM", 00:00..23:59 -> minutes.
static bool hhmm(const cJSON *j, uint16_t *min)
{
    const char *s = cJSON_IsString(j) ? j->valuestring : "";
    if (strlen(s) != 5 || s[2] != ':') {
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (i != 2 && (s[i] < '0' || s[i] > '9')) {
            return false;
        }
    }
    int h = (s[0] - '0') * 10 + (s[1] - '0'), m = (s[3] - '0') * 10 + (s[4] - '0');
    if (h > 23 || m > 59) {
        return false;
    }
    *min = (uint16_t)(h * 60 + m);
    return true;
}

static const char *parse(const cJSON *root, hb_cfg_t *c)
{
    const cJSON *j = get(root, "cv");
    if (!is_int(j, 1, 1)) {
        return "cv";
    }
    j = get(root, "n");
    if (!is_int(j, 1, 2147483647)) {
        return "n";
    }
    c->n = (uint32_t)j->valuedouble;
    j = get(root, "did");
    if (!cJSON_IsString(j) || strlen(j->valuestring) != 9 || strncmp(j->valuestring, "HB-", 3) != 0 ||
        !hex_in(j->valuestring + 3, 6, true)) {
        return "did";
    }
    memcpy(c->did, j->valuestring, 10);

    BLOCK(srv, "srv");
    if ((j = get(srv, "hosts"))) {
        int n = cJSON_GetArraySize(j);
        if (!cJSON_IsArray(j) || n > HB_CFG_HOSTS) {
            return "srv.hosts";
        }
        c->n_hosts = 0;
        const cJSON *h;
        cJSON_ArrayForEach(h, j) {
            if (!str_in(h, 1, 64, c->hosts[c->n_hosts++])) {
                return "srv.hosts";
            }
        }
    }
    if ((j = get(srv, "mqtt"))) {
        int n = cJSON_GetArraySize(j);
        if (!cJSON_IsArray(j) || n < 1 || n > HB_CFG_PORTS) {
            return "srv.mqtt";
        }
        c->n_mqtt = 0;
        const cJSON *p;
        cJSON_ArrayForEach(p, j) {
            if (!is_int(p, 1, 65535)) {
                return "srv.mqtt";
            }
            c->mqtt[c->n_mqtt++] = (uint16_t)p->valuedouble;
        }
    }
    OPT_INT(srv, "api", "srv.api", 1, 65535, c->api);
    OPT_STR(root, "name", "name", 0, 32, c->name);

    BLOCK(led, "led");
    OPT_INT(led, "bright", "led.bright", 0, 100, c->bright);

    BLOCK(sched, "sched");
    OPT_BOOL(sched, "on", "sched.on", c->sched_on);
    OPT_INT(sched, "tz", "sched.tz", -720, 840, c->tz_min);
    if ((j = get(sched, "days"))) {
        const char *d = cJSON_IsString(j) ? j->valuestring : "";
        if (strlen(d) != 7 || strspn(d, "01") != 7) {
            return "sched.days";
        }
        c->days = 0;
        for (int i = 0; i < 7; i++) {
            c->days |= (uint8_t)((d[i] == '1') << i);
        }
    }
    if ((j = get(sched, "start")) && !hhmm(j, &c->start_min)) {
        return "sched.start";
    }
    if ((j = get(sched, "stop")) && !hhmm(j, &c->stop_min)) {
        return "sched.stop";
    }

    BLOCK(pwr, "pwr");
    OPT_INT(pwr, "idle_nap_min", "pwr.idle_nap_min", 0, 1440, c->idle_nap_min);
    OPT_INT(pwr, "nap_wake_min", "pwr.nap_wake_min", 1, 1440, c->nap_wake_min);
    OPT_INT(pwr, "batt_low_mv", "pwr.batt_low_mv", 3000, 4200, c->batt_low_mv);
    OPT_INT(pwr, "batt_off_mv", "pwr.batt_off_mv", 3000, 4200, c->batt_off_mv);
    if (c->batt_off_mv >= c->batt_low_mv) {
        return "pwr.batt_off_mv";
    }
    OPT_INT(pwr, "batt_cal_ppm", "pwr.batt_cal_ppm", -200000, 200000, c->batt_cal_ppm);

    BLOCK(ble, "ble");
    OPT_INT(ble, "adv_fast_ms", "ble.adv_fast_ms", 20, 10240, c->adv_fast_ms);
    OPT_INT(ble, "adv_slow_ms", "ble.adv_slow_ms", 20, 10240, c->adv_slow_ms);
    OPT_INT(ble, "adv_fast_s", "ble.adv_fast_s", 0, 600, c->adv_fast_s);
    OPT_INT(ble, "conn_ms", "ble.conn_ms", 8, 4000, c->conn_ms);
    OPT_INT(ble, "latency", "ble.latency", 0, 499, c->latency);
    OPT_INT(ble, "sup_ms", "ble.sup_ms", 100, 32000, c->sup_ms);
    if ((uint32_t)c->sup_ms <= 2u * (1u + c->latency) * c->conn_ms) {
        return "ble.sup_ms";
    }

    BLOCK(net, "net");
    OPT_BOOL(net, "use_wifi", "net.use_wifi", c->use_wifi);
    OPT_INT(net, "near_hold_s", "net.near_hold_s", 0, 3600, c->near_hold_s);
    OPT_INT(net, "away_grace_s", "net.away_grace_s", 0, 3600, c->away_grace_s);
    OPT_INT(net, "tele_min", "net.tele_min", 1, 1440, c->tele_min);

    BLOCK(fw, "fw");
    OPT_STR(fw, "ver", "fw.ver", 0, 16, c->fw_ver);
    OPT_STR(fw, "path", "fw.path", 0, 96, c->fw_path);
    if ((j = get(fw, "sha256"))) {
        const char *s = cJSON_IsString(j) ? j->valuestring : "-";
        if (!(s[0] == 0 || (strlen(s) == 64 && hex_in(s, 64, false)))) {
            return "fw.sha256";
        }
        memcpy(c->fw_sha256, s, strlen(s) + 1);
    }
    OPT_INT(fw, "min_mv", "fw.min_mv", 3000, 4200, c->fw_min_mv);
    return NULL;
}

const char *hb_config_parse(const char *buf, size_t len, hb_cfg_t *out)
{
    if (len > HB_CFG_MAX) {
        return "size";
    }
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(buf, len, &end, false);
    const char *err = cJSON_IsObject(root) ? NULL : "json";
    for (; !err && end < buf + len; end++) {  // only JSON whitespace may follow (as PROTOCOL.md 2.2)
        if (*end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') {
            err = "json";
        }
    }
    if (!err) {
        *out = hb_cfg_default;
        err = parse(root, out);
    }
    cJSON_Delete(root);
    return err;
}

int hb_b64url_decode(const char *s, uint8_t *out, size_t cap)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0, i = 0;
    for (; s[i]; i++) {
        char ch = s[i];
        uint32_t v;
        if (ch >= 'A' && ch <= 'Z') {
            v = (uint32_t)(ch - 'A');
        } else if (ch >= 'a' && ch <= 'z') {
            v = (uint32_t)(ch - 'a' + 26);
        } else if (ch >= '0' && ch <= '9') {
            v = (uint32_t)(ch - '0' + 52);
        } else if (ch == '-' || ch == '_') {
            v = ch == '-' ? 62 : 63;
        } else {
            return -1;  // '=' padding included: the contract says without
        }
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n == cap) {
                return -1;
            }
            out[n++] = (uint8_t)(acc >> bits);
            acc &= (1u << bits) - 1;
        }
    }
    return i % 4 == 1 ? -1 : (int)n;
}

bool hb_config_verify(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t len, const uint8_t *sig,
                      size_t sig_len)
{
#if defined(MBEDTLS_USE_PSA_CRYPTO)
    if (psa_crypto_init() != PSA_SUCCESS) {
        return false;
    }
#endif
    uint8_t hash[32];
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, key, key_len) == 0 && mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) &&
              mbedtls_sha256(msg, len, hash, 0) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, sig, sig_len) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

const char *hb_config_accept(const cJSON *body, const char *did, uint32_t stored_n, const uint8_t *key,
                             size_t key_len, hb_cfg_t *out, uint8_t raw[HB_CFG_MAX], size_t *raw_len)
{
    const cJSON *n = get(body, "n"), *b = get(body, "b"), *s = get(body, "s");
    uint8_t sig[80];  // DER P-256 signatures are 70-72 bytes
    int len = cJSON_IsString(b) ? hb_b64url_decode(b->valuestring, raw, HB_CFG_MAX) : -1;
    int slen = cJSON_IsString(s) ? hb_b64url_decode(s->valuestring, sig, sizeof sig) : -1;
    if (!is_int(n, 1, 2147483647) || len < 0 || slen < 0) {
        return "body";
    }
    if (!hb_config_verify(key, key_len, raw, (size_t)len, sig, (size_t)slen)) {
        return "sig";
    }
    const char *err = hb_config_parse((const char *)raw, (size_t)len, out);
    if (err) {
        return err;
    }
    if (strcmp(out->did, did) != 0) {
        return "did";
    }
    if (out->n <= stored_n || out->n != (uint32_t)n->valuedouble) {
        return "n";
    }
    *raw_len = (size_t)len;
    return NULL;
}
