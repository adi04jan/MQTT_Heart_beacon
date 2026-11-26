// WS2812-class pixels on HB_LED_GPIO through the RMT TX channel. The channel is enabled only around a frame:
// an enabled channel holds a power-management lock and would keep the chip out of light sleep.
// Rev B gates the LED rail (HB_LED_PWR_GPIO, high = on): on before the first lit frame, off once the pixels have
// stayed dark for RAIL_HOLD_US, so the dark gaps inside an effect (between heartbeats) don't cycle the rail and
// its inrush. The data line idles low, so nothing back-powers the strip through DIN while the rail is off.
#include "hb_port.h"

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#define PWR CONFIG_HB_LED_PWR_GPIO
#define RAIL_HOLD_US 2000000  // dark this long before the rail goes off
#define RAIL_UP_US 1000       // ponytail: pixel power-on settle, a guess; raise it if rev B drops the first frame

#define RES_HZ 10000000  // 0.1 us per tick
#if CONFIG_HB_LED_WS2811_400K
#define T0H 5   // Adafruit esp.c WS2811_T*_NS: 0.5/2.0 us and 1.2/1.3 us
#define T0L 20
#define T1H 12
#define T1L 13
#else
#define T0H 4   // Adafruit esp.c on IDF 5, whatever the speed flag: 0.4/0.8 us and 0.8/0.4 us
#define T0L 8
#define T1H 8
#define T1L 4
#endif

static const char *TAG = "led";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;
#if PWR >= 0
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_off;
static bool s_on, s_dark = true;

static void rail(bool on)
{
    gpio_set_level(PWR, on);
    s_on = on;
}

static void rail_off(void *arg)  // esp_timer task; the lock orders it against a lit frame on the main task
{
    portENTER_CRITICAL(&s_mux);
    if (s_on && s_dark) {
        rail(false);
    }
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGD(TAG, "rail off");
}
#endif

esp_err_t hb_led_init(void)
{
    gpio_hold_dis(HB_LED_GPIO);  // hb_led_park() held it low through deep sleep
    gpio_deep_sleep_hold_dis();
#if PWR >= 0
    gpio_config_t pc = {.pin_bit_mask = BIT64(PWR), .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&pc), TAG, "rail gpio");
    rail(false);
    ESP_RETURN_ON_ERROR(gpio_sleep_sel_dis(PWR), TAG, "rail sleep");  // keep driving it through light sleep
    esp_timer_create_args_t ta = {.callback = rail_off, .name = "led_rail"};
    ESP_RETURN_ON_ERROR(esp_timer_create(&ta, &s_off), TAG, "rail timer");
#endif
    rmt_tx_channel_config_t cc = {
        .gpio_num = HB_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RES_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&cc, &s_chan), TAG, "channel");
    rmt_bytes_encoder_config_t ec = {
        .bit0 = {.level0 = 1, .duration0 = T0H, .level1 = 0, .duration1 = T0L},
        .bit1 = {.level0 = 1, .duration0 = T1H, .level1 = 0, .duration1 = T1L},
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&ec, &s_enc), TAG, "encoder");
    // With tickless idle IDF floats every pad in light sleep (ESP_SLEEP_GPIO_RESET_WORKAROUND); a floating
    // data line can clock garbage into the pixels, so this one keeps its idle-low drive.
    return gpio_sleep_sel_dis(HB_LED_GPIO);
}

esp_err_t hb_led_show(const uint8_t rgb[HB_PIXELS][3])
{
    uint8_t grb[HB_PIXELS * 3];
    for (int i = 0; i < HB_PIXELS; i++) {
        grb[3 * i] = rgb[i][1];
        grb[3 * i + 1] = rgb[i][0];
        grb[3 * i + 2] = rgb[i][2];
    }
#if PWR >= 0
    bool dark = true;
    for (size_t i = 0; i < sizeof grb && dark; i++) {
        dark = grb[i] == 0;
    }
    if (dark) {
        if (!s_on) {
            return ESP_OK;  // an unpowered strip is already dark
        }
        s_dark = true;
        esp_timer_stop(s_off);
        esp_timer_start_once(s_off, RAIL_HOLD_US);
    } else {
        esp_timer_stop(s_off);
        portENTER_CRITICAL(&s_mux);
        s_dark = false;
        bool was = s_on;
        if (!was) {
            rail(true);
        }
        portEXIT_CRITICAL(&s_mux);
        if (!was) {
            ESP_LOGD(TAG, "rail on");
            esp_rom_delay_us(RAIL_UP_US);
        }
    }
#endif
    rmt_transmit_config_t tc = {.loop_count = 0, .flags.eot_level = 0};
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "enable");
    esp_err_t err = rmt_transmit(s_chan, s_enc, grb, sizeof grb, &tc);
    if (err == ESP_OK) {
        err = rmt_tx_wait_all_done(s_chan, 50);  // a frame is 0.3-0.7 ms
    }
    rmt_disable(s_chan);
    return err;
}

void hb_led_park(void)
{
#if PWR >= 0
    esp_timer_stop(s_off);
    rail(false);  // R8 keeps the switch off through deep sleep
#endif
    gpio_set_direction(HB_LED_GPIO, GPIO_MODE_OUTPUT);  // takes the pad back from the RMT
    gpio_set_level(HB_LED_GPIO, 0);
    gpio_hold_en(HB_LED_GPIO);
    gpio_deep_sleep_hold_en();
}
