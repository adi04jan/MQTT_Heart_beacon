// hb_sim: the beacon's hb_core + hb_wire behind a line-based TCP socket (fw/sim/README.md). It links the
// same hb_core library the host tests use and calls it the way fw/main/main.c does; the client plays the
// radios, the button and the clock. One client at a time; every connection starts a fresh beacon.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hb_core.h"
#include "hb_policy.h"
#include "hb_wire.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define sock_close close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0  // Windows has no SIGPIPE
#endif
#ifndef HB_SIM_KEY
#define HB_SIM_KEY "cfg-sign.pub"
#endif

static sock_t s_cli = INVALID_SOCKET;
static hb_core_t s_core;
static hb_actions_t s_act;
static hb_cfg_t s_cfg;       // what the next boot loads: the last accepted config (the firmware's NVS copy)
static hb_wire_ctx_t s_x;
static hb_rx_t s_rx;
static uint32_t s_now, s_mv;
static bool s_pressed, s_asleep;

static void out(const char *fmt, ...)
{
    char line[2400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > 0) {
        send(s_cli, line, n < (int)sizeof line ? n : (int)sizeof line - 1, MSG_NOSIGNAL);
    }
}

static void pub(void *arg, const hb_pub_t *p)
{
    (void)arg;
    if (p->path == HB_PATH_WIFI) {
        out("pub %s %d %s\n", p->topic, p->retain, p->json);
    } else {
        out("ble %s\n", p->json);  // BLE arrives in M4: shown, not sent (the firmware drops it too)
    }
}

// The firmware's apply(): LEDs, link command, publishes, state, battery measurement, deep sleep.
static void apply(void)
{
    static const char *const links[] = {"none", "fast", "scan", "mqtt", "down"};
    const hb_actions_t *a = &s_act;
    if (a->led_dirty) {
        char hex[HB_PIXELS * 6 + 1];
        for (int i = 0; i < HB_PIXELS; i++) {
            snprintf(hex + i * 6, 7, "%02x%02x%02x", a->led[i][0], a->led[i][1], a->led[i][2]);
        }
        out("led %s\n", hex);
    }
    if (a->link == HB_LINK_MQTT) {
        char will[HB_MSG_MAX_BEACON + 1], topic[HB_TOPIC_MAX];
        hb_wire_topic(topic, s_x.did, "state");
        if (hb_wire_will(&s_core, s_now, will, sizeof will) > 0) {
            out("link mqtt d-%s %s %s %s\n", s_x.did, s_x.did, topic, will);  // client id, user, LWT
        }
    } else if (a->link) {
        out("link %s\n", links[a->link]);
    }
    hb_wire_emit(&s_core, a, &s_x, s_now, pub, NULL);
    if (a->state_changed) {
        out("state %s\n", hb_state_name(a->state));
    }
    if (a->sleep) {
        out("sleep %lu\n", (unsigned long)a->sleep_s);
        s_asleep = true;
    }
}

static void step(const hb_ev_t *ev)
{
    hb_core_step(&s_core, ev, s_now, &s_act);
    apply();
    while (!s_asleep && s_act.measure_batt) {  // the port reads the ADC at once
        hb_ev_t b = {.type = HB_EV_BATT, .mv = s_mv};
        hb_core_step(&s_core, &b, s_now, &s_act);
        apply();
    }
}

// The firmware's main loop between events: wake after next_ms, sample the button, step.
static void advance(uint32_t to)
{
    while (!s_asleep && (int32_t)(to - s_now) > 0) {
        uint32_t dt = s_act.next_ms < to - s_now ? s_act.next_ms : to - s_now;
        s_now += dt;
        hb_ev_t e = {.type = HB_EV_TICK, .pressed = s_pressed};
        step(&e);
    }
    s_now = to;
}

static const char *boot(uint32_t ms, char *args)
{
    static const char *const wakes[] = {"poweron", "button", "timer"};
    static uint64_t boots;  // ids must differ between runs: the router dedupes on (did, id)
    hb_boot_t b = {.claimed = true, .wifi = true, .mv = 3900,
                   .seed = ((uint64_t)time(NULL) << 32) ^ ((uint64_t)clock() << 16) ^ ++boots};
    char *w = strtok(args, " ");
    int i = 0;
    while (w && i < 3 && strcmp(w, wakes[i]) != 0) {
        i++;
    }
    if (i == 3) {
        return "boot <ms> poweron|button|timer [claimed= mv= epoch= wifi= cached= pressed= seed=]";
    }
    b.wake = (hb_wake_t)i;
    for (char *kv; (kv = strtok(NULL, " "));) {
        char *v = strchr(kv, '=');
        if (!v) {
            return "boot options are key=value";
        }
        *v++ = 0;
        unsigned long n = strtoul(v, NULL, 10);
        if (!strcmp(kv, "claimed")) b.claimed = n;
        else if (!strcmp(kv, "mv")) b.mv = (uint32_t)n;
        else if (!strcmp(kv, "epoch")) b.epoch = (uint32_t)n;
        else if (!strcmp(kv, "wifi")) b.wifi = n;
        else if (!strcmp(kv, "cached")) b.ap_cached = n;
        else if (!strcmp(kv, "pressed")) b.pressed = n;
        else if (!strcmp(kv, "seed")) b.seed = strtoull(v, NULL, 10);
        else return "unknown boot option";
    }
    s_x.rst = wakes[i];
    s_mv = b.mv;
    s_pressed = b.pressed;
    s_now = ms;
    s_asleep = false;
    hb_core_boot(&s_core, &s_cfg, &b, s_now, &s_act);
    apply();
    return NULL;
}

// One command line; NULL = ok, else the error text.
static const char *command(char *line)
{
    char *verb = strtok(line, " "), *ms_s = strtok(NULL, " "), *rest = strtok(NULL, "");
    if (!verb) {
        return "empty line";
    }
    if (!strcmp(verb, "quit")) {
        exit(0);
    }
    if (!ms_s) {
        return "missing <ms>";
    }
    uint32_t ms = (uint32_t)strtoul(ms_s, NULL, 10);
    if (!strcmp(verb, "boot")) {
        return boot(ms, rest ? rest : (char *)"");
    }
    if (s_asleep) {
        return "asleep: boot first";
    }
    if ((int32_t)(ms - s_now) < 0) {
        return "time went backwards";
    }
    advance(ms);
    if (s_asleep) {
        return NULL;  // slept on the way there
    }
    hb_ev_t e = {.type = HB_EV_TICK, .pressed = s_pressed};
    char *a1 = rest ? strtok(rest, " ") : NULL;
    if (!strcmp(verb, "run")) {
        return NULL;
    } else if (!strcmp(verb, "press") || !strcmp(verb, "release")) {
        s_pressed = e.pressed = verb[0] == 'p';
    } else if (!strcmp(verb, "batt") && a1) {
        e = (hb_ev_t){.type = HB_EV_BATT, .mv = s_mv = (uint32_t)strtoul(a1, NULL, 10)};
    } else if (!strcmp(verb, "wifi") && a1) {
        char *rssi = strtok(NULL, " ");
        e = (hb_ev_t){.type = HB_EV_WIFI, .up = !strcmp(a1, "up")};
        if (e.up && rssi) {
            s_x.rssi = atoi(rssi);
        }
    } else if (!strcmp(verb, "mqtt") && a1) {
        e = (hb_ev_t){.type = HB_EV_MQTT, .up = !strcmp(a1, "up")};
        if (e.up) {  // the port subscribes on CONNACK, before the core hears of it
            out("sub hb/v1/d/%s/down\nsub hb/v1/d/%s/cfg\n", s_x.did, s_x.did);
        }
    } else if (!strcmp(verb, "puback") && a1) {
        e.type = HB_EV_ACK;
        if (!hb_id_parse(a1, &e.id)) {
            return "puback <ms> <16 hex id>";
        }
    } else if (!strcmp(verb, "rx") && a1) {
        char *json = strtok(NULL, "");
        if (!json) {
            return "rx <ms> <topic> <json>";
        }
        hb_wire_rx(&s_core, &s_x, a1, json, strlen(json), s_now, &s_rx, pub, NULL);
        if (s_rx.cfg_err) {
            out("cfg refused %s\n", s_rx.cfg_err);
        }
        if (!s_rx.has_ev) {
            return NULL;
        }
        if (s_rx.ev.type == HB_EV_CFG) {
            s_cfg = s_rx.cfg;  // persisted, as the firmware writes it to NVS
            out("cfg ok %lu\n", (unsigned long)s_cfg.n);
        }
        e = s_rx.ev;
    } else {
        return "unknown command (boot run press release batt wifi mqtt puback rx quit)";
    }
    step(&e);
    return NULL;
}

static void serve(void)
{
    char buf[4096];
    size_t len = 0;
    s_cfg = hb_cfg_default;
    s_asleep = true;
    s_now = 0;
    memset(&s_core, 0, sizeof s_core);
    for (;;) {
        int n = recv(s_cli, buf + len, (int)(sizeof buf - 1 - len), 0);
        if (n <= 0) {
            return;
        }
        len += (size_t)n;
        buf[len] = 0;
        char *nl;
        while ((nl = strchr(buf, '\n'))) {
            *nl = 0;
            if (nl > buf && nl[-1] == '\r') {
                nl[-1] = 0;
            }
            const char *err = command(buf);
            if (err) {
                out("err %s\n", err);
            } else {
                if (!s_asleep) {
                    out("next %lu\n", (unsigned long)s_act.next_ms);
                }
                out("ok\n");
            }
            len -= (size_t)(nl + 1 - buf);
            memmove(buf, nl + 1, len + 1);
        }
        if (len == sizeof buf - 1) {
            out("err line too long\n");
            len = 0;
        }
    }
}

int main(int argc, char **argv)
{
    unsigned port = 0;
    const char *key_path = HB_SIM_KEY;
    snprintf(s_x.did, sizeof s_x.did, "HB-000001");
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--port")) port = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--did")) snprintf(s_x.did, sizeof s_x.did, "%s", argv[i + 1]);
        else if (!strcmp(argv[i], "--key")) key_path = argv[i + 1];
    }
    static uint8_t key[4096];
    FILE *f = fopen(key_path, "rb");
    size_t klen = f ? fread(key, 1, sizeof key - 1, f) : 0;
    if (f) {
        fclose(f);
    }
    if (!klen) {
        fprintf(stderr, "hb_sim: cannot read config key %s\n", key_path);
        return 1;
    }
    key[klen] = 0;
    s_x.cfg_key = key;
    s_x.cfg_key_len = key[0] == 0x30 ? klen : klen + 1;  // DER as is; PEM with its NUL
    s_x.fw = "sim";
    s_x.hw = "sim";
    s_x.rst = "poweron";
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return 1;
    }
#endif
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((unsigned short)port)};
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof addr;
    if (srv == INVALID_SOCKET || bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(srv, 1) != 0 ||
        getsockname(srv, (struct sockaddr *)&addr, &alen) != 0) {
        fprintf(stderr, "hb_sim: cannot listen on 127.0.0.1:%u\n", port);
        return 1;
    }
    printf("listening %u %s\n", ntohs(addr.sin_port), s_x.did);
    fflush(stdout);
    for (;;) {
        s_cli = accept(srv, NULL, NULL);
        if (s_cli == INVALID_SOCKET) {
            continue;
        }
        serve();
        sock_close(s_cli);
    }
}
