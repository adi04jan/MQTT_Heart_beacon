// Golden vectors (protocol/vectors.json) against hb_msg, hb_frame and hb_cfg_default. Config parse vectors
// join in M3 (hb_config).
#include "unity.h"
#include "hb_msg.h"
#include "hb_frame.h"
#include "hb_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HB_VECTORS_PATH
#error "HB_VECTORS_PATH must be defined (fw/test/host/CMakeLists.txt)"
#endif

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

static void test_cfg_defaults_match_vectors(void)
{
    const cJSON *d = section("config", "defaults");
    const cJSON *led = cJSON_GetObjectItemCaseSensitive(d, "led");
    const cJSON *sched = cJSON_GetObjectItemCaseSensitive(d, "sched");
    const cJSON *pwr = cJSON_GetObjectItemCaseSensitive(d, "pwr");
    const hb_cfg_t *c = &hb_cfg_default;
    TEST_ASSERT_EQUAL_INT(num(led, "bright"), c->bright);
    TEST_ASSERT_EQUAL(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sched, "on")), c->sched_on);
    TEST_ASSERT_EQUAL_INT(num(sched, "tz"), c->tz_min);
    uint8_t days = 0;
    for (int i = 0; i < 7; i++) {
        days |= (uint8_t)((str(sched, "days")[i] == '1') << i);
    }
    TEST_ASSERT_EQUAL_HEX8(days, c->days);
    TEST_ASSERT_EQUAL_INT(hhmm(str(sched, "start")), c->start_min);
    TEST_ASSERT_EQUAL_INT(hhmm(str(sched, "stop")), c->stop_min);
    TEST_ASSERT_EQUAL_INT(num(pwr, "idle_nap_min"), c->idle_nap_min);
    TEST_ASSERT_EQUAL_INT(num(pwr, "nap_wake_min"), c->nap_wake_min);
    TEST_ASSERT_EQUAL_INT(num(pwr, "batt_low_mv"), c->batt_low_mv);
    TEST_ASSERT_EQUAL_INT(num(pwr, "batt_off_mv"), c->batt_off_mv);
    TEST_ASSERT_EQUAL_INT(num(pwr, "batt_cal_ppm"), c->batt_cal_ppm);
}

int main(void)
{
    FILE *f = fopen(HB_VECTORS_PATH, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", HB_VECTORS_PATH);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) {
        return 1;
    }
    text[n] = 0;
    fclose(f);
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
    RUN_TEST(test_print_refuses_small_buffer_and_oversize);
    RUN_TEST(test_split_and_reassemble);
    RUN_TEST(test_split_rejects);
    RUN_TEST(test_reassemble_steps);
    RUN_TEST(test_cfg_defaults_match_vectors);
    int rc = UNITY_END();
    cJSON_Delete(g_vec);
    return rc;
}
