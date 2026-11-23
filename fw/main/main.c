// Heart Beacon v2 (M1: radios off). One queue, one loop: events in, hb_core_step, actions out.
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hb_core.h"
#include "hb_policy.h"
#include "hb_port.h"
#include "sdkconfig.h"

#if CONFIG_HB_HAS_XTAL32K && CONFIG_HB_BTN_GPIO <= 1
#error "the 32 kHz crystal takes GPIO0/1: move the button (HB_BTN_GPIO) to GPIO2-5"
#endif
#if CONFIG_HB_HAS_XTAL32K && !CONFIG_RTC_CLK_SRC_EXT_CRYS
#error "HB_HAS_XTAL32K needs CONFIG_RTC_CLK_SRC_EXT_CRYS=y"
#endif

#define BTN CONFIG_HB_BTN_GPIO

static const char *TAG = "hb";
static RTC_DATA_ATTR hb_core_t s_core;  // kept through deep sleep: the state we slept in, battery flags
static QueueHandle_t s_q;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool btn_pressed(void)
{
    return gpio_get_level(BTN) == 0;
}

// Low-level interrupt, so a press also wakes automatic light sleep. Disarmed here (a held button would
// fire forever) and re-armed by the loop once the pin reads released; hb_core samples it on each tick.
static void btn_isr(void *arg)
{
    (void)arg;
    gpio_intr_disable(BTN);
    hb_ev_t ev = {.type = HB_EV_TICK};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_q, &ev, &woken);
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

static void apply(const hb_actions_t *a)
{
    static const char *const moods[] = {"none", "heart", "sorry", "annoy"};
    if (a->led_dirty) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_show(a->led));
    }
    if (a->mood) {
        ESP_LOGI(TAG, "mood %s (radios off, not sent)", moods[a->mood]);
    }
    if (a->state_changed) {
        ESP_LOGI(TAG, "state %s", hb_state_name(a->state));
    }
    if (a->measure_batt) {
        hb_ev_t ev = {.type = HB_EV_BATT, .mv = hb_adc_batt_mv()};
        ESP_LOGD(TAG, "battery %lu mV", (unsigned long)ev.mv);
        xQueueSend(s_q, &ev, 0);
    }
    if (!btn_pressed()) {
        gpio_intr_enable(BTN);
    }
    if (a->sleep) {
        ESP_LOGI(TAG, "deep sleep, timer %lu s", (unsigned long)a->sleep_s);
        hb_sleep_deep(a->sleep_s);
    }
}

void app_main(void)
{
    // The v1 bootloader has rollback on: an OTA-written image stays NEW until confirmed, and the next
    // reset would return the unit to v1. ponytail: confirmed at once; M7 confirms after a self-test.
    esp_ota_mark_app_valid_cancel_rollback();
    hb_wake_t wake = hb_sleep_wake_cause();
    static const char *const wakes[] = {"power-on", "button", "timer"};
    ESP_LOGI(TAG, "Heart beacon %s, wake %s", esp_app_get_description()->version, wakes[wake]);

    s_q = xQueueCreate(16, sizeof(hb_ev_t));
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_store_init());
    static const uint8_t dark[HB_PIXELS][3];  // hb_core assumes the pixels start dark
    if (ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_init()) == ESP_OK) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(hb_led_show(dark));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_adc_init());
    btn_init();
    ESP_ERROR_CHECK_WITHOUT_ABORT(hb_sleep_auto_light());

    hb_cfg_t cfg = hb_cfg_default;  // hb_config (M3) loads the signed config here
#if CONFIG_HB_BENCH_FAST_NAP
    cfg.idle_nap_min = 1;
    cfg.nap_wake_min = 1;
#endif
    time_t t = time(NULL);  // RTC time survives deep sleep; nothing sets it until `time` arrives (M3)
    hb_boot_t boot = {
        .wake = wake,
        .claimed = hb_store_get_u8("claimed", 0),
        .pressed = btn_pressed(),
        .mv = hb_adc_batt_mv(),
        .epoch = t > 1700000000 ? (uint32_t)t : 0,
    };
    ESP_LOGI(TAG, "battery %lu mV, claimed %d, clock %s", (unsigned long)boot.mv, boot.claimed,
             boot.epoch ? "set" : "unknown");

    hb_actions_t act;
    hb_core_boot(&s_core, &cfg, &boot, now_ms(), &act);
    for (;;) {
        apply(&act);
        hb_ev_t ev;
        if (xQueueReceive(s_q, &ev, pdMS_TO_TICKS(act.next_ms)) != pdTRUE) {
            ev.type = HB_EV_TICK;
        }
        if (ev.type == HB_EV_TICK) {
            ev.pressed = btn_pressed();
        }
        hb_core_step(&s_core, &ev, now_ms(), &act);
    }
}
