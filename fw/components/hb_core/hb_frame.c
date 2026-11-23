#include "hb_frame.h"

#include <string.h>

enum { IDLE, COLLECT, SKIP };

int hb_frame_next(const uint8_t *msg, size_t len, uint8_t seq, uint16_t mtu, size_t *off, uint8_t *out)
{
    if (mtu < HB_FRAME_MIN_MTU || len == 0 || len > HB_FRAME_MAX_MSG || seq > 127 || *off > len) {
        return -1;
    }
    size_t n = len - *off;
    size_t room = (size_t)mtu - 4;  // ATT payload is mtu - 3, minus our header byte
    if (n == 0) {
        return 0;
    }
    if (n > room) {
        n = room;
    }
    out[0] = (uint8_t)(seq | (*off + n < len ? HB_FRAME_MORE : 0));
    memcpy(out + 1, msg + *off, n);
    *off += n;
    return (int)n + 1;
}

void hb_reasm_reset(hb_reasm_t *r)
{
    r->state = IDLE;
    r->len = 0;
}

hb_frame_res_t hb_reasm_push(hb_reasm_t *r, const uint8_t *frame, size_t len, uint32_t now_ms)
{
    if (len == 0) {
        return HB_FRAME_EMPTY;
    }
    uint8_t seq = frame[0] & 0x7f;
    int more = (frame[0] & HB_FRAME_MORE) != 0;
    size_t n = len - 1;

    if (r->state != IDLE && (uint32_t)(now_ms - r->last_ms) > HB_FRAME_TIMEOUT_MS) {
        r->state = seq == r->seq ? SKIP : IDLE;  // same seq = late tail of the dropped message
    }
    if (r->state != IDLE && seq != r->seq) {
        r->state = IDLE;  // sender abandoned the previous message
    }
    if (r->state == SKIP) {
        r->last_ms = now_ms;
        if (!more) {
            r->state = IDLE;
        }
        return HB_FRAME_SKIP;
    }
    if (r->state == IDLE) {
        r->state = COLLECT;
        r->seq = seq;
        r->len = 0;
    }
    if (r->len + n > HB_FRAME_MAX_MSG) {
        r->state = more ? SKIP : IDLE;
        r->last_ms = now_ms;
        r->len = 0;
        return HB_FRAME_OVERSIZE;
    }
    memcpy(r->buf + r->len, frame + 1, n);
    r->len += n;
    r->last_ms = now_ms;
    if (more) {
        return HB_FRAME_PARTIAL;
    }
    r->buf[r->len] = 0;
    r->state = IDLE;
    return HB_FRAME_COMPLETE;
}

const char *hb_frame_res_str(hb_frame_res_t res)
{
    static const char *const names[] = {"partial", "complete", "skip", "oversize", "empty"};
    return (unsigned)res < sizeof names / sizeof names[0] ? names[res] : "?";
}
