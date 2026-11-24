// hb_config: the signed config (PROTOCOL.md 7, 7.1). Parse + validate are pure C (cJSON); the signature
// check uses mbedTLS, which the firmware links from IDF and the host build compiles from IDF's copy.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"
#include "hb_cfg.h"

#define HB_CFG_MAX 1024  // raw config bytes

// Parses and validates config bytes: NULL with *out filled (missing keys from hb_cfg_default), or the
// first failing key in table order: "size", "json", "cv", "n", "did", "srv", "srv.hosts", ... "fw.min_mv".
// *out is scratch on failure.
const char *hb_config_parse(const char *buf, size_t len, hb_cfg_t *out);

// base64url without padding -> bytes. Returns the length, or -1 (bad character or length, over cap).
int hb_b64url_decode(const char *s, uint8_t *out, size_t cap);

// ECDSA P-256 with SHA-256 over msg[0..len), DER signature, against a SubjectPublicKeyInfo key (DER, or PEM
// with its terminating NUL counted in key_len).
bool hb_config_verify(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t len, const uint8_t *sig,
                      size_t sig_len);

// A `cfg` message body {n, b, s}: signature over b's bytes, parse, did == own did, n above stored_n and equal
// to the body's n. NULL = accepted: *out holds the config and raw[0..*raw_len) its bytes (to persist).
// Otherwise the reason: "body", "sig", a parse error, "did" or "n". out and raw are scratch on failure.
const char *hb_config_accept(const cJSON *body, const char *did, uint32_t stored_n, const uint8_t *key,
                             size_t key_len, hb_cfg_t *out, uint8_t raw[HB_CFG_MAX], size_t *raw_len);
