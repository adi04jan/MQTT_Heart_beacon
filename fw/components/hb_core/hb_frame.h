// hb_frame: BLE fragmentation and reassembly (protocol/PROTOCOL.md section 6.1).
// Pure C, no IDF, no clock: the caller passes now_ms (a free-running ms counter, may wrap).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define HB_FRAME_MAX_MSG 1024
#define HB_FRAME_MIN_MTU 23
#define HB_FRAME_TIMEOUT_MS 2000
#define HB_FRAME_MORE 0x80

// Next frame of msg[0..len) for this MTU and seq (0..127). *off starts at 0 and is advanced.
// out needs room for mtu - 3 bytes. Returns the frame length, 0 when the message is done,
// -1 for bad arguments (mtu < 23, len 0 or > 1024, seq > 127).
int hb_frame_next(const uint8_t *msg, size_t len, uint8_t seq, uint16_t mtu, size_t *off, uint8_t *out);

typedef enum {
    HB_FRAME_PARTIAL,
    HB_FRAME_COMPLETE,  // message is r->buf[0..r->len), NUL-terminated, valid until the next push
    HB_FRAME_SKIP,
    HB_FRAME_OVERSIZE,
    HB_FRAME_EMPTY,
} hb_frame_res_t;

typedef struct {
    uint8_t state;  // idle / collect / skip
    uint8_t seq;
    uint32_t last_ms;
    size_t len;
    uint8_t buf[HB_FRAME_MAX_MSG + 1];
} hb_reasm_t;

void hb_reasm_reset(hb_reasm_t *r);  // also on disconnect
hb_frame_res_t hb_reasm_push(hb_reasm_t *r, const uint8_t *frame, size_t len, uint32_t now_ms);
// "partial", "complete", "skip", "oversize", "empty" (the vector names).
const char *hb_frame_res_str(hb_frame_res_t res);
