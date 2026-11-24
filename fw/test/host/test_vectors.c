// Golden vectors (protocol/vectors.json) against hb_msg, hb_frame, hb_config (parse, defaults, signature)
// and the schedule; plus the files the firmware pins (fw/main/certs) parse.
#include "unity.h"
#include "hb_msg.h"
#include "hb_frame.h"
#include "hb_fx.h"
#include "hb_cfg.h"
#include "hb_config.h"
#include "hb_policy.h"
#include "hb_wire.h"
#include "mbedtls/pk.h"
#include "mbedtls/oid.h"
#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(HB_VECTORS_PATH) || !defined(HB_CERTS_PATH)
#error "HB_VECTORS_PATH and HB_CERTS_PATH must be defined (fw/test/host/CMakeLists.txt)"
#endif

// Whole file plus a NUL (counted in *len, as mbedTLS wants for PEM); NULL if unreadable.
static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (text && fread(text, 1, (size_t)n, f) == (size_t)n) {
        text[n] = 0;
        *len = (size_t)n + 1;
    } else {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

static cJSON *g_vec;

void setUp(void) {}
void tearDown(void) {}

static cJSON *section(const char *a, const char *b)
{
    cJSON *s = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(g_vec, a), b);
    TEST_ASSERT_NOT_NULL_MESSAGE(s, b);
    return s;
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

static double num(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k)->valuedouble;
}

static size_t unhex(const char *s, uint8_t *out)
{
    size_t n = strlen(s) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        sscanf(s + 2 * i, "%2x", &b);
        out[i] = (uint8_t)b;
    }
    return n;
}

// Decoded root must hold exactly the header keys plus `body`, with equal values.
static void assert_body(const hb_msg_t *m, const cJSON *body, const char *name)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE(4 + cJSON_GetArraySize(body), cJSON_GetArraySize(m->root), name);
    const cJSON *kv;
    cJSON_ArrayForEach(kv, body) {
        const cJSON *got = cJSON_GetObjectItemCaseSensitive(m->root, kv->string);
        TEST_ASSERT_TRUE_MESSAGE(got && cJSON_Compare(got, kv, true), name);
    }
}

static void test_known_types(void)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, section("envelope", "types")) {
        TEST_ASSERT_TRUE_MESSAGE(hb_msg_known_type(t->valuestring), t->valuestring);
    }
    TEST_ASSERT_EQUAL_INT(20, cJSON_GetArraySize(section("envelope", "types")));
    TEST_ASSERT_FALSE(hb_msg_known_type("dance"));
    TEST_ASSERT_FALSE(hb_msg_known_type(""));
}

static void test_encode_and_round_trip(void)
{
    const cJSON *v;
    char buf[HB_MSG_MAX + 1];
    cJSON_ArrayForEach(v, section("envelope", "encode")) {
        const char *name = str(v, "name");
        cJSON *m = hb_msg_new(str(v, "t"), str(v, "id"), (uint32_t)num(v, "ts"));
        const cJSON *body = cJSON_GetObjectItemCaseSensitive(v, "body"), *kv;
        cJSON_ArrayForEach(kv, body) {
            cJSON_AddItemToObject(m, kv->string, cJSON_Duplicate(kv, true));
        }
        int n = hb_msg_print(m, buf, sizeof buf);
        cJSON_Delete(m);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(str(v, "json"), buf, name);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(str(v, "json")), n, name);

        hb_msg_t d;
        TEST_ASSERT_EQUAL_STRING_MESSAGE("ok", hb_msg_err_str(hb_msg_decode(buf, (size_t)n, &d)), name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(str(v, "t"), d.t, name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(str(v, "id"), d.id, name);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)num(v, "ts"), d.ts, name);
        assert_body(&d, body, name);
        hb_msg_free(&d);
    }
}

static void test_decode(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("envelope", "decode")) {
        const char *name = str(v, "name"), *json = str(v, "json"), *err = str(v, "err");
        hb_msg_t d;
        hb_msg_err_t e = hb_msg_decode(json, strlen(json), &d);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(err ? err : "ok", hb_msg_err_str(e), name);
        if (e == HB_MSG_OK) {
            TEST_ASSERT_EQUAL_STRING_MESSAGE(str(v, "t"), d.t, name);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(str(v, "id"), d.id, name);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)num(v, "ts"), d.ts, name);
            assert_body(&d, cJSON_GetObjectItemCaseSensitive(v, "body"), name);
            hb_msg_free(&d);
        }
    }
}

static void test_unknown_t_gets_error_resp(void)
{
    const cJSON *v;
    char buf[HB_MSG_MAX + 1];
    cJSON_ArrayForEach(v, section("envelope", "unknown_t")) {
        const char *name = str(v, "name"), *json = str(v, "json"), *want = str(v, "resp_json");
        hb_msg_t d;
        TEST_ASSERT_EQUAL_INT_MESSAGE(HB_MSG_OK, hb_msg_decode(json, strlen(json), &d), name);
        if (hb_msg_known_type(d.t)) {
            TEST_ASSERT_NULL_MESSAGE(want, name);
        } else {
            cJSON *r = hb_msg_resp_error(d.id, "unknown_t", str(v, "resp_id"), (uint32_t)num(v, "resp_ts"));
            TEST_ASSERT_TRUE_MESSAGE(hb_msg_print(r, buf, sizeof buf) > 0, name);
            cJSON_Delete(r);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(want, buf, name);
        }
        hb_msg_free(&d);
    }
}

static void test_did_from_mac(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("envelope", "did")) {
        uint8_t mac[6];
        char did[HB_DID_LEN + 1];
        unhex(str(v, "mac"), mac);
        hb_did_from_mac(mac, did);
        TEST_ASSERT_EQUAL_STRING(str(v, "did"), did);
    }
}

static void test_state_names(void)
{
    const cJSON *states = section("envelope", "states"), *s;
    TEST_ASSERT_EQUAL_INT(HB_ST_LOWBATT + 1, cJSON_GetArraySize(states));
    int i = 0;
    cJSON_ArrayForEach(s, states) {
        TEST_ASSERT_EQUAL_STRING(s->valuestring, hb_state_name((hb_state_t)i++));
    }
}

// The beacon's encoder: every message it prints is capped at 896 B (envelope.size beacon_ok).
static void test_beacon_size_cap(void)
{
    TEST_ASSERT_EQUAL_INT(HB_MSG_MAX_BEACON, num(cJSON_GetObjectItemCaseSensitive(g_vec, "envelope"), "max_beacon_bytes"));
    const cJSON *v;
    static char buf[2 * HB_MSG_MAX];
    cJSON_ArrayForEach(v, section("envelope", "size")) {
        const char *name = str(v, "name");
        cJSON *m = hb_msg_new(str(v, "t"), str(v, "id"), (uint32_t)num(v, "ts"));
        const cJSON *kv;
        cJSON_ArrayForEach(kv, cJSON_GetObjectItemCaseSensitive(v, "body")) {
            cJSON_AddItemToObject(m, kv->string, cJSON_Duplicate(kv, true));
        }
        int n = hb_msg_print(m, buf, sizeof buf);
        cJSON_Delete(m);
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "beacon_ok"))) {
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)num(v, "bytes"), n, name);
        } else {
            TEST_ASSERT_EQUAL_INT_MESSAGE(-1, n, name);
        }
    }
}

static void test_id_parse_and_format(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("envelope", "encode")) {
        uint64_t id;
        char back[HB_ID_LEN + 1];
        TEST_ASSERT_TRUE(hb_id_parse(str(v, "id"), &id));
        hb_id_format(id, back);
        TEST_ASSERT_EQUAL_STRING(str(v, "id"), back);
    }
    uint64_t id;
    TEST_ASSERT_TRUE(hb_id_parse("ffffffffffffffff", &id) && id == UINT64_MAX);
    TEST_ASSERT_TRUE(hb_id_parse("00000000000000aa", &id) && id == 0xaa);
    TEST_ASSERT_FALSE(hb_id_parse("0123456789ABCDEF", &id));
    TEST_ASSERT_FALSE(hb_id_parse("0123456789abcde", &id));
    TEST_ASSERT_FALSE(hb_id_parse("0123456789abcdef0", &id));
    TEST_ASSERT_FALSE(hb_id_parse("", &id));
}

static void test_print_refuses_small_buffer_and_oversize(void)
{
    char small[8], big[2 * HB_MSG_MAX];
    cJSON *m = hb_msg_new("mood", "0123456789abcdef", 0);
    TEST_ASSERT_EQUAL_INT(-1, hb_msg_print(m, small, sizeof small));
    char pad[HB_MSG_MAX];
    memset(pad, 'x', sizeof pad - 1);
    pad[sizeof pad - 1] = 0;
    cJSON_AddStringToObject(m, "pad", pad);
    TEST_ASSERT_EQUAL_INT(-1, hb_msg_print(m, big, sizeof big));
    cJSON_Delete(m);
}

static void test_split_and_reassemble(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("frame", "split")) {
        const char *name = str(v, "name"), *msg = str(v, "msg");
        const cJSON *frames = cJSON_GetObjectItemCaseSensitive(v, "frames"), *f;
        uint16_t mtu = (uint16_t)num(v, "mtu");
        size_t off = 0, len = strlen(msg);
        uint8_t got[600], want[600];
        static hb_reasm_t r;
        hb_reasm_reset(&r);
        uint32_t now = 0;
        hb_frame_res_t res = HB_FRAME_EMPTY;
        cJSON_ArrayForEach(f, frames) {
            int n = hb_frame_next((const uint8_t *)msg, len, (uint8_t)num(v, "seq"), mtu, &off, got);
            size_t wn = unhex(f->valuestring, want);
            TEST_ASSERT_EQUAL_INT_MESSAGE((int)wn, n, name);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(want, got, wn, name);
            res = hb_reasm_push(&r, got, (size_t)n, now += 10);
        }
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, hb_frame_next((const uint8_t *)msg, len, 0, mtu, &off, got), name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("complete", hb_frame_res_str(res), name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(msg, (const char *)r.buf, name);
    }
}

static void test_split_rejects(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("frame", "split_reject")) {
        const char *msg = str(v, "msg");
        uint8_t out[600];
        size_t off = 0;
        int seq = (int)num(v, "seq");
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, hb_frame_next((const uint8_t *)msg, strlen(msg), (uint8_t)seq,
                                                        (uint16_t)num(v, "mtu"), &off, out), str(v, "name"));
    }
}

static void test_reassemble_steps(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("frame", "reassemble")) {
        const char *name = str(v, "name");
        static hb_reasm_t r;
        hb_reasm_reset(&r);
        const cJSON *s;
        cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(v, "steps")) {
            uint8_t frame[600];
            size_t n = unhex(str(s, "frame"), frame);
            hb_frame_res_t res = hb_reasm_push(&r, frame, n, (uint32_t)num(s, "at"));
            TEST_ASSERT_EQUAL_STRING_MESSAGE(str(s, "out"), hb_frame_res_str(res), name);
            if (res == HB_FRAME_COMPLETE) {
                TEST_ASSERT_EQUAL_STRING_MESSAGE(str(s, "msg"), (const char *)r.buf, name);
                TEST_ASSERT_EQUAL_size_t_MESSAGE(strlen(str(s, "msg")), r.len, name);
            }
        }
    }
}

static int hhmm(const char *s)
{
    return ((s[0] - '0') * 10 + (s[1] - '0')) * 60 + (s[3] - '0') * 10 + (s[4] - '0');
}

// hb_cfg_t in the vectors' JSON shape (test-side, so a parser mistake cannot hide behind its own printer).
static cJSON *cfg_json(const hb_cfg_t *c)
{
    char days[8], hm[2][6];
    for (int i = 0; i < 7; i++) {
        days[i] = (char)('0' + ((c->days >> i) & 1));
    }
    days[7] = 0;
    snprintf(hm[0], sizeof hm[0], "%02u:%02u", (unsigned)(c->start_min / 60) % 100, (unsigned)c->start_min % 60);
    snprintf(hm[1], sizeof hm[1], "%02u:%02u", (unsigned)(c->stop_min / 60) % 100, (unsigned)c->stop_min % 60);
    cJSON *j = cJSON_CreateObject(), *b;
    cJSON_AddNumberToObject(j, "cv", 1);
    cJSON_AddNumberToObject(j, "n", c->n);
    cJSON_AddStringToObject(j, "did", c->did);
    b = cJSON_AddObjectToObject(j, "srv");
    cJSON *hosts = cJSON_AddArrayToObject(b, "hosts"), *ports = cJSON_AddArrayToObject(b, "mqtt");
    for (int i = 0; i < c->n_hosts; i++) {
        cJSON_AddItemToArray(hosts, cJSON_CreateString(c->hosts[i]));
    }
    for (int i = 0; i < c->n_mqtt; i++) {
        cJSON_AddItemToArray(ports, cJSON_CreateNumber(c->mqtt[i]));
    }
    cJSON_AddNumberToObject(b, "api", c->api);
    cJSON_AddStringToObject(j, "name", c->name);
    cJSON_AddNumberToObject(cJSON_AddObjectToObject(j, "led"), "bright", c->bright);
    b = cJSON_AddObjectToObject(j, "sched");
    cJSON_AddBoolToObject(b, "on", c->sched_on);
    cJSON_AddNumberToObject(b, "tz", c->tz_min);
    cJSON_AddStringToObject(b, "days", days);
    cJSON_AddStringToObject(b, "start", hm[0]);
    cJSON_AddStringToObject(b, "stop", hm[1]);
    b = cJSON_AddObjectToObject(j, "pwr");
    cJSON_AddNumberToObject(b, "idle_nap_min", c->idle_nap_min);
    cJSON_AddNumberToObject(b, "nap_wake_min", c->nap_wake_min);
    cJSON_AddNumberToObject(b, "batt_low_mv", c->batt_low_mv);
    cJSON_AddNumberToObject(b, "batt_off_mv", c->batt_off_mv);
    cJSON_AddNumberToObject(b, "batt_cal_ppm", c->batt_cal_ppm);
    b = cJSON_AddObjectToObject(j, "ble");
    cJSON_AddNumberToObject(b, "adv_fast_ms", c->adv_fast_ms);
    cJSON_AddNumberToObject(b, "adv_fast_s", c->adv_fast_s);
    cJSON_AddNumberToObject(b, "adv_slow_ms", c->adv_slow_ms);
    cJSON_AddNumberToObject(b, "conn_ms", c->conn_ms);
    cJSON_AddNumberToObject(b, "latency", c->latency);
    cJSON_AddNumberToObject(b, "sup_ms", c->sup_ms);
    b = cJSON_AddObjectToObject(j, "net");
    cJSON_AddBoolToObject(b, "use_wifi", c->use_wifi);
    cJSON_AddNumberToObject(b, "near_hold_s", c->near_hold_s);
    cJSON_AddNumberToObject(b, "away_grace_s", c->away_grace_s);
    cJSON_AddNumberToObject(b, "tele_min", c->tele_min);
    b = cJSON_AddObjectToObject(j, "fw");
    cJSON_AddStringToObject(b, "ver", c->fw_ver);
    cJSON_AddStringToObject(b, "path", c->fw_path);
    cJSON_AddStringToObject(b, "sha256", c->fw_sha256);
    cJSON_AddNumberToObject(b, "min_mv", c->fw_min_mv);
    return j;
}

static void assert_cfg(const cJSON *want, const hb_cfg_t *got, const char *name)
{
    cJSON *j = cfg_json(got);
    if (!cJSON_Compare(want, j, true)) {
        char *a = cJSON_PrintUnformatted(want), *b = cJSON_PrintUnformatted(j);
        printf("%s\n want %s\n  got %s\n", name, a, b);
        cJSON_free(a);
        cJSON_free(b);
        TEST_FAIL_MESSAGE(name);
    }
    cJSON_Delete(j);
}

static void test_cfg_defaults_match_vectors(void)
{
    cJSON *want = cJSON_Duplicate(section("config", "defaults"), true);
    cJSON_AddNumberToObject(want, "cv", 1);
    cJSON_AddNumberToObject(want, "n", 0);
    cJSON_AddStringToObject(want, "did", "");
    assert_cfg(want, &hb_cfg_default, "defaults");
    cJSON_Delete(want);
    TEST_ASSERT_EQUAL_INT(HB_CFG_MAX, num(cJSON_GetObjectItemCaseSensitive(g_vec, "config"), "max_bytes"));
}

static void test_config_parse(void)
{
    const cJSON *v;
    int n = 0;
    cJSON_ArrayForEach(v, section("config", "parse")) {
        const char *name = str(v, "name"), *json = str(v, "json"), *err = str(v, "err");
        hb_cfg_t c;
        const char *got = hb_config_parse(json, strlen(json), &c);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(err ? err : "(ok)", got ? got : "(ok)", name);
        if (!got) {
            assert_cfg(cJSON_GetObjectItemCaseSensitive(v, "cfg"), &c, name);
        }
        n++;
    }
    TEST_ASSERT_TRUE(n >= 30);
}

static void test_config_signature(void)
{
    const cJSON *sig = section("config", "signature"), *v;
    uint8_t key[128], msg[HB_CFG_MAX], s[80];
    size_t klen = unhex(str(sig, "pub_spki_hex"), key);
    int n = 0;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(sig, "verify")) {
        const char *name = str(v, "name");
        int mlen = hb_b64url_decode(str(v, "b"), msg, sizeof msg), slen = hb_b64url_decode(str(v, "s"), s, sizeof s);
        TEST_ASSERT_TRUE_MESSAGE(mlen > 0 && slen > 0, name);
        TEST_ASSERT_EQUAL_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "ok")),
                                  hb_config_verify(key, klen, msg, (size_t)mlen, s, (size_t)slen), name);
        n++;
    }
    TEST_ASSERT_EQUAL_INT(3, n);
}

static void test_b64url(void)
{
    uint8_t out[8];
    TEST_ASSERT_EQUAL_INT(0, hb_b64url_decode("", out, sizeof out));
    TEST_ASSERT_EQUAL_INT(1, hb_b64url_decode("_w", out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8(0xff, out[0]);
    TEST_ASSERT_EQUAL_INT(3, hb_b64url_decode("-_-_", out, sizeof out));
    TEST_ASSERT_EQUAL_HEX8(0xfb, out[0]);
    TEST_ASSERT_EQUAL_INT(-1, hb_b64url_decode("_w==", out, sizeof out));  // no padding
    TEST_ASSERT_EQUAL_INT(-1, hb_b64url_decode("ab+/", out, sizeof out));  // standard alphabet
    TEST_ASSERT_EQUAL_INT(-1, hb_b64url_decode("abcde", out, sizeof out)); // 1 char past a quad
    TEST_ASSERT_EQUAL_INT(-1, hb_b64url_decode("AAAAAAAAAAAA", out, sizeof out));  // 9 bytes > 8
}

// The CA and config key the firmware embeds (fw/main/certs) parse as what mbedTLS will be handed.
static void test_pinned_files_parse(void)
{
    size_t len;
    char *pem = slurp(HB_CERTS_PATH "/cfg-sign.pub", &len);
    TEST_ASSERT_NOT_NULL(pem);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    TEST_ASSERT_EQUAL_INT(0, mbedtls_pk_parse_public_key(&pk, (const uint8_t *)pem, len));
    TEST_ASSERT_TRUE(mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA));
    TEST_ASSERT_EQUAL_size_t(256, mbedtls_pk_get_bitlen(&pk));
    mbedtls_pk_free(&pk);
    free(pem);

    pem = slurp(HB_CERTS_PATH "/ca.crt", &len);
    TEST_ASSERT_NOT_NULL(pem);
    mbedtls_x509_crt crt;
    mbedtls_x509_crt_init(&crt);
    TEST_ASSERT_EQUAL_INT(0, mbedtls_x509_crt_parse(&crt, (const uint8_t *)pem, len));
    TEST_ASSERT_TRUE(mbedtls_x509_crt_has_ext_type(&crt, MBEDTLS_X509_EXT_BASIC_CONSTRAINTS));
    mbedtls_x509_crt_free(&crt);
    free(pem);
}

static void test_schedule(void)
{
    const cJSON *v;
    cJSON_ArrayForEach(v, section("config", "sched")) {
        const char *name = str(v, "name");
        const cJSON *s = cJSON_GetObjectItemCaseSensitive(v, "sched");
        hb_cfg_t c = hb_cfg_default;
        c.sched_on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "on"));
        c.tz_min = (int16_t)num(s, "tz");
        c.days = 0;
        for (int i = 0; i < 7; i++) {
            c.days |= (uint8_t)((str(s, "days")[i] == '1') << i);
        }
        c.start_min = (uint16_t)hhmm(str(s, "start"));
        c.stop_min = (uint16_t)hhmm(str(s, "stop"));
        uint32_t epoch = (uint32_t)num(v, "epoch");
        TEST_ASSERT_EQUAL_MESSAGE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "in_hours")),
                                  hb_sched_in_hours(&c, epoch), name);
        if (cJSON_GetObjectItemCaseSensitive(v, "next_start_s")) {
            TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)num(v, "next_start_s"), hb_sched_secs_to_start(&c, epoch), name);
        }
    }
}

// The `cfg` handler (PROTOCOL.md 3: rejected if signature bad, did mismatch, or n <= stored) on the
// signature vectors' key and bytes ({"cv":1,"n":1,"did":"HB-3FA2C1"}).
static int g_cfg_pubs;
static void no_pub(void *arg, const hb_pub_t *p)
{
    (void)arg;
    (void)p;
    g_cfg_pubs++;
}

static const char *cfg_rx(hb_core_t *c, hb_wire_ctx_t *x, const char *topic, int n, const char *b, const char *s,
                          hb_rx_t *rx)
{
    char msg[HB_MSG_MAX + 1];
    snprintf(msg, sizeof msg, "{\"v\":1,\"t\":\"cfg\",\"id\":\"0123456789abcdef\",\"ts\":0,\"n\":%d,\"b\":\"%s\",\"s\":\"%s\"}",
             n, b, s);
    hb_wire_rx(c, x, topic, msg, strlen(msg), 0, rx, no_pub, NULL);
    TEST_ASSERT_TRUE(rx->has_ev == (rx->cfg_err == NULL));
    return rx->cfg_err ? rx->cfg_err : "(ok)";
}

static void test_cfg_message_accepts_once_and_rejects_bad_sig_old_n_wrong_did(void)
{
    const cJSON *sig = section("config", "signature"), *v, *good = NULL, *other = NULL, *raw = NULL;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(sig, "verify")) {
        const char *name = str(v, "name");
        good = strcmp(name, "good") == 0 ? v : good;
        other = strcmp(name, "other_bytes") == 0 ? v : other;
        raw = strcmp(name, "raw_r_s_is_not_der") == 0 ? v : raw;
    }
    TEST_ASSERT_TRUE(good && other && raw);
    static uint8_t key[128];
    static hb_core_t c;
    static hb_rx_t rx;
    hb_wire_ctx_t x = {.did = "HB-3FA2C1", .fw = "t", .hw = "t", .rst = "t", .cfg_key = key,
                       .cfg_key_len = unhex(str(sig, "pub_spki_hex"), key)};
    const char *topic = "hb/v1/d/HB-3FA2C1/cfg";
    memset(&c, 0, sizeof c);
    c.cfg = hb_cfg_default;  // n 0: nothing accepted yet

    TEST_ASSERT_EQUAL_STRING("sig", cfg_rx(&c, &x, topic, 2, str(other, "b"), str(other, "s"), &rx));
    TEST_ASSERT_EQUAL_STRING("sig", cfg_rx(&c, &x, topic, 1, str(raw, "b"), str(raw, "s"), &rx));
    TEST_ASSERT_EQUAL_STRING("body", cfg_rx(&c, &x, topic, 1, "not*b64", str(good, "s"), &rx));
    TEST_ASSERT_EQUAL_STRING("n", cfg_rx(&c, &x, topic, 5, str(good, "b"), str(good, "s"), &rx));  // body n != cfg n
    TEST_ASSERT_EQUAL_STRING("(ok)", cfg_rx(&c, &x, topic, 1, str(good, "b"), str(good, "s"), &rx));
    TEST_ASSERT_EQUAL_INT(HB_EV_CFG, rx.ev.type);
    TEST_ASSERT_EQUAL_UINT32(1, rx.ev.cfg->n);
    TEST_ASSERT_EQUAL_STRING("HB-3FA2C1", rx.ev.cfg->did);
    TEST_ASSERT_EQUAL_STRING("{\"cv\":1,\"n\":1,\"did\":\"HB-3FA2C1\"}", (const char *)rx.raw);  // b's bytes, to persist
    TEST_ASSERT_EQUAL_size_t(strlen("{\"cv\":1,\"n\":1,\"did\":\"HB-3FA2C1\"}"), rx.raw_len);

    c.cfg = *rx.ev.cfg;  // stored n = 1: the retained copy arriving again on the next connect is old
    TEST_ASSERT_EQUAL_STRING("n", cfg_rx(&c, &x, topic, 1, str(good, "b"), str(good, "s"), &rx));

    c.cfg = hb_cfg_default;
    snprintf(x.did, sizeof x.did, "HB-000001");  // a config signed for another beacon
    TEST_ASSERT_EQUAL_STRING("did", cfg_rx(&c, &x, "hb/v1/d/HB-000001/cfg", 1, str(good, "b"), str(good, "s"), &rx));
    TEST_ASSERT_EQUAL_INT(0, g_cfg_pubs);  // a refused config is never answered
}

// "fx": every frame the app's Kotlin port is checked against comes from this code.
static void test_fx_frames(void)
{
    static const char *kinds[] = {"none", "heart", "sorry", "annoy", "pair", "batt", "low"};
    const cJSON *c;
    int n = 0;
    cJSON_ArrayForEach(c, cJSON_GetObjectItem(cJSON_GetObjectItem(g_vec, "fx"), "cases")) {
        const char *k = cJSON_GetObjectItem(c, "kind")->valuestring;
        int kind = -1;
        for (int i = 0; i < 7; i++) {
            if (strcmp(k, kinds[i]) == 0) {
                kind = i;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(kind >= 0, k);
        hb_fx_t fx;
        uint8_t rgb[HB_PIXELS][3];
        hb_fx_start(&fx, (hb_fx_kind_t)kind, (uint8_t)cJSON_GetObjectItem(c, "arg")->valueint, 5000);
        bool more = hb_fx_frame(&fx, 5000u + (uint32_t)cJSON_GetObjectItem(c, "t")->valueint,
                                (uint8_t)cJSON_GetObjectItem(c, "bright")->valueint, rgb);
        TEST_ASSERT_EQUAL(cJSON_IsTrue(cJSON_GetObjectItem(c, "more")), more);
        const cJSON *px = cJSON_GetObjectItem(c, "rgb");
        for (int i = 0; i < HB_PIXELS; i++) {
            for (int j = 0; j < 3; j++) {
                TEST_ASSERT_EQUAL_UINT8(cJSON_GetArrayItem(cJSON_GetArrayItem(px, i), j)->valueint, rgb[i][j]);
            }
        }
        n++;
    }
    TEST_ASSERT_GREATER_THAN(40, n);
}

int main(void)
{
    size_t len;
    char *text = slurp(HB_VECTORS_PATH, &len);
    if (!text) {
        fprintf(stderr, "cannot read %s\n", HB_VECTORS_PATH);
        return 1;
    }
    g_vec = cJSON_Parse(text);
    free(text);
    if (!g_vec) {
        fprintf(stderr, "vectors.json does not parse\n");
        return 1;
    }

    UNITY_BEGIN();
    RUN_TEST(test_known_types);
    RUN_TEST(test_encode_and_round_trip);
    RUN_TEST(test_decode);
    RUN_TEST(test_unknown_t_gets_error_resp);
    RUN_TEST(test_did_from_mac);
    RUN_TEST(test_state_names);
    RUN_TEST(test_beacon_size_cap);
    RUN_TEST(test_id_parse_and_format);
    RUN_TEST(test_print_refuses_small_buffer_and_oversize);
    RUN_TEST(test_split_and_reassemble);
    RUN_TEST(test_split_rejects);
    RUN_TEST(test_reassemble_steps);
    RUN_TEST(test_cfg_defaults_match_vectors);
    RUN_TEST(test_schedule);
    RUN_TEST(test_config_parse);
    RUN_TEST(test_config_signature);
    RUN_TEST(test_b64url);
    RUN_TEST(test_pinned_files_parse);
    RUN_TEST(test_cfg_message_accepts_once_and_rejects_bad_sig_old_n_wrong_did);
    RUN_TEST(test_fx_frames);
    int rc = UNITY_END();
    cJSON_Delete(g_vec);
    return rc;
}
