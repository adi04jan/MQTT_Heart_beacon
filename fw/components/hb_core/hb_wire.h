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
    bool dump;                  // a core dump waits in flash (`tele` says so until it is read and erased)
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

// A BLE-only request the port carries out (NVS, WiFi driver, BLE bonds), then answers with hb_wire_resp.
// hb_wire has already checked the fields and refused what is malformed or not allowed.
typedef enum {
    HB_CMD_NONE,
    HB_CMD_CLAIM,        // host, mport, aport, user, pass; replaces any earlier claim
    HB_CMD_WIFI_SCAN,    // resp d: {nets: [{ssid, rssi, sec}]}
    HB_CMD_WIFI_LIST,    // resp d: {ssids: [...]} (PSKs are never read back)
    HB_CMD_WIFI_SET,     // ssid, psk: store (max 4), join, answer with the result
    HB_CMD_WIFI_FORGET,  // ssid
    HB_CMD_RESET,        // factory: everything incl. bonds; else the WiFi networks only
} hb_cmd_type_t;

typedef struct {
    hb_cmd_type_t type;
    char re[HB_ID_LEN + 1];  // the request id, for the resp
    char host[65], user[33], pass[65];
    uint16_t mport, aport;
    char ssid[33], psk[64];
    bool factory;
} hb_cmd_t;

typedef struct {
    bool has_ev;              // step hb_core with ev
    hb_ev_t ev;
    const char *cfg_err;      // a `cfg` that was refused, and why ("sig", "did", "n", a parse error, ...)
    hb_cfg_t cfg;             // HB_EV_CFG points here
    uint8_t raw[HB_CFG_MAX];  // the accepted config's bytes, for the port to persist
    size_t raw_len;
    hb_cmd_t cmd;             // BLE only: a request for the port
} hb_rx_t;

// One message the broker delivered on d/{did}/down or d/{did}/cfg (other topics are ignored). `get info`
// and an unknown `t` are answered through pub at once; `show`, `time`, `sleep` and an accepted `cfg` become
// rx->ev. Anything that fails the envelope checks is dropped unanswered (PROTOCOL.md 2.2).
void hb_wire_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *topic, const char *data, size_t len,
                uint32_t now_ms, hb_rx_t *rx, hb_pub_fn pub, void *arg);

// One message the phone wrote over the encrypted BLE link. As hb_wire_rx, answered on BLE, plus: `cfg`
// (answered with resp), `link` (HB_EV_PHONE), the phone's `resp` ack (HB_EV_ACK), and the BLE-only
// requests (claim, wifi.*, reset) as rx->cmd for the port.
void hb_wire_ble_rx(hb_core_t *c, const hb_wire_ctx_t *x, const char *data, size_t len, uint32_t now_ms,
                    hb_rx_t *rx, hb_pub_fn pub, void *arg);

// resp{re, ok, err (when not ok), d (when given; taken over and freed)} on path.
void hb_wire_resp(hb_core_t *c, const hb_wire_ctx_t *x, const char *re, bool ok, const char *err, cJSON *d,
                  uint32_t now_ms, hb_path_t path, hb_pub_fn pub, void *arg);
