// hb_msg: the v1 JSON envelope (protocol/PROTOCOL.md section 2). Pure C + cJSON, no IDF, no clock.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"

#define HB_MSG_MAX 1024        // bytes, whole envelope, any sender (what a receiver accepts)
#define HB_MSG_MAX_BEACON 896  // bytes, what the beacon may send: a phone's `relay` around it still fits 1024
#define HB_ID_LEN 16     // lowercase hex chars
#define HB_DID_LEN 9     // "HB-" + 6 hex

typedef enum {
    HB_MSG_OK = 0,
    HB_MSG_TOO_BIG,
    HB_MSG_BAD_JSON,
    HB_MSG_BAD_V,
    HB_MSG_BAD_T,
    HB_MSG_BAD_ID,
    HB_MSG_BAD_TS,
} hb_msg_err_t;

typedef struct {
    cJSON *root;     // whole object, owned; body = every key except v/t/id/ts
    const char *t;   // points into root
    const char *id;  // points into root
    uint32_t ts;
} hb_msg_t;

// Decode buf[0..len) (no NUL needed). On HB_MSG_OK the caller must hb_msg_free(out).
hb_msg_err_t hb_msg_decode(const char *buf, size_t len, hb_msg_t *out);
void hb_msg_free(hb_msg_t *m);
// Vector names: "too_big", "bad_json", ...; "ok" for HB_MSG_OK.
const char *hb_msg_err_str(hb_msg_err_t e);

// True if t is one of the 20 message types of PROTOCOL.md section 3.
bool hb_msg_known_type(const char *t);

// New object holding the header keys in canonical order; add body keys, print, cJSON_Delete.
cJSON *hb_msg_new(const char *t, const char *id, uint32_t ts);
// {"t":"resp", re, ok:false, err} with a fresh id and ts supplied by the caller.
cJSON *hb_msg_resp_error(const char *re, const char *err, const char *id, uint32_t ts);
// Compact print into buf (NUL-terminated) of a message the beacon sends. Returns length, or -1 if it does
// not fit in buf or exceeds HB_MSG_MAX_BEACON (PROTOCOL.md 2.1).
int hb_msg_print(const cJSON *msg, char *buf, size_t cap);

// Envelope id <-> 64 bits (hb_core keeps ids as integers). Parse accepts exactly 16 chars [0-9a-f].
bool hb_id_parse(const char *s, uint64_t *out);
void hb_id_format(uint64_t id, char out[HB_ID_LEN + 1]);

// "HB-" + last 3 MAC bytes, uppercase hex.
void hb_did_from_mac(const uint8_t mac[6], char out[HB_DID_LEN + 1]);
