// Heart Beacon v2 (M3: WiFi + MQTT). One queue, one loop: events in, hb_core_step, actions out. hb_wire turns
// actions into publishes and broker messages into events, the same code hb_sim runs on the host.
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

// Pinned at build time (fw/main/certs; the committed files are test-only, see fw/sim/README.md).
extern const char ca_crt[] asm("_binary_ca_crt_start");
extern const uint8_t cfg_key[] asm("_binary_cfg_sign_pub_start");
extern const uint8_t cfg_key_end[] asm("_binary_cfg_sign_pub_end");

static const char *TAG = "hb";
static RTC_DATA_ATTR hb_core_t s_core;  // kept through deep sleep: the state we slept in, battery flags, outbox

typedef struct {
    enum { Q_EV, Q_RX, Q_PUBACK } kind;
    hb_ev_t ev;     // Q_EV
    char *buf;      // Q_RX: topic NUL payload NUL, malloc'ed, freed by the loop
    uint16_t tlen;  // Q_RX topic length
    uint16_t dlen;  // Q_RX payload length
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
    if (xQueueSend(s_q, &q, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(q.buf);
    }
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
    if (p->path != HB_PATH_WIFI) {
        ESP_LOGI(TAG, "no BLE before M4, dropped %s", p->json);
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
        if (hb_wire_will(&s_core, now_ms(), will, sizeof will) < 0 || hb_mqtt_start(&m) != ESP_OK) {
            hb_ev_t ev = {.type = HB_EV_MQTT, .up = false};
            hb_port_post(&ev);
        }
        break;
    }
    case HB_LINK_DOWN:
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
    hb_wire_emit(&s_core, a, &s_x, now_ms(), pub, NULL);
    if (a->state_changed) {
        ESP_LOGI(TAG, "state %s", hb_state_name(a->state));
    }
    if (a->measure_batt) {
        hb_ev_t ev = {.type = HB_EV_BATT, .mv = hb_adc_batt_mv()};
        ESP_LOGD(TAG, "battery %lu mV", (unsigned long)ev.mv);
        hb_port_post(&ev);
    }
    if (!btn_pressed()) {
        gpio_intr_enable(BTN);
    }
    if (a->sleep) {
        hb_mqtt_flush(SLEEP_FLUSH_MS);  // the retained state (with its wake time) first
        hb_mqtt_stop();                 // then a clean DISCONNECT: the broker drops the LWT
        hb_wifi_stop();
        ESP_LOGI(TAG, "deep sleep, timer %lu s", (unsigned long)a->sleep_s);
        hb_sleep_deep(a->sleep_s);
    }
}

// A queue item as an hb_core event; false if there is nothing to step.
static bool to_event(q_t *q, hb_ev_t *ev)
{
    if (q->kind == Q_PUBACK) {
        for (size_t i = 0; i < sizeof s_moods / sizeof s_moods[0]; i++) {
            if (s_moods[i].msg_id == q->msg_id) {
                s_moods[i].msg_id = 0;
                *ev = (hb_ev_t){.type = HB_EV_ACK, .id = s_moods[i].id};
                return true;
            }
        }
        return false;
    }
    if (q->kind == Q_RX) {
        const char *topic = q->buf, *data = q->buf + q->tlen + 1;
        hb_wire_rx(&s_core, &s_x, topic, data, q->dlen, now_ms(), &s_rx, pub, NULL);
        free(q->buf);
        if (s_rx.cfg_err) {
            ESP_LOGI(TAG, "cfg refused: %s", s_rx.cfg_err);
        }
        if (!s_rx.has_ev) {
            return false;
        }
        if (s_rx.ev.type == HB_EV_CFG) {
            ESP_LOGI(TAG, "cfg %lu accepted", (unsigned long)s_rx.cfg.n);
            ESP_ERROR_CHECK_WITHOUT_ABORT(hb_store_set_blob("cfg", s_rx.raw, s_rx.raw_len));
            s_cfg = s_rx.cfg;
        } else if (s_rx.ev.type == HB_EV_TIME) {  // the RTC keeps it through deep sleep (boot.epoch)
            struct timeval tv = {.tv_sec = s_rx.ev.ts};
            settimeofday(&tv, NULL);
        }
        *ev = s_rx.ev;
        return true;
    }
    *ev = q->ev;
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

// Provisioning (console.c) and the stored config. Returns whether the link has what it needs.
static bool load(void)
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
    char ssid[33];
    hb_store_get_str("ssid", ssid, sizeof ssid);

    static uint8_t raw[HB_CFG_MAX];
    size_t n = hb_store_get_blob("cfg", raw, sizeof raw);
    const char *err = n ? hb_config_parse((const char *)raw, n, &s_cfg) : "none stored";
    if (!err && strcmp(s_cfg.did, s_x.did) != 0) {
        err = "did";
    }
    if (err) {
        s_cfg = hb_cfg_default;
    }
    ESP_LOGI(TAG, "did %s, server %s, wifi %s, config %lu (%s)", s_x.did, s_cfg.n_hosts ? s_cfg.hosts[0] : s_host,
             ssid[0] ? ssid : "(none)", (unsigned long)s_cfg.n, err ? err : "ok");
    return ssid[0] && s_pass[0] && (s_host[0] || s_cfg.n_hosts);
}

void app_main(void)
{
    // The v1 bootloader has rollback on: an OTA-written image stays NEW until confirmed, and the next
    // reset would return the unit to v1. ponytail: confirmed at once; M7 confirms after a self-test.
    esp_ota_mark_app_valid_cancel_rollback();
    hb_wake_t wake = hb_sleep_wake_cause();
    static const char *const wakes[] = {"power-on", "button", "timer"};
    ESP_LOGI(TAG, "Heart beacon %s, wake %s", esp_app_get_description()->version, wakes[wake]);

    s_q = xQueueCreate(24, sizeof(q_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_store_init());
    static const uint8_t dark[HB_PIXELS][3];  // hb_core assumes the pixels start dark
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_init()) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_show(dark));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_adc_init());
    btn_init();
    bool provisioned = load();
    hb_console_start();
    bool wifi = ESP_ERROR_CHECK_WITHOUT_ABORT(hb_wifi_init()) == ESP_OK && provisioned;
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_sleep_auto_light());

    s_x.fw = esp_app_get_description()->version;
    s_x.hw = "A";
    s_x.rst = reset_name();
    s_x.cfg_key = cfg_key;
    s_x.cfg_key_len = (size_t)(cfg_key_end - cfg_key);  // PEM with its NUL (EMBED_TXTFILES)
#if CONFIG_HB_BENCH_FAST_NAP
    s_cfg.idle_nap_min = 1;
    s_cfg.nap_wake_min = 1;
#endif
    time_t t = time(NULL);  // RTC time survives deep sleep; set by the router's `time` after each connect
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
    ESP_LOGI(TAG, "battery %lu mV, claimed %d, clock %s, link %s", (unsigned long)boot.mv, boot.claimed,
             boot.epoch ? "set" : "unknown", wifi ? "on" : "off (prov: see console)");

    hb_actions_t act;
    hb_core_boot(&s_core, &s_cfg, &boot, now_ms(), &act);
    for (;;) {
        apply(&act);
        q_t q;
        hb_ev_t ev = {.type = HB_EV_TICK};
        if (xQueueReceive(s_q, &q, pdMS_TO_TICKS(act.next_ms)) == pdTRUE && !to_event(&q, &ev)) {
            ev.type = HB_EV_TICK;
        }
        if (ev.type == HB_EV_TICK) {
            ev.pressed = btn_pressed();
        }
        hb_core_step(&s_core, &ev, now_ms(), &act);
    }
}
