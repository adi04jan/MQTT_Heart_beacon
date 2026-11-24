// hb_wire: hb_core's actions -> messages to publish, and the broker's messages -> hb_core events
// (PROTOCOL.md 3 and 5). The firmware's main loop and hb_sim both go through here, so a simulated beacon
// puts the same bytes on the same topics as a real one. Pure C + cJSON (+ mbedTLS for `cfg`).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hb_config.h"
#include "hb_core.h"
#include "hb_msg.h"

#define HB_TOPIC_MAX 32  // "hb/v1/d/HB-XXXXXX/state" + NUL fits

typedef struct {
    char did[HB_DID_LEN + 1];
    const char *fw, *hw, *rst;  // `tele` and `get info`
    int rssi;                   // dBm of the AP, 0 = unknown
    uint32_t heap;              // free heap, bytes
    const uint8_t *cfg_key;     // pinned config key: SubjectPublicKeyInfo, PEM (NUL counted) or DER
    size_t cfg_key_len;
} hb_wire_ctx_t;

typedef struct {
    hb_path_t path;     // WIFI: publish on topic, QoS 1; BLE: the M4 port sends json to the phone
    const char *topic;
    const char *json;   // NUL-terminated, len bytes, at most HB_MSG_MAX_BEACON
    int len;
    bool retain;
    bool mood;          // a mood: report its PUBACK to hb_core as HB_EV_ACK with id
    uint64_t id;
} hb_pub_t;

typedef void (*hb_pub_fn)(void *arg, const hb_pub_t *p);

// hb/v1/d/{did}/{leaf}
void hb_wire_topic(char out[HB_TOPIC_MAX], const char *did, const char *leaf);

// The MQTT last will: state{st:"lost"}, to be set retained, QoS 1, on d/{did}/state. Returns the length or -1.
int hb_wire_will(hb_core_t *c, uint32_t now_ms, char *buf, size_t cap);

// Everything one step's actions publish, in order: `state` (retained) when it changed or the link just came
// up, `tele`, then up[] (`shown`, `mood`). Nothing without a live path.
void hb_wire_emit(hb_core_t *c, const hb_actions_t *a, const hb_wire_ctx_t *x, uint32_t now_ms, hb_pub_fn pub,
                  void *arg);

typedef struct {
    bool has_ev;              // step hb_core with ev
    hb_ev_t ev;
    const char *cfg_err;      // a `cfg` that was refused, and why ("sig", "did", "n", a parse error, ...)
    hb_cfg_t cfg;             // HB_EV_CFG points here
    uint8_t raw[HB_CFG_MAX];  // the accepted config's bytes, for the port to persist
    size_t raw_len;
} hb_rx_t;

// One message the broker delivered on d/{did}/down or d/{did}/cfg (other topics are ignored). `get info`
// and an unknown `t` are answered through pub at once; `show`, `time`, `sleep` and an accepted `cfg` become
// rx->ev. Anything that fails the envelope checks is dropped unanswered (PROTOCOL.md 2.2).
void hb_wire_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *topic, const char *data, size_t len,
                uint32_t now_ms, hb_rx_t *rx, hb_pub_fn pub, void *arg);
