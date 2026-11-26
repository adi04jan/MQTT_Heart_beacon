// BLE peripheral (PROTOCOL.md 6) on NimBLE: one service, RX written by the phone and TX notified to it, both
// behind encryption. LE Secure Connections, Just Works, up to 3 bonds in NVS (oldest dropped). A phone may
// bond only inside the pairing window; outside it an unbonded or unencrypted link is dropped.
#include "hb_port.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hb_frame.h"
#include "hb_msg.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);                     // NimBLE's NVS bond store (no public header)
void ble_gatts_set_clt_cfg_perm_flags(uint8_t flags);  // ble_gatts.c: CCCD permissions

static const char *TAG = "ble";

#define GUARD_US 5000000  // PROTOCOL.md 6: 5 s to encrypt (outside the window), then the connection parameters
#define MTU 247           // PROTOCOL.md 6: both sides request 247

// 4842000n-6265-6163-6F6E-763200000000, little-endian as NimBLE stores it.
#define HB_UUID(n) BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x32, 0x76, 0x6e, 0x6f, 0x63, 0x61, 0x65, 0x62, n, 0x00, 0x42, 0x48)
static const ble_uuid128_t k_svc = HB_UUID(0x01), k_rx = HB_UUID(0x02), k_tx = HB_UUID(0x03);

static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_enc;     // encrypted with a bond: the link hb_core counts
static volatile bool s_window;  // pairing window: new bonds allowed
static volatile bool s_synced;
static volatile uint16_t s_mtu = BLE_ATT_MTU_DFLT;
static uint8_t s_tx_seq, s_own_addr_type, s_param_tries;
static hb_reasm_t s_reasm;  // NimBLE host task only
static char s_name[HB_DID_LEN + 1];
static struct {
    uint16_t fast_ms, fast_s, slow_ms, conn_ms, latency, sup_ms;
} s_p;
static esp_timer_handle_t s_slow, s_guard;
static volatile int s_tx_pending;  // notifications handed to NimBLE, not yet reported sent (NOTIFY_TX)

static int gap_event(struct ble_gap_event *e, void *arg);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void post_link(bool up)
{
    hb_ev_t ev = {.type = HB_EV_BLE, .up = up};
    hb_port_post(&ev);
}

static int rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn, (void)attr, (void)arg;
    uint8_t frame[BLE_ATT_MTU_MAX];
    uint16_t len = 0;
    // WRITE_ENC only asks for encryption; an unbonded peer is encrypted for a moment before ENC_CHANGE drops
    // it. Only the bonded link (s_enc) may talk.
    if (conn != s_conn || !s_enc) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || OS_MBUF_PKTLEN(ctxt->om) > sizeof frame) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, frame, sizeof frame, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (hb_reasm_push(&s_reasm, frame, len, now_ms()) == HB_FRAME_COMPLETE) {
        hb_port_post_ble((const char *)s_reasm.buf, s_reasm.len);
    }
    return 0;
}

static int tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn, (void)attr, (void)ctxt, (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;  // notify only
}

static const struct ble_gatt_svc_def k_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_svc.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                // Write and write-without-response (the app sends all but a message's last frame without one); both need
                // encryption, and rx_access takes them only from the bonded link.
                {.uuid = &k_rx.u, .access_cb = rx_access,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
                {.uuid = &k_tx.u, .access_cb = tx_access, .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_tx_handle},
                {0},
            },
    },
    {0},
};

static void adv_start(bool fast)
{
    if (!s_synced || s_conn != BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    ble_gap_adv_stop();
    esp_timer_stop(s_slow);
    struct ble_hs_adv_fields f = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &k_svc,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    struct ble_hs_adv_fields r = {.name = (uint8_t *)s_name, .name_len = (uint8_t)strlen(s_name), .name_is_complete = 1};
    uint16_t ms = fast ? s_p.fast_ms : s_p.slow_ms;
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ms),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ms),
    };
    int rc = ble_gap_adv_set_fields(&f);
    rc = rc ? rc : ble_gap_adv_rsp_set_fields(&r);
    rc = rc ? rc : ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc) {
        ESP_LOGW(TAG, "advertise: rc %d", rc);
    } else if (fast && s_p.fast_s) {
        esp_timer_start_once(s_slow, (uint64_t)s_p.fast_s * 1000000u);
    }
}

static void slow_cb(void *arg)
{
    (void)arg;
    adv_start(false);
}

// The phone's defaults while pairing and claiming (fast); the config's slow, low-power ones after that.
static void request_params(void)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_enc || s_window) {
        return;
    }
    struct ble_gap_upd_params u = {
        .itvl_min = BLE_GAP_CONN_ITVL_MS(s_p.conn_ms),
        .itvl_max = BLE_GAP_CONN_ITVL_MS(s_p.conn_ms),
        .latency = s_p.latency,
        .supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(s_p.sup_ms),
    };
    int rc = ble_gap_update_params(conn, &u);
    ESP_LOGI(TAG, "params %u ms, latency %u: rc %d", s_p.conn_ms, s_p.latency, rc);
}

static void guard_cb(void *arg)
{
    (void)arg;
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    if (s_enc) {
        s_param_tries = 1;
        request_params();
    } else if (!s_window) {
        ESP_LOGW(TAG, "not encrypted after 5 s, dropped");
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        esp_timer_start_once(s_guard, GUARD_US);  // pairing may take a while; look again
    }
}

static int gap_event(struct ble_gap_event *e, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc d;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status != 0) {
            adv_start(true);
            return 0;
        }
        s_conn = e->connect.conn_handle;
        s_enc = false;
        s_mtu = BLE_ATT_MTU_DFLT;
        s_param_tries = 0;
        hb_reasm_reset(&s_reasm);
        esp_timer_stop(s_slow);
        esp_timer_start_once(s_guard, GUARD_US);
        ESP_LOGI(TAG, "connect");
        return 0;
    case BLE_GAP_EVENT_DISCONNECT: {
        bool was = s_enc;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_enc = false;
        hb_reasm_reset(&s_reasm);
        esp_timer_stop(s_guard);
        ESP_LOGI(TAG, "disconnect: reason 0x%x", e->disconnect.reason);
        if (was) {
            post_link(false);
        }
        adv_start(true);  // PROTOCOL.md 6: fast after a disconnect
        return 0;
    }
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (e->enc_change.status != 0 || ble_gap_conn_find(e->enc_change.conn_handle, &d) != 0) {
            ESP_LOGW(TAG, "encryption failed: status %d", e->enc_change.status);
            return 0;
        }
        if (!d.sec_state.bonded) {  // paired outside the window, where bonding is off
            ESP_LOGW(TAG, "unbonded peer, dropped");
            ble_gap_terminate(e->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
            return 0;
        }
        ESP_LOGI(TAG, "encrypted, bonded");
        s_enc = true;
        post_link(true);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // The phone lost its keys but we kept ours: it may pair again only inside the window.
        if (!s_window || ble_gap_conn_find(e->repeat_pairing.conn_handle, &d) != 0) {
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        ble_store_util_delete_peer(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_MTU:
        s_mtu = e->mtu.value;
        return 0;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (!e->notify_tx.indication && __atomic_sub_fetch(&s_tx_pending, 1, __ATOMIC_RELAXED) < 0) {
            s_tx_pending = 0;
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (e->conn_update.status != 0 && s_param_tries == 1) {
            s_param_tries = 2;  // PROTOCOL.md 6: re-request once if refused
            request_params();
        }
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start(true);
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);  // public: the chip's own BT address
    s_synced = true;
    adv_start(true);
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void hb_ble_params(const hb_cfg_t *cfg)
{
    s_p.fast_ms = cfg->adv_fast_ms;
    s_p.fast_s = cfg->adv_fast_s;
    s_p.slow_ms = cfg->adv_slow_ms;
    s_p.conn_ms = cfg->conn_ms;
    s_p.latency = cfg->latency;
    s_p.sup_ms = cfg->sup_ms;
}

esp_err_t hb_ble_init(const char *did, const hb_cfg_t *cfg)
{
    snprintf(s_name, sizeof s_name, "%s", did);
    hb_ble_params(cfg);
    esp_timer_create_args_t slow = {.callback = slow_cb, .name = "ble_slow"};
    esp_timer_create_args_t guard = {.callback = guard_cb, .name = "ble_guard"};
    esp_err_t err = esp_timer_create(&slow, &s_slow);
    err = err ? err : esp_timer_create(&guard, &s_guard);
    err = err ? err : nimble_port_init();
    if (err != ESP_OK) {
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;  // a fourth bond replaces the oldest
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;              // Just Works
    ble_hs_cfg.sm_bonding = 0;                               // hb_ble_window turns it on
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    // Subscribing to TX needs encryption too: that CCCD write is what makes a new phone pair (GattLink.kt).
    ble_gatts_set_clt_cfg_perm_flags(BLE_ATT_F_READ | BLE_ATT_F_WRITE | BLE_ATT_F_WRITE_ENC);
    int rc = ble_gatts_count_cfg(k_svcs);
    rc = rc ? rc : ble_gatts_add_svcs(k_svcs);
    rc = rc ? rc : ble_svc_gap_device_name_set(s_name);
    rc = rc ? rc : ble_att_set_preferred_mtu(MTU);
    if (rc) {
        ESP_LOGE(TAG, "gatt: rc %d", rc);
        return ESP_FAIL;
    }
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void hb_ble_window(bool open)
{
    if (open == s_window) {
        return;
    }
    s_window = open;
    ble_hs_cfg.sm_bonding = open;
    ESP_LOGI(TAG, "pairing window %s", open ? "open" : "closed");
    if (open) {
        adv_start(true);  // the phone finds it at once
    } else {
        request_params();
    }
}

bool hb_ble_send(const char *json, int len)
{
    uint16_t conn = s_conn, mtu = s_mtu > BLE_ATT_MTU_MAX ? BLE_ATT_MTU_MAX : s_mtu;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_enc) {
        return false;
    }
    uint8_t frame[BLE_ATT_MTU_MAX];
    uint8_t seq = s_tx_seq;
    s_tx_seq = (uint8_t)((s_tx_seq + 1) & 0x7f);
    size_t off = 0;
    int n;
    while ((n = hb_frame_next((const uint8_t *)json, (size_t)len, seq, mtu, &off, frame)) > 0) {
        int rc = BLE_HS_ENOMEM;
        // ponytail: a burst (state + tele + resp) can run the mbuf pool dry; wait for it, bounded at 200 ms
        for (int tries = 0; rc == BLE_HS_ENOMEM && tries < 10; tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(frame, (uint16_t)n);
            rc = om ? ble_gatts_notify_custom(conn, s_tx_handle, om) : BLE_HS_ENOMEM;  // consumes om
            if (rc == BLE_HS_ENOMEM) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        if (rc) {
            ESP_LOGW(TAG, "notify: rc %d", rc);
            return false;
        }
        __atomic_add_fetch(&s_tx_pending, 1, __ATOMIC_RELAXED);
    }
    return n == 0;
}

void hb_ble_flush(uint32_t ms)
{
    uint32_t t0 = now_ms();
    while (s_tx_pending > 0 && now_ms() - t0 < ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    // Handed to the controller is not on the air yet: give it two connection events (bounded), then hang up so
    // the phone sees the disconnect at once instead of after the supervision timeout.
    uint32_t air = 2u * s_p.conn_ms * (1u + s_p.latency);
    vTaskDelay(pdMS_TO_TICKS(air < 1500 ? air : 1500));
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    for (int i = 0; i < 25 && s_conn != BLE_HS_CONN_HANDLE_NONE; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void hb_ble_forget_all(void)
{
    ble_store_clear();
}
