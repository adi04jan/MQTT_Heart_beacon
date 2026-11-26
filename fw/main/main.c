// Heart Beacon v2. One queue, one loop: events in, hb_core_step, actions out. hb_wire turns actions into
// messages and incoming messages (MQTT or BLE) into events, the same code hb_sim runs on the host. This file
// adds what needs the hardware: NVS for claim and WiFi, the WiFi test join of wifi.set, factory reset.
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_core_dump.h"
#include "esp_task_wdt.h"
#include "hb_config.h"
#include "hb_core.h"
#include "hb_msg.h"
#include "hb_policy.h"
#include "hb_port.h"
#include "hb_wire.h"
#include "sdkconfig.h"

#if CONFIG_HB_HAS_XTAL32K && CONFIG_HB_BTN_GPIO <= 1
#error "the 32 kHz crystal takes GPIO0/1: move the button (HB_BTN_GPIO) to GPIO2-5"
#endif
#if CONFIG_HB_HAS_XTAL32K && !CONFIG_RTC_CLK_SRC_EXT_CRYS
#error "HB_HAS_XTAL32K needs CONFIG_RTC_CLK_SRC_EXT_CRYS=y"
#endif

#define BTN CONFIG_HB_BTN_GPIO
#define SLEEP_FLUSH_MS 2000  // the retained `state` gets this long to be acked before a deep sleep
#define TRY_MS 20000         // wifi.set: join and get an IP within this (the app waits 30 s)

// Pinned at build time (fw/main/certs).
extern const char ca_crt[] asm("_binary_ca_crt_start");
extern const uint8_t cfg_key[] asm("_binary_cfg_sign_pub_start");
extern const uint8_t cfg_key_end[] asm("_binary_cfg_sign_pub_end");

static const char *TAG = "hb";
// Kept through deep sleep and through crash, watchdog and brownout resets (RTC_NOINIT, checked by magic): the
// state we slept in, battery flags, the outbox and the show dedupe ring. Power-on garbage fails the magic.
#define RTC_MAGIC 0x48425632u  // "HBV2"
static RTC_NOINIT_ATTR struct {
    uint32_t magic;
    uint32_t brownouts;  // brownout resets in a row
    hb_core_t core;
} s_rtc;

typedef struct {
    enum { Q_EV, Q_RX, Q_PUBACK, Q_BLE } kind;
    hb_ev_t ev;     // Q_EV
    char *buf;      // Q_RX: topic NUL payload NUL; Q_BLE: message NUL. malloc'ed, freed by the loop
    uint16_t tlen;  // Q_RX topic length
    uint16_t dlen;  // Q_RX / Q_BLE payload length
    int msg_id;     // Q_PUBACK
} q_t;
static QueueHandle_t s_q;

static hb_wire_ctx_t s_x;
static char s_user[33], s_pass[65], s_host[65];
static hb_cfg_t s_cfg;   // the config hb_core runs on (NVS copy, then whatever is accepted)
static hb_rx_t s_rx;     // big: one, used by the loop only
static struct {
    int msg_id;
    uint64_t id;
} s_moods[8];            // PUBACK msg id -> the mood's envelope id (loop only)
static uint8_t s_moods_next;
static unsigned s_srv;   // index into srv.hosts / srv.mqtt; moves on after a session that never came up
static bool s_srv_ok = true;
static struct {          // wifi.set in progress: the link is held off while the network is tried
    bool on;
    uint32_t due;
    char re[HB_ID_LEN + 1], ssid[33], psk[64];
} s_try;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool btn_pressed(void)
{
    return gpio_get_level(BTN) == 0;
}

void hb_port_post(const hb_ev_t *ev)
{
    q_t q = {.kind = Q_EV, .ev = *ev};
    if (xQueueSend(s_q, &q, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, event %d dropped", ev->type);
    }
}

static void post_buf(q_t *q)
{
    if (xQueueSend(s_q, q, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(q->buf);
    }
}

void hb_port_post_rx(const char *topic, int topic_len, const char *data, int data_len)
{
    q_t q = {.kind = Q_RX, .tlen = (uint16_t)topic_len, .dlen = (uint16_t)data_len};
    if (topic_len <= 0 || topic_len > 64 || data_len < 0 || data_len > HB_MSG_MAX ||
        !(q.buf = malloc((size_t)topic_len + (size_t)data_len + 2))) {
        return;
    }
    memcpy(q.buf, topic, (size_t)topic_len);
    q.buf[topic_len] = 0;
    memcpy(q.buf + topic_len + 1, data, (size_t)data_len);
    q.buf[topic_len + 1 + data_len] = 0;
    post_buf(&q);
}

void hb_port_post_ble(const char *msg, size_t len)
{
    q_t q = {.kind = Q_BLE, .dlen = (uint16_t)len};
    if (len == 0 || len > HB_MSG_MAX || !(q.buf = malloc(len + 1))) {
        return;
    }
    memcpy(q.buf, msg, len);
    q.buf[len] = 0;
    post_buf(&q);
}

void hb_port_post_puback(int msg_id)
{
    q_t q = {.kind = Q_PUBACK, .msg_id = msg_id};
    xQueueSend(s_q, &q, pdMS_TO_TICKS(100));
}

// Low-level interrupt, so a press also wakes automatic light sleep. Disarmed here (a held button would
// fire forever) and re-armed by the loop once the pin reads released; hb_core samples it on each tick.
static void btn_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(BTN);
    q_t q = {.kind = Q_EV, .ev = {.type = HB_EV_TICK}};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_q, &q, &woken);
    portYIELD_FROM_ISR(woken);
}

static void btn_init(void)
{
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BTN, btn_isr, NULL));
    gpio_config_t io = {
        .pin_bit_mask = BIT64(BTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_wakeup_enable(BTN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

static void pub(void *arg, const hb_pub_t *p)
{
    (void)arg;
    if (p->path == HB_PATH_BLE) {
        bool ok = hb_ble_send(p->json, p->len);  // a mood is acked by the phone's resp (HB_EV_ACK)
        ESP_LOGI(TAG, "ble %s%s", p->json, ok ? "" : " (no link)");
        return;
    }
    int mid = hb_mqtt_publish(p->topic, p->json, p->len, p->retain);
    ESP_LOGI(TAG, "pub %s %s (%d)", p->topic, p->json, mid);
    if (mid > 0 && p->mood) {
        s_moods[s_moods_next].msg_id = mid;
        s_moods[s_moods_next].id = p->id;
        s_moods_next = (uint8_t)((s_moods_next + 1) % (sizeof s_moods / sizeof s_moods[0]));
    }
}

static void link(uint8_t cmd)
{
    if (s_try.on) {
        return;  // wifi.set owns the radio; HB_EV_PROV restarts the link afterwards
    }
    switch (cmd) {
    case HB_LINK_FAST:
    case HB_LINK_SCAN:
        hb_wifi_join(cmd == HB_LINK_FAST);
        break;
    case HB_LINK_MQTT: {
        static char will[HB_MSG_MAX_BEACON + 1];
        s_srv += !s_srv_ok;  // ponytail: rotates hosts and ports together; per-host health if a list grows
        s_srv_ok = false;
        const char *host = s_cfg.n_hosts ? s_cfg.hosts[s_srv % s_cfg.n_hosts] : s_host;
        hb_mqtt_cfg_t m = {.host = host, .port = s_cfg.mqtt[s_srv % s_cfg.n_mqtt], .did = s_x.did,
                           .user = s_user, .pass = s_pass, .will = will, .ca = ca_crt};
        if (hb_wire_will(&s_rtc.core, now_ms(), will, sizeof will) < 0 || hb_mqtt_start(&m) != ESP_OK) {
            hb_ev_t ev = {.type = HB_EV_MQTT, .up = false};
            hb_port_post(&ev);
        }
        break;
    }
    case HB_LINK_DOWN:
        ESP_LOGI(TAG, "link down");
        hb_mqtt_stop();
        hb_wifi_stop();
        break;
    default:
        break;
    }
}

static void apply(const hb_actions_t *a)
{
    if (a->led_dirty) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_show(a->led));
    }
    if (a->link) {
        link(a->link);
    }
    s_x.rssi = hb_wifi_rssi();
    s_x.heap = esp_get_free_heap_size();
    hb_wire_emit(&s_rtc.core, a, &s_x, now_ms(), pub, NULL);
    if (a->state_changed) {
        ESP_LOGI(TAG, "state %s", hb_state_name(a->state));
        hb_ble_window(a->state == HB_ST_PAIRING);
    }
    if (a->measure_batt) {
        hb_ev_t ev = {.type = HB_EV_BATT, .mv = hb_adc_batt_mv()};
        ESP_LOGD(TAG, "battery %lu mV", (unsigned long)ev.mv);
        hb_port_post(&ev);
    }
    if (!btn_pressed()) {
        gpio_intr_enable(BTN);
    }
    if (a->factory_reset) {  // review R8: a 15 s hold, the white flash has played
        ESP_LOGW(TAG, "factory reset from the button");
        hb_mqtt_stop();
        hb_wifi_clear();
        hb_ble_forget_all();
        hb_store_erase_all();
        s_rtc.magic = 0;
        esp_restart();
    }
    if (a->sleep) {
        hb_mqtt_flush(SLEEP_FLUSH_MS);  // the retained state (with its wake time) first
        hb_ble_flush(1500);             // and its BLE copy for a NEAR phone, then hang up
        hb_mqtt_stop();                 // then a clean DISCONNECT: the broker drops the LWT
        hb_wifi_stop();
        ESP_LOGI(TAG, "deep sleep, timer %lu s", (unsigned long)a->sleep_s);
        hb_sleep_deep(a->sleep_s);
    }
}

static void resp(const char *re, bool ok, const char *err, cJSON *d)
{
    hb_wire_resp(&s_rtc.core, &s_x, re, ok, err, d, now_ms(), HB_PATH_BLE, pub, NULL);
}

// A WiFi network and a server: what the link needs to run.
static bool provisioned(void)
{
    char ssids[HB_WIFI_NETS][33];
    return hb_wifi_nets(ssids) && s_pass[0] && (s_host[0] || s_cfg.n_hosts);
}

static void post_prov(void)
{
    hb_ev_t ev = {.type = HB_EV_PROV, .up = provisioned()};
    hb_port_post(&ev);
}

// wifi.set: try the network on its own (the link is held off), store it only if it joined, answer, then
// give the radio back to hb_link with a fresh round.
static void try_end(bool joined)
{
    const char *why = joined ? NULL : !s_try.due ? "no answer" : hb_wifi_why();
    if (joined && hb_wifi_add(s_try.ssid, s_try.psk) != ESP_OK) {
        joined = false;
        why = "could not store it";
    }
    ESP_LOGI(TAG, "wifi.set %s: %s", s_try.ssid, joined ? "joined" : why);
    hb_wifi_stop();
    resp(s_try.re, joined, why, NULL);
    memset(&s_try, 0, sizeof s_try);
    post_prov();
}

static void request(const hb_cmd_t *k)
{
    switch (k->type) {
    case HB_CMD_CLAIM: {
        esp_err_t err = hb_store_set_str("host", k->host);
        err = err ? err : hb_store_set_str("user", k->user);
        err = err ? err : hb_store_set_str("pass", k->pass);
        err = err ? err : hb_store_set_u8("claimed", 1);
        if (err) {
            resp(k->re, false, "could not store it", NULL);
            return;
        }
        // ponytail: mport/aport are not stored; the signed cfg the app writes right after carries srv.*
        strcpy(s_host, k->host);
        strcpy(s_user, k->user);
        strcpy(s_pass, k->pass);
        ESP_LOGI(TAG, "claimed, server %s", s_host);
        hb_ev_t ev = {.type = HB_EV_CLAIMED};
        hb_port_post(&ev);
        post_prov();
        resp(k->re, true, NULL, NULL);
        return;
    }
    case HB_CMD_WIFI_SCAN: {
        static hb_net_t nets[10];  // 10 rows always fit the 896 B cap
        int n = s_try.on ? -1 : hb_wifi_scan(nets, 10);
        if (n < 0) {
            resp(k->re, false, "busy", NULL);
            return;
        }
        cJSON *d = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(d, "nets");
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
            cJSON_AddNumberToObject(o, "rssi", nets[i].rssi);
            cJSON_AddBoolToObject(o, "sec", nets[i].sec);
            cJSON_AddItemToArray(arr, o);
        }
        resp(k->re, true, NULL, d);
        return;
    }
    case HB_CMD_WIFI_LIST: {
        char ssids[HB_WIFI_NETS][33];
        int n = hb_wifi_nets(ssids);
        cJSON *d = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(d, "ssids");
        for (int i = 0; i < n; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(ssids[i]));
        }
        resp(k->re, true, NULL, d);
        return;
    }
    case HB_CMD_WIFI_SET:
        if (s_try.on) {
            resp(k->re, false, "busy", NULL);
            return;
        }
        if (!hb_wifi_room(k->ssid)) {
            resp(k->re, false, "4 networks stored already; forget one first", NULL);
            return;
        }
        s_try.on = true;
        s_try.due = now_ms() + TRY_MS;
        memcpy(s_try.re, k->re, sizeof s_try.re);
        strcpy(s_try.ssid, k->ssid);
        strcpy(s_try.psk, k->psk);
        hb_mqtt_stop();
        hb_wifi_try(s_try.ssid, s_try.psk);
        return;
    case HB_CMD_WIFI_FORGET:
        if (hb_wifi_remove(k->ssid) != ESP_OK) {
            resp(k->re, false, "not stored", NULL);
            return;
        }
        resp(k->re, true, NULL, NULL);
        post_prov();
        return;
    case HB_CMD_RESET:
        hb_wifi_clear();
        if (!k->factory) {
            resp(k->re, true, NULL, NULL);
            post_prov();
            return;
        }
        resp(k->re, true, NULL, NULL);
        vTaskDelay(pdMS_TO_TICKS(500));  // let the answer leave
        hb_ble_forget_all();
        hb_store_erase_all();
        s_rtc.magic = 0;  // and the RTC core: the next boot starts clean
        ESP_LOGW(TAG, "factory reset");
        esp_restart();  // the owner's request; the beacon never reboots on its own
    case HB_CMD_NONE:
        return;
    }
}

// A message the loop decoded into s_rx: persist what it carries, run its request.
static bool received(hb_ev_t *ev)
{
    if (s_rx.cfg_err) {
        ESP_LOGI(TAG, "cfg refused: %s", s_rx.cfg_err);
    }
    if (s_rx.cmd.type != HB_CMD_NONE) {
        request(&s_rx.cmd);
    }
    if (!s_rx.has_ev) {
        return false;
    }
    if (s_rx.ev.type == HB_EV_CFG) {
        ESP_LOGI(TAG, "cfg %lu accepted", (unsigned long)s_rx.cfg.n);
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_store_set_blob("cfg", s_rx.raw, s_rx.raw_len));
        s_cfg = s_rx.cfg;
        hb_ble_params(&s_cfg);
    } else if (s_rx.ev.type == HB_EV_TIME) {  // the RTC keeps it through deep sleep (boot.epoch)
        struct timeval tv = {.tv_sec = s_rx.ev.ts};
        settimeofday(&tv, NULL);
    }
    *ev = s_rx.ev;
    return true;
}

// A queue item as an hb_core event; false if there is nothing to step.
static bool to_event(q_t *q, hb_ev_t *ev)
{
    switch (q->kind) {
    case Q_PUBACK:
        for (size_t i = 0; i < sizeof s_moods / sizeof s_moods[0]; i++) {
            if (s_moods[i].msg_id == q->msg_id) {
                s_moods[i].msg_id = 0;
                *ev = (hb_ev_t){.type = HB_EV_ACK, .id = s_moods[i].id};
                return true;
            }
        }
        return false;
    case Q_RX:
        hb_wire_rx(&s_rtc.core, &s_x, q->buf, q->buf + q->tlen + 1, q->dlen, now_ms(), &s_rx, pub, NULL);
        free(q->buf);
        return received(ev);
    case Q_BLE: {
        hb_ev_t hello = {.type = HB_EV_HELLO};  // anything from the phone says it is alive (HB_PHONE_SILENT_MS)
        hb_port_post(&hello);
        hb_wire_ble_rx(&s_rtc.core, &s_x, q->buf, q->dlen, now_ms(), &s_rx, pub, NULL);
        free(q->buf);
        return received(ev);
    }
    case Q_EV:
        break;
    }
    *ev = q->ev;
    if (ev->type == HB_EV_WIFI && s_try.on) {
        try_end(ev->up);  // the tried network's result, not the link's
        return false;
    }
    if (ev->type == HB_EV_TICK) {
        ev->pressed = btn_pressed();
    } else if (ev->type == HB_EV_MQTT && ev->up) {
        s_srv_ok = true;
    }
    return true;
}

static const char *reset_name(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

// The claim (or the bench console) and the stored config.
static void load(void)
{
    hb_store_get_str("did", s_x.did, sizeof s_x.did);
    if (!s_x.did[0]) {
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        hb_did_from_mac(mac, s_x.did);
    }
    hb_store_get_str("user", s_user, sizeof s_user);
    if (!s_user[0]) {
        strcpy(s_user, s_x.did);  // PROTOCOL.md 5: a beacon logs in as its did
    }
    hb_store_get_str("pass", s_pass, sizeof s_pass);
    hb_store_get_str("host", s_host, sizeof s_host);

    static uint8_t raw[HB_CFG_MAX];
    size_t n = hb_store_get_blob("cfg", raw, sizeof raw);
    const char *err = n ? hb_config_parse((const char *)raw, n, &s_cfg) : "none stored";
    if (!err && strcmp(s_cfg.did, s_x.did) != 0) {
        err = "did";
    }
    if (err) {
        s_cfg = hb_cfg_default;
    }
    char ssids[HB_WIFI_NETS][33];
    ESP_LOGI(TAG, "did %s, server %s, %d wifi networks, config %lu (%s)", s_x.did,
             s_cfg.n_hosts ? s_cfg.hosts[0] : s_host[0] ? s_host : "(none)", hb_wifi_nets(ssids),
             (unsigned long)s_cfg.n, err ? err : "ok");
}

void app_main(void)
{
    // Rollback is on: an OTA-written image stays NEW until confirmed, and the next reset would return the
    // unit to the previous one. ponytail: confirmed at once; M7 confirms after a self-test.
    esp_ota_mark_app_valid_cancel_rollback();
    hb_wake_t wake = hb_sleep_wake_cause();
    esp_reset_reason_t rr = esp_reset_reason();
    bool kept = s_rtc.magic == RTC_MAGIC && rr != ESP_RST_POWERON;
    if (!kept) {
        memset(&s_rtc, 0, sizeof s_rtc);
    }
    s_rtc.brownouts = rr == ESP_RST_BROWNOUT ? s_rtc.brownouts + 1 : 0;
    if (wake == HB_WAKE_POWERON && kept &&
        (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT ||
         rr == ESP_RST_BROWNOUT || rr == ESP_RST_SW)) {
        wake = HB_WAKE_RESET;  // keep the outbox, start quietly
    }
    s_rtc.magic = RTC_MAGIC;
    if (s_rtc.brownouts >= 2) {  // the cell sags under load: sleep before any LED or radio, button wake only
        ESP_LOGW(TAG, "brownout %lu times in a row: low-battery sleep", (unsigned long)s_rtc.brownouts);
        s_rtc.brownouts = 0;
        hb_sleep_deep(0);
    }
    static const char *const wakes[] = {"power-on", "button", "timer", "reset"};
    ESP_LOGI(TAG, "Heart beacon %s, wake %s", esp_app_get_description()->version, wakes[wake]);

    s_q = xQueueCreate(24, sizeof(q_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_store_init());
    static const uint8_t dark[HB_PIXELS][3];  // hb_core assumes the pixels start dark
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_init()) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_show(dark));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_adc_init());
    btn_init();
    load();
    hb_console_start();
    bool wifi = ESP_ERROR_CHECK_WITHOUT_ABORT(hb_wifi_init()) == ESP_OK && provisioned();
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_ble_init(s_x.did, &s_cfg));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_sleep_auto_light());

    s_x.fw = esp_app_get_description()->version;
    s_x.hw = "A";
    s_x.rst = reset_name();
    s_x.cfg_key = cfg_key;
    s_x.cfg_key_len = (size_t)(cfg_key_end - cfg_key);  // PEM with its NUL (EMBED_TXTFILES)
    s_x.dump = esp_core_dump_image_check() == ESP_OK;
    if (s_x.dump) {
        ESP_LOGW(TAG, "a core dump is waiting: idf.py coredump-info, then `dump erase` on the console");
    }
#if CONFIG_HB_BENCH_FAST_NAP
    s_cfg.idle_nap_min = 1;
    s_cfg.nap_wake_min = 1;
#endif
    time_t t = time(NULL);  // RTC time survives deep sleep; set by `time` from the router or the phone
    hb_boot_t boot = {
        .wake = wake,
        .claimed = hb_store_get_u8("claimed", 0),
        .pressed = btn_pressed(),
        .mv = hb_adc_batt_mv(),
        .epoch = t > 1700000000 ? (uint32_t)t : 0,
        .seed = (uint64_t)esp_random() << 32 | esp_random(),
        .wifi = wifi,
        .ap_cached = hb_wifi_cached(),
    };
    ESP_LOGI(TAG, "battery %lu mV, claimed %d, clock %s, wifi %s", (unsigned long)boot.mv, boot.claimed,
             boot.epoch ? "set" : "unknown", wifi ? "on" : "off (claim and wifi.set over BLE)");

    hb_actions_t act;
    hb_core_boot(&s_rtc.core, &s_cfg, &boot, now_ms(), &act);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_add(NULL));  // a hung loop reboots (60 s, panics)
    for (;;) {
        esp_task_wdt_reset();
        if (s_rtc.brownouts && now_ms() > 60000) {
            s_rtc.brownouts = 0;  // a minute up under load: the cell is fine
        }
        apply(&act);
        uint32_t wait = act.next_ms;
        if (s_try.on) {
            int32_t left = (int32_t)(s_try.due - now_ms());
            if (left <= 0) {
                s_try.due = 0;  // timed out: "no answer"
                try_end(false);
            } else if ((uint32_t)left < wait) {
                wait = (uint32_t)left;
            }
        }
        q_t q;
        hb_ev_t ev = {.type = HB_EV_TICK};
        if (xQueueReceive(s_q, &q, pdMS_TO_TICKS(wait)) == pdTRUE && !to_event(&q, &ev)) {
            ev.type = HB_EV_TICK;
        }
        if (ev.type == HB_EV_TICK) {
            ev.pressed = btn_pressed();
        }
        hb_core_step(&s_rtc.core, &ev, now_ms(), &act);
    }
}
