// hb_link: the WiFi + MQTT link as a pure state machine (plan "Firmware layout", "Power / States" OFFLINE).
// Driver events in, one command out; time injected. A round is: fast connect to the cached BSSID + channel
// (if any), on failure a full scan at once, then MQTT. A failed round waits 5 s, 15 s, 60 s, 5 min, 15 min,
// then every 30 min; a link that drops after being up retries after 1 s first. Never reboots.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HB_LINK_FAST_MS 5000    // cached AP: associated with an IP within this (v1 WIFI_FAST_TIMEOUT_MS)
#define HB_LINK_SCAN_MS 15000   // all-channel scan + join
#define HB_LINK_MQTT_MS 20000   // TLS + CONNACK after the IP
#define HB_LINK_QUICK_MS 1000   // an up link that drops retries after this, before any backoff
#define HB_LINK_STEPS 6

extern const uint32_t hb_link_backoff_ms[HB_LINK_STEPS];

typedef enum {
    HB_LINK_NONE,   // nothing to do
    HB_LINK_FAST,   // WiFi on, join the cached BSSID on its channel (no scan)
    HB_LINK_SCAN,   // WiFi on, scan all channels and join the strongest AP of the known network
    HB_LINK_MQTT,   // start the MQTT session (WiFi has an IP)
    HB_LINK_DOWN,   // stop MQTT and WiFi
} hb_link_cmd_t;

typedef enum { HB_LS_OFF, HB_LS_FAST, HB_LS_SCAN, HB_LS_MQTT, HB_LS_UP, HB_LS_WAIT } hb_link_st_t;

typedef enum {
    HB_LI_POLL,       // deadlines
    HB_LI_START,      // WiFi wanted: a network is known and net.use_wifi
    HB_LI_STOP,       // WiFi not wanted
    HB_LI_WIFI_UP,    // associated with an IP (the port has cached the BSSID + channel)
    HB_LI_WIFI_DOWN,  // join failed or association lost (not a disconnect the port asked for)
    HB_LI_MQTT_UP,    // CONNACK
    HB_LI_MQTT_DOWN,  // connect failed or session lost
} hb_link_in_t;

typedef struct {
    uint8_t st;       // hb_link_st_t
    uint8_t fails;    // failed rounds in a row: the backoff step
    bool cached;      // the port holds a BSSID + channel from an earlier join (RTC memory)
    uint32_t due;     // deadline of the current attempt, or the end of WAIT
} hb_link_t;

hb_link_cmd_t hb_link_ev(hb_link_t *l, hb_link_in_t in, uint32_t now_ms);
